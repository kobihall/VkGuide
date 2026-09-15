#pragma once

// The raytracer as the user sees it: one "Raytrace Render" panel, one RenderSettings block, one
// Render button, one "Raytraced Output" window - backed by whichever backend is selected.
//
// The GPU path tracer (rt_gpu.h) is the real backend. The CPU raytracer (rt_job.h) stays
// selectable behind the Backend combo so the two can be rendered from the same snapshot into
// the same window at the same resolution and compared; when the GPU one is trusted, RaytraceJob
// is deleted and this keeps its shape (docs/plans/compute-pipeline-raytracing.md §2.14).
//
// The display image (rgba8, TonemapPass::DISPLAY_FORMAT) is persistent and owned here. The GPU
// backend tonemaps into it every frame of a running render from inside draw()'s command buffer;
// the CPU backend uploads its finished linear buffer and tonemaps it once. It is recreated only
// when the render resolution changes, after a device-wide wait - a Render click is a menu-like
// action and a one-frame stall there is simpler than a deferred-destroy list. Between frames it
// always sits in SHADER_READ_ONLY_OPTIMAL, the layout DisplayRegistry needs.

#include <chrono>

#include <rt_gpu.h>
#include <rt_job.h>
#include <rt_scene_types.h>
#include <vk_types.h>

class VulkanEngine;
class RaytraceSceneEditor;

enum class RaytraceBackend {
	Gpu,
	CpuLegacy
};

class RaytraceRenderer {
public:
	// after the tonemap pass and default data exist
	void init(VulkanEngine* engine);

	// once per frame from the main thread, before the imgui frame's content is built, so a CPU
	// render that finished since last frame is on screen this frame
	void update(VulkanEngine* engine);

	void drawPanel(VulkanEngine* engine, const RaytraceSceneEditor& editor);

	// inside draw()'s command buffer, before the raster passes: this frame's GPU path-tracing
	// work and its tonemap into the display image
	void record(VkCommandBuffer cmd, VulkanEngine* engine);

	// ends a running render early and keeps what it has. Also what destroying the environment
	// map calls, since a GPU render samples it
	void cancelRender();

	// joins the CPU worker and releases every GPU resource. Requires the device to be idle, and
	// must run before the display registry is torn down
	void shutdown(VulkanEngine* engine);

	bool* visibilityFlag() { return &m_showPanel; }

	// saved with the scene
	RenderSettings& settings() { return m_settings; }
	const RenderSettings& settings() const { return m_settings; }
	void setSettings(const RenderSettings& settings);

private:
	void startRender(VulkanEngine* engine, const RaytraceSceneEditor& editor);
	void ensureDisplayImage(VulkanEngine* engine, uint32_t width, uint32_t height);
	void destroyDisplayImage(VulkanEngine* engine);
	void publishCpuImage(VulkanEngine* engine);
	void drawSettings(VulkanEngine* engine);
	bool isRunning() const;

	RaytraceBackend m_backend { RaytraceBackend::Gpu };
	RenderSettings m_settings;
	// the Resolution combo's selection: 0 is "Match viewport", 1.. index RESOLUTION_PRESETS
	int m_resolutionChoice { DEFAULT_RESOLUTION_PRESET + 1 };

	RaytraceJob m_cpu;
	GpuPathTracer m_gpu;

	AllocatedImage m_displayImage {};
	bool m_hasDisplayImage { false };

	// what the CPU backend was started with, since the job itself does not keep settings
	int m_cpuRenderSamples { 0 };
	float m_cpuRenderExposure { 1.f };
	bool m_gpuWasRunning { false };
	std::chrono::steady_clock::time_point m_renderStart;

	// about the last render that finished or was stopped. Its backend also says who owns the
	// display image's contents
	float m_lastRenderMs { 0.f };
	int m_lastRenderSamples { 0 };
	RaytraceBackend m_lastRenderBackend { RaytraceBackend::CpuLegacy };
	bool m_hasLastRender { false };

	bool m_showPanel { true };
};
