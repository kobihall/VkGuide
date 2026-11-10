#pragma once

// The "Stats" window: every live measurement of the app in one place, plotted where it moves frame to frame and listed
// where it does not, as the profiling overlays of Unreal (stat unit, stat unitgraph), Unity (the Stats and Profiler
// windows) and Godot (Monitors) present theirs. Sections, top to bottom:
//   - Frame: the frame time, the scene update, the raster pass's recording and GPU time and the path tracer's GPU time
//     over the last FRAME_HISTORY_SECONDS seconds on one plot
//   - Raster preview: whether it drew, its draw calls and triangles with their recent history, what occlusion skipped
//   - the render's (RaytraceRenderer::drawStats(), rt_renderer_stats.cpp): progress, cost, throughput over time, the
//     per-bounce counts and the acceleration structure
//   - GPU memory: VMA's per-heap use against its budget, and the renderer's share by owner
// Nothing here is a setting. The raster preview's settings are in the View menu.

#include <cstdint>
#include <vector>

#include <imgui.h>

class VulkanEngine;

// One frame as the Stats window plots it. Doubles throughout, so ImPlot reads every field with one stride; a time the
// frame did not have (no raster pass, no render) is NaN, which the plot leaves as a gap
struct FrameSample {
	// seconds since the app started
	double time { 0.0 };
	// the main loop's wall time: the CPU's whole frame, the wait for the display included
	double frameMs { 0.0 };
	double updateMs { 0.0 };
	// drawGeometry() recording the raster pass's commands, on the CPU
	double rasterRecordMs { 0.0 };
	// the raster pass on the GPU, background to encode (timestamps), FRAME_OVERLAP frames late
	double rasterGpuMs { 0.0 };
	// the path tracer's compute work on the GPU this frame
	double raytraceGpuMs { 0.0 };
	double drawCalls { 0.0 };
	double triangles { 0.0 };
	// the beam solver this frame, on the CPU: a frame with nothing to re-trace still compares the optics
	double beamSolveMs { 0.0 };
};

// the last CAPACITY frames in a ring, which ImPlot reads in place through its offset
class FrameHistory {
public:
	static constexpr int CAPACITY = 4096;

	void record(const FrameSample& sample);
	int size() const { return m_size; }
	// the oldest sample's index in data()
	int offset() const { return m_size < CAPACITY ? 0 : m_next; }
	const FrameSample* data() const { return m_samples.data(); }
	// the i-th sample, oldest first
	const FrameSample& at(int i) const { return m_samples[(offset() + i) % CAPACITY]; }
	const FrameSample* latest() const { return m_size > 0 ? &at(m_size - 1) : nullptr; }

private:
	std::vector<FrameSample> m_samples = std::vector<FrameSample>(CAPACITY);
	int m_size { 0 };
	int m_next { 0 };
};

// how much of the history the Frame plot and the sparklines show
inline constexpr double FRAME_HISTORY_SECONDS = 10.0;

// the window, registered by VulkanEngine::registerWindows()
void drawStatsWindow(VulkanEngine* engine, bool* open);

// A table of readouts, a label column and a value column, shared by every section
bool beginStatsTable(const char* id);
void statsRow(const char* label, const char* format, ...) IM_FMTARGS(2);
void endStatsTable();
// a count as an axis's tick label, 800k or 2M (an ImPlotFormatter)
int formatCount(double value, char* buffer, int size, void* data);
// bytes as KB, MB or GB, into a buffer of the caller's
const char* formatBytes(char* buffer, size_t size, uint64_t bytes);
