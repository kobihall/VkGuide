#include <rt_light_schedule.h>

#include <algorithm>

#include <vk_engine.h>
#include <vk_images.h>

namespace {

//header slots per frame slot in the readback buffer: the ray queue after kernel 11 and every bounce, then the shadow
//queue (the camera connections) after kernel 11 and every bounce
constexpr uint32_t READBACK_SHADOW_HEADERS = CRT_MAX_DEPTH + 1;
constexpr uint32_t READBACK_HEADERS_PER_SLOT = 2 * (CRT_MAX_DEPTH + 1);

}

void LightSchedule::start(GpuRenderer& renderer, VulkanEngine* engine)
{
	const GpuRenderSnapshot& snapshot = renderer.snapshot();
	//one light path per pixel sample, in a pool sized as the path tracer's: what fits under the caps
	uint32_t samplesPerFrame = (uint32_t)std::clamp(snapshot.settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	const uint32_t pixels = snapshot.width * snapshot.height;
	if (!snapshot.settings.antialiasing) {
		samplesPerFrame = 1;
	}
	const uint32_t family = laneFamily(snapshot.settings.spectral);
	const VkDeviceSize bytesPerPath = CRT_PATH_STATE_SIZE + CRT_HIT_RECORD_SIZE + 4 * CRT_QUEUE_COUNT + 16 + shadowRayBytes(family) + pathSpectrumBytes(family);
	while (samplesPerFrame > 1 && ((VkDeviceSize)pixels * samplesPerFrame > CRT_MAX_POOL || (VkDeviceSize)pixels * samplesPerFrame * bytesPerPath > CRT_MAX_POOL_BYTES)) {
		samplesPerFrame--;
	}
	const KernelVariant* sampler = snapshot.kernels.selected(KernelSlot::GenerateSamples);
	const bool sampleRecord = sampler != nullptr && sampler->implemented && sampler->writesRecord;
	//light paths keep no auxiliary output
	renderer.wavefrontPool().ensure(engine, renderer.resources(), snapshot.width, snapshot.height, samplesPerFrame, sampleRecord, false, family);

	if (m_headerReadback.buffer == VK_NULL_HANDLE) {
		m_headerReadback = engine->createBuffer((VkDeviceSize)FRAME_OVERLAP * READBACK_HEADERS_PER_SLOT * sizeof(CrtQueueHeader), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
		m_slotHeaderCount.assign(FRAME_OVERLAP, 0);
	}
	m_samplesPerFrame = samplesPerFrame;
	m_clearPending = true;
	m_traversalWork = {};
	for (ScheduleCounter& counter : m_counters) {
		counter.values.clear();
	}
}

void LightSchedule::release(GpuRenderer& renderer, VulkanEngine* engine)
{
	(void)renderer;
	if (m_headerReadback.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_headerReadback);
		m_headerReadback = {};
	}
	m_slotHeaderCount.clear();
}

double LightSchedule::estimatedWorkPerFrame(const WorkEstimate& estimate) const
{
	//every bounce traces the path's next segment and a connection to the camera, as the path tracer's bounce traces a
	//light sample; and the light's own point connects once
	const double pixels = (double)std::max(estimate.settings.width, 2) * (double)std::max(estimate.settings.height, 2);
	const double samples = (double)std::clamp(estimate.settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	return pixels * samples * estimate.costPerRay * 2.0;
}

void LightSchedule::record(GpuRenderer& renderer, VulkanEngine* engine, VkCommandBuffer cmd, uint32_t frameSlot, CrtParams params)
{
	const WavefrontPool& pool = renderer.wavefrontPool();
	params.samplesPerFrame = pool.samplesPerFrame();
	params.poolSize = pool.size();
	//one light path per pixel sample: every slot that samples starts one, and its splat counts once
	params.lightPathCount = pool.size();
	params.lightPathScale = 1.f;
	params.sampleDimensionBase = SAMPLE_LIGHT_PATH_BASE;

	const VkPipelineStageFlags2 computeStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
	const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
	const VkPipelineStageFlags2 transferStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
	const VkAccessFlags2 transferAccess = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

	if (m_clearPending) {
		m_clearPending = false;
		vkCmdFillBuffer(cmd, pool.sampleBudget().buffer, 0, VK_WHOLE_SIZE, m_samplesPerFrame);
	}

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
	auto launch = [&](const KernelVariant& variant, uint32_t bounce, MaterialClass materialClass = MaterialClass::Basic) {
		renderer.dispatch(cmd, engine, variant, params, pool.dispatchSize(variant, bounce, materialClass), materialClass);
	};
	auto launchClasses = [&](const KernelVariant& variant, uint32_t bounce) {
		const uint32_t classes = variant.perMaterialClass ? (uint32_t)MaterialClass::Count : 1u;
		for (uint32_t materialClass = 0; materialClass < classes; materialClass++) {
			launch(variant, bounce, (MaterialClass)materialClass);
		}
	};
	//a detector target has no camera: nothing connects to one or splats (docs/plans/completed/physoptics-detectors.md)
	const bool connects = renderer.snapshot().cameraConnections;
	const KernelVariant* splat = connects ? passFor(KernelSlot::TraceCameraConnections) : nullptr;
	//only while the scene has a detector: a scene without one runs the stages it always did
	const KernelVariant* deposit = renderer.detectors().deposits() ? passFor(KernelSlot::DepositOnDetectors) : nullptr;

	//---------------------------------------------------------------- 11 emit light rays
	pool.resetHeader(cmd, CRT_QUEUE_RAY_A);
	pool.resetHeader(cmd, CRT_QUEUE_SHADOW);
	vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);
	//the sampler's camera pass writes the light paths' emission dimensions over the pool first
	if (const KernelVariant* samples = passFor(KernelSlot::GenerateSamples); samples != nullptr && samples->cameraPass) {
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
	copyHeader(CRT_QUEUE_SHADOW, READBACK_SHADOW_HEADERS);
	//---- 14 the lights' own points seen by the camera
	if (splat != nullptr) {
		launch(*splat, 0);
	}

	for (uint32_t bounce = 0; bounce < params.rayDepth; bounce++) {
		const uint32_t next = (bounce + 1u) & 1u;
		params.bounce = bounce;
		//the splats of the last pass read the shadow queue, which this bounce refills
		vkutil::memory_barrier(cmd, computeStages, computeAccess, transferStage, transferAccess);
		pool.resetHeader(cmd, next);
		pool.resetHeader(cmd, CRT_QUEUE_ESCAPED);
		pool.resetHeader(cmd, CRT_QUEUE_EMISSIVE);
		pool.resetHeader(cmd, CRT_QUEUE_SURFACE);
		pool.resetHeader(cmd, CRT_QUEUE_SURFACE_COMPLEX);
		pool.resetHeader(cmd, CRT_QUEUE_SHADOW);
		vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);

		if (const KernelVariant* samples = passFor(KernelSlot::GenerateSamples)) {
			launch(*samples, bounce);
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		}
		if (const KernelVariant* intersect = passFor(KernelSlot::IntersectClosest)) {
			launch(*intersect, bounce);
		}
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		//---- 26 deposit on the detectors, and 12 connect to the camera, both before 13 moves the paths on: they read the
		//paths and the hits and write different buffers, so neither waits for the other
		if (deposit != nullptr) {
			launch(*deposit, bounce);
		}
		if (const KernelVariant* connect = connects ? passFor(KernelSlot::ConnectToCamera) : nullptr) {
			launchClasses(*connect, bounce);
		}
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		//---- 13 scatter, and 14 splat what 12 queued: they touch different buffers, so neither waits for the other
		if (const KernelVariant* scatter = passFor(KernelSlot::ScatterLightPaths)) {
			launchClasses(*scatter, bounce);
		}
		if (splat != nullptr) {
			launch(*splat, bounce);
		}
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
		copyHeader(next, bounce + 1);
		copyHeader(CRT_QUEUE_SHADOW, READBACK_SHADOW_HEADERS + bounce + 1);
	}

	//---------------------------------------------------------------- 09 update film
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

void LightSchedule::collectReadbacks(VulkanEngine* engine, uint32_t frameSlot, const uint32_t* stats)
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

	std::vector<uint32_t>& alive = m_counters[0].values;
	std::vector<uint32_t>& connections = m_counters[1].values;
	alive.resize(headerCount);
	connections.resize(headerCount);
	TraversalWork work;
	for (uint32_t i = 0; i < headerCount; i++) {
		alive[i] = headers[i].rayCount;
		connections[i] = headers[READBACK_SHADOW_HEADERS + i].rayCount;
		work.shadowRays += connections[i];
	}
	for (uint32_t bounce = 0; bounce + 1 < headerCount; bounce++) {
		work.rays += headers[bounce].rayCount;
		work.nodes += stats[2 * bounce];
		work.primitives += stats[2 * bounce + 1];
		work.shadowNodes += stats[CRT_SHADOW_STATS_BASE + 2 * bounce];
		work.shadowPrimitives += stats[CRT_SHADOW_STATS_BASE + 2 * bounce + 1];
	}
	work.primaryRays = headers[0].rayCount;
	work.primaryNodes = stats[0];
	work.primaryPrimitives = stats[1];
	m_traversalWork = work;
}
