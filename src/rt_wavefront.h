#pragma once

// THE WAVEFRONT POOL: the per-path buffers of any schedule that traces through the queues - the
// path states, the hit records, the queue set and its headers, the radiance slots, the shadow rays
// and the per-pixel sample budget - sized for width x height x samplesPerFrame paths. The path
// tracer's schedule runs on it, and so will every integrator that runs the path tracer's stages
// (docs/plans/completed/integrator-layer.md), which is why it belongs to the renderer rather than
// to one schedule.
//
// It publishes its buffers into the renderer's resource table, and turns a stage's KernelDomain
// into a dispatch size, since every domain is measured against the pool: its slots, its pixels or
// one of its queues.

#include <rt_gpu_types.h>

class VulkanEngine;

// shaders/rt/include/crt_common.glsl CRT_QUEUE_*: two ping-ponging ray queues, the three
// classification queues kernel 02 sorts into, and the shadow queue kernel 06 fills. All six are
// allocated whether or not a selected variant uses them - the pool is sized once per render and a
// queue is four bytes per path
inline constexpr uint32_t CRT_QUEUE_RAY_A = 0;
inline constexpr uint32_t CRT_QUEUE_ESCAPED = 2;
inline constexpr uint32_t CRT_QUEUE_EMISSIVE = 3;
inline constexpr uint32_t CRT_QUEUE_SURFACE = 4;
inline constexpr uint32_t CRT_QUEUE_SHADOW = 5;
inline constexpr uint32_t CRT_QUEUE_COUNT = 6;

// mirrors of the remaining GLSL structs, for buffer sizing only
inline constexpr VkDeviceSize CRT_PATH_STATE_SIZE = 64;
inline constexpr VkDeviceSize CRT_HIT_RECORD_SIZE = 24;
inline constexpr VkDeviceSize CRT_SHADOW_RAY_SIZE = 48;

class WavefrontPool {
public:
	// allocates the pool for width x height x samplesPerFrame paths if it is not that size already,
	// after a device-wide wait (the frames in flight may still read the old one - rare, since it
	// happens only when the resolution or the samples per frame change), and publishes the buffers.
	// True when it reallocated
	bool ensure(VulkanEngine* engine, CrtResourceTable& resources, uint32_t width, uint32_t height, uint32_t samplesPerFrame);
	// requires the pool to be out of use
	void destroy(VulkanEngine* engine, CrtResourceTable& resources);

	bool allocated() const { return m_allocated; }
	uint32_t width() const { return m_width; }
	uint32_t height() const { return m_height; }
	uint32_t samplesPerFrame() const { return m_samplesPerFrame; }
	uint32_t size() const { return m_size; }

	const AllocatedBuffer& headers() const { return m_headers; }
	const AllocatedBuffer& sampleBudget() const { return m_sampleBudget; }

	// the workgroups `variant` launches over its domain. Everything per-bounce is indirect, reading
	// the header the previous kernel filled as its VkDispatchIndirectCommand
	DispatchSize dispatchSize(const KernelVariant& variant, uint32_t bounce) const;
	// an empty header is a legal no-op dispatch. Reset from the command buffer, never by a shader,
	// so an empty queue cannot leave a stale count behind
	void resetHeader(VkCommandBuffer cmd, uint32_t queue) const;

private:
	uint32_t m_width { 0 };
	uint32_t m_height { 0 };
	uint32_t m_samplesPerFrame { 0 };
	uint32_t m_size { 0 };
	bool m_allocated { false };

	AllocatedBuffer m_paths {};
	AllocatedBuffer m_hits {};
	AllocatedBuffer m_queues {};
	AllocatedBuffer m_headers {};
	AllocatedBuffer m_radiance {};
	AllocatedBuffer m_sampleBudget {};
	// one ShadowRay per pool slot, indexed like the hits by ray queue position
	AllocatedBuffer m_shadowRays {};
};
