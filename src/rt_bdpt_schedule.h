#pragma once

// PBRT 3'S BIDIRECTIONAL PATH TRACER'S SCHEDULE: the `bdpt` integrator (docs/plans/completed/pbrt-bidirectional-path-tracing.md;
// PBR 3ed 16.3), three passes a frame on the renderer's wavefront pool:
//
//   light pass    11 emit -> [01 -> 02 -> 17 store and scatter (importance)] x depth
//   camera pass   00 generate -> [01 -> 02 -> 17 over the escaped, emissive and surface queues (radiance)] x depth
//   connections   18 connect subpaths -> 09 film
//
// Each pixel sample traces a light subpath and a camera subpath, whose vertices binding 53 keeps, rayDepth + 1 and
// rayDepth of them, and kernel 18 evaluates every strategy joining them with pbrt's MIS weights. The vertices of a
// whole image rarely fit, so a frame takes the tile of pixels the memory budget holds (BidirectionalSettings::
// cacheBudgetMB): the sample budget lets only the tile's pixels spawn, the tiles sweep the image, and a sweep is one
// sample per pixel, which is what samplesPerFrame() reports on the frame that finishes it. One sample a pixel a sweep:
// samples per frame do not apply.

#include <array>

#include <rt_schedule.h>

// the uvec4s binding 53 keeps a vertex in (crt_bdpt.glsl CRT_BDPT_VERTEX_WORDS)
constexpr uint32_t bdptVertexWords(uint32_t family)
{
	return 5u + (family <= 4u ? 1u : family / 4u);
}

class BdptSchedule final : public IntegratorSchedule {
public:
	void start(GpuRenderer& renderer, VulkanEngine* engine) override;
	void release(GpuRenderer& renderer, VulkanEngine* engine) override;
	void record(GpuRenderer& renderer, VulkanEngine* engine, VkCommandBuffer cmd, uint32_t frameSlot, CrtParams params) override;
	void collectReadbacks(VulkanEngine* engine, uint32_t frameSlot, const uint32_t* stats) override;

	uint32_t samplesPerFrame() const override { return m_sweepFinished ? 1u : 0u; }
	double estimatedWorkPerFrame(const WorkEstimate& estimate) const override;
	const TraversalWork& traversalWork() const override { return m_traversalWork; }
	std::span<const ScheduleCounter> counters() const override { return m_counters; }

private:
	bool m_clearPending { false };
	// the pixels a frame's tile holds, where the next tile starts, and whether the frame just recorded finished a sweep
	uint32_t m_tile { 0 };
	uint32_t m_nextTile { 0 };
	bool m_sweepFinished { false };
	uint32_t m_verticesPerSlot { 0 };
	uint32_t m_vertexWords { 0 };

	// binding 53: the header, a uvec4 per tile slot, the vertices
	AllocatedBuffer m_vertices {};
	VkDeviceSize m_vertexBytes { 0 };

	AllocatedBuffer m_headerReadback {};
	std::vector<uint32_t> m_slotHeaderCount;

	TraversalWork m_traversalWork;
	std::array<ScheduleCounter, 2> m_counters { { { "Light subpaths alive per bounce", {} }, { "Camera subpaths alive per bounce", {} } } };
};
