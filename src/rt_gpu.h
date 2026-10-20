#pragma once

// The GPU renderer: what every integrator shares, around the one integrator a render runs.
//
// One render is a snapshot taken at start() - camera, scene, settings, seed, kernel selection -
// refined every engine frame until maxSamples, or stop(). Per frame, record() zeroes the traversal
// counters, asks the active integrator's schedule (rt_schedule.h) to record its work, copies the
// counters back, measures the reference and tonemaps the running mean into the renderer's display
// image. The path tracer's schedule is rt_path_schedule.h; the design is
// docs/plans/completed/integrator-layer.md, and the buffer contracts are
// shaders/rt/include/crt_common.glsl, mirrored in rt_gpu_types.h.
//
// Owned here: the scene upload (the BVH geometry device-local, the per-edit instances, TLAS and
// light list host-visible), the environment's sampling tables, the film (the accumulation and
// sample-count images), the reference and its error pass, timestamps, the traversal counters and
// their readback, the wavefront pool every queue-based schedule runs on, the resource table every
// descriptor set is written from, and the pipeline cache. The RaytraceRenderer owns the display
// image, the BVH settings and the panels.

#include <map>
#include <tuple>
#include <memory>
#include <span>
#include <vector>

#include <image_io.h>
#include <rt_accel.h>
#include <rt_gpu_types.h>
#include <rt_kernels.h>
#include <rt_scene_types.h>
#include <rt_wavefront.h>
#include <vk_compute.h>
#include <vk_types.h>

class VulkanEngine;
class IntegratorSchedule;

// how far the running render is from the reference image, over every pixel and channel
struct ReferenceError {
	// sqrt of the mean squared difference
	double rmse { 0.0 };
	// the mean of (render - reference)^2 / (reference^2 + 0.01): an error that weighs a dark pixel's
	// noise like a bright one's, the usual way renderers' outputs are compared
	double relativeMse { 0.0 };
	// the mean of |render - reference| / max(|reference|, 0.01): a fraction, 0.05 is 5 percent
	double mape { 0.0 };
	// the mean of render - reference over every pixel and channel: a bias the noise does not show
	double meanDifference { 0.0 };
	// the samples per pixel the render had when this was measured
	uint32_t samples { 0 };
	bool valid { false };
};

enum class CrtDebugView : uint32_t {
	None = 0,
	PrimaryDirection,
	HitMiss,
	Normal,
	BounceHeat,
	SampleCountHeat,
	TraversalCost,
	MisWeights,
	NormalDisagreement,
	Count
};

const char* crtDebugViewName(CrtDebugView view);

// what start() renders: taken by value at the click, never re-read
struct GpuRenderSnapshot {
	uint32_t width { 0 };
	uint32_t height { 0 };
	RTCameraSnapshot camera;
	// the shapes and placed meshes, with their BVHs, shared immutably. Compared by pointer to
	// decide whether an upload is needed at all: a new one is only ever built for a real change
	std::shared_ptr<const RaytraceSceneAccel> scene;
	RenderSettings settings;
	// the seed actually used (settings.seed if fixed, otherwise drawn at Render)
	uint32_t seed { 0 };
	// the integrator, the traversal and the variant of every stage. Taken at the click like
	// everything else here, so changing a strategy mid-render restarts rather than mixes
	KernelSelection kernels;
	// the environment map's sampling tables, when the map is the background and so a light kernel
	// 06 can sample; compared by pointer, since one is built per map load. Null otherwise
	std::shared_ptr<const EnvironmentDistribution> environment;
	// sample the engine's environment map on a miss; false falls back to the sky gradient
	bool useEnvironmentMap { false };
	float environmentIntensity { 1.f };
	// the power of two the map's texels are stored divided by (VulkanEngine::m_environmentMapScale)
	float environmentMapScale { 1.f };
	// a missed ray sees backgroundColor instead of the environment map or the sky gradient
	bool solidBackground { false };
	glm::vec3 backgroundColor { 0.f };
};

// Why `sampler` cannot run a render of width x height with `settings` (as the render uses them, after
// RaytraceRenderer::renderSettings()), or empty when it can. A sampler that needs the sample count
// cannot run an unlimited render, one that interleaves pixels cannot run an image wider than
// SAMPLER_MAX_IMAGE_SIDE, the Sobol and PMJ02BN samplers need their tables (`tablesLoaded`), and
// PMJ02BN at most PMJ02BN_MAX_SAMPLES_PER_PIXEL samples. Such a render runs the independent sampler.
// GpuRenderer::start() decides with it, and the Sampler section says it for the settings as they are
std::string samplerFallbackReason(const KernelVariant& sampler, const RenderSettings& settings, uint32_t width, uint32_t height, bool tablesLoaded);

class GpuRenderer {
public:
	GpuRenderer();
	~GpuRenderer();

	// builds the default selection's pipelines, so the first render does not wait for them
	void init(VulkanEngine* engine);
	// requires the device to be idle
	void destroy(VulkanEngine* engine);

	// starts (or restarts) a render: switches schedule if the integrator changed (after a
	// device-wide wait - rare), reallocates the film if its size changed, uploads the scene, builds
	// any pipeline the selection needs that is not built yet, and has the schedule size its own
	// buffers. Cheap enough to call on every camera move
	void start(VulkanEngine* engine, GpuRenderSnapshot snapshot);
	// keeps the image
	void stop();

	bool isRunning() const { return m_running; }
	uint32_t samplesAccumulated() const { return m_samplesAccumulated; }
	// 0 = unlimited
	uint32_t maxSamples() const { return m_maxSamples; }
	// what one frame of the active integrator adds, after its own clamping
	uint32_t samplesPerFrame() const;
	uint32_t width() const { return m_snapshot.width; }
	uint32_t height() const { return m_snapshot.height; }

	// once per frame inside draw(), after the fence wait: collects the timestamps and readbacks
	// recorded FRAME_OVERLAP frames ago into this frame's slot
	void beginFrame(VulkanEngine* engine);

	// records this frame's work into cmd while running, and whenever the film holds a render,
	// tonemaps it (scaled by `exposure`) into displayImage, which must be in VK_IMAGE_LAYOUT_GENERAL
	void record(VkCommandBuffer cmd, VulkanEngine* engine, const AllocatedImage& displayImage, float exposure);

	// the film holds a render (running or finished) worth tonemapping
	bool hasImage() const { return m_hasImage; }

	// GPU time of the last recorded frame of this render, and the running total; 0 when the
	// device has no timestamps
	bool hasTimestamps() const { return m_hasTimestamps; }
	float lastFrameGpuMs() const { return m_lastFrameGpuMs; }
	float totalGpuMs() const { return m_totalGpuMs; }
	// what the traversal did in the last frame read back: counted, not timed, so it compares
	// builders and layouts reliably even on a GPU whose clocks move (a throttling laptop)
	const TraversalWork& traversalWork() const;
	// samples kernel 09 has left out of the mean as non-finite since the render started. Anything
	// above zero means a kernel produced a NaN or an infinity, and the image is biased by the
	// samples it lost
	uint64_t droppedSamples() const { return m_droppedSamples; }
	// the active schedule's per-bounce counts, from the last frame read back
	std::span<const ScheduleCounter> counters() const;

	// the schedule that records `id`, for pricing a render that has not started
	const IntegratorSchedule& schedule(IntegratorId id) const;

	// assets/sampler/ loaded, which the Sobol and PMJ02BN samplers need (samplerFallbackReason())
	bool samplerTablesLoaded() const { return m_samplerTablesLoaded; }
	// the render's sampler choices as CrtParams carries them
	void sampleParams(CrtParams& params) const;

	// THE REFERENCE: a copy of the accumulation to measure later renders against, for comparing
	// strategies at equal samples or equal time. Captured from the next frame recorded; kept until
	// cleared or the renderer is destroyed, and measured only while the render is the same size
	void captureReference();
	void clearReference(VulkanEngine* engine);
	bool hasReference() const { return m_hasReference; }
	uint32_t referenceSamples() const { return m_referenceSamples; }
	VkExtent2D referenceExtent() const { return { m_reference.imageExtent.width, m_reference.imageExtent.height }; }
	// the last measurement read back, of the render currently in the accumulation
	const ReferenceError& referenceError() const { return m_referenceError; }
	// every measurement of the running render against the reference, one per sample count: the
	// convergence curve. Cleared when a render starts and when the reference changes
	const std::vector<ReferenceError>& convergence() const { return m_convergence; }
	// The accumulation, or the reference, as an RGB image on the host. They wait for the device
	// to be idle, so call them between renders or accept the stall. False when there is none
	bool readAccumulation(VulkanEngine* engine, Image& image);
	bool readReference(VulkanEngine* engine, Image& image);
	// Replaces the reference with a loaded image, of any size: a render of another size is not
	// compared against it. Its sample count is unknown and shows as 0
	void loadReference(VulkanEngine* engine, const Image& image);

	// switchable mid-render; written through the radiance slots, so it accumulates like radiance
	CrtDebugView debugView { CrtDebugView::None };


	// FOR SCHEDULES. The render being recorded, every binding's resource, and the wavefront pool
	const GpuRenderSnapshot& snapshot() const { return m_snapshot; }
	CrtResourceTable& resources() { return m_resources; }
	WavefrontPool& wavefrontPool() { return m_pool; }
	// the pipeline of `variant`, built on first use. A kernel that traces is built for the
	// snapshot's traversal, with the traversal's bindings added to its own; a kernel built per
	// material class, for `materialClass`
	const ComputePass& pass(VulkanEngine* engine, const KernelVariant& variant, MaterialClass materialClass = MaterialClass::Basic);
	// one launch of `variant`: its descriptor set written from the resource table for exactly the
	// bindings it declared (and its traversal's), its push constants, its size
	void dispatch(VkCommandBuffer cmd, VulkanEngine* engine, const KernelVariant& variant, const CrtParams& params, const DispatchSize& size,
		MaterialClass materialClass = MaterialClass::Basic);

private:
	// the pipeline of `variant` for traversal `traversal` (ignored for a kernel that does not trace),
	// `materialClass` (ignored for a kernel not built per class) and `sampleRecord`, whether the
	// sampler writes a record (ignored for a kernel that does not read it)
	const ComputePass& pass(VulkanEngine* engine, const KernelVariant& variant, uint32_t traversal, MaterialClass materialClass, bool sampleRecord);
	// builds every pipeline the selection's integrator runs
	void preparePipelines(VulkanEngine* engine, const KernelSelection& selection);
	// the bindings `variant` is built and written with under `traversal`: its own, plus the
	// traversal's if it traces, each once
	static std::vector<CrtBinding> bindingsOf(const KernelVariant& variant, uint32_t traversal);

	// the accumulation and sample-count images, reallocated (after a device-wide wait) when the
	// render's size changes
	void ensureFilm(VulkanEngine* engine, uint32_t width, uint32_t height);
	void freeFilm(VulkanEngine* engine);
	void uploadScene(VulkanEngine* engine);
	void uploadGeometry(VulkanEngine* engine, const std::shared_ptr<const RaytraceGeometry>& geometry);
	// hands a buffer the frame in flight may still read to the deletion queue of that frame's slot
	void retireBuffer(VulkanEngine* engine, AllocatedBuffer& buffer);
	void collectReadbacks(VulkanEngine* engine, uint32_t slot);
	// the engine-owned images (the environment map, the texture array), which can change between
	// frames, published each frame before anything is written
	void publishEngineImages(VulkanEngine* engine);
	// the environment map's tables, device-local, once per map; a stub when there is none
	void uploadEnvironment(VulkanEngine* engine, const std::shared_ptr<const EnvironmentDistribution>& environment);
	// the push constants every integrator shares: the camera, the scene's counts, the flags
	CrtParams baseParams(VulkanEngine* engine) const;
	// the reference copy and its error measurement, inside record()
	void recordReference(VkCommandBuffer cmd, VulkanEngine* engine, uint32_t slot);

	CrtResourceTable m_resources;

	// ONE PIPELINE PER (VARIANT, TRAVERSAL, MATERIAL CLASS, SAMPLE RECORD), built on first use: a kernel that traces
	// is built once per traversal, one that does not once, keyed with NO_TRAVERSAL; a kernel built
	// per material class once per class, any other keyed with NO_CLASS. The registry's entries never
	// move, so a variant's address is its identity
	static constexpr uint32_t NO_TRAVERSAL = ~0u;
	static constexpr uint32_t NO_CLASS = ~0u;
	std::map<std::tuple<const KernelVariant*, uint32_t, uint32_t, uint32_t>, ComputePass> m_passes;

	// one schedule per integrator, IntegratorId order, created in the constructor; the active one
	// is the snapshot's integrator. m_scheduleActive says whether it has buffers to release
	std::vector<std::unique_ptr<IntegratorSchedule>> m_schedules;
	IntegratorId m_activeIntegrator { IntegratorId::Path };
	bool m_scheduleActive { false };

	WavefrontPool m_pool;

	// the film, sized for the render
	AllocatedImage m_accumulation {};
	AllocatedImage m_sampleCount {};
	bool m_hasFilm { false };
	bool m_hasImage { false };

	// the BLASes' nodes, triangles and shading attributes, device-local. Uploaded once per BLAS set
	// - on a model import or a BVH settings change - through a staging buffer
	AllocatedBuffer m_geometryBuffer {};
	std::shared_ptr<const RaytraceGeometry> m_uploadedGeometry;

	// the per-edit part: instances, materials, the per-surface material table, the TLAS and the
	// light list. Small, host-visible, and replaced whenever the scene changes - a gizmo drag under
	// restart-on-change rebuilds it every frame, which is why the geometry lives elsewhere
	AllocatedBuffer m_sceneBuffer {};
	uint32_t m_tlasInstanceCount { 0 };
	uint32_t m_unboundedCount { 0 };
	// every instance, TLAS or not: what the linear traversal scans
	uint32_t m_instanceCount { 0 };
	std::shared_ptr<const RaytraceSceneAccel> m_uploadedScene;

	// pbrt's Sobol matrices for the samplers (crt_common.glsl SamplerTableBuffer), device-local,
	// uploaded once at start-up from assets/sampler/. Without the files (m_samplerTablesLoaded false)
	// the binding names a stub and the samplers that read it do not run
	void uploadSamplerTables(VulkanEngine* engine);
	SamplerTables m_samplerData;
	AllocatedBuffer m_samplerTableBuffer {};
	bool m_samplerTablesLoaded { false };
	// PMJ02BN's pixel samples are sorted for one sample count, into a region of the table buffer that
	// starts at word m_pmjRegionWord; rewritten (after a device-wide wait) when a render with another
	// count starts. 0 for none written yet
	void uploadPmjPixelSamples(VulkanEngine* engine, uint32_t samplesPerPixel);
	uint32_t m_pmjRegionWord { 0 };
	uint32_t m_pmjUploadedSamples { 0 };

	// the environment map's two sampling tables (crt_common.glsl EnvironmentSamplingBuffer),
	// device-local, replaced only when another map is loaded
	AllocatedBuffer m_environmentBuffer {};
	std::shared_ptr<const EnvironmentDistribution> m_uploadedEnvironment;
	bool m_hasEnvironmentBuffer { false };

	// per frame slot: timestamps (two queries)
	VkQueryPool m_queryPool { VK_NULL_HANDLE };
	bool m_hasTimestamps { false };
	float m_timestampPeriodNs { 1.f };
	// which render each slot's readback belongs to; 0 = nothing recorded
	std::vector<uint64_t> m_slotRender;

	GpuRenderSnapshot m_snapshot;
	uint64_t m_renderSerial { 0 };
	bool m_running { false };
	bool m_clearPending { false };
	uint32_t m_samplesAccumulated { 0 };
	uint32_t m_maxSamples { 0 };
	float m_lastFrameGpuMs { 0.f };
	float m_totalGpuMs { 0.f };
	uint64_t m_droppedSamples { 0 };
	// the traversal counters every kernel that traces adds to, zeroed per frame, and their readback
	// per frame slot
	AllocatedBuffer m_traversalStats {};
	AllocatedBuffer m_statsReadback {};

	// the reference image, its error pass and the per-workgroup partial sums that pass writes, read
	// back per frame slot
	AllocatedImage m_reference {};
	bool m_hasReference { false };
	bool m_capturePending { false };
	uint32_t m_referenceSamples { 0 };
	ComputePass m_errorPass;
	AllocatedBuffer m_errorPartials {};
	AllocatedBuffer m_errorReadback {};
	VkDeviceSize m_errorPartialsBytes { 0 };
	// per frame slot: how many partials were written (0 for none) and at how many samples
	std::vector<uint32_t> m_slotErrorGroups;
	std::vector<uint32_t> m_slotErrorSamples;
	ReferenceError m_referenceError;
	std::vector<ReferenceError> m_convergence;
};
