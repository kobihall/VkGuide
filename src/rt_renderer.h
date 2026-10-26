#pragma once

// The raytracer as the user sees it: the "Raytrace Render" panel, the RenderSettings, the
// choice of scene camera, the Render/Stop buttons and the "Raytraced Output" window, in front
// of the GPU renderer (rt_gpu.h) and whichever integrator the selection names.
//
// A render is of a scene camera. It starts from what the scene, the camera and the settings
// are at that moment; with "restart on change" (the default) the renderer watches all three
// while the render runs and starts over the moment any of them differs from what it was
// started from, so with unlimited samples the output window is a live, converging view of the
// scene. A finished or stopped render is a snapshot and stays until the next Render click.
// Exposure is the one exception: it is applied by the tonemap every frame and never restarts.
//
// The display image (rgba8, TonemapPass::DISPLAY_FORMAT) is persistent and owned here. It is
// recreated only when the render resolution changes, after a device-wide wait - a Render is a
// menu-like action and a one-frame stall there is simpler than a deferred-destroy list.
// Between frames it always sits in SHADER_READ_ONLY_OPTIMAL, the layout DisplayRegistry needs.

#include <chrono>
#include <filesystem>

#include <rt_accel.h>
#include <rt_gpu.h>
#include <rt_kernels.h>
#include <rt_scene_types.h>
#include <vk_types.h>

class VulkanEngine;
class RaytraceSceneEditor;

class RaytraceRenderer {
public:
	// after the tonemap pass and default data exist
	void init(VulkanEngine* engine);

	// once per frame from the main thread, before the imgui frame's content is built: reports
	// a finished render and restarts a render whose inputs changed
	void update(VulkanEngine* engine, const RaytraceSceneEditor& editor);

	// non-const: the Outputs section edits the scene's light groups and the camera's relight gains
	void drawPanel(VulkanEngine* engine, RaytraceSceneEditor& editor);

	// inside draw()'s command buffer, before the raster passes: this frame's path-tracing
	// work and its tonemap into the display image
	void record(VkCommandBuffer cmd, VulkanEngine* engine, const RaytraceSceneEditor& editor);

	// ends a running render and keeps what it has. Also what destroying the environment map
	// calls, since a render samples it
	void cancelRender();

	// releases every GPU resource. Requires the device to be idle, and must run before the
	// display registry is torn down
	void shutdown(VulkanEngine* engine);

	bool* visibilityFlag() { return &m_showPanel; }

	// the models changed (an import, a removal, a scene load): builds the BLAS of every mesh not
	// already built. Called from the engine's scene-change path, so the BVH work happens at load
	// time and never when a render starts
	void prepareGeometry(VulkanEngine* engine);

	// The work one frame of a render with these settings would do, in the SAH's units (roughly node
	// visits + primitive tests), as the selected integrator's schedule prices it from the cost of
	// one ray. Before the BVH that cost was the triangle count, and a 1M-triangle model at 1080p
	// (~2e12 tests in one dispatch) overran the GPU watchdog and took the window server - and the
	// kernel - down with it. The BVH brings the cost per ray to tens, but the guard stays: it is
	// the one thing between a pathological scene (thousands of overlapping instances) and a dead
	// machine.
	double estimatedWorkPerFrame(const RenderSettings& settings) const;
	// what one ray of the CURRENTLY SELECTED traversal costs in this scene, in the units above. A
	// BVH ray and a brute-force ray differ by orders of magnitude, so the guard has to ask the
	// traversal rather than assume one
	double sceneCostPerRay() const;
	// roughly a second of the extend stage on the development GPU, which leaves the watchdog a wide
	// margin. Deliberately not a setting: the override below is per-render and never saved
	static constexpr double WORK_PER_FRAME_BUDGET = 5.0e8;

	// saved with the scene
	RenderSettings& settings() { return m_settings; }
	const RenderSettings& settings() const { return m_settings; }
	void setSettings(const RenderSettings& settings);

	// which scene camera renders. An id the scene no longer has falls back to its first camera
	uint64_t renderCameraId() const { return m_renderCameraId; }
	void setRenderCamera(uint64_t id) { m_renderCameraId = id; }

	// Reference images and render exports as files. They wait for the device, so a running render stalls for the
	// readback
	IoResult saveReference(VulkanEngine* engine, const std::filesystem::path& path);
	IoResult loadReference(VulkanEngine* engine, const std::filesystem::path& path);
	// EXPORT (docs/plans/completed/pbrt-aovs-and-image-output.md). Writes the render as the file's extension says:
	// .exr the picture with every output the render kept as named channels (src/render_export.h) and pbrt's
	// metadata, .pfm and .hdr the picture, .png the display image as the window shows it, 8-bit
	IoResult exportRender(VulkanEngine* engine, const RaytraceSceneEditor& editor, const std::filesystem::path& path);
	// how File > Export Render writes an EXR file, and whether a render saves itself when it reaches its sample target
	// (to the path set here): saved with the scene, and changing it restarts nothing
	const ExportSettings& exportSettings() const { return m_exportSettings; }
	void setExportSettings(const ExportSettings& settings) { m_exportSettings = settings; }
	// the last export, from File > Export Render or Save when finished
	const IoResult& exportResult() const { return m_exportIo; }
	// the render, for the batch mode (src/command_line.h): whether one is running, its samples, and the
	// measurements the panel would show
	// starts a render now, as the Render button does, with the reason it was refused (if it was)
	void render(VulkanEngine* engine, const RaytraceSceneEditor& editor) { startRender(engine, editor); }
	const std::string& blockedReason() const { return m_blockedReason; }
	uint64_t droppedSamples() const { return m_gpu.droppedSamples(); }
	float totalGpuMs() const { return m_gpu.totalGpuMs(); }
	void setReferenceIo(const IoResult& result) { m_referenceIo = result; }
	bool renderRunning() const { return m_gpu.isRunning(); }
	// the display-side controls the batch mode sets: the debug view, the ambient occlusion distance, Relight
	GpuRenderer& gpu() { return m_gpu; }
	bool hasRender() const { return m_hasRender; }
	uint32_t samplesAccumulated() const { return m_gpu.samplesAccumulated(); }
	uint32_t maxSamples() const { return m_gpu.maxSamples(); }

	// the integrator, the traversal and the variant of each stage (src/rt_kernels.h). Saved with
	// the scene; changing one restarts a running render, exactly like a setting
	const KernelSelection& kernels() const { return m_kernels; }
	void setKernels(const KernelSelection& selection);

	// the BVH builder and layout, saved with the scene alongside the kernel selection because
	// the two are chosen together: the selected traversal is what fixes the layout
	const AccelSettings& accelSettings() const { return m_accelSettings; }
	void setAccelSettings(VulkanEngine* engine, const AccelSettings& settings);
	// the BLAS node layout the selected traversal can read. Selecting the traversal is what
	// chooses the layout - there is no separate control for it, because a mismatch between the
	// two is not a preference, it is an unreadable buffer
	BvhLayout requiredLayout() const;

	// the "Raytracer Shaders" window: the integrator and the traversal, then one row per stage the
	// integrator runs, each a combo over that stage's registered variants
	void drawKernelPanel(VulkanEngine* engine);
	bool* kernelPanelVisibilityFlag() { return &m_showKernelPanel; }

private:
	// everything a render depends on, compared every frame against what the running render
	// was started from
	struct RenderKey {
		RTCameraSnapshot camera;
		RenderSettings settings;
		uint64_t sceneRevision { 0 };
		// the engine's own, bumped when the loaded models change. The editor's revision moves for
		// most of those too, but not for every one - a model whose nodes were all deleted already
		// adds and removes nothing
		uint64_t modelRevision { 0 };
		// which BLAS set, so rebuilding with other BVH settings restarts a running render
		uint64_t accelRevision { 0 };
		KernelSelection kernels;
		std::filesystem::path environmentMapPath;
		float environmentIntensity { 1.f };
		bool solidBackground { false };
		glm::vec3 backgroundColor { 0.f };
		uint32_t width { 0 };
		uint32_t height { 0 };
		// the scene's light groups, which the sums a render keeps are split by
		uint32_t lightGroupCount { 1 };
		uint32_t environmentGroup { 0 };

		bool operator==(const RenderKey&) const = default;
	};

	// the camera the next render would use, or null when the scene has none
	const SceneCamera* resolveCamera(const RaytraceSceneEditor& editor);
	RenderKey currentKey(VulkanEngine* engine, const RaytraceSceneEditor& editor, const SceneCamera& camera) const;
	void startRender(VulkanEngine* engine, const RaytraceSceneEditor& editor);
	// the TLAS, instances and materials over the current BLAS set, rebuilt only when the objects or
	// shapes actually differ from what it was built from. A render restarted by a camera drag
	// happens every frame and must not pay for this
	void ensureSceneAccel(VulkanEngine* engine, const RaytraceSceneEditor& editor);
	// the "Acceleration structure" section: builder and layout, and what they produced
	void drawAccelSettings(VulkanEngine* engine);
	// the "Sampler" section: kernel 01's variant, its randomization, and why a render runs another
	void drawSamplerSettings(VulkanEngine* engine);
	void drawFilmSettings(VulkanEngine* engine);
	// the "Spectral" section (docs/plans/completed/pbrt-spectral-rendering.md): the representation, its lanes, range and
	// sampling, the working space, the design wavelength, and how a monochromatic render is shown
	void drawSpectralSettings();
	// the "Outputs" section: the auxiliary outputs, the light groups and Relight
	void drawOutputsSettings(VulkanEngine* engine, RaytraceSceneEditor& editor);
	// the "Export" section: the file options, Save when finished and the Export Render button
	void drawExportSettings(VulkanEngine* engine, RaytraceSceneEditor& editor);
	// m_settings as a render started now would use them: the viewport's size under Match viewport,
	// one sample without antialiasing, every count clamped to what the tracer allows
	RenderSettings renderSettings(VulkanEngine* engine) const;
	// the "Intersection precision" section: error-bounded ray offsets and watertight triangles. Apart
	// from the acceleration structure's, since every traversal takes both
	void drawIntersectionSettings();
	// the "Integrator" combo, shared by both panels: true when the selection changed
	bool drawIntegratorCombo();
	// the "Direct lighting" combo over kernel 06's variants and what the light list holds
	void drawLightingSettings(VulkanEngine* engine);
	// the "Compare to reference" section: capture, save, load, clear, the running render's error and
	// its convergence curve
	void drawReference(VulkanEngine* engine);
	// the label a render started now gets on the convergence plot: what sets it apart
	std::string convergenceLabel() const;
	// the finished or restarted render's curve joins the kept ones
	void keepConvergenceCurve();
	// the environment as the light list sees it: the map's tables when the map is the background
	EnvironmentLight environmentLight(VulkanEngine* engine) const;
	void ensureDisplayImage(VulkanEngine* engine, uint32_t width, uint32_t height);
	void destroyDisplayImage(VulkanEngine* engine);
	void drawSettings(VulkanEngine* engine);

	// the Resolution combo's selection: RESOLUTION_VIEWPORT is "Match viewport",
	// RESOLUTION_CUSTOM the typed-in size, and 0.. index RESOLUTION_PRESETS
	static constexpr int RESOLUTION_VIEWPORT = -1;
	static constexpr int RESOLUTION_CUSTOM = -2;

	RenderSettings m_settings;
	KernelSelection m_kernels { defaultKernelSelection() };
	bool m_showKernelPanel { false };
	int m_resolutionChoice { DEFAULT_RESOLUTION_PRESET };
	// what the custom width/height fields hold while they are being typed in. Only committed
	// into m_settings when the field is left or Enter is pressed, so a half-typed number never
	// becomes a resolution (and, under restart-on-change, never restarts the render)
	int m_customWidth { RESOLUTION_PRESETS[DEFAULT_RESOLUTION_PRESET].width };
	int m_customHeight { RESOLUTION_PRESETS[DEFAULT_RESOLUTION_PRESET].height };
	uint64_t m_renderCameraId { 0 };

	GpuRenderer m_gpu;

	// THE CONVERGENCE PLOT. Each render measured against the reference leaves a curve of relative
	// MSE against samples per pixel; the ones before the running render are kept so strategies can
	// be compared on one plot. All cleared when the reference changes
	struct ConvergenceCurve {
		std::string label;
		std::vector<ReferenceError> points;
	};
	std::vector<ConvergenceCurve> m_curves;
	std::string m_curveLabel;
	IoResult m_referenceIo;
	ExportSettings m_exportSettings;
	IoResult m_exportIo;

	// the BVH settings, as applied (m_accelSettings) and as being edited in the panel
	AccelSettings m_accelSettings { defaultAccelSettings() };
	AccelSettings m_pendingAccelSettings { defaultAccelSettings() };
	RaytraceBlasCache m_blasCache;
	// every loaded mesh's BLAS and their packed geometry; replaced by prepareGeometry()
	std::shared_ptr<const RaytraceBlasSet> m_blasSet;
	std::shared_ptr<const RaytraceGeometry> m_geometry;
	uint64_t m_accelRevision { 0 };

	// the scene the next render traces, and exactly what it was built from
	std::shared_ptr<const RaytraceSceneAccel> m_sceneAccel;
	std::vector<SceneMeshObject> m_sceneAccelObjects;
	std::vector<SceneShape> m_sceneAccelShapes;
	std::vector<SceneLight> m_sceneAccelLights;
	std::vector<MaterialDef> m_sceneAccelMaterials;
	EnvironmentLight m_sceneAccelEnvironment;
	SpectralSceneContext m_sceneAccelSpectral;
	uint64_t m_sceneAccelModelRevision { 0 };
	uint64_t m_sceneAccelRevision { 0 };
	// the editor's content revision the scene acceleration structure was built or last checked at
	uint64_t m_sceneAccelContentRevision { 0 };
	bool m_hasSceneAccel { false };

	AllocatedImage m_displayImage {};
	bool m_hasDisplayImage { false };

	// what the running render was started from
	RenderKey m_renderKey;
	bool m_hasRender { false };
	bool m_wasRunning { false };
	std::chrono::steady_clock::time_point m_renderStart;

	// about the last render that finished or was stopped
	float m_lastRenderMs { 0.f };
	int m_lastRenderSamples { 0 };
	bool m_hasLastRender { false };

	// the user has explicitly accepted a render over the budget for the scene as it stands. Reset
	// by anything that changes what would be rendered, so an acknowledgement can never carry over
	// to a heavier scene than the one it was given for
	bool m_acceptedHeavyRender { false };
	double m_acceptedWork { 0.0 };
	// why the last startRender() refused, empty when it did not
	std::string m_blockedReason;

	bool m_showPanel { true };
};
