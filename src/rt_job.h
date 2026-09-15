#pragma once

// The CPU raytracer backend: a worker thread rendering a RaytraceScene snapshot into a linear
// pixel buffer. Kept behind RaytraceRenderer's backend switch until the GPU path tracer is
// trusted, then deleted.
//
// Threading contract: the worker is handed a RaytraceScene by move at spawn time and touches
// nothing else - no VulkanEngine, no Vulkan handle, no live editor state. The main thread polls
// update() once per frame; when it reports the render finished, the pixel buffer is readable
// until the next start(). Nothing here touches Vulkan: the renderer that owns this uploads the
// finished buffer.

#include <atomic>
#include <thread>

#include <rt_scene.h>
#include <vk_types.h>

class RaytraceJob {
public:
	~RaytraceJob();

	void start(RaytraceScene&& scene);

	// asks the worker to stop at its next scanline; update() reports the join
	void cancel();

	// call once per frame from the main thread. Returns true exactly once per render, on the frame
	// the worker has been joined - completed() then says whether it ran to the end, and pixels()
	// holds the image if it did
	bool update();

	// joins an in-flight render. Call before the owner is destroyed
	void shutdown();

	bool isRunning() const { return m_running; }
	float progress() const { return m_progress.load(std::memory_order_relaxed); }

	// only meaningful after update() returned true and until the next start()
	bool completed() const { return m_completed; }
	float renderMs() const { return m_workerRenderMs; }
	const std::vector<glm::vec4>& pixels() const { return m_pixels; }
	uint32_t width() const { return m_pixelWidth; }
	uint32_t height() const { return m_pixelHeight; }

private:
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
	bool m_running { false };
};
