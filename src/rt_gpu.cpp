#include <rt_gpu.h>

#include <algorithm>
#include <cstring>

#include <vk_engine.h>
#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_tonemap.h>

namespace {

//the push-constant block every stage takes (shaders/crt_common.glsl `Params`); `bounce` is the
//only field that changes between dispatches of one frame
struct CrtParams {
	glm::vec4 origin;
	glm::vec4 lowerLeft;
	glm::vec4 horizontal;
	glm::vec4 vertical;
	// n, with the lens radius in w
	glm::vec4 lensU;
	// b
	glm::vec4 lensV;
	uint32_t width;
	uint32_t height;
	uint32_t samplesPerFrame;
	uint32_t poolSize;
	uint32_t seed;
	uint32_t bounce;
	uint32_t rayDepth;
	uint32_t instanceCount;
	uint32_t pad2;
	uint32_t flags;
	uint32_t minBouncesBeforeRoulette;
	uint32_t debugView;
	uint32_t maxSamples;
	float environmentIntensity;
	float pad0;
	float pad1;
};
static_assert(sizeof(CrtParams) == 160);

constexpr uint32_t CRT_FLAG_JITTER = 1u << 0;
constexpr uint32_t CRT_FLAG_ROULETTE = 1u << 1;
constexpr uint32_t CRT_FLAG_ENVIRONMENT_MAP = 1u << 2;

//shaders/crt_common.glsl CRT_WORKGROUP; every stage is 1-D at this size
constexpr uint32_t CRT_WORKGROUP = 64;

//mirrors of the remaining GLSL structs, for buffer sizing only
constexpr VkDeviceSize PATH_STATE_SIZE = 48;
constexpr VkDeviceSize HIT_RECORD_SIZE = 48;

//header slots per frame slot in the readback buffer: one after generate, one after every shade
constexpr uint32_t READBACK_HEADERS_PER_SLOT = CRT_MAX_DEPTH + 1;

//shaders/crt_common.glsl traversalStats: nodes visited and primitives tested, per bounce
constexpr VkDeviceSize TRAVERSAL_STATS_BYTES = sizeof(uint32_t) * 2 * CRT_MAX_DEPTH;

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

}

const char* crtDebugViewName(CrtDebugView view)
{
	switch (view) {
	case CrtDebugView::None:
		return "none";
	case CrtDebugView::PrimaryDirection:
		return "primary direction";
	case CrtDebugView::HitMiss:
		return "hit / miss";
	case CrtDebugView::Normal:
		return "normal";
	case CrtDebugView::BounceHeat:
		return "bounce heat";
	case CrtDebugView::SampleCountHeat:
		return "sample-count heat";
	case CrtDebugView::TraversalCost:
		return "BVH traversal cost";
	case CrtDebugView::Count:
		break;
	}
	return "unknown";
}

void GpuPathTracer::init(VulkanEngine* engine)
{
	const std::string shaderDir = engine->m_rootPath + "shaders/";

	//every stage binds the same set: the whole pool, the scene and the images, at fixed bindings
	//(shaders/crt_common.glsl). A shader that does not use a binding simply does not declare it,
	//and one set per frame written once serves all four dispatches - descriptor set layouts that
	//are defined identically are compatible
	auto buildStage = [&](const char* shaderFile) {
		return ComputePassBuilder(shaderDir + shaderFile)
			.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // paths
			.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // hits
			.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // queues
			.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // headers
			.addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // radiance
			.addBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // sample budget
			.addBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // instances
			.addBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // materials
			.addBinding(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) // accumulation
			.addBinding(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) // sample count
			.addBinding(10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) // environment map
			.addBinding(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // BLAS triangles
			.addBinding(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) // base-colour texture array
			.addBinding(13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // BLAS nodes
			.addBinding(14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // triangle attributes
			.addBinding(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // TLAS nodes
			.addBinding(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // per-instance surface materials
			.addBinding(17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) // traversal counters
			.setPushConstants<CrtParams>()
			.setWorkgroupSize(CRT_WORKGROUP)
			.build(engine->m_device);
	};

	m_generate = buildStage("crt_generate.comp.spv");
	m_extend = buildStage("crt_extend.comp.spv");
	m_extendCwbvh = buildStage("crt_extend_cwbvh.comp.spv");
	m_shade = buildStage("crt_shade.comp.spv");
	m_resolve = buildStage("crt_resolve.comp.spv");

	//timestamps are optional on the device; the readout is simply absent without them
	const VkPhysicalDeviceLimits& limits = engine->m_gpuProperties.limits;
	m_hasTimestamps = limits.timestampComputeAndGraphics == VK_TRUE && limits.timestampPeriod > 0.f;
	m_timestampPeriodNs = limits.timestampPeriod;
	if (m_hasTimestamps) {
		VkQueryPoolCreateInfo queryInfo { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queryInfo.queryCount = 2 * FRAME_OVERLAP;
		checkVkResult(vkCreateQueryPool(engine->m_device, &queryInfo, nullptr, &m_queryPool));

		//queries must be reset before their first use; the per-frame reset covers every later one
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			vkCmdResetQueryPool(cmd, m_queryPool, 0, 2 * FRAME_OVERLAP);
		});
	}
	fmt::println("GpuPathTracer: timestamps {} (period {} ns)", m_hasTimestamps ? "available" : "unavailable", m_timestampPeriodNs);

	m_headerReadback = engine->createBuffer((VkDeviceSize)FRAME_OVERLAP * READBACK_HEADERS_PER_SLOT * sizeof(CrtQueueHeader), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	m_traversalStats = engine->createBuffer(TRAVERSAL_STATS_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_statsReadback = engine->createBuffer((VkDeviceSize)FRAME_OVERLAP * TRAVERSAL_STATS_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	m_slotRender.assign(FRAME_OVERLAP, 0);
	m_slotHeaderCount.assign(FRAME_OVERLAP, 0);
}

void GpuPathTracer::destroy(VulkanEngine* engine)
{
	freePool(engine);

	if (m_sceneBuffer.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_sceneBuffer);
		m_sceneBuffer = {};
	}
	if (m_geometryBuffer.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_geometryBuffer);
		m_geometryBuffer = {};
	}
	m_uploadedScene.reset();
	m_uploadedGeometry.reset();
	if (m_headerReadback.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_headerReadback);
		m_headerReadback = {};
	}
	if (m_traversalStats.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_traversalStats);
		engine->destroyBuffer(m_statsReadback);
		m_traversalStats = {};
		m_statsReadback = {};
	}
	if (m_queryPool != VK_NULL_HANDLE) {
		vkDestroyQueryPool(engine->m_device, m_queryPool, nullptr);
		m_queryPool = VK_NULL_HANDLE;
	}

	m_generate.destroy(engine->m_device);
	m_extend.destroy(engine->m_device);
	m_extendCwbvh.destroy(engine->m_device);
	m_shade.destroy(engine->m_device);
	m_resolve.destroy(engine->m_device);
}

void GpuPathTracer::allocatePool(VulkanEngine* engine, uint32_t width, uint32_t height, uint32_t samplesPerFrame)
{
	if (m_hasPool && m_poolWidth == width && m_poolHeight == height && m_samplesPerFrame == samplesPerFrame) {
		return;
	}

	freePool(engine);

	m_poolWidth = width;
	m_poolHeight = height;
	m_samplesPerFrame = samplesPerFrame;
	m_poolSize = width * height * samplesPerFrame;

	const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	m_paths = engine->createBuffer(PATH_STATE_SIZE * m_poolSize, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_hits = engine->createBuffer(HIT_RECORD_SIZE * m_poolSize, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_queues = engine->createBuffer(sizeof(uint32_t) * 2 * (VkDeviceSize)m_poolSize, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_radiance = engine->createBuffer(sizeof(glm::vec4) * (VkDeviceSize)m_poolSize, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_sampleBudget = engine->createBuffer(sizeof(uint32_t) * (VkDeviceSize)width * height, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	//the headers are the indirect arguments, reset by the command buffer and read back for the readout
	m_headers = engine->createBuffer(sizeof(CrtQueueHeader) * 2, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	const VkExtent3D extent { width, height, 1 };
	m_accumulation = engine->createImage(extent, TonemapPass::LINEAR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	m_sampleCount = engine->createImage(extent, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

	//both images live in GENERAL for good: written by resolve, read by the tonemap and cleared
	//by the command buffer, all of which accept it
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_accumulation.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, m_sampleCount.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	});

	m_hasPool = true;

	const double megabytes = double((PATH_STATE_SIZE + HIT_RECORD_SIZE + 8 + 16) * m_poolSize + 4 * (VkDeviceSize)width * height + 20 * (VkDeviceSize)width * height) / (1024.0 * 1024.0);
	fmt::println("GpuPathTracer: pool {} x {} x {} = {} paths, {:.0f} MB", width, height, samplesPerFrame, m_poolSize, megabytes);
}

void GpuPathTracer::freePool(VulkanEngine* engine)
{
	if (!m_hasPool) {
		return;
	}

	engine->destroyBuffer(m_paths);
	engine->destroyBuffer(m_hits);
	engine->destroyBuffer(m_queues);
	engine->destroyBuffer(m_radiance);
	engine->destroyBuffer(m_sampleBudget);
	engine->destroyBuffer(m_headers);
	engine->destroyImage(m_accumulation);
	engine->destroyImage(m_sampleCount);
	m_paths = m_hits = m_queues = m_radiance = m_sampleBudget = m_headers = {};
	m_accumulation = m_sampleCount = {};
	m_hasPool = false;
	m_hasImage = false;
}

namespace {

//the tagged-union form the shader scatters with. `albedoLayer` is the caller's, since only a
//triangle's glTF material has one
CrtMaterial gpuMaterial(const SphereMaterial& material, int albedoLayer)
{
	CrtMaterial out {};
	out.albedo = material.albedo;
	out.type = (uint32_t)material.type;
	out.albedoLayer = albedoLayer;
	switch (material.type) {
	case MaterialType::Lambertian:
		out.param = 0.f;
		break;
	case MaterialType::Metal:
		out.param = std::min(material.fuzz, 1.f);
		break;
	case MaterialType::Phong:
		out.param = material.smoothness;
		break;
	case MaterialType::Dielectric:
		out.param = material.ir;
		break;
	}
	return out;
}

}

void GpuPathTracer::retireBuffer(VulkanEngine* engine, AllocatedBuffer& buffer)
{
	if (buffer.buffer == VK_NULL_HANDLE) {
		return;
	}
	//the previous render's buffer may still be read by the frame in flight, so it goes to the
	//deletion queue of the slot that frame used - flushed once its fence has been waited on, at the
	//start of the frame after next
	const AllocatedBuffer old = buffer;
	engine->m_frames[(engine->m_frameNumber + 1) % FRAME_OVERLAP].deletionQueue.push_function([engine, old]() {
		engine->destroyBuffer(old);
	});
	buffer = {};
}

void GpuPathTracer::uploadGeometry(VulkanEngine* engine, const std::shared_ptr<const RaytraceGeometry>& geometry)
{
	if (m_uploadedGeometry == geometry && m_geometryBuffer.buffer != VK_NULL_HANDLE) {
		return;
	}

	//three ranges in one device-local buffer. The traversal reads nodes and triangles on every step
	//of every ray, so they belong in VRAM rather than in host-visible memory the GPU would fetch
	//across the bus; a zero-length range is invalid, so an empty one still gets one (unused) entry
	const VkDeviceSize alignment = std::max<VkDeviceSize>(engine->m_gpuProperties.limits.minStorageBufferOffsetAlignment, 16);
	const size_t nodes = geometry != nullptr ? geometry->nodes.size() : 0;
	const size_t triangles = geometry != nullptr ? geometry->triangles.size() : 0;
	const size_t attributes = geometry != nullptr ? geometry->attributes.size() : 0;
	m_blasNodesOffset = 0;
	m_blasNodesBytes = sizeof(glm::uvec4) * std::max<size_t>(nodes, 1);
	m_blasTrianglesOffset = alignUp(m_blasNodesOffset + m_blasNodesBytes, alignment);
	m_blasTrianglesBytes = sizeof(BvhTriangle) * std::max<size_t>(triangles, 1);
	m_attributesOffset = alignUp(m_blasTrianglesOffset + m_blasTrianglesBytes, alignment);
	m_attributesBytes = sizeof(GpuTriangleAttributes) * std::max<size_t>(attributes, 1);
	const VkDeviceSize totalBytes = m_attributesOffset + m_attributesBytes;

	AllocatedBuffer staging = engine->createBuffer(totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	uint8_t* mapped = (uint8_t*)staging.info.pMappedData;
	memset(mapped, 0, totalBytes);
	if (geometry != nullptr) {
		memcpy(mapped + m_blasNodesOffset, geometry->nodes.data(), sizeof(glm::uvec4) * nodes);
		memcpy(mapped + m_blasTrianglesOffset, geometry->triangles.data(), sizeof(BvhTriangle) * triangles);
		memcpy(mapped + m_attributesOffset, geometry->attributes.data(), sizeof(GpuTriangleAttributes) * attributes);
	}
	vmaFlushAllocation(engine->m_memAllocator, staging.allocation, 0, totalBytes); //flush vma on MoltenVK

	retireBuffer(engine, m_geometryBuffer);
	m_geometryBuffer = engine->createBuffer(totalBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy copy {};
		copy.size = totalBytes;
		vkCmdCopyBuffer(cmd, staging.buffer, m_geometryBuffer.buffer, 1, &copy);
	});
	engine->destroyBuffer(staging);

	m_uploadedGeometry = geometry;
	fmt::println("GpuPathTracer: BVH geometry uploaded - {} node words, {} triangles, {} attribute records, {:.1f} MB",
		nodes, triangles, attributes, double(totalBytes) / (1024.0 * 1024.0));
}

void GpuPathTracer::uploadScene(VulkanEngine* engine)
{
	const std::shared_ptr<const RaytraceSceneAccel>& scene = m_snapshot.scene;
	uploadGeometry(engine, scene != nullptr ? scene->geometry : nullptr);

	//a render restarted by a camera drag arrives here every frame with the same scene; the scene
	//object is only ever rebuilt for a real change, so the pointer says whether anything moved
	if (m_uploadedScene == scene && m_sceneBuffer.buffer != VK_NULL_HANDLE) {
		return;
	}

	const size_t instances = scene != nullptr ? scene->instances.size() : 0;
	const size_t materials = scene != nullptr ? scene->materials.size() : 0;
	const size_t instanceMaterials = scene != nullptr ? scene->instanceMaterials.size() : 0;
	const size_t tlasWords = scene != nullptr ? scene->tlas.bvh.nodes.size() : 0;
	//nothing is traced through a TLAS that failed to pack; the instance count is what the shader checks
	const bool traceable = scene != nullptr && scene->tlas.bvh.error.empty() && scene->tlas.bvh.nodeCount > 0;
	m_instanceCount = traceable ? (uint32_t)instances : 0;

	const VkDeviceSize alignment = std::max<VkDeviceSize>(engine->m_gpuProperties.limits.minStorageBufferOffsetAlignment, 16);
	m_instancesBytes = sizeof(GpuInstance) * std::max<size_t>(instances, 1);
	m_materialsOffset = alignUp(m_instancesBytes, alignment);
	m_materialsBytes = sizeof(CrtMaterial) * std::max<size_t>(materials, 1);
	m_instanceMaterialsOffset = alignUp(m_materialsOffset + m_materialsBytes, alignment);
	m_instanceMaterialsBytes = sizeof(uint32_t) * std::max<size_t>(instanceMaterials, 1);
	m_tlasOffset = alignUp(m_instanceMaterialsOffset + m_instanceMaterialsBytes, alignment);
	m_tlasBytes = sizeof(glm::uvec4) * std::max<size_t>(tlasWords, 4);
	const VkDeviceSize totalBytes = m_tlasOffset + m_tlasBytes;

	retireBuffer(engine, m_sceneBuffer);
	m_sceneBuffer = engine->createBuffer(totalBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	uint8_t* mapped = (uint8_t*)m_sceneBuffer.info.pMappedData;
	memset(mapped, 0, totalBytes);
	if (scene != nullptr) {
		memcpy(mapped, scene->instances.data(), sizeof(GpuInstance) * instances);
		CrtMaterial* gpuMaterials = (CrtMaterial*)(mapped + m_materialsOffset);
		for (size_t i = 0; i < materials; i++) {
			gpuMaterials[i] = gpuMaterial(scene->materials[i].material, scene->materials[i].albedoLayer);
		}
		memcpy(mapped + m_instanceMaterialsOffset, scene->instanceMaterials.data(), sizeof(uint32_t) * instanceMaterials);
		memcpy(mapped + m_tlasOffset, scene->tlas.bvh.nodes.data(), sizeof(glm::uvec4) * tlasWords);
	}
	vmaFlushAllocation(engine->m_memAllocator, m_sceneBuffer.allocation, 0, totalBytes); //flush vma on MoltenVK

	m_uploadedScene = scene;
	fmt::println("GpuPathTracer: scene uploaded - {} instance(s) ({} sphere(s)), {} material(s), {} TLAS node(s), {:.2f} MB",
		instances, scene != nullptr ? scene->sphereCount : 0, materials, tlasWords / 4, double(totalBytes) / (1024.0 * 1024.0));
}

void GpuPathTracer::start(VulkanEngine* engine, GpuRenderSnapshot snapshot)
{
	m_snapshot = std::move(snapshot);

	//K is what fits: a pool over the cap lowers samplesPerFrame rather than refusing the render
	uint32_t samplesPerFrame = (uint32_t)std::clamp(m_snapshot.settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	const uint32_t pixels = m_snapshot.width * m_snapshot.height;
	if (!m_snapshot.settings.antialiasing) {
		samplesPerFrame = 1;
	}
	while (samplesPerFrame > 1 && pixels * samplesPerFrame > CRT_MAX_POOL) {
		samplesPerFrame--;
	}
	if (samplesPerFrame != (uint32_t)m_snapshot.settings.samplesPerFrame) {
		fmt::println("GpuPathTracer: samples per frame lowered to {} to keep the pool under {} paths", samplesPerFrame, CRT_MAX_POOL);
	}

	//a pool of a different size has to be reallocated, and the previous render's frames may
	//still be executing against the old one: a device-wide wait is the obviously-correct way
	//to retire it, and it only happens when the resolution or K changes
	if (!m_hasPool || m_poolWidth != m_snapshot.width || m_poolHeight != m_snapshot.height || m_samplesPerFrame != samplesPerFrame) {
		vkDeviceWaitIdle(engine->m_device);
		allocatePool(engine, m_snapshot.width, m_snapshot.height, samplesPerFrame);
	}
	uploadScene(engine);

	m_renderSerial++;
	m_running = true;
	m_clearPending = true;
	m_samplesAccumulated = 0;
	m_maxSamples = m_snapshot.settings.unlimitedSamples ? 0u : (uint32_t)std::max(m_snapshot.settings.maxSamples, 1);
	m_lastFrameGpuMs = 0.f;
	m_totalGpuMs = 0.f;
	m_traversalWork = {};
	m_pathsAlive.clear();
}

void GpuPathTracer::stop()
{
	m_running = false;
}

void GpuPathTracer::beginFrame(VulkanEngine* engine)
{
	//draw() has just waited on this slot's fence, so what it recorded FRAME_OVERLAP frames ago is done
	collectReadbacks(engine, (uint32_t)(engine->m_frameNumber % FRAME_OVERLAP));
}

void GpuPathTracer::collectReadbacks(VulkanEngine* engine, uint32_t slot)
{
	if (m_slotRender[slot] == 0) {
		return;
	}
	const bool currentRender = m_slotRender[slot] == m_renderSerial;
	m_slotRender[slot] = 0;

	const uint32_t headerCount = m_slotHeaderCount[slot];

	if (m_hasTimestamps) {
		//value, availability pairs for the two queries
		uint64_t results[4] = {};
		const VkResult result = vkGetQueryPoolResults(engine->m_device, m_queryPool, slot * 2, 2, sizeof(results), results, sizeof(uint64_t) * 2, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		if (result == VK_SUCCESS && results[1] != 0 && results[3] != 0 && currentRender) {
			const double ms = double(results[2] - results[0]) * m_timestampPeriodNs / 1.0e6;
			m_lastFrameGpuMs = (float)ms;
			m_totalGpuMs += (float)ms;
		}
	}

	if (headerCount > 0 && currentRender) {
		const VkDeviceSize offset = (VkDeviceSize)slot * READBACK_HEADERS_PER_SLOT * sizeof(CrtQueueHeader);
		const VkDeviceSize bytes = (VkDeviceSize)headerCount * sizeof(CrtQueueHeader);
		vmaInvalidateAllocation(engine->m_memAllocator, m_headerReadback.allocation, offset, bytes);
		const CrtQueueHeader* headers = (const CrtQueueHeader*)((const uint8_t*)m_headerReadback.info.pMappedData + offset);
		m_pathsAlive.resize(headerCount);
		for (uint32_t i = 0; i < headerCount; i++) {
			m_pathsAlive[i] = headers[i].rayCount;
		}

		//the traversal counters, per extended queue - every queue but the last
		const VkDeviceSize statsOffset = (VkDeviceSize)slot * TRAVERSAL_STATS_BYTES;
		vmaInvalidateAllocation(engine->m_memAllocator, m_statsReadback.allocation, statsOffset, TRAVERSAL_STATS_BYTES);
		const uint32_t* stats = (const uint32_t*)((const uint8_t*)m_statsReadback.info.pMappedData + statsOffset);
		TraversalWork work;
		for (uint32_t bounce = 0; bounce + 1 < headerCount; bounce++) {
			work.rays += headers[bounce].rayCount;
			work.nodes += stats[2 * bounce];
			work.primitives += stats[2 * bounce + 1];
		}
		work.primaryRays = headers[0].rayCount;
		work.primaryNodes = stats[0];
		work.primaryPrimitives = stats[1];
		m_traversalWork = work;
	}
}

VkDescriptorSet GpuPathTracer::writeSet(VulkanEngine* engine, const ComputePass& pass)
{
	const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, pass.setLayout);

	//with no map loaded (or the snapshot not using it) the miss branch never samples the
	//binding, but a combined image sampler still has to name a valid view
	const bool hasMap = m_snapshot.useEnvironmentMap && engine->m_environmentMap.image != VK_NULL_HANDLE;
	const AllocatedImage& map = hasMap ? engine->m_environmentMap : engine->m_greyImage;

	DescriptorWriter writer;
	writer.writeBuffer(0, m_paths.buffer, PATH_STATE_SIZE * m_poolSize, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(1, m_hits.buffer, HIT_RECORD_SIZE * m_poolSize, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(2, m_queues.buffer, sizeof(uint32_t) * 2 * (VkDeviceSize)m_poolSize, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(3, m_headers.buffer, sizeof(CrtQueueHeader) * 2, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(4, m_radiance.buffer, sizeof(glm::vec4) * (VkDeviceSize)m_poolSize, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(5, m_sampleBudget.buffer, sizeof(uint32_t) * (VkDeviceSize)m_poolWidth * m_poolHeight, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(6, m_sceneBuffer.buffer, m_instancesBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(7, m_sceneBuffer.buffer, m_materialsBytes, m_materialsOffset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeImage(8, m_accumulation.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(9, m_sampleCount.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(10, map.imageView, engine->m_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.writeBuffer(11, m_geometryBuffer.buffer, m_blasTrianglesBytes, m_blasTrianglesOffset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	//the array always exists, even for a scene with no textures at all, so the binding is never
	//left naming nothing
	writer.writeImage(12, engine->m_raytraceTextures.image().imageView, engine->m_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.writeBuffer(13, m_geometryBuffer.buffer, m_blasNodesBytes, m_blasNodesOffset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(14, m_geometryBuffer.buffer, m_attributesBytes, m_attributesOffset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(15, m_sceneBuffer.buffer, m_tlasBytes, m_tlasOffset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(16, m_sceneBuffer.buffer, m_instanceMaterialsBytes, m_instanceMaterialsOffset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(17, m_traversalStats.buffer, TRAVERSAL_STATS_BYTES, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.updateSet(engine->m_device, set);

	return set;
}

void GpuPathTracer::record(VkCommandBuffer cmd, VulkanEngine* engine, const AllocatedImage& displayImage, float exposure)
{
	if (!m_hasPool) {
		return;
	}

	const uint32_t slot = (uint32_t)(engine->m_frameNumber % FRAME_OVERLAP);

	if (m_running) {
		if (m_hasTimestamps) {
			vkCmdResetQueryPool(cmd, m_queryPool, slot * 2, 2);
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, slot * 2);
		}

		//the stage masks the bounce loop hands buffers between. Generous on purpose: the cost is
		//nil at a dozen barriers per frame, and a missing bit here shows up as an intermittent
		//stale count, the worst kind of bug to chase
		const VkPipelineStageFlags2 computeStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
		const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
		const VkPipelineStageFlags2 transferStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
		const VkAccessFlags2 transferAccess = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

		if (m_clearPending) {
			m_clearPending = false;
			m_hasImage = true;

			//a fresh render: zero the mean and the counts, and give every pixel the full budget
			const VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
			const VkClearColorValue zeroFloat { { 0.f, 0.f, 0.f, 0.f } };
			VkClearColorValue zeroUint {};
			zeroUint.uint32[0] = 0;
			vkCmdClearColorImage(cmd, m_accumulation.image, VK_IMAGE_LAYOUT_GENERAL, &zeroFloat, 1, &range);
			vkCmdClearColorImage(cmd, m_sampleCount.image, VK_IMAGE_LAYOUT_GENERAL, &zeroUint, 1, &range);
			vkCmdFillBuffer(cmd, m_sampleBudget.buffer, 0, VK_WHOLE_SIZE, m_samplesPerFrame);
			vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);
		}

		//the RTIOW camera: origin, lower-left corner of the image plane, its two edges, and the lens basis
		const RTCameraSnapshot& camera = m_snapshot.camera;
		const float aspect = (float)m_poolWidth / (float)m_poolHeight;
		const float theta = glm::radians(camera.vfovDegrees);
		const float h = glm::tan(theta / 2.f);
		const float viewportHeight = 2.f * h;
		const float viewportWidth = aspect * viewportHeight;
		const glm::vec3 t = glm::normalize(camera.lookFrom - camera.lookAt);
		const glm::vec3 n = glm::normalize(glm::cross(camera.vUp, t));
		const glm::vec3 b = glm::cross(t, n);
		const glm::vec3 horizontal = camera.focusDistance * viewportWidth * n;
		const glm::vec3 vertical = camera.focusDistance * viewportHeight * b;
		const glm::vec3 lowerLeft = camera.lookFrom - horizontal / 2.f - vertical / 2.f - camera.focusDistance * t;

		const RenderSettings& settings = m_snapshot.settings;
		CrtParams params {};
		params.origin = glm::vec4(camera.lookFrom, 0.f);
		params.lowerLeft = glm::vec4(lowerLeft, 0.f);
		params.horizontal = glm::vec4(horizontal, 0.f);
		params.vertical = glm::vec4(vertical, 0.f);
		params.lensU = glm::vec4(n, camera.aperture / 2.f);
		params.lensV = glm::vec4(b, 0.f);
		params.width = m_poolWidth;
		params.height = m_poolHeight;
		params.samplesPerFrame = m_samplesPerFrame;
		params.poolSize = m_poolSize;
		params.seed = m_snapshot.seed;
		params.bounce = 0;
		params.rayDepth = (uint32_t)std::clamp(settings.rayDepth, 1, CRT_MAX_DEPTH);
		params.instanceCount = m_instanceCount;
		params.flags = (settings.antialiasing ? CRT_FLAG_JITTER : 0u)
			| (settings.russianRoulette ? CRT_FLAG_ROULETTE : 0u)
			| ((m_snapshot.useEnvironmentMap && engine->m_environmentMap.image != VK_NULL_HANDLE) ? CRT_FLAG_ENVIRONMENT_MAP : 0u);
		params.minBouncesBeforeRoulette = (uint32_t)std::max(settings.minBouncesBeforeRoulette, 0);
		params.debugView = (uint32_t)debugView;
		params.maxSamples = std::max(m_maxSamples, 1u);
		params.environmentIntensity = m_snapshot.environmentIntensity;

		const VkDescriptorSet set = writeSet(engine, m_generate);
		//the extend build that traverses the layout this scene's BLASes were packed in
		const bool cwbvh = m_snapshot.scene != nullptr && m_snapshot.scene->blases != nullptr && m_snapshot.scene->blases->settings.layout == BvhLayout::Cwbvh8;
		const ComputePass& extend = cwbvh ? m_extendCwbvh : m_extend;

		const VkDeviceSize readbackBase = (VkDeviceSize)slot * READBACK_HEADERS_PER_SLOT * sizeof(CrtQueueHeader);
		auto copyHeader = [&](uint32_t queue, uint32_t readbackIndex) {
			VkBufferCopy region {};
			region.srcOffset = queue * sizeof(CrtQueueHeader);
			region.dstOffset = readbackBase + readbackIndex * sizeof(CrtQueueHeader);
			region.size = sizeof(CrtQueueHeader);
			vkCmdCopyBuffer(cmd, m_headers.buffer, m_headerReadback.buffer, 1, &region);
		};

		//an empty header is a legal no-op dispatch; reset from the command buffer, never by a
		//shader, so an empty queue cannot leave a stale count behind
		const CrtQueueHeader emptyHeader { 0, 1, 1, 0 };
		auto resetHeader = [&](uint32_t queue) {
			vkCmdUpdateBuffer(cmd, m_headers.buffer, queue * sizeof(CrtQueueHeader), sizeof(CrtQueueHeader), &emptyHeader);
		};

		//generate: every (pixel, k) pair, appending the spawned paths to queue 0
		resetHeader(0);
		vkCmdFillBuffer(cmd, m_traversalStats.buffer, 0, VK_WHOLE_SIZE, 0);
		vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);
		dispatchComputePass(cmd, m_generate, set, &params, { (m_poolSize + CRT_WORKGROUP - 1) / CRT_WORKGROUP, 1, 1 });
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
		copyHeader(0, 0);

		for (uint32_t bounce = 0; bounce < params.rayDepth; bounce++) {
			const uint32_t current = bounce & 1u;
			const uint32_t next = current ^ 1u;
			params.bounce = bounce;

			//extend over the current queue: a hit record per queue position
			dispatchComputePassIndirect(cmd, extend, set, &params, m_headers.buffer, current * sizeof(CrtQueueHeader));
			//hits visible to shade, and the next header - read as arguments by the previous
			//bounce - free to be reset
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
			resetHeader(next);
			vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);

			//shade over the current queue: terminate or append to the next
			dispatchComputePassIndirect(cmd, m_shade, set, &params, m_headers.buffer, current * sizeof(CrtQueueHeader));
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
			copyHeader(next, bounce + 1);
		}

		//the frame's traversal counters, read back with the headers FRAME_OVERLAP frames later
		VkBufferCopy statsRegion {};
		statsRegion.dstOffset = (VkDeviceSize)slot * TRAVERSAL_STATS_BYTES;
		statsRegion.size = TRAVERSAL_STATS_BYTES;
		vkCmdCopyBuffer(cmd, m_traversalStats.buffer, m_statsReadback.buffer, 1, &statsRegion);

		//resolve: fold each pixel's K slots into the running mean
		dispatchComputePass(cmd, m_resolve, set, &params, { (m_poolWidth * m_poolHeight + CRT_WORKGROUP - 1) / CRT_WORKGROUP, 1, 1 });
		vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

		m_slotRender[slot] = m_renderSerial;
		m_slotHeaderCount[slot] = params.rayDepth + 1;

		m_samplesAccumulated += m_samplesPerFrame;
		if (m_maxSamples > 0 && m_samplesAccumulated >= m_maxSamples) {
			m_running = false;
		}
	}

	if (m_hasImage) {
		engine->m_tonemapPass.dispatch(cmd, engine->m_device, engine->getCurrentFrame().frameDescriptors, m_accumulation, displayImage, exposure);
	}

	if (m_running && m_hasTimestamps) {
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, slot * 2 + 1);
	} else if (m_slotRender[slot] != 0 && m_hasTimestamps) {
		//the frame that just finished the render still needs its closing timestamp
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, slot * 2 + 1);
	}
}
