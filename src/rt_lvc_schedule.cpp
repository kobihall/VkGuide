#include <rt_lvc_schedule.h>

#include <algorithm>
#include <cstring>

#include <vk_engine.h>
#include <vk_images.h>

namespace {

//per frame slot in the readback buffer: the light ray queue after kernel 11 and every bounce, the camera ray queue after
//kernel 00 and every bounce, then the cache's header
constexpr uint32_t READBACK_CAMERA_HEADERS = CRT_MAX_DEPTH + 1;
constexpr uint32_t READBACK_CACHE_HEADER = 2 * (CRT_MAX_DEPTH + 1);
constexpr uint32_t READBACK_HEADERS_PER_SLOT = READBACK_CACHE_HEADER + 1;

//binding 52's header (crt_light_vertex_cache.glsl)
struct CacheHeader {
	uint32_t count;
	uint32_t capacity;
	uint32_t stride;
	uint32_t vertexBase;
};

}

void LvcSchedule::start(GpuRenderer& renderer, VulkanEngine* engine)
{
	const GpuRenderSnapshot& snapshot = renderer.snapshot();
	uint32_t samplesPerFrame = (uint32_t)std::clamp(snapshot.settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	const uint32_t pixels = snapshot.width * snapshot.height;
	if (!snapshot.settings.antialiasing) {
		samplesPerFrame = 1;
	}
	const uint32_t family = laneFamily(snapshot.settings.spectral);
	const VkDeviceSize bytesPerPath = CRT_PATH_STATE_SIZE + CRT_HIT_RECORD_SIZE + 4 * CRT_QUEUE_COUNT + 16 + shadowRayBytes(family) + pathSpectrumBytes(family) + 16;
	while (samplesPerFrame > 1 && ((VkDeviceSize)pixels * samplesPerFrame > CRT_MAX_POOL || (VkDeviceSize)pixels * samplesPerFrame * bytesPerPath > CRT_MAX_POOL_BYTES)) {
		samplesPerFrame--;
	}
	const KernelVariant* sampler = snapshot.kernels.selected(KernelSlot::GenerateSamples);
	const bool sampleRecord = sampler != nullptr && sampler->implemented && sampler->writesRecord;
	WavefrontPool& pool = renderer.wavefrontPool();
	pool.ensure(engine, renderer.resources(), snapshot.width, snapshot.height, samplesPerFrame, sampleRecord, renderer.aovSettings().accumulates(), family);

	//the cache: what the budget holds after the header and a state per pool slot, in vertices; and the light paths whose
	//vertices it holds for certain, each storing at most one per bounce that leaves room for a connection
	m_vertexWords = lvcVertexWords(family);
	const VkDeviceSize budget = (VkDeviceSize)std::clamp(snapshot.settings.bidirectional.cacheBudgetMB, MIN_CACHE_BUDGET_MB, MAX_CACHE_BUDGET_MB) << 20;
	const VkDeviceSize stateBytes = 16 + 16 * (VkDeviceSize)pool.size();
	const VkDeviceSize vertexBytes = 16 * (VkDeviceSize)m_vertexWords;
	m_capacity = budget > stateBytes ? (uint32_t)std::min<VkDeviceSize>((budget - stateBytes) / vertexBytes, 0xFFFFFFFFu / 2) : 0u;
	const uint32_t rayDepth = (uint32_t)std::clamp(snapshot.settings.rayDepth, 1, (int)CRT_MAX_DEPTH);
	const uint32_t storedPerPath = std::max(rayDepth, 3u) - 2u;
	m_lightPaths = std::max(std::min(pool.size(), m_capacity / storedPerPath), 1u);

	const VkDeviceSize bytes = stateBytes + vertexBytes * std::max(m_capacity, 1u);
	if (m_cache.buffer == VK_NULL_HANDLE || m_cacheBytes != bytes) {
		vkDeviceWaitIdle(engine->m_device);
		if (m_cache.buffer != VK_NULL_HANDLE) {
			engine->destroyBuffer(m_cache);
		}
		m_cache = engine->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		m_cacheBytes = bytes;
		fmt::println("LvcSchedule: light vertex cache of {} vertices ({} bytes each), {:.0f} MB, for {} light paths a frame", m_capacity, vertexBytes, double(bytes) / (1024.0 * 1024.0), m_lightPaths);
	}
	renderer.resources().setBuffer(CrtBinding::LightVertexCache, m_cache.buffer, m_cacheBytes);

	if (m_headerReadback.buffer == VK_NULL_HANDLE) {
		m_headerReadback = engine->createBuffer((VkDeviceSize)FRAME_OVERLAP * READBACK_HEADERS_PER_SLOT * sizeof(CrtQueueHeader), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
		m_slotHeaderCount.assign(FRAME_OVERLAP, 0);
	}
	m_samplesPerFrame = samplesPerFrame;
	m_clearPending = true;
	m_frame = 0;
	m_traversalWork = {};
	for (ScheduleCounter& counter : m_counters) {
		counter.values.clear();
	}
}

void LvcSchedule::release(GpuRenderer& renderer, VulkanEngine* engine)
{
	renderer.publishBidirectionalStubs();
	if (m_cache.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_cache);
		m_cache = {};
		m_cacheBytes = 0;
	}
	if (m_headerReadback.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_headerReadback);
		m_headerReadback = {};
	}
	m_slotHeaderCount.clear();
}

double LvcSchedule::estimatedWorkPerFrame(const WorkEstimate& estimate) const
{
	//the light pass traces a segment and a connection to the camera per bounce of up to every pixel sample; the camera
	//pass a segment, a light sample and the cache's connections per bounce of every pixel sample
	const double pixels = (double)std::max(estimate.settings.width, 2) * (double)std::max(estimate.settings.height, 2);
	const double samples = (double)std::clamp(estimate.settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	const double connections = (double)std::clamp(estimate.settings.bidirectional.connections, MIN_CACHE_CONNECTIONS, MAX_CACHE_CONNECTIONS);
	return pixels * samples * estimate.costPerRay * (2.0 + 2.0 + connections);
}

void LvcSchedule::record(GpuRenderer& renderer, VulkanEngine* engine, VkCommandBuffer cmd, uint32_t frameSlot, CrtParams params)
{
	const WavefrontPool& pool = renderer.wavefrontPool();
	const GpuRenderSnapshot& snapshot = renderer.snapshot();
	params.samplesPerFrame = pool.samplesPerFrame();
	params.poolSize = pool.size();
	params.lightPathCount = m_lightPaths;
	//each light path stands for the frame's pixel samples over its light paths
	params.lightPathScale = (float)pool.size() / (float)m_lightPaths;
	params.cacheConnections = (uint32_t)std::clamp(snapshot.settings.bidirectional.connections, MIN_CACHE_CONNECTIONS, MAX_CACHE_CONNECTIONS);
	//one hero wavelength sample for the whole frame, a fresh one each frame
	uint32_t hash = (snapshot.seed ^ 0x85ebca6bu) + 0x9e3779b9u * (m_frame + 1u);
	hash ^= hash >> 16;
	hash *= 0x7feb352du;
	hash ^= hash >> 15;
	hash *= 0x846ca68bu;
	hash ^= hash >> 16;
	params.frameLambdaU = (float)(hash >> 8) * (1.f / 16777216.f);
	params.bidirFlags |= CRT_BIDIR_FLAG_FRAME_WAVELENGTHS;
	m_frame++;

	const VkPipelineStageFlags2 computeStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
	const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
	const VkPipelineStageFlags2 transferStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
	const VkAccessFlags2 transferAccess = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

	if (m_clearPending) {
		m_clearPending = false;
		vkCmdFillBuffer(cmd, pool.sampleBudget().buffer, 0, VK_WHOLE_SIZE, m_samplesPerFrame);
	}
	//a frame's cache starts empty
	const CacheHeader header { 0u, m_capacity, m_vertexWords, pool.size() };
	vkCmdUpdateBuffer(cmd, m_cache.buffer, 0, sizeof(header), &header);

	const KernelSelection& selection = snapshot.kernels;
	auto passFor = [&](KernelSlot kernelSlot) -> const KernelVariant* {
		const KernelVariant* variant = selection.selected(kernelSlot);
		return (variant != nullptr && variant->implemented) ? variant : nullptr;
	};
	const VkDeviceSize readbackBase = (VkDeviceSize)frameSlot * READBACK_HEADERS_PER_SLOT * sizeof(CrtQueueHeader);
	auto copyHeader = [&](uint32_t queue, uint32_t readbackIndex) {
		VkBufferCopy region {};
		region.srcOffset = queue * sizeof(CrtQueueHeader);
		region.dstOffset = readbackBase + readbackIndex * sizeof(CrtQueueHeader);
		region.size = sizeof(CrtQueueHeader);
		vkCmdCopyBuffer(cmd, pool.headers().buffer, m_headerReadback.buffer, 1, &region);
	};
	auto launch = [&](const KernelVariant& variant, uint32_t bounce, MaterialClass materialClass = MaterialClass::Basic) {
		renderer.dispatch(cmd, engine, variant, params, pool.dispatchSize(variant, bounce, materialClass), materialClass);
	};
	auto launchClasses = [&](const KernelVariant& variant, uint32_t bounce) {
		const uint32_t classes = variant.perMaterialClass ? (uint32_t)MaterialClass::Count : 1u;
		for (uint32_t materialClass = 0; materialClass < classes; materialClass++) {
			launch(variant, bounce, (MaterialClass)materialClass);
		}
	};
	auto resetBounceQueues = [&](uint32_t next) {
		vkutil::memory_barrier(cmd, computeStages, computeAccess, transferStage, transferAccess);
		pool.resetHeader(cmd, next);
		pool.resetHeader(cmd, CRT_QUEUE_ESCAPED);
		pool.resetHeader(cmd, CRT_QUEUE_EMISSIVE);
		pool.resetHeader(cmd, CRT_QUEUE_SURFACE);
		pool.resetHeader(cmd, CRT_QUEUE_SURFACE_COMPLEX);
		pool.resetHeader(cmd, CRT_QUEUE_SHADOW);
		vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);
	};
	const KernelVariant* samples = passFor(KernelSlot::GenerateSamples);
	const KernelVariant* intersect = passFor(KernelSlot::IntersectClosest);

	//================================================================ the light pass
	params.sampleDimensionBase = SAMPLE_LIGHT_PATH_BASE;
	pool.resetHeader(cmd, CRT_QUEUE_RAY_A);
	pool.resetHeader(cmd, CRT_QUEUE_SHADOW);
	vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);
	if (samples != nullptr && samples->cameraPass) {
		params.bounce = CRT_CAMERA_PASS;
		launch(*samples, CRT_CAMERA_PASS);
		params.bounce = 0;
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
	}
	if (const KernelVariant* emit = passFor(KernelSlot::EmitLightRays)) {
		launch(*emit, 0);
	}
	vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
	copyHeader(CRT_QUEUE_RAY_A, 0);
	for (uint32_t bounce = 0; bounce < params.rayDepth; bounce++) {
		const uint32_t next = (bounce + 1u) & 1u;
		params.bounce = bounce;
		resetBounceQueues(next);
		if (samples != nullptr) {
			launch(*samples, bounce);
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		}
		if (intersect != nullptr) {
			launch(*intersect, bounce);
		}
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		if (const KernelVariant* connect = passFor(KernelSlot::ConnectToCamera)) {
			launchClasses(*connect, bounce);
		}
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		if (const KernelVariant* scatter = passFor(KernelSlot::ScatterLightPaths)) {
			launchClasses(*scatter, bounce);
		}
		if (const KernelVariant* splat = passFor(KernelSlot::TraceCameraConnections)) {
			launch(*splat, bounce);
		}
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
		copyHeader(next, bounce + 1);
	}
	//the cache's count, for the panel
	{
		VkBufferCopy region {};
		region.dstOffset = readbackBase + READBACK_CACHE_HEADER * sizeof(CrtQueueHeader);
		region.size = sizeof(CacheHeader);
		vkCmdCopyBuffer(cmd, m_cache.buffer, m_headerReadback.buffer, 1, &region);
	}

	//================================================================ the camera pass
	params.sampleDimensionBase = 0;
	params.bounce = 0;
	vkutil::memory_barrier(cmd, computeStages | transferStage, computeAccess | transferAccess, transferStage, transferAccess);
	pool.resetHeader(cmd, CRT_QUEUE_RAY_A);
	pool.resetHeader(cmd, CRT_QUEUE_SHADOW);
	vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);
	if (samples != nullptr && samples->cameraPass) {
		params.bounce = CRT_CAMERA_PASS;
		launch(*samples, CRT_CAMERA_PASS);
		params.bounce = 0;
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
	}
	if (const KernelVariant* generate = passFor(KernelSlot::GenerateCameraRays)) {
		launch(*generate, 0);
	}
	vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
	copyHeader(CRT_QUEUE_RAY_A, READBACK_CAMERA_HEADERS);
	const KernelVariant* emitters = passFor(KernelSlot::HandleEmittersBidirectional);
	for (uint32_t bounce = 0; bounce < params.rayDepth; bounce++) {
		const uint32_t next = (bounce + 1u) & 1u;
		params.bounce = bounce;
		resetBounceQueues(next);
		if (samples != nullptr) {
			launch(*samples, bounce);
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		}
		if (intersect != nullptr) {
			launch(*intersect, bounce);
		}
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		//---- 15 over the escaped queue, then the emissive one; a surface that glows and scatters is on both the
		//emissive and the surface queue, so 15 reads its throughput before 16 moves it on
		if (emitters != nullptr) {
			launch(*emitters, bounce);
			CrtParams emissive = params;
			emissive.bidirFlags |= CRT_BIDIR_FLAG_EMISSIVE_PASS;
			renderer.dispatch(cmd, engine, *emitters, emissive, DispatchSize { .indirectBuffer = pool.headers().buffer, .indirectOffset = CRT_QUEUE_EMISSIVE * sizeof(CrtQueueHeader) });
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		}
		if (const KernelVariant* scatter = passFor(KernelSlot::ScatterAndConnect)) {
			launchClasses(*scatter, bounce);
		}
		if (const KernelVariant* shadow = passFor(KernelSlot::TraceShadowRays)) {
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
			launch(*shadow, bounce);
		}
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
		copyHeader(next, READBACK_CAMERA_HEADERS + bounce + 1);
	}

	//================================================================ 09 update film
	if (const KernelVariant* film = passFor(KernelSlot::UpdateFilm)) {
		for (uint32_t filmPass = 0; filmPass < film->passes; filmPass++) {
			params.bounce = filmPass;
			launch(*film, filmPass);
			if (filmPass + 1 < film->passes) {
				vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
			}
		}
	}
	m_slotHeaderCount[frameSlot] = params.rayDepth + 1;
}

void LvcSchedule::collectReadbacks(VulkanEngine* engine, uint32_t frameSlot, const uint32_t* stats)
{
	const uint32_t headerCount = m_slotHeaderCount[frameSlot];
	if (headerCount == 0) {
		return;
	}
	m_slotHeaderCount[frameSlot] = 0;
	const VkDeviceSize offset = (VkDeviceSize)frameSlot * READBACK_HEADERS_PER_SLOT * sizeof(CrtQueueHeader);
	const VkDeviceSize bytes = (VkDeviceSize)READBACK_HEADERS_PER_SLOT * sizeof(CrtQueueHeader);
	vmaInvalidateAllocation(engine->m_memAllocator, m_headerReadback.allocation, offset, bytes);
	const CrtQueueHeader* headers = (const CrtQueueHeader*)((const uint8_t*)m_headerReadback.info.pMappedData + offset);

	std::vector<uint32_t>& light = m_counters[0].values;
	std::vector<uint32_t>& camera = m_counters[1].values;
	light.resize(headerCount);
	camera.resize(headerCount);
	for (uint32_t i = 0; i < headerCount; i++) {
		light[i] = headers[i].rayCount;
		camera[i] = headers[READBACK_CAMERA_HEADERS + i].rayCount;
	}
	CacheHeader cache;
	std::memcpy(&cache, &headers[READBACK_CACHE_HEADER], sizeof(cache));
	m_counters[2].values = { std::min(cache.count, cache.capacity) };

	//the traversal counters sum both passes' extension rays per bounce, and the light samples, camera connections and
	//cache connections with the shadow rays
	TraversalWork work;
	for (uint32_t bounce = 0; bounce + 1 < headerCount; bounce++) {
		work.rays += light[bounce] + camera[bounce];
		work.nodes += stats[2 * bounce];
		work.primitives += stats[2 * bounce + 1];
		work.shadowNodes += stats[CRT_SHADOW_STATS_BASE + 2 * bounce];
		work.shadowPrimitives += stats[CRT_SHADOW_STATS_BASE + 2 * bounce + 1];
	}
	work.primaryRays = camera[0];
	m_traversalWork = work;
}
