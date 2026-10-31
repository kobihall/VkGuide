#include <stats_window.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <vector>

#include <implot.h>

#include <vk_engine.h>

void FrameHistory::record(const FrameSample& sample)
{
	m_samples[m_next] = sample;
	m_next = (m_next + 1) % CAPACITY;
	m_size = std::min(m_size + 1, CAPACITY);
}

bool beginStatsTable(const char* id)
{
	if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
		return false;
	}
	ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
	ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
	return true;
}

void statsRow(const char* label, const char* format, ...)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(label);
	ImGui::TableNextColumn();
	va_list args;
	va_start(args, format);
	ImGui::TextV(format, args);
	va_end(args);
}

void endStatsTable()
{
	ImGui::EndTable();
}

const char* formatBytes(char* buffer, size_t size, uint64_t bytes)
{
	if (bytes >= (1ull << 30)) {
		snprintf(buffer, size, "%.2f GB", (double)bytes / (double)(1ull << 30));
	} else if (bytes >= (1ull << 20)) {
		snprintf(buffer, size, "%.1f MB", (double)bytes / (double)(1ull << 20));
	} else {
		snprintf(buffer, size, "%.0f KB", (double)bytes / 1024.0);
	}
	return buffer;
}

// a count as an axis shows it: 800k rather than 800000
int formatCount(double value, char* buffer, int size, void*)
{
	const double magnitude = std::fabs(value);
	if (magnitude >= 1.0e6) {
		return snprintf(buffer, size, "%gM", value / 1.0e6);
	}
	if (magnitude >= 1.0e3) {
		return snprintf(buffer, size, "%gk", value / 1.0e3);
	}
	return snprintf(buffer, size, "%g", value);
}

namespace {

// a time axis's labels as seconds before the newest sample, whose time is the user data
int formatSecondsAgo(double value, char* buffer, int size, void* data)
{
	//rounded to whole seconds here, so the newest tick reads 0 rather than -0
	const double ago = std::round(value - *(const double*)data);
	return snprintf(buffer, size, "%.0f s", ago == 0.0 ? 0.0 : ago);
}

// the mean of one field over the samples of the last `seconds`, NaNs (a frame without that work) left out; NaN when
// none had it
double recentMean(const FrameHistory& history, double FrameSample::*field, double seconds)
{
	const FrameSample* latest = history.latest();
	double sum = 0.0;
	int count = 0;
	for (int i = history.size() - 1; i >= 0; i--) {
		const FrameSample& sample = history.at(i);
		if (sample.time < latest->time - seconds) {
			break;
		}
		if (!std::isnan(sample.*field)) {
			sum += sample.*field;
			count++;
		}
	}
	return count > 0 ? sum / count : NAN;
}

// the Frame plot's series: one field each, named for what it measures and where it runs
struct FrameSeries {
	const char* name;
	double FrameSample::*field;
	const char* description;
};

constexpr FrameSeries FRAME_SERIES[] = {
	{ "Frame (CPU)", &FrameSample::frameMs, "The main loop's wall time: input, UI, recording and the wait for the display. Bounded below by the display's refresh" },
	{ "Scene update (CPU)", &FrameSample::updateMs, "updateScene(): the camera matrices and the raster draw list" },
	{ "Raster record (CPU)", &FrameSample::rasterRecordMs, "drawGeometry() recording the raster pass's commands. 0 while the preview is paused or hidden" },
	{ "Raster (GPU)", &FrameSample::rasterGpuMs, "The raster preview on the GPU, background to sRGB encode, from timestamp queries. A gap while it is paused or hidden" },
	{ "Raytrace compute (GPU)", &FrameSample::raytraceGpuMs, "The path tracer's compute kernels on the GPU this frame, from timestamp queries. A gap while no render runs" },
};

// the largest value of one field over the plot's span, NaN when no sample had it
double recentMax(const FrameHistory& history, double FrameSample::*field)
{
	const double from = history.latest()->time - FRAME_HISTORY_SECONDS;
	double peak = NAN;
	for (int i = history.size() - 1; i >= 0 && history.at(i).time >= from; i--) {
		const double value = history.at(i).*field;
		if (!std::isnan(value) && (std::isnan(peak) || value > peak)) {
			peak = value;
		}
	}
	return peak;
}

void drawFrameSection(VulkanEngine* engine)
{
	const FrameHistory& history = engine->m_frameHistory;
	const FrameSample* latest = history.latest();
	if (latest == nullptr) {
		return;
	}
	double now = latest->time;

	//the headline Unreal's stat unit leads with, averaged over a second so it can be read
	const double frameMs = recentMean(history, &FrameSample::frameMs, 1.0);
	ImGui::Text("%.0f fps, %.2f ms a frame", frameMs > 0.0 ? 1000.0 / frameMs : 0.0, frameMs);
	ImGui::SetItemTooltip("The mean of the last second");

	//which series the plot shows, toggled in the table below it
	static bool shown[std::size(FRAME_SERIES)] = { true, true, true, true, true };

	//the scale: the 98th percentile of what the plot shows, with room above it, and never below a 60 Hz frame. A spike (a
	//scene load, a pipeline built) clips at the top rather than flattening every other line; the table has its height
	std::vector<double> visible;
	for (int i = history.size() - 1; i >= 0 && history.at(i).time >= now - FRAME_HISTORY_SECONDS; i--) {
		for (int k = 0; k < (int)std::size(FRAME_SERIES); k++) {
			const double value = history.at(i).*FRAME_SERIES[k].field;
			if (shown[k] && !std::isnan(value)) {
				visible.push_back(value);
			}
		}
	}
	double top = 1000.0 / 60.0 * 1.25;
	if (!visible.empty()) {
		const size_t rank = (visible.size() - 1) * 98 / 100;
		std::nth_element(visible.begin(), visible.begin() + (ptrdiff_t)rank, visible.end());
		top = std::max(top, visible[rank] * 1.25);
	}

	if (ImPlot::BeginPlot("##frame", ImVec2(-1.f, ImGui::GetFontSize() * 11.f), ImPlotFlags_NoMenus | ImPlotFlags_NoLegend)) {
		ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoHighlight);
		ImPlot::SetupAxisLimits(ImAxis_X1, now - FRAME_HISTORY_SECONDS, now, ImGuiCond_Always);
		ImPlot::SetupAxisFormat(ImAxis_X1, formatSecondsAgo, &now);
		ImPlot::SetupAxis(ImAxis_Y1, "ms");
		ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, top, ImGuiCond_Always);

		//a 60 Hz frame, for scale
		{
			const double budget = 1000.0 / 60.0;
			ImPlotSpec spec;
			spec.LineColor = ImVec4(0.6f, 0.6f, 0.6f, 0.35f);
			spec.Flags = ImPlotItemFlags_NoFit | ImPlotInfLinesFlags_Horizontal;
			ImPlot::PlotInfLines("60 Hz", &budget, 1, spec);
			ImPlot::TagY(budget, ImVec4(0.4f, 0.4f, 0.4f, 1.f), "60 Hz");
		}

		const FrameSample* data = history.data();
		for (int i = 0; i < (int)std::size(FRAME_SERIES); i++) {
			if (!shown[i]) {
				continue;
			}
			const FrameSeries& series = FRAME_SERIES[i];
			ImPlotSpec spec;
			spec.LineColor = ImPlot::GetColormapColor(i);
			spec.Offset = history.offset();
			spec.Stride = sizeof(FrameSample);
			//a NaN, a frame without that work, is a gap: ImPlotLineFlags_SkipNaN would join the line across it
			ImPlot::PlotLine(series.name, &data->time, &(data->*series.field), history.size(), spec);
		}
		ImPlot::EndPlot();
	}

	//the legend as a table, Unreal's stat unit beside its graph: what each line is, its mean over the last second and its
	//peak over the plot's span. A table and not ImPlot's legend, which covers the lines or the plot in a narrow window
	if (ImGui::BeginTable("##frameSeries", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
		ImGui::TableSetupColumn("series", ImGuiTableColumnFlags_WidthStretch, 0.56f);
		ImGui::TableSetupColumn("mean, 1 s", ImGuiTableColumnFlags_WidthStretch, 0.22f);
		ImGui::TableSetupColumn("max", ImGuiTableColumnFlags_WidthStretch, 0.22f);
		ImGui::TableHeadersRow();
		for (int i = 0; i < (int)std::size(FRAME_SERIES); i++) {
			const FrameSeries& series = FRAME_SERIES[i];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(i);
			ImGui::PushStyleColor(ImGuiCol_CheckMark, ImPlot::GetColormapColor(i));
			ImGui::Checkbox(series.name, &shown[i]);
			ImGui::PopStyleColor();
			ImGui::SetItemTooltip("%s", series.description);
			ImGui::PopID();
			const double mean = recentMean(history, series.field, 1.0);
			const double peak = recentMax(history, series.field);
			ImGui::TableNextColumn();
			if (std::isnan(mean)) {
				ImGui::TextDisabled("-");
			} else {
				ImGui::Text("%.2f ms", mean);
			}
			ImGui::TableNextColumn();
			if (std::isnan(peak)) {
				ImGui::TextDisabled("-");
			} else {
				ImGui::Text("%.2f ms", peak);
			}
		}
		ImGui::EndTable();
	}
}

// one field's last FRAME_HISTORY_SECONDS as a line with no axes, the width of its table cell
void sparkline(const char* id, const FrameHistory& history, double FrameSample::*field, const ImVec4& color)
{
	const FrameSample* latest = history.latest();
	ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0.f, 0.f));
	if (ImPlot::BeginPlot(id, ImVec2(-1.f, ImGui::GetTextLineHeight()), ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs)) {
		ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit);
		ImPlot::SetupAxisLimits(ImAxis_X1, latest->time - FRAME_HISTORY_SECONDS, latest->time, ImGuiCond_Always);
		ImPlotSpec spec;
		spec.LineColor = color;
		spec.Offset = history.offset();
		spec.Stride = sizeof(FrameSample);
		ImPlot::PlotLine(id, &history.data()->time, &(history.data()->*field), history.size(), spec);
		ImPlot::EndPlot();
	}
	ImPlot::PopStyleVar();
}

void drawRasterSection(VulkanEngine* engine)
{
	const FrameHistory& history = engine->m_frameHistory;
	if (history.latest() == nullptr) {
		return;
	}
	const EngineStats& stats = engine->m_stats;
	if (!ImGui::BeginTable("##raster", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
		return;
	}
	ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.35f);
	ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.3f);
	ImGui::TableSetupColumn("history", ImGuiTableColumnFlags_WidthStretch, 0.35f);

	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted("Preview");
	ImGui::TableNextColumn();
	if (!engine->m_viewportVisible) {
		ImGui::TextDisabled("hidden");
	} else if (engine->m_rasterPaused) {
		ImGui::TextUnformatted("paused");
		ImGui::SetItemTooltip("Nothing it shows has changed, so the last picture is kept (View -> Pause Raster Preview)");
	} else {
		ImGui::TextUnformatted("drawing");
	}

	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted("Draw calls");
	ImGui::TableNextColumn();
	ImGui::Text("%d", stats.drawcallCount);
	ImGui::TableNextColumn();
	sparkline("##draws", history, &FrameSample::drawCalls, ImPlot::GetColormapColor(0));

	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted("Triangles");
	ImGui::TableNextColumn();
	ImGui::Text("%d", stats.triangleCount);
	ImGui::TableNextColumn();
	sparkline("##triangles", history, &FrameSample::triangles, ImPlot::GetColormapColor(1));

	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted("Occluded");
	ImGui::SetItemTooltip("Surfaces of %u or more triangles the occlusion queries found hidden, and skipped (View -> Occlusion Culling)", MeshDrawList::OCCLUSION_MIN_TRIANGLES);
	ImGui::TableNextColumn();
	ImGui::Text("%d (%llu triangles)", stats.occludedCount, (unsigned long long)stats.occludedTriangles);

	ImGui::EndTable();
}

void drawMemorySection(VulkanEngine* engine)
{
	char a[32];
	char b[32];

	//VMA's budget per heap: what this app has allocated against what the device lets it have
	const VkPhysicalDeviceMemoryProperties* properties = nullptr;
	vmaGetMemoryProperties(engine->m_memAllocator, &properties);
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(engine->m_memAllocator, budgets);
	uint64_t allocated = 0;
	for (uint32_t heap = 0; heap < properties->memoryHeapCount; heap++) {
		const VmaBudget& budget = budgets[heap];
		allocated += budget.statistics.allocationBytes;
		const bool deviceLocal = (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
		const float fraction = budget.budget > 0 ? (float)((double)budget.usage / (double)budget.budget) : 0.f;
		char overlay[96];
		snprintf(overlay, sizeof(overlay), "%s of %s", formatBytes(a, sizeof(a), budget.usage), formatBytes(b, sizeof(b), budget.budget));
		ImGui::Text("%s heap %u", deviceLocal ? "Device" : "Host", heap);
		ImGui::SameLine(ImGui::GetFontSize() * 7.f);
		ImGui::ProgressBar(std::min(fraction, 1.f), ImVec2(-1.f, 0.f), overlay);
		ImGui::SetItemTooltip("%s memory: %u allocation(s) in %u block(s). The budget is VMA's estimate of what the device allows this process",
			deviceLocal ? "Device-local" : "Host-visible", budget.statistics.allocationCount, budget.statistics.blockCount);
	}

	//the renderer's share by owner, and the rest: meshes, textures, the draw images and staging
	const GpuMemoryUse renderer = engine->m_raytracer.memoryUse(engine->m_memAllocator);
	const uint64_t environment = allocationBytes(engine->m_memAllocator, engine->m_environmentMap.allocation);
	const uint64_t drawImages = allocationBytes(engine->m_memAllocator, engine->m_drawImage.allocation) + allocationBytes(engine->m_memAllocator, engine->m_depthImage.allocation);
	const uint64_t known = renderer.film + renderer.outputs + renderer.wavefront + renderer.scene + renderer.other + environment + drawImages;
	const uint64_t rest = allocated > known ? allocated - known : 0;

	const char* labels[] = { "Render film", "Render outputs", "Wavefront pool", "Render scene", "Render other", "Environment map", "Draw images", "Meshes, textures, rest" };
	const double megabytes[] = { (double)renderer.film, (double)renderer.outputs, (double)renderer.wavefront, (double)renderer.scene, (double)renderer.other,
		(double)environment, (double)drawImages, (double)rest };
	constexpr int count = (int)std::size(labels);
	double values[count];
	double positions[count];
	for (int i = 0; i < count; i++) {
		values[i] = megabytes[i] / (double)(1ull << 20);
		positions[i] = (double)(count - 1 - i);
	}
	if (ImPlot::BeginPlot("##memory", ImVec2(-1.f, ImGui::GetFontSize() * 11.f), ImPlotFlags_NoMenus | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
		ImPlot::SetupAxis(ImAxis_X1, "MB", ImPlotAxisFlags_AutoFit);
		ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_AutoFit);
		ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, HUGE_VAL);
		ImPlot::SetupAxisTicks(ImAxis_Y1, positions, count, labels);
		ImPlotSpec spec;
		spec.Flags = ImPlotBarsFlags_Horizontal;
		ImPlot::PlotBars("bytes", values, positions, count, 0.67, spec);
		//the hovered bar's size, exactly
		if (ImPlot::IsPlotHovered()) {
			const int row = count - 1 - (int)std::lround(ImPlot::GetPlotMousePos().y);
			if (row >= 0 && row < count) {
				ImGui::SetTooltip("%s: %s", labels[row], formatBytes(a, sizeof(a), (uint64_t)megabytes[row]));
			}
		}
		ImPlot::EndPlot();
	}
	ImGui::TextDisabled("%s allocated in all", formatBytes(a, sizeof(a), allocated));
}

}

void drawStatsWindow(VulkanEngine* engine, bool* open)
{
	if (!*open) {
		return;
	}
	if (!ImGui::Begin("Stats", open)) {
		ImGui::End();
		return;
	}
	if (ImGui::CollapsingHeader("Frame", ImGuiTreeNodeFlags_DefaultOpen)) {
		drawFrameSection(engine);
	}
	if (ImGui::CollapsingHeader("Raster preview", ImGuiTreeNodeFlags_DefaultOpen)) {
		drawRasterSection(engine);
	}
	engine->m_raytracer.drawStats(engine, engine->m_raytraceScene);
	if (ImGui::CollapsingHeader("GPU memory")) {
		drawMemorySection(engine);
	}
	ImGui::End();
}
