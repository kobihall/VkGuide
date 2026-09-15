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

#include <rt_gpu.h>
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

	// saved with the scene
	RenderSettings& settings() { return m_settings; }
	const RenderSettings& settings() const { return m_settings; }
	void setSettings(const RenderSettings& settings);

	// which scene camera renders. An id the scene no longer has falls back to its first camera
	uint64_t renderCameraId() const { return m_renderCameraId; }
	void setRenderCamera(uint64_t id) { m_renderCameraId = id; }

private:
	// everything a render depends on, compared every frame against what the running render
	// was started from
	struct RenderKey {
		RTCameraSnapshot camera;
		RenderSettings settings;
		uint64_t sceneRevision { 0 };
		std::filesystem::path environmentMapPath;
		float environmentIntensity { 1.f };
		uint32_t width { 0 };
		uint32_t height { 0 };

		bool operator==(const RenderKey&) const = default;
	};

	// the camera the next render would use, or null when the scene has none
	const SceneCamera* resolveCamera(const RaytraceSceneEditor& editor);
	RenderKey currentKey(VulkanEngine* engine, const RaytraceSceneEditor& editor, const SceneCamera& camera) const;
	void startRender(VulkanEngine* engine, const RaytraceSceneEditor& editor);
	void ensureDisplayImage(VulkanEngine* engine, uint32_t width, uint32_t height);
	void destroyDisplayImage(VulkanEngine* engine);
	void drawSettings(VulkanEngine* engine);

	RenderSettings m_settings;
	// the Resolution combo's selection: 0 is "Match viewport", 1.. index RESOLUTION_PRESETS,
	// -1 a size from a file that matches no preset
	int m_resolutionChoice { DEFAULT_RESOLUTION_PRESET + 1 };
	uint64_t m_renderCameraId { 0 };

	GpuPathTracer m_gpu;

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

	bool m_showPanel { true };
};
