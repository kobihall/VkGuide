#pragma once

// Owns the raytrace worker thread, its output pixels, and the GPU image that output ends up in.
//
// Threading contract: the worker is handed a RaytraceScene by move at spawn time and touches
// nothing else - no VulkanEngine, no Vulkan handle, no live editor state. Everything that has
// to talk to Vulkan (uploading the finished buffer, registering it for display, destroying the
// previous one) happens on the main thread in update().

#include <atomic>
#include <thread>

#include <rt_scene.h>
#include <vk_types.h>

class VulkanEngine;
class RaytraceSceneEditor;

class RaytraceJob {
public:
	~RaytraceJob();

	// call once per frame from the main thread, before the imgui frame's content is built, so
	// a render that finished since last frame is registered in time to be drawn this frame
	void update(VulkanEngine* engine);

	// the render settings / Render / Cancel / progress window
	void drawControlPanel(VulkanEngine* engine, const RaytraceSceneEditor& editor);

	// cancels and joins an in-flight render, then releases the output image. Requires the
	// device to be idle, and must run before the display registry is torn down
	void shutdown(VulkanEngine* engine);

	bool* visibilityFlag() { return &m_showPanel; }

private:
	void start(RaytraceScene&& scene);
	void publishOutput(VulkanEngine* engine);
	void retirePendingImages(VulkanEngine* engine, bool force);

	std::thread m_thread;

	// shared with the worker while it runs, so atomic
	std::atomic<bool> m_finished { false };
	std::atomic<bool> m_cancelRequested { false };
	std::atomic<float> m_progress { 0.f };

	// written by the worker, and read by the main thread only after join(). The worker sets
	// m_finished last, after everything else it writes, and the join is what orders the two -
	// so nothing outside update() may read these while a render is running
	std::vector<glm::vec4> m_pixels; // linear radiance, rows top-down, alpha 1
	bool m_completed { false };
	float m_workerRenderMs { 0.f };

	// main thread only
	uint32_t m_pixelWidth { 0 };
	uint32_t m_pixelHeight { 0 };
	// duration of the last render that ran to completion, copied out of m_workerRenderMs after the join
	float m_lastRenderMs { 0.f };
	bool m_running { false };
	RenderSettings m_settings;
	int m_resolutionPreset { DEFAULT_RESOLUTION_PRESET };
	// keeps starting a fresh render as soon as the previous one finishes
	bool m_renderEveryFrame { false };
	AllocatedImage m_outputImage {};
	bool m_hasOutput { false };
	bool m_showPanel { true };

	// a replaced output image can still be referenced by command buffers that have not finished
	// executing, so destruction waits until they provably have - the same deferral, for the same
	// reason, that DisplayRegistry applies to its descriptor sets
	struct PendingImageDestroy {
		AllocatedImage image;
		uint64_t retireFrame;
	};
	std::vector<PendingImageDestroy> m_pendingDestroys;
};
