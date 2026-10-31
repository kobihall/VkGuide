// The render's sections of the "Stats" window (stats_window.h): what the render is doing and has done, what it costs,
// how fast it goes, what its paths do at each bounce, and the acceleration structure it traces. Part of
// RaytraceRenderer; apart from rt_renderer.cpp only to keep that file to the settings.

#include <rt_renderer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <implot.h>

#include <rt_scene_editor.h>
#include <stats_window.h>
#include <vk_engine.h>

namespace {

const ImVec4 WARNING_COLOR(1.f, 0.5f, 0.3f, 1.f);

// how far back the throughput plot's rates reach: long enough to smooth a frame's jitter, short enough to follow a change
constexpr float THROUGHPUT_WINDOW_SECONDS = 0.5f;

}

GpuMemoryUse RaytraceRenderer::memoryUse(VmaAllocator allocator) const
{
	//the display image is the film as the window shows it
	GpuMemoryUse use = m_gpu.memoryUse(allocator);
	use.film += allocationBytes(allocator, m_displayImage.allocation);
	return use;
}

void RaytraceRenderer::recordThroughput()
{
	//the rays of the last frame whose counters came back: the extend stage's over every bounce, and the shadow rays
	const TraversalWork& work = m_gpu.traversalWork();
	ThroughputPoint point;
	point.seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_renderStart).count();
	point.samples = m_gpu.samplesAccumulated();
	point.rays = (m_throughput.empty() ? 0.0 : m_throughput.back().rays) + (double)(work.rays + work.shadowRays);
	if (!m_throughput.empty() && point.samples == m_throughput.back().samples) {
		//a frame that took no sample traced nothing either
		point.rays = m_throughput.back().rays;
	}
	if (m_throughput.size() >= MAX_THROUGHPUT_POINTS) {
		//a long render keeps its whole curve at half the resolution
		size_t kept = 0;
		for (size_t i = 0; i < m_throughput.size(); i += 2) {
			m_throughput[kept++] = m_throughput[i];
		}
		m_throughput.resize(kept);
	}
	m_throughput.push_back(point);
}

void RaytraceRenderer::drawStats(VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
	const bool running = m_gpu.isRunning();
	char a[32];

	if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (running) {
			const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_renderStart).count();
			if (m_gpu.maxSamples() > 0) {
				ImGui::Text("Rendering: %u of %u samples, %.1f s", m_gpu.samplesAccumulated(), m_gpu.maxSamples(), seconds);
			} else {
				ImGui::Text("Rendering: %u samples, %.1f s", m_gpu.samplesAccumulated(), seconds);
			}
		} else if (m_hasLastRender) {
			ImGui::Text("Last render: %.0f ms, %d sample(s) a pixel", m_lastRenderMs, m_lastRenderSamples);
		} else {
			ImGui::TextDisabled("No render yet");
		}

		const RenderEstimate cost = estimate(engine, editor);
		if (beginStatsTable("##render")) {
			if (m_hasLastRender && m_gpu.hasTimestamps() && !running) {
				statsRow("GPU time, last render", "%.0f ms", m_gpu.totalGpuMs());
			}
			if (running) {
				if (m_gpu.hasTimestamps()) {
					statsRow("GPU time a frame", "%.2f ms", m_gpu.lastFrameGpuMs());
				}
				statsRow("Samples a frame", "%u", m_gpu.samplesPerFrame());
			}
			if (m_sceneAccel != nullptr && m_sceneAccel->meshInstanceCount > 0) {
				statsRow("Triangles placed", "%zu", m_sceneAccel->placedTriangles);
			}
			statsRow("Cost per ray", "~%.0f steps", cost.costPerRay);
			ImGui::SetItemTooltip("The BVH's expected cost of one ray (SAH: node visits + primitive tests)");

			//the render guard's measure, as a share of its budget: past the end, Render asks first
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted("Work a frame");
			ImGui::SetItemTooltip("Cost per ray x pixels x samples a frame x the rays the integrator traces a sample, against the budget\n"
				"past which a frame can trip the GPU watchdog. Render asks before it starts a render over it");
			ImGui::TableNextColumn();
			char overlay[64];
			snprintf(overlay, sizeof(overlay), "%.1e steps, %.0f%% of budget", cost.work, 100.0 * cost.work / WORK_PER_FRAME_BUDGET);
			if (cost.overBudget) {
				ImGui::PushStyleColor(ImGuiCol_PlotHistogram, WARNING_COLOR);
			}
			ImGui::ProgressBar((float)std::min(cost.work / WORK_PER_FRAME_BUDGET, 1.0), ImVec2(-1.f, 0.f), overlay);
			if (cost.overBudget) {
				ImGui::PopStyleColor();
			}

			//what the traversal did for the last frame traced, running or finished
			const TraversalWork& traversal = m_gpu.traversalWork();
			if (traversal.rays > 0) {
				statsRow("BVH work per ray", "%.1f nodes + %.1f prims", traversal.nodesPerRay(), traversal.primitivesPerRay());
				ImGui::SetItemTooltip("Counted by the extend stage over every bounce of the last frame. Unlike a timing it does not move with\n"
					"the GPU's clocks, so it compares builders fairly even on a thermally throttling laptop. A wide node tests\n"
					"8 boxes and a binary node 2, so compare node counts within a layout, and time across layouts");
				statsRow("  camera rays", "%.1f nodes + %.1f prims", traversal.primaryNodesPerRay(), traversal.primaryPrimitivesPerRay());
			}
			if (traversal.shadowRays > 0) {
				statsRow("Shadow ray work", "%.1f nodes + %.1f prims", traversal.shadowNodesPerRay(), traversal.shadowPrimitivesPerRay());
				ImGui::SetItemTooltip("Kernel 08's any-hit walks, which stop at the first thing in the way: usually cheaper than the closest-hit rays");
			}

			//kernel 09 leaves a NaN or infinite sample out of the mean rather than poison the pixel, which hides the bug
			//that made it
			if (m_gpu.hasImage() && m_gpu.droppedSamples() > 0) {
				const double taken = (double)m_gpu.samplesAccumulated() * (double)m_gpu.width() * (double)m_gpu.height();
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextColored(WARNING_COLOR, "Dropped samples");
				ImGui::SetItemTooltip("Kernel 09 leaves a sample out of the mean when any channel is NaN or infinite.\n"
					"Something produced such a value, and the image is biased by the samples it lost");
				ImGui::TableNextColumn();
				ImGui::TextColored(WARNING_COLOR, "%llu (%.3g%% of those taken)", (unsigned long long)m_gpu.droppedSamples(),
					taken > 0.0 ? 100.0 * (double)m_gpu.droppedSamples() / taken : 0.0);
			}
			endStatsTable();
		}

		//THROUGHPUT: samples a pixel and rays a second over the render, from the frames' counts. Wall clock, so a
		//render's rate is bounded by the display's refresh times the samples a frame
		if (m_throughput.size() >= 2) {
			std::vector<float> seconds;
			std::vector<float> samplesPerSecond;
			std::vector<float> megaraysPerSecond;
			size_t from = 0;
			for (size_t i = 1; i < m_throughput.size(); i++) {
				const ThroughputPoint& to = m_throughput[i];
				while (from + 1 < i && to.seconds - m_throughput[from + 1].seconds >= THROUGHPUT_WINDOW_SECONDS) {
					from++;
				}
				const float span = to.seconds - m_throughput[from].seconds;
				if (span <= 0.f) {
					continue;
				}
				seconds.push_back(to.seconds);
				samplesPerSecond.push_back((float)(to.samples - m_throughput[from].samples) / span);
				megaraysPerSecond.push_back((float)((to.rays - m_throughput[from].rays) / span / 1.0e6));
			}
			if (!seconds.empty() && ImPlot::BeginPlot("##throughput", ImVec2(-1.f, ImGui::GetFontSize() * 10.f), ImPlotFlags_NoMenus)) {
				ImPlot::SetupAxis(ImAxis_X1, "seconds into the render", ImPlotAxisFlags_AutoFit);
				ImPlot::SetupAxis(ImAxis_Y1, "samples/pixel/s", ImPlotAxisFlags_AutoFit);
				ImPlot::SetupAxis(ImAxis_Y2, "Mrays/s", ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_AuxDefault);
				ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0.0, HUGE_VAL);
				ImPlot::SetupAxisLimitsConstraints(ImAxis_Y2, 0.0, HUGE_VAL);
				ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
				ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
				ImPlot::PlotLine("samples/pixel/s", seconds.data(), samplesPerSecond.data(), (int)seconds.size());
				ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
				ImPlot::PlotLine("Mrays/s", seconds.data(), megaraysPerSecond.data(), (int)seconds.size());
				ImPlot::EndPlot();
			}
		}
	}

	//THE PER-BOUNCE COUNTS the schedule reads back from its queues, one bar chart each, a bar per bounce. The paths alive
	//have a bar more than the bounces, for the paths generate spawned; a list that is empty (no light to sample, no
	//complex material) is left out. Log scale: the camera rays outnumber a deep bounce's paths by orders of magnitude
	const std::span<const ScheduleCounter> counters = m_gpu.counters();
	std::vector<const ScheduleCounter*> shown;
	for (const ScheduleCounter& counter : counters) {
		if (!counter.values.empty()) {
			shown.push_back(&counter);
		}
	}
	if (!shown.empty() && ImGui::CollapsingHeader("Per bounce", ImGuiTreeNodeFlags_DefaultOpen)) {
		const int columns = ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 30.f ? 2 : 1;
		const int rows = ((int)shown.size() + columns - 1) / columns;
		if (ImPlot::BeginSubplots("##bounces", rows, columns, ImVec2(-1.f, ImGui::GetFontSize() * 9.f * (float)rows), ImPlotSubplotFlags_NoResize)) {
			for (size_t k = 0; k < shown.size(); k++) {
				const ScheduleCounter& counter = *shown[k];
				const int bins = (int)counter.values.size();
				//the section says per bounce, so the titles need not, and fit a narrow column
				std::string title = counter.label;
				if (const size_t at = title.find(" per bounce"); at != std::string::npos) {
					title.erase(at, std::strlen(" per bounce"));
				}
				if (ImPlot::BeginPlot(title.c_str(), ImVec2(), ImPlotFlags_NoMenus | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
					ImPlot::SetupAxes("bounce", nullptr);
					ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
					ImPlot::SetupAxisFormat(ImAxis_Y1, formatCount);
					//one bin per bounce, fixed: a bounce no path reached is an empty bin, not a narrower plot
					ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, (double)bins - 0.5, ImGuiCond_Always);
					//set, not fitted: a bar's base is 0, which a log axis fits a thousand decades below the data. From
					//under a count of 1 to a decade over the largest, so a bar of 1 still shows
					const uint32_t largest = *std::max_element(counter.values.begin(), counter.values.end());
					ImPlot::SetupAxisLimits(ImAxis_Y1, 0.5, std::max((double)largest, 1.0) * 10.0, ImGuiCond_Always);
					std::vector<double> ticks((size_t)bins);
					for (int i = 0; i < bins; i++) {
						ticks[(size_t)i] = (double)i;
					}
					ImPlot::SetupAxisTicks(ImAxis_X1, ticks.data(), bins);
					ImPlotSpec spec;
					spec.FillColor = ImPlot::GetColormapColor((int)k);
					spec.LineColor = ImPlot::GetColormapColor((int)k);
					ImPlot::PlotBars(counter.label, counter.values.data(), bins, 0.8, 0.0, spec);
					if (ImPlot::IsPlotHovered()) {
						const int bounce = (int)std::lround(ImPlot::GetPlotMousePos().x);
						if (bounce >= 0 && bounce < bins) {
							ImGui::SetTooltip("Bounce %d: %u", bounce, counter.values[(size_t)bounce]);
						}
					}
					ImPlot::EndPlot();
				}
			}
			ImPlot::EndSubplots();
		}
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("Read back from the queues each frame, the last frame's. Paths alive fall bounce by bounce, which shows the compaction works");
		ImGui::PopStyleColor();
	}

	//what the BVH builds produced, which moves only when a model loads, the BVH settings change or the scene is edited
	const bool hasBlas = m_blasSet != nullptr && !m_blasSet->blases.empty();
	if ((hasBlas || m_sceneAccel != nullptr) && ImGui::CollapsingHeader("Acceleration structure")) {
		if (ImGui::BeginTable("##accel", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.4f);
			ImGui::TableSetupColumn("BLAS", ImGuiTableColumnFlags_WidthStretch, 0.3f);
			ImGui::TableSetupColumn("TLAS", ImGuiTableColumnFlags_WidthStretch, 0.3f);
			ImGui::TableHeadersRow();
			const RaytraceBlasSet* set = hasBlas ? m_blasSet.get() : nullptr;
			const RaytraceSceneAccel* scene = m_sceneAccel.get();
			auto row = [](const char* label, const std::string& blas, const std::string& tlas, const char* tooltip = nullptr) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(label);
				if (tooltip != nullptr) {
					ImGui::SetItemTooltip("%s", tooltip);
				}
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(blas.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(tlas.c_str());
			};
			row("Holds", set != nullptr ? fmt::format("{} mesh(es)", set->blases.size()) : "",
				scene != nullptr ? fmt::format("{} inst. + {} shapes", scene->meshInstanceCount, scene->shapeCount - scene->unboundedCount) : "");
			row("Triangles", set != nullptr ? fmt::format("{}", set->triangles) : "", scene != nullptr ? fmt::format("{} placed", scene->placedTriangles) : "");
			row("Nodes", set != nullptr ? fmt::format("{}", set->nodes) : "", scene != nullptr ? fmt::format("{}", scene->tlas.bvh.nodeCount) : "");
			row("Depth", set != nullptr ? fmt::format("{}", set->maxDepth) : "", "");
			row("Memory", set != nullptr ? std::string(formatBytes(a, sizeof(a), set->bytes)) : "", "");
			row("Build time", set != nullptr ? fmt::format("{:.0f} ms", set->buildMs) : "", scene != nullptr ? fmt::format("{:.2f} ms", scene->tlas.buildMs) : "");
			row("SAH cost", set != nullptr ? fmt::format("{:.1f} mean", set->meanSahCost) : "", scene != nullptr ? fmt::format("{:.1f} a ray", scene->tlas.sceneSahCost) : "",
				"Expected node visits + primitive tests. The TLAS's is for a random ray through the whole scene: lower is faster.\n"
				"Compare builders and layouts by it and by the GPU time a frame. The \"BVH traversal cost\" debug view shows the real cost per pixel");
			if (set != nullptr && set->triangles > 0) {
				row("Spatial-split duplicates", fmt::format("{:.1f}%", 100.0 * ((double)set->triangleRefs / (double)set->triangles - 1.0)), "");
			}
			if (scene != nullptr && scene->unboundedCount > 0) {
				row("Infinite planes", "", fmt::format("{}, tested by every ray", scene->unboundedCount));
			}
			ImGui::EndTable();
		}
	}
}
