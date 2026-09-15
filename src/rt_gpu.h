#pragma once

// The GPU path tracer: a wavefront tracer over the sphere scene, progressive, in compute.
//
// One render is a snapshot taken at start() - camera, spheres, settings, seed - refined by
// samplesPerFrame samples per pixel every engine frame until maxSamples, or stop(). Per frame,
// record() runs generate -> [extend -> shade] x rayDepth -> resolve over a path pool of
// width x height x samplesPerFrame paths, with index queues compacted per bounce by a
// workgroup-aggregated allocator whose header doubles as the indirect dispatch arguments, then
// tonemaps the running mean into the renderer's display image. The design and its reasoning are
// docs/plans/compute-pipeline-raytracing.md §2.7-§2.12; the buffer contracts are
// shaders/crt_common.glsl, mirrored below with static_asserts.
//
// Everything Vulkan-facing is owned here: the four ComputePasses, every pool buffer, the
// accumulation images, the sphere upload, timestamp queries and the queue-header readback.
// The renderer owns the display image and the panel.

#include <vector>

#include <rt_scene_types.h>
#include <vk_compute.h>
#include <vk_types.h>

class VulkanEngine;

// C++ mirrors of the std430 structs in shaders/crt_common.glsl. vec3 pads to 16 bytes there, so
// every struct is laid out with an explicit fourth component
struct CrtSphere {
	glm::vec3 center;
	float radius;
};
static_assert(sizeof(CrtSphere) == 16);

struct CrtMaterial {
	glm::vec3 albedo;
	// fuzz | smoothness | ir, by type
	float param;
	uint32_t type;
	uint32_t pad[3];
};
static_assert(sizeof(CrtMaterial) == 32);

struct CrtQueueHeader {
	// == VkDispatchIndirectCommand, maintained by the allocator as ceil(rayCount / workgroup)
	uint32_t groupCountX;
	uint32_t groupCountY;
	uint32_t groupCountZ;
	uint32_t rayCount;
};
static_assert(sizeof(CrtQueueHeader) == 16);

// the largest path pool the tracer allocates: width x height x samplesPerFrame is clamped to it
// by lowering samplesPerFrame. 104 bytes per path, so ~416 MB at the cap
inline constexpr uint32_t CRT_MAX_POOL = 4u * 1024u * 1024u;
inline constexpr int CRT_MAX_DEPTH = 16;
inline constexpr int CRT_MAX_SAMPLES_PER_FRAME = 8;

enum class CrtDebugView : uint32_t {
	None = 0,
	PrimaryDirection,
	HitMiss,
	Normal,
	BounceHeat,
	SampleCountHeat,
	Count
};

const char* crtDebugViewName(CrtDebugView view);

// what start() renders: taken by value at the click, never re-read
struct GpuRenderSnapshot {
	uint32_t width { 0 };
	uint32_t height { 0 };
	RTCameraSnapshot camera;
	std::vector<SceneSphere> spheres;
	RenderSettings settings;
	// the seed actually used (settings.seed if fixed, otherwise drawn at Render)
	uint32_t seed { 0 };
	// sample the engine's environment map on a miss; false falls back to the sky gradient
	bool useEnvironmentMap { false };
	float environmentIntensity { 1.f };
};

class GpuPathTracer {
public:
	void init(VulkanEngine* engine);
	// requires the device to be idle
	void destroy(VulkanEngine* engine);

	// allocates or reallocates the pool for the snapshot's size (after a device-wide wait if the
	// size changed), uploads the spheres, and arms the next record() to clear the accumulation
	void start(VulkanEngine* engine, GpuRenderSnapshot snapshot);
	// keeps the image
	void stop();

	bool isRunning() const { return m_running; }
	uint32_t samplesAccumulated() const { return m_samplesAccumulated; }
	uint32_t maxSamples() const { return m_maxSamples; }
	// the pool's actual samples per frame, after clamping to CRT_MAX_POOL
	uint32_t samplesPerFrame() const { return m_samplesPerFrame; }
	uint32_t width() const { return m_snapshot.width; }
	uint32_t height() const { return m_snapshot.height; }

	// once per frame inside draw(), after the fence wait: collects the timestamps and queue
	// headers recorded FRAME_OVERLAP frames ago into this frame's slot
	void beginFrame(VulkanEngine* engine);

	// records this frame's tracing work into cmd while running, and whenever the accumulation
	// holds a render, tonemaps it (scaled by `exposure`) into displayImage, which must be in
	// VK_IMAGE_LAYOUT_GENERAL
	void record(VkCommandBuffer cmd, VulkanEngine* engine, const AllocatedImage& displayImage, float exposure);

	// the accumulation holds a render (running or finished) worth tonemapping
	bool hasImage() const { return m_hasImage; }

	// GPU time of the last recorded frame of this render, and the running total; 0 when the
	// device has no timestamps
	bool hasTimestamps() const { return m_hasTimestamps; }
	float lastFrameGpuMs() const { return m_lastFrameGpuMs; }
	float totalGpuMs() const { return m_totalGpuMs; }
	// paths alive entering each bounce, [0] being what generate spawned; the proof the
	// compaction works. From the last frame read back
	const std::vector<uint32_t>& pathsAlivePerBounce() const { return m_pathsAlive; }

	// switchable mid-render; written through the radiance slots, so it accumulates like radiance
	CrtDebugView debugView { CrtDebugView::None };

private:
	void allocatePool(VulkanEngine* engine, uint32_t width, uint32_t height, uint32_t samplesPerFrame);
	void freePool(VulkanEngine* engine);
	void uploadScene(VulkanEngine* engine);
	void collectReadbacks(VulkanEngine* engine, uint32_t slot);
	VkDescriptorSet writeSet(VulkanEngine* engine, const ComputePass& pass);

	ComputePass m_generate;
	ComputePass m_extend;
	ComputePass m_shade;
	ComputePass m_resolve;

	// the pool, sized for m_poolWidth x m_poolHeight x m_samplesPerFrame paths
	uint32_t m_poolWidth { 0 };
	uint32_t m_poolHeight { 0 };
	uint32_t m_samplesPerFrame { 0 };
	uint32_t m_poolSize { 0 };
	bool m_hasPool { false };
	bool m_hasImage { false };
	AllocatedBuffer m_paths {};
	AllocatedBuffer m_hits {};
	AllocatedBuffer m_queues {};
	AllocatedBuffer m_headers {};
	AllocatedBuffer m_radiance {};
	AllocatedBuffer m_sampleBudget {};
	AllocatedImage m_accumulation {};
	AllocatedImage m_sampleCount {};

	// the snapshot's spheres, one buffer, uploaded once per render
	AllocatedBuffer m_sceneBuffer {};
	uint32_t m_sphereCount { 0 };
	VkDeviceSize m_materialsOffset { 0 };

	// per frame slot: timestamps (two queries) and the queue headers after each producer
	VkQueryPool m_queryPool { VK_NULL_HANDLE };
	bool m_hasTimestamps { false };
	float m_timestampPeriodNs { 1.f };
	AllocatedBuffer m_headerReadback {};
	// which render, and how many headers, each slot's readback belongs to; 0 = nothing recorded
	std::vector<uint64_t> m_slotRender;
	std::vector<uint32_t> m_slotHeaderCount;

	GpuRenderSnapshot m_snapshot;
	uint64_t m_renderSerial { 0 };
	bool m_running { false };
	bool m_clearPending { false };
	uint32_t m_samplesAccumulated { 0 };
	uint32_t m_maxSamples { 0 };
	float m_lastFrameGpuMs { 0.f };
	float m_totalGpuMs { 0.f };
	std::vector<uint32_t> m_pathsAlive;
};
