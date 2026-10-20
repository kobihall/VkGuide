#pragma once

// THE PATH TRACER'S SCHEDULE: the `path` integrator, recorded as the wavefront of PBR 4ed chapter 15,
//
//     00 generate -> [01 samples -> 02 intersect -> {03 escaped, 04 emissive, 06 scatter} -> 08 shadow rays] x depth -> 09 film
//
// over the renderer's wavefront pool of width x height x samplesPerFrame paths, with index queues
// compacted per bounce by a workgroup-aggregated allocator whose header doubles as the indirect
// dispatch arguments. Each stage runs whichever variant the render's selection names, and the ones
// that trace run the build of its traversal; nothing here names a strategy. A sampler that writes the
// sample record (docs/plans/completed/pbrt-sample-generation.md) also has a camera pass, which runs over
// the whole pool before 00 so that 00 can read the camera's dimensions.

#include <array>

#include <rt_schedule.h>

class PathSchedule final : public IntegratorSchedule {
public:
	void start(GpuRenderer& renderer, VulkanEngine* engine) override;
	void release(GpuRenderer& renderer, VulkanEngine* engine) override;
	void record(GpuRenderer& renderer, VulkanEngine* engine, VkCommandBuffer cmd, uint32_t frameSlot, CrtParams params) override;
	void collectReadbacks(VulkanEngine* engine, uint32_t frameSlot, const uint32_t* stats) override;

	uint32_t samplesPerFrame() const override { return m_samplesPerFrame; }
	double estimatedWorkPerFrame(const WorkEstimate& estimate) const override;
	const TraversalWork& traversalWork() const override { return m_traversalWork; }
	std::span<const ScheduleCounter> counters() const override { return m_counters; }

private:
	// the samples per frame the pool was sized for: the setting, lowered to keep the pool under
	// CRT_MAX_POOL paths
	uint32_t m_samplesPerFrame { 0 };
	// the first frame of a render gives every pixel its full sample budget
	bool m_clearPending { false };

	// per frame slot: the queue headers after each producer, and how many were copied
	AllocatedBuffer m_headerReadback {};
	std::vector<uint32_t> m_slotHeaderCount;

	TraversalWork m_traversalWork;
	// paths alive entering each bounce, [0] being what generate spawned - the proof the compaction
	// works - the shadow rays kernel 06 emitted at each bounce, empty when there were none, and the
	// surface hits each material class's build of kernel 06 scattered, the complex one shown only
	// when it had any
	std::array<ScheduleCounter, 4> m_counters { { { "Paths alive per bounce", {} }, { "Shadow rays per bounce", {} },
		{ "Surface hits per bounce (basic)", {} }, { "Surface hits per bounce (complex)", {} } } };
};
