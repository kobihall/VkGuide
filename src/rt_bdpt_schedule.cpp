#include <rt_bdpt_schedule.h>

#include <algorithm>

#include <vk_engine.h>
#include <vk_images.h>

namespace {

constexpr uint32_t READBACK_CAMERA_HEADERS = CRT_MAX_DEPTH + 1;
constexpr uint32_t READBACK_HEADERS_PER_SLOT = 2 * (CRT_MAX_DEPTH + 1);

//binding 53's header (crt_bdpt.glsl)
struct VertexHeader {
	uint32_t tileStart;
	uint32_t verticesPerSlot;
	uint32_t stride;
	uint32_t vertexBase;
};

}

void BdptSchedule::start(GpuRenderer& renderer, VulkanEngine* engine)
{
	const GpuRenderSnapshot& snapshot = renderer.snapshot();
	const uint32_t pixels = snapshot.width * snapshot.height;
	const uint32_t family = laneFamily(snapshot.settings.spectral);
	const KernelVariant* sampler = snapshot.kernels.selected(KernelSlot::GenerateSamples);
	const bool sampleRecord = sampler != nullptr && sampler->implemented && sampler->writesRecord;
	//one sample a pixel a sweep
	WavefrontPool& pool = renderer.wavefrontPool();
	pool.ensure(engine, renderer.resources(), snapshot.width, snapshot.height, 1, sampleRecord, renderer.aovSettings().accumulates(), family);

	//the tile: as many pixel samples as the budget holds the vertices of, rayDepth + 1 on the camera subpath and rayDepth on
	//the light subpath each
	const uint32_t rayDepth = (uint32_t)std::clamp(snapshot.settings.rayDepth, 1, (int)CRT_MAX_DEPTH);
	m_verticesPerSlot = 2 * rayDepth + 1;
	m_vertexWords = bdptVertexWords(family);
	const VkDeviceSize budget = (VkDeviceSize)std::clamp(snapshot.settings.bidirectional.cacheBudgetMB, MIN_CACHE_BUDGET_MB, MAX_CACHE_BUDGET_MB) << 20;
	const VkDeviceSize slotBytes = 16 + 16 * (VkDeviceSize)m_verticesPerSlot * m_vertexWords;
	//kernel 18 takes an invocation per camera vertex of each, in one dispatch of at most 65535 groups
	const VkDeviceSize maxTile = 65535ull * CRT_WORKGROUP / (rayDepth + 1);
	m_tile = (uint32_t)std::clamp<VkDeviceSize>((budget - 16) / slotBytes, 1, std::min<VkDeviceSize>(pixels, maxTile));
	const VkDeviceSize bytes = 16 + slotBytes * m_tile;
	if (m_vertices.buffer == VK_NULL_HANDLE || m_vertexBytes != bytes) {
		vkDeviceWaitIdle(engine->m_device);
		if (m_vertices.buffer != VK_NULL_HANDLE) {
			engine->destroyBuffer(m_vertices);
		}
		m_vertices = engine->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		m_vertexBytes = bytes;
		fmt::println("BdptSchedule: {} vertices a pixel sample ({} bytes each), {} pixels a frame of {} ({:.0f} MB)", m_verticesPerSlot, 16 * m_vertexWords, m_tile, pixels,
			double(bytes) / (1024.0 * 1024.0));
	}
	renderer.resources().setBuffer(CrtBinding::BdptVertices, m_vertices.buffer, m_vertexBytes);

	if (m_headerReadback.buffer == VK_NULL_HANDLE) {
		m_headerReadback = engine->createBuffer((VkDeviceSize)FRAME_OVERLAP * READBACK_HEADERS_PER_SLOT * sizeof(CrtQueueHeader), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
		m_slotHeaderCount.assign(FRAME_OVERLAP, 0);
	}
	m_clearPending = true;
	m_nextTile = 0;
	m_sweepFinished = false;
	m_traversalWork = {};
	for (ScheduleCounter& counter : m_counters) {
		counter.values.clear();
	}
}

void BdptSchedule::release(GpuRenderer& renderer, VulkanEngine* engine)
{
	renderer.publishBidirectionalStubs();
	if (m_vertices.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_vertices);
		m_vertices = {};
		m_vertexBytes = 0;
	}
	if (m_headerReadback.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_headerReadback);
		m_headerReadback = {};
	}
	m_slotHeaderCount.clear();
}

double BdptSchedule::estimatedWorkPerFrame(const WorkEstimate& estimate) const
{
	//both subpaths' segments, and a visibility test for every strategy of up to rayDepth^2, for the tile the budget
	//holds, at most every pixel
	const double pixels = (double)std::max(estimate.settings.width, 2) * (double)std::max(estimate.settings.height, 2);
	const double depth = (double)std::clamp(estimate.settings.rayDepth, 1, (int)CRT_MAX_DEPTH);
	const double slotBytes = 16.0 + 16.0 * (2.0 * depth + 1.0) * 6.0;
	const double tile = std::min(pixels, (double)((VkDeviceSize)std::clamp(estimate.settings.bidirectional.cacheBudgetMB, MIN_CACHE_BUDGET_MB, MAX_CACHE_BUDGET_MB) << 20) / slotBytes);
	return tile * estimate.costPerRay * (2.0 + depth * 0.5);
}

void BdptSchedule::record(GpuRenderer& renderer, VulkanEngine* engine, VkCommandBuffer cmd, uint32_t frameSlot, CrtParams params)
{
	const WavefrontPool& pool = renderer.wavefrontPool();
	const uint32_t pixels = pool.width() * pool.height();
	const uint32_t tileStart = m_nextTile;
	const uint32_t tileSize = std::min(m_tile, pixels - tileStart);
	m_nextTile = tileStart + tileSize >= pixels ? 0u : tileStart + tileSize;
	m_sweepFinished = m_nextTile == 0u;

	params.samplesPerFrame = 1;
	params.poolSize = pool.size();
	//kernel 18's slots: the tile's pixel samples, one light subpath each
	params.lightPathCount = tileSize;
	params.lightPathScale = 1.f;

	const VkPipelineStageFlags2 computeStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
	const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
	const VkPipelineStageFlags2 transferStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
	const VkAccessFlags2 transferAccess = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

	//only the tile's pixels sample this frame; the subpaths' counts start at none
	m_clearPending = false;
	vkCmdFillBuffer(cmd, pool.sampleBudget().buffer, 0, VK_WHOLE_SIZE, 0u);
	vkCmdFillBuffer(cmd, pool.sampleBudget().buffer, (VkDeviceSize)tileStart * sizeof(uint32_t), (VkDeviceSize)tileSize * sizeof(uint32_t), 1u);
	const VertexHeader header { tileStart, m_verticesPerSlot, m_vertexWords, m_tile };
	vkCmdUpdateBuffer(cmd, m_vertices.buffer, 0, sizeof(header), &header);
	vkCmdFillBuffer(cmd, m_vertices.buffer, 16, 16 * (VkDeviceSize)m_tile, 0u);

	const KernelSelection& selection = renderer.snapshot().kernels;
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
	auto launch = [&](const KernelVariant& variant, const CrtParams& p, uint32_t bounce, MaterialClass materialClass = MaterialClass::Basic) {
		renderer.dispatch(cmd, engine, variant, p, pool.dispatchSize(variant, bounce, materialClass), materialClass);
	};
	auto launchQueue = [&](const KernelVariant& variant, const CrtParams& p, uint32_t queue) {
		renderer.dispatch(cmd, engine, variant, p, DispatchSize { .indirectBuffer = pool.headers().buffer, .indirectOffset = queue * sizeof(CrtQueueHeader) }, MaterialClass::Basic);
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
	const KernelVariant* store = passFor(KernelSlot::StoreVertices);

	//one subpath kind through the pool: `generate` starts it, 17 stores and scatters it
	auto tracePass = [&](bool light, const KernelVariant* generate, uint32_t readbackBase) {
		CrtParams p = params;
		p.sampleDimensionBase = light ? SAMPLE_LIGHT_PATH_BASE : 0u;
		p.bidirFlags |= light ? CRT_BIDIR_FLAG_LIGHT_PASS : 0u;
		vkutil::memory_barrier(cmd, computeStages | transferStage, computeAccess | transferAccess, transferStage, transferAccess);
		pool.resetHeader(cmd, CRT_QUEUE_RAY_A);
		pool.resetHeader(cmd, CRT_QUEUE_SHADOW);
		vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);
		if (samples != nullptr && samples->cameraPass) {
			p.bounce = CRT_CAMERA_PASS;
			launch(*samples, p, CRT_CAMERA_PASS);
			p.bounce = 0;
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		}
		if (generate != nullptr) {
			launch(*generate, p, 0);
		}
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
		copyHeader(CRT_QUEUE_RAY_A, readbackBase);
		for (uint32_t bounce = 0; bounce < p.rayDepth; bounce++) {
			const uint32_t next = (bounce + 1u) & 1u;
			p.bounce = bounce;
			resetBounceQueues(next);
			if (samples != nullptr) {
				launch(*samples, p, bounce);
				vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
			}
			if (intersect != nullptr) {
				launch(*intersect, p, bounce);
			}
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
			if (store != nullptr) {
				//a camera subpath also ends on the lights at infinity and on surfaces that only glow
				if (!light) {
					CrtParams escaped = p;
					escaped.bidirFlags |= CRT_BIDIR_FLAG_ESCAPED_PASS;
					launchQueue(*store, escaped, CRT_QUEUE_ESCAPED);
					CrtParams emissive = p;
					emissive.bidirFlags |= CRT_BIDIR_FLAG_EMISSIVE_PASS;
					launchQueue(*store, emissive, CRT_QUEUE_EMISSIVE);
					vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
				}
				for (uint32_t materialClass = 0; materialClass < (uint32_t)MaterialClass::Count; materialClass++) {
					launch(*store, p, bounce, (MaterialClass)materialClass);
				}
			}
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
			copyHeader(next, readbackBase + bounce + 1);
		}
	};

	tracePass(true, passFor(KernelSlot::EmitLightRays), 0);
	tracePass(false, passFor(KernelSlot::GenerateCameraRays), READBACK_CAMERA_HEADERS);

	//---------------------------------------------------------------- 18 connect subpaths, over the tile's slots and each one's
	//camera vertices
	vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
	if (const KernelVariant* connect = passFor(KernelSlot::ConnectSubpaths)) {
		CrtParams p = params;
		p.bounce = 0;
		const uint32_t invocations = tileSize * (params.rayDepth + 1);
		renderer.dispatch(cmd, engine, *connect, p, DispatchSize { .groups = (invocations + CRT_WORKGROUP - 1) / CRT_WORKGROUP });
	}
	vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);

	//---------------------------------------------------------------- 09 update film
	if (const KernelVariant* film = passFor(KernelSlot::UpdateFilm)) {
		for (uint32_t filmPass = 0; filmPass < film->passes; filmPass++) {
			CrtParams p = params;
			p.bounce = filmPass;
			launch(*film, p, filmPass);
			if (filmPass + 1 < film->passes) {
				vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
			}
		}
	}
	m_slotHeaderCount[frameSlot] = params.rayDepth + 1;
}

void BdptSchedule::collectReadbacks(VulkanEngine* engine, uint32_t frameSlot, const uint32_t* stats)
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
	TraversalWork work;
	for (uint32_t i = 0; i < headerCount; i++) {
		light[i] = headers[i].rayCount;
		camera[i] = headers[READBACK_CAMERA_HEADERS + i].rayCount;
	}
	for (uint32_t bounce = 0; bounce + 1 < headerCount; bounce++) {
		work.rays += light[bounce] + camera[bounce];
		work.nodes += stats[2 * bounce];
		work.primitives += stats[2 * bounce + 1];
	}
	work.shadowNodes = stats[CRT_SHADOW_STATS_BASE];
	work.shadowPrimitives = stats[CRT_SHADOW_STATS_BASE + 1];
	work.primaryRays = camera[0];
	m_traversalWork = work;
}
