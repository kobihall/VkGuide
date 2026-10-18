#pragma once

// THE SCHEDULE INTERFACE: how one integrator records a frame. The renderer (rt_gpu.h) owns what
// every integrator shares - the scene, the film, the reference, the tonemap, the traversal
// counters, the resource table, the pipelines and the wavefront pool - and asks the active
// integrator's schedule to record the frame's work between them. A schedule owns only what its
// algorithm adds: the order of its stages, its barriers, and any buffers no other integrator reads.
//
// ADDING AN INTEGRATOR. Its IntegratorId and IntegratorInfo row in rt_kernels.cpp, its stages in
// the stage library (a new stage only where no existing one will do), a class implementing this
// interface, and one case in createSchedule(). docs/plans/completed/integrator-layer.md has the
// design, and docs/plans/pbrt-roadmap.md reserves the stage numbers.

#include <memory>
#include <span>
#include <vector>

#include <rt_gpu.h>

// what the render guard prices a frame from, before the render starts
struct WorkEstimate {
	// the render's size and samples per frame
	const RenderSettings& settings;
	// what one ray costs the selected traversal in this scene (RaytraceRenderer::sceneCostPerRay)
	double costPerRay;
	const KernelSelection& kernels;
	// the scene's light list, null without a scene
	const GpuLightHeader* lights;
};

class IntegratorSchedule {
public:
	virtual ~IntegratorSchedule() = default;

	// a render of renderer.snapshot() is starting: size what the schedule owns for it (after a
	// device-wide wait if a size changed), publish it into the resource table, and arm the first
	// frame to clear it
	virtual void start(GpuRenderer& renderer, VulkanEngine* engine) = 0;
	// another integrator takes over, or the renderer shuts down: release what start() allocated.
	// The device is idle
	virtual void release(GpuRenderer& renderer, VulkanEngine* engine) = 0;

	// one frame of the integrator's work into cmd. The renderer has zeroed the traversal counters
	// before, and after it copies them back, tonemaps the film and measures the reference. `params`
	// holds everything integrator-independent; the schedule fills in the rest
	virtual void record(GpuRenderer& renderer, VulkanEngine* engine, VkCommandBuffer cmd, uint32_t frameSlot, CrtParams params) = 0;
	// what this schedule recorded into frameSlot FRAME_OVERLAP frames ago, for the render still
	// running. `stats` is that frame's traversal counters (CRT_TRAVERSAL_STATS_WORDS of them)
	virtual void collectReadbacks(VulkanEngine* engine, uint32_t frameSlot, const uint32_t* stats) = 0;

	// the samples per pixel one frame adds: what the progress bar and Max samples count in
	virtual uint32_t samplesPerFrame() const = 0;
	// one frame's work in the render guard's units (node visits + primitive tests), for the render
	// the settings describe. Pessimistic by design: an underestimate is how a scene hangs the GPU
	virtual double estimatedWorkPerFrame(const WorkEstimate& estimate) const = 0;
	// the traversal counters of the last frame read back, per ray
	virtual const TraversalWork& traversalWork() const = 0;
	virtual std::span<const ScheduleCounter> counters() const = 0;
};

// the schedule that records `id`: the one place an integrator is tied to its class
std::unique_ptr<IntegratorSchedule> createSchedule(IntegratorId id);
