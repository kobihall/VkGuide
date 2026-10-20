#include <rt_path_schedule.h>

#include <algorithm>

#include <vk_engine.h>
#include <vk_images.h>

namespace {

//header slots per frame slot in the readback buffer: the ray queue after generate and after every
//bounce, then the shadow queue after every bounce, then the basic and the complex surface queues
//after every bounce
constexpr uint32_t READBACK_HEADERS_PER_SLOT = 4 * CRT_MAX_DEPTH + 1;
constexpr uint32_t READBACK_SHADOW_HEADERS = CRT_MAX_DEPTH + 1;
constexpr uint32_t READBACK_SURFACE_HEADERS = 2 * CRT_MAX_DEPTH + 1;
constexpr uint32_t READBACK_COMPLEX_HEADERS = 3 * CRT_MAX_DEPTH + 1;

}

void PathSchedule::start(GpuRenderer& renderer, VulkanEngine* engine)
{
	const GpuRenderSnapshot& snapshot = renderer.snapshot();

	//K is what fits: a pool over the cap lowers samplesPerFrame rather than refusing the render
	uint32_t samplesPerFrame = (uint32_t)std::clamp(snapshot.settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	const uint32_t pixels = snapshot.width * snapshot.height;
	if (!snapshot.settings.antialiasing) {
		samplesPerFrame = 1;
	}
	while (samplesPerFrame > 1 && pixels * samplesPerFrame > CRT_MAX_POOL) {
		samplesPerFrame--;
	}
	if (samplesPerFrame != (uint32_t)snapshot.settings.samplesPerFrame) {
		fmt::println("PathSchedule: samples per frame lowered to {} to keep the pool under {} paths", samplesPerFrame, CRT_MAX_POOL);
	}
	//the sample record exists only while the selected sampler writes one
	const KernelVariant* sampler = snapshot.kernels.selected(KernelSlot::GenerateSamples);
	const bool sampleRecord = sampler != nullptr && sampler->implemented && sampler->writesRecord;
	renderer.wavefrontPool().ensure(engine, renderer.resources(), snapshot.width, snapshot.height, samplesPerFrame, sampleRecord);

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

void PathSchedule::release(GpuRenderer& renderer, VulkanEngine* engine)
{
	//the wavefront pool is the renderer's, shared with any integrator that runs these stages
	(void)renderer;
	if (m_headerReadback.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_headerReadback);
		m_headerReadback = {};
	}
	m_slotHeaderCount.clear();
}

double PathSchedule::estimatedWorkPerFrame(const WorkEstimate& estimate) const
{
	//a shadow ray is a second walk through the scene, priced like the first: the any-hit early out
	//makes it cheaper, which leaves the guard erring on the safe side. Kernel 06 sends one only if the
	//scene has a light of the kind its variant samples
	double tracesPerBounce = 1.0;
	const KernelVariant* scatter = estimate.kernels.selected(KernelSlot::SurfaceScattering);
	if (scatter != nullptr && estimate.lights != nullptr) {
		switch (scatter->nextEvent) {
		case NextEvent::AllLights:
			tracesPerBounce = estimate.lights->count > 0 ? 2.0 : 1.0;
			break;
		case NextEvent::DeltaLights:
			tracesPerBounce = estimate.lights->deltaCount > 0 ? 2.0 : 1.0;
			break;
		case NextEvent::None:
			break;
		}
	}
	const double pixels = (double)std::max(estimate.settings.width, 2) * (double)std::max(estimate.settings.height, 2);
	const double samples = (double)std::clamp(estimate.settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	return pixels * samples * estimate.costPerRay * tracesPerBounce;
}

void PathSchedule::record(GpuRenderer& renderer, VulkanEngine* engine, VkCommandBuffer cmd, uint32_t frameSlot, CrtParams params)
{
	const WavefrontPool& pool = renderer.wavefrontPool();
	params.samplesPerFrame = pool.samplesPerFrame();
	params.poolSize = pool.size();

	//the stage masks the bounce loop hands buffers between. Generous on purpose: the cost is nil at a
	//dozen barriers per frame, and a missing bit here shows up as an intermittent stale count, the
	//worst kind of bug to chase
	const VkPipelineStageFlags2 computeStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
	const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
	const VkPipelineStageFlags2 transferStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
	const VkAccessFlags2 transferAccess = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

	if (m_clearPending) {
		m_clearPending = false;
		//a fresh render: every pixel gets the full budget
		vkCmdFillBuffer(cmd, pool.sampleBudget().buffer, 0, VK_WHOLE_SIZE, m_samplesPerFrame);
	}

	//THE SCHEDULE. Each stage is looked up in the selection this render was started with, and
	//dispatched only if it has an implemented variant. Nothing below names a strategy: swapping the
	//traversal for the linear one changes which pipeline the renderer binds and nothing else about
	//the frame.
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

	//one kernel launch: its own descriptor set (written from the bindings it declared), its own
	//domain over the pool
	auto launch = [&](const KernelVariant& variant, uint32_t bounce, MaterialClass materialClass = MaterialClass::Basic) {
		renderer.dispatch(cmd, engine, variant, params, pool.dispatchSize(variant, bounce, materialClass), materialClass);
	};

	//---------------------------------------------------------------- 00 generate camera rays
	pool.resetHeader(cmd, CRT_QUEUE_RAY_A);
	vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);
	//---- 01 generate samples, the camera pass: a sampler that writes a record fills in the camera's
	//dimensions over the whole pool first, for kernel 00 to read
	if (const KernelVariant* samples = passFor(KernelSlot::GenerateSamples); samples != nullptr && samples->cameraPass) {
		params.bounce = CRT_CAMERA_PASS;
		launch(*samples, CRT_CAMERA_PASS);
		params.bounce = 0;
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
	}
	if (const KernelVariant* generate = passFor(KernelSlot::GenerateCameraRays)) {
		launch(*generate, 0);
	}
	vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
	copyHeader(CRT_QUEUE_RAY_A, 0);

	for (uint32_t bounce = 0; bounce < params.rayDepth; bounce++) {
		const uint32_t next = (bounce + 1u) & 1u;
		params.bounce = bounce;

		//every queue this bounce is about to fill. The ray queue it READS was filled last bounce and
		//is left alone
		pool.resetHeader(cmd, next);
		pool.resetHeader(cmd, CRT_QUEUE_ESCAPED);
		pool.resetHeader(cmd, CRT_QUEUE_EMISSIVE);
		pool.resetHeader(cmd, CRT_QUEUE_SURFACE);
		pool.resetHeader(cmd, CRT_QUEUE_SURFACE_COMPLEX);
		pool.resetHeader(cmd, CRT_QUEUE_SHADOW);
		vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);

		//---- 01 generate samples, over the live ray queue
		if (const KernelVariant* samples = passFor(KernelSlot::GenerateSamples)) {
			launch(*samples, bounce);
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		}

		//---- 02 intersect closest: a hit record per queue position, and the paths sorted onto the
		//escaped / emissive / surface queues
		if (const KernelVariant* intersect = passFor(KernelSlot::IntersectClosest)) {
			launch(*intersect, bounce);
		}
		//the hit records, the three queues' contents, and the three headers that are about to be
		//read as dispatch arguments
		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);

		//---- 03, 04 and 06 drain those queues. A miss is on the escaped queue alone, so 03 needs no
		//barrier. A hit on a surface that both glows and scatters (a pbr material with emission) is
		//on the emissive AND the surface queue: 04 reads its throughput, which 06 then overwrites, so
		//04 finishes first
		if (const KernelVariant* escaped = passFor(KernelSlot::HandleEscaped)) {
			launch(*escaped, bounce);
		}
		if (const KernelVariant* emissive = passFor(KernelSlot::HandleEmissive)) {
			launch(*emissive, bounce);
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		}
		if (const KernelVariant* medium = passFor(KernelSlot::SampleMediumInteraction)) {
			launch(*medium, bounce);
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
		}
		//once per material class, each build over its own surface queue, basic first. The two touch
		//disjoint paths and append to the next ray queue and the shadow queue through the allocator's
		//atomics, so neither waits for the other, and an empty complex queue is a zero-group launch
		if (const KernelVariant* surface = passFor(KernelSlot::SurfaceScattering)) {
			const uint32_t classes = surface->perMaterialClass ? (uint32_t)MaterialClass::Count : 1u;
			for (uint32_t materialClass = 0; materialClass < classes; materialClass++) {
				launch(*surface, bounce, (MaterialClass)materialClass);
			}
		}
		if (const KernelVariant* mediumScatter = passFor(KernelSlot::SampleMediumScattering)) {
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
			launch(*mediumScatter, bounce);
		}

		//---- 08 trace shadow rays, over whatever 06 and 07 emitted
		if (const KernelVariant* shadow = passFor(KernelSlot::TraceShadowRays)) {
			vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
			launch(*shadow, bounce);
		}

		vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages | transferStage, computeAccess | transferAccess);
		copyHeader(next, bounce + 1);
		copyHeader(CRT_QUEUE_SHADOW, READBACK_SHADOW_HEADERS + bounce);
		copyHeader(CRT_QUEUE_SURFACE, READBACK_SURFACE_HEADERS + bounce);
		copyHeader(CRT_QUEUE_SURFACE_COMPLEX, READBACK_COMPLEX_HEADERS + bounce);
	}

	//---------------------------------------------------------------- 09 update film
	if (const KernelVariant* film = passFor(KernelSlot::UpdateFilm)) {
		launch(*film, 0);
	}

	m_slotHeaderCount[frameSlot] = params.rayDepth + 1;
}

void PathSchedule::collectReadbacks(VulkanEngine* engine, uint32_t frameSlot, const uint32_t* stats)
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
	alive.resize(headerCount);
	for (uint32_t i = 0; i < headerCount; i++) {
		alive[i] = headers[i].rayCount;
	}

	//the traversal counters, per extended queue - every queue but the last
	TraversalWork work;
	for (uint32_t bounce = 0; bounce + 1 < headerCount; bounce++) {
		work.rays += headers[bounce].rayCount;
		work.nodes += stats[2 * bounce];
		work.primitives += stats[2 * bounce + 1];
	}
	work.primaryRays = headers[0].rayCount;
	work.primaryNodes = stats[0];
	work.primaryPrimitives = stats[1];

	//the shadow queue after every bounce, and what tracing it cost
	std::vector<uint32_t>& shadows = m_counters[1].values;
	shadows.resize(headerCount - 1);
	for (uint32_t bounce = 0; bounce + 1 < headerCount; bounce++) {
		shadows[bounce] = headers[READBACK_SHADOW_HEADERS + bounce].rayCount;
		work.shadowRays += shadows[bounce];
		work.shadowNodes += stats[CRT_SHADOW_STATS_BASE + 2 * bounce];
		work.shadowPrimitives += stats[CRT_SHADOW_STATS_BASE + 2 * bounce + 1];
	}
	//a scene without a light to sample has nothing to show there
	if (work.shadowRays == 0) {
		shadows.clear();
	}

	//the hits each build of kernel 06 scattered, per bounce; the complex queue only once a complex
	//material is in the scene
	std::vector<uint32_t>& basic = m_counters[2].values;
	std::vector<uint32_t>& complex = m_counters[3].values;
	basic.resize(headerCount - 1);
	complex.resize(headerCount - 1);
	uint64_t complexHits = 0;
	for (uint32_t bounce = 0; bounce + 1 < headerCount; bounce++) {
		basic[bounce] = headers[READBACK_SURFACE_HEADERS + bounce].rayCount;
		complex[bounce] = headers[READBACK_COMPLEX_HEADERS + bounce].rayCount;
		complexHits += complex[bounce];
	}
	if (complexHits == 0) {
		complex.clear();
	}
	m_traversalWork = work;
}
