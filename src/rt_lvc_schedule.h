#pragma once

// THE LIGHT VERTEX CACHE'S SCHEDULE: the `lvc` integrator (docs/plans/completed/pbrt-bidirectional-path-tracing.md;
// Davidovic et al. 2014, with Georgiev's recursive MIS), two passes a frame on the renderer's wavefront pool:
//
//   light pass    11 emit -> [01 -> 02 -> 12 connect to the camera -> 13 scatter and store -> 14 splat] x depth
//   camera pass   00 generate -> [01 -> 02 -> 15 emitters -> 16 scatter and connect -> 08 shadow rays] x depth -> 09 film
//
// The light pass traces N_L light paths, as many as the cache's budget holds the vertices of (each path can store one
// per bounce that leaves room for a connection), into binding 52, and splats their connections to the camera, each
// weighed by the recursive MIS. The camera pass traces a camera path per pixel sample, which samples a light and
// connects to vertices picked from the cache at every surface. Light paths serve N_C / N_L pixel samples each
// (CrtParams::lightPathScale). In a spectral build with hero wavelengths, every path of a frame shares one wavelength
// sample (CrtParams::frameLambdaU), which a connection between two paths needs: the user's choice on 2026-11-01.

#include <array>

#include <rt_schedule.h>

// the uvec4s binding 52 keeps a vertex in (crt_light_vertex_cache.glsl CRT_LVC_VERTEX_WORDS)
constexpr uint32_t lvcVertexWords(uint32_t family)
{
	return 4u + (family <= 4u ? 1u : family / 4u);
}

class LvcSchedule final : public IntegratorSchedule {
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
	// the frame's light paths, the cache's capacity in vertices and its record's size
	uint32_t m_lightPaths { 0 };
	uint32_t m_capacity { 0 };
	uint32_t m_vertexWords { 0 };
	uint32_t m_frame { 0 };

	// binding 52: the header, a uvec4 per pool slot, the vertices
	AllocatedBuffer m_cache {};
	VkDeviceSize m_cacheBytes { 0 };

	// per frame slot: the light paths and camera paths alive entering each bounce, and the cache's count
	AllocatedBuffer m_headerReadback {};
	std::vector<uint32_t> m_slotHeaderCount;

	TraversalWork m_traversalWork;
	std::array<ScheduleCounter, 3> m_counters { { { "Light paths alive per bounce", {} }, { "Camera paths alive per bounce", {} }, { "Light vertices cached", {} } } };
};
