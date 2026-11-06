#pragma once

// THE LIGHT TRACER'S SCHEDULE: the `light` integrator (docs/plans/completed/pbrt-bidirectional-path-tracing.md;
// pbrt-v4's LightPathIntegrator), recorded on the renderer's wavefront pool as
//
//     11 emit -> 14 splat -> [01 samples -> 02 intersect -> 26 deposit, 12 connect to the camera -> 13 scatter -> 14 splat] x depth -> 09 film
//
// with stage 26 only while the scene has a detector (docs/plans/completed/physoptics-detectors.md), and the detectors' fold
// after 09 (GpuRenderer::record(), src/rt_detectors.h).
//
// One light path per pixel sample, from a light picked by its power: every vertex connects to the camera, and the
// unoccluded connections splat onto the film, which kernel 09 folds into each pixel's value over its sample count. The
// stages it shares with the path tracer (01, 02, 09) run the render's selected variants; kernel 02 still sorts hits on
// emitters onto the emissive queue, which nothing here drains, so a light path ignores the emitters it meets. The light
// paths draw their numbers from their own dimensions of each pixel sample (src/sampler.h SAMPLE_LIGHT_PATH_BASE).

#include <array>

#include <rt_schedule.h>

class LightSchedule final : public IntegratorSchedule {
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
	uint32_t m_samplesPerFrame { 0 };
	bool m_clearPending { false };

	// per frame slot: the ray queue after kernel 11 and after every bounce, then the camera connections kernel 11 and
	// each bounce's kernel 12 queued
	AllocatedBuffer m_headerReadback {};
	std::vector<uint32_t> m_slotHeaderCount;

	TraversalWork m_traversalWork;
	std::array<ScheduleCounter, 2> m_counters { { { "Light paths alive per bounce", {} }, { "Camera connections per bounce", {} } } };
};
