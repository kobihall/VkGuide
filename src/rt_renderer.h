#pragma once

// The raytracer as the user sees it: the "Raytrace Render" panel, the RenderSettings, the
// choice of scene camera, the Render/Stop buttons and the "Raytraced Output" window, in front
// of the GPU path tracer (rt_gpu.h).
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

	void drawPanel(VulkanEngine* engine, const RaytraceSceneEditor& editor);

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

	// The work one extend dispatch of a render at this size would do, in the SAH's units (roughly
	// node visits + primitive tests): pixels x samples per frame x the scene's expected cost per ray.
	// Before the BVH that cost was the triangle count, and a 1M-triangle model at 1080p (~2e12
	// tests in one dispatch) overran the GPU watchdog and took the window server - and the kernel -
	// down with it. The BVH brings the cost per ray to tens, but the guard stays: it is the one
	// thing between a pathological scene (thousands of overlapping instances) and a dead machine.
	static double estimatedWorkPerFrame(const RenderSettings& settings, double costPerRay);
	// what one ray of the CURRENTLY SELECTED kernel 02 variant costs in this scene, in the units
	// above. A BVH ray and a brute-force ray differ by orders of magnitude, so the guard has to
	// ask the strategy rather than assume one
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

	// which kernel variant runs in each slot of the wavefront (src/rt_kernels.h). Saved with the
	// scene; changing one restarts a running render, exactly like a setting
	const KernelSelection& kernels() const { return m_kernels; }
	void setKernels(const KernelSelection& selection);

	// the BVH builder and layout, saved with the scene alongside the kernel selection because
	// the two are chosen together: the selected kernel 02 variant is what fixes the layout
	const AccelSettings& accelSettings() const { return m_accelSettings; }
	void setAccelSettings(VulkanEngine* engine, const AccelSettings& settings);
	// the BLAS node layout the selected kernel 02 variant can read. Selecting the traversal is
	// what chooses the layout - there is no separate control for it, because a mismatch between
	// the two is not a preference, it is an unreadable buffer
	BvhLayout requiredLayout() const;

	// the "Raytracer Shaders" window: one row per kernel slot of the PBR wavefront figure, each
	// a combo over that slot's registered variants
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

	GpuPathTracer m_gpu;

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
	uint64_t m_sceneAccelModelRevision { 0 };
	uint64_t m_sceneAccelRevision { 0 };
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
