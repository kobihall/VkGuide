#include <rt_renderer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include <imgui.h>
#include <implot.h>

#include <file_dialog.h>
#include <render_export.h>
#include <vk_readback.h>
#include <rt_scene.h>
#include <rt_schedule.h>
#include <rt_scene_editor.h>
#include <vk_engine.h>
#include <vk_images.h>
#include <vk_ui.h>
#include <vk_initializers.h>
#include <vk_tonemap.h>

namespace {

constexpr const char* OUTPUT_IMAGE_NAME = "Raytraced Output";

}

void RaytraceRenderer::init(VulkanEngine* engine)
{
	m_gpu.init(engine);
}

const SceneCamera* RaytraceRenderer::resolveCamera(const RaytraceSceneEditor& editor)
{
	const SceneCamera* camera = editor.findCamera(m_renderCameraId);
	if (camera == nullptr && !editor.cameras().empty()) {
		camera = &editor.cameras().front();
		m_renderCameraId = camera->id;
	}
	return camera;
}

RaytraceRenderer::RenderKey RaytraceRenderer::currentKey(VulkanEngine* engine, const RaytraceSceneEditor& editor, const SceneCamera& camera) const
{
	RenderKey key;
	key.camera = cameraSnapshot(camera);
	key.settings = m_settings;
	//toggling the watch itself is not a reason to start over
	key.settings.restartOnChange = false;
	//swapping a kernel changes the image (or at least the noise), so it restarts like a setting
	key.kernels = m_kernels;
	key.sceneRevision = editor.revision();
	key.modelRevision = engine->m_sceneRevision;
	key.accelRevision = m_accelRevision;
	key.environmentMapPath = engine->m_environmentMapPath;
	key.environmentIntensity = engine->m_environmentIntensity;
	key.environmentRotation = engine->m_environmentRotation;
	key.background = engine->m_backgroundMode;
	key.backgroundColor = engine->m_backgroundColor;
	key.width = m_settings.matchViewport ? engine->m_drawExtent.width : (uint32_t)m_settings.width;
	key.height = m_settings.matchViewport ? engine->m_drawExtent.height : (uint32_t)m_settings.height;
	key.lightGroupCount = (uint32_t)editor.lightGroupNames().size();
	key.environmentGroup = editor.environmentGroup();
	return key;
}

void RaytraceRenderer::update(VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
	//the render ends inside record(), by reaching maxSamples or by Stop; report it here
	if (m_wasRunning && !m_gpu.isRunning()) {
		m_lastRenderMs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_renderStart).count() / 1000.f;
		m_lastRenderSamples = (int)m_gpu.samplesAccumulated();
		m_hasLastRender = true;
		//Save when finished: a render that reached its sample target writes itself, a stopped one does not. Its last
		//frame was submitted at the end of the frame before this one, and the readback waits for the device
		const bool reachedTarget = m_gpu.maxSamples() > 0 && m_gpu.samplesAccumulated() >= m_gpu.maxSamples();
		if (m_exportSettings.saveWhenFinished && reachedTarget && !m_exportSettings.saveWhenFinishedPath.empty()) {
			m_exportIo = exportRender(engine, editor, m_exportSettings.saveWhenFinishedPath);
			fmt::println("RaytraceRenderer: save when finished: {}", m_exportIo.message);
		}
	}
	//the Render button's start, made here before any window is built: a new resolution recreates the display image,
	//which the image windows would otherwise have recorded this frame by the descriptor set being freed
	if (m_startRequested) {
		m_startRequested = false;
		startRender(engine, editor);
	}
	m_wasRunning = m_gpu.isRunning();
	if (m_gpu.isRunning()) {
		recordThroughput();
	}

	//restart on change: while a render is running and the camera moves, the scene or the
	//environment is edited, a setting changes or the viewport is resized under "Match viewport",
	//the render starts over. A finished or stopped render is left alone until the next Render
	if (m_gpu.isRunning() && m_settings.restartOnChange) {
		const SceneCamera* camera = resolveCamera(editor);
		if (camera != nullptr && !(currentKey(engine, editor, *camera) == m_renderKey)) {
			startRender(engine, editor);
		}
	}
}

void RaytraceRenderer::cancelRender()
{
	m_gpu.stop();
}

void RaytraceRenderer::setKernels(const KernelSelection& selection)
{
	m_kernels = selection;
	//the traversal is what decides how the BLASes are packed, so a selection that disagrees with the
	//built set is reconciled by the caller through requiredLayout() - see drawKernelPanel() and
	//openScene()
}

BvhLayout RaytraceRenderer::requiredLayout() const
{
	const TraversalVariant* traversal = m_kernels.selectedTraversal();
	return (traversal != nullptr && traversal->requiresLayout) ? traversal->layout : m_accelSettings.layout;
}

void RaytraceRenderer::setAccelSettings(VulkanEngine* engine, const AccelSettings& settings)
{
	AccelSettings applied = settings;
	//the selected traversal has the final say on the layout: a CWBVH kernel cannot read binary
	//nodes, so a scene file that disagrees is corrected rather than allowed to render garbage
	applied.layout = requiredLayout();
	if (applied == m_accelSettings && m_blasSet != nullptr) {
		return;
	}
	m_accelSettings = applied;
	m_pendingAccelSettings = applied;
	if (engine != nullptr) {
		prepareGeometry(engine);
	}
}

void RaytraceRenderer::setSettings(const RenderSettings& settings)
{
	m_settings = settings;

	//the combo's selection follows the settings, not the other way round. The custom fields always
	//track the loaded size too, so switching to Custom starts from what is on screen
	m_customWidth = m_settings.width;
	m_customHeight = m_settings.height;
	if (m_settings.matchViewport) {
		m_resolutionChoice = RESOLUTION_VIEWPORT;
		return;
	}
	//a size that is not one of the presets is a custom one, whether it was typed in here or
	//came out of a scene file
	m_resolutionChoice = RESOLUTION_CUSTOM;
	for (int i = 0; i < (int)std::size(RESOLUTION_PRESETS); i++) {
		if (RESOLUTION_PRESETS[i].width == m_settings.width && RESOLUTION_PRESETS[i].height == m_settings.height) {
			m_resolutionChoice = i;
		}
	}
}

RaytraceRenderer::RenderEstimate RaytraceRenderer::estimate(VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
	//what the next render would cost, from the scene as it stands
	ensureSceneAccel(engine, editor);
	RenderSettings prospective = m_settings;
	if (prospective.matchViewport) {
		prospective.width = (int)engine->m_drawExtent.width;
		prospective.height = (int)engine->m_drawExtent.height;
	}
	RenderEstimate result;
	result.costPerRay = sceneCostPerRay();
	result.work = estimatedWorkPerFrame(prospective);
	result.overBudget = result.work > WORK_PER_FRAME_BUDGET;
	if (!result.overBudget) {
		//the acknowledgement is for one cost only; a scene back under the budget withdraws it
		m_acceptedHeavyRender = false;
	}
	result.blocked = result.overBudget && !(m_acceptedHeavyRender && result.work <= m_acceptedWork);
	return result;
}

void RaytraceRenderer::drawMenuBarControls(VulkanEngine* engine, RaytraceSceneEditor& editor)
{
	const bool running = m_gpu.isRunning();
	const SceneCamera* camera = resolveCamera(editor);
	const RenderEstimate cost = estimate(engine, editor);
	const ImVec4 warning(1.f, 0.5f, 0.3f, 1.f);

	//Render and Stop are one button of one width at the middle of the bar, so it stays put as the progress bar after it
	//comes and goes; the menus before it push it right only when the window is too narrow for both
	const ImGuiStyle& style = ImGui::GetStyle();
	const float buttonWidth = std::max(ImGui::CalcTextSize("Render").x, ImGui::CalcTextSize("Stop").x) + style.FramePadding.x * 2.f;
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - buttonWidth) * 0.5f));

	if (running) {
		if (ImGui::Button("Stop", ImVec2(buttonWidth, 0.f))) {
			cancelRender();
		}
	} else {
		ImGui::BeginDisabled(camera == nullptr);
		//over the budget the button warns, and asks before it starts: a frame that long does not fail as a slow render,
		//it trips the GPU watchdog and takes the display driver down with it
		if (cost.blocked) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.3f, 0.12f, 1.f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.38f, 0.15f, 1.f));
		}
		if (ImGui::Button("Render", ImVec2(buttonWidth, 0.f))) {
			if (cost.blocked) {
				ImGui::OpenPopup("##renderGuard");
			} else {
				m_startRequested = true;
			}
		}
		if (cost.blocked) {
			ImGui::PopStyleColor(2);
			ImGui::SetItemTooltip("Over the GPU watchdog budget: %.1e steps/frame against %.0e. Click to decide", cost.work, WORK_PER_FRAME_BUDGET);
		}
		ImGui::EndDisabled();
		if (camera == nullptr) {
			ImGui::SetItemTooltip("Add a camera in the Scene panel to render");
		}
	}

	if (ImGui::BeginPopup("##renderGuard")) {
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
		ImGui::PushStyleColor(ImGuiCol_Text, warning);
		ImGui::TextWrapped("Too heavy to render safely: ~%.0f steps per ray is %.1e steps per frame, over the %.0e budget.", cost.costPerRay, cost.work, WORK_PER_FRAME_BUDGET);
		ImGui::PopStyleColor();
		ImGui::TextWrapped("A frame this long can trip the GPU watchdog and freeze or crash the display. Lower the resolution or the samples per frame, or hide some objects.");
		ImGui::PopTextWrapPos();
		if (ImGui::Button("Render anyway")) {
			//the acknowledgement is for this cost only; making the scene heavier withdraws it
			m_acceptedHeavyRender = true;
			m_acceptedWork = cost.work;
			m_startRequested = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if (running) {
		ImGui::SameLine();
		const ImVec2 size(ImGui::GetFontSize() * 14.f, 0.f);
		if (m_gpu.maxSamples() > 0) {
			const float fraction = (float)m_gpu.samplesAccumulated() / (float)m_gpu.maxSamples();
			const std::string overlay = fmt::format("{} / {} samples", m_gpu.samplesAccumulated(), m_gpu.maxSamples());
			ImGui::ProgressBar(fraction, size, overlay.c_str());
		} else {
			//no end to reach: an indeterminate bar with the count
			const std::string overlay = fmt::format("{} samples", m_gpu.samplesAccumulated());
			ImGui::ProgressBar(-1.f * (float)ImGui::GetTime(), size, overlay.c_str());
		}
		if (ImGui::BeginItemTooltip()) {
			if (m_gpu.hasTimestamps()) {
				ImGui::Text("GPU %.2f ms/frame, %u sample(s)/frame", m_gpu.lastFrameGpuMs(), m_gpu.samplesPerFrame());
			} else {
				ImGui::Text("%u sample(s)/frame", m_gpu.samplesPerFrame());
			}
			ImGui::TextDisabled(m_settings.restartOnChange ? "Restarts when the camera, scene or settings change" : "Rendering a snapshot: changes apply to the next Render");
			ImGui::EndTooltip();
		}
	} else if (!m_blockedReason.empty()) {
		ImGui::SameLine();
		ImGui::TextColored(warning, "Refused");
		ImGui::SetItemTooltip("%s", m_blockedReason.c_str());
	}

	//kernel 09 leaves a NaN or infinite sample out of the mean rather than poison the pixel, which hides the bug that
	//made it: a mark here, running or finished, and the count in Stats
	if (m_gpu.hasImage() && m_gpu.droppedSamples() > 0) {
		ImGui::SameLine();
		ImGui::TextColored(warning, "(!)");
		ImGui::SetItemTooltip("%llu samples dropped as non-finite. Something produced a NaN or an infinity, and the image is biased by the samples it lost. Stats -> Render has the share", (unsigned long long)m_gpu.droppedSamples());
	}
}

void RaytraceRenderer::drawCameraCombo(const RaytraceSceneEditor& editor)
{
	//which scene camera to render from; a change restarts under restart-on-change like any other
	const SceneCamera* camera = resolveCamera(editor);
	if (ImGui::BeginCombo("Camera", camera != nullptr ? camera->name.c_str() : "(no camera in scene)")) {
		for (const SceneCamera& candidate : editor.cameras()) {
			ImGui::PushID((int)candidate.id);
			if (ImGui::Selectable(candidate.name.c_str(), candidate.id == m_renderCameraId)) {
				m_renderCameraId = candidate.id;
			}
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("The scene camera the render looks through. Its lens and exposure are in the Scene panel");
}

void RaytraceRenderer::drawSettingsPanel(VulkanEngine* engine, RaytraceSceneEditor& editor)
{
	if (!m_showSettingsPanel) {
		return;
	}
	if (!ImGui::Begin("Render Settings", &m_showSettingsPanel)) {
		ImGui::End();
		return;
	}

	drawIntegratorCombo();
	drawCameraCombo(editor);
	drawSettings(engine);
	//under the Analysis view, whose error and difference views compare with the same reference
	drawReference(engine);

	//a section for kernel 06's strategies only while the integrator runs kernel 06
	if (integratorRunsStage(m_kernels.integrator, KernelSlot::SurfaceScattering)) {
		ImGui::SeparatorText("Direct lighting");
		drawLightingSettings(engine);
		ImGui::Separator();
	}

	drawSamplerSettings(engine);
	drawSpectralSettings();
	drawFilmSettings(engine);
	drawOutputsSettings(engine, editor);
	drawExportSettings(engine, editor);
	drawAccelSettings(engine);
	drawIntersectionSettings();
	drawTextureSettings(engine);

	//what the scene's capabilities add, such as the optical bench's Optics section
	forEachEnabledCapability(engine->m_capabilities, [engine](const CapabilityInfo& capability) {
		if (capability.renderSettings != nullptr) {
			capability.renderSettings(engine);
		}
	});

	ImGui::End();
}

void RaytraceRenderer::drawSettings(VulkanEngine* engine)
{
	std::string resolutionLabel;
	if (m_resolutionChoice == RESOLUTION_VIEWPORT) {
		resolutionLabel = fmt::format("Match viewport ({} x {})", engine->m_drawExtent.width, engine->m_drawExtent.height);
	} else if (m_resolutionChoice == RESOLUTION_CUSTOM) {
		resolutionLabel = fmt::format("Custom ({} x {})", m_settings.width, m_settings.height);
	} else {
		resolutionLabel = RESOLUTION_PRESETS[m_resolutionChoice].label;
	}
	if (ImGui::BeginCombo("Resolution", resolutionLabel.c_str())) {
		if (ImGui::Selectable("Match viewport", m_resolutionChoice == RESOLUTION_VIEWPORT)) {
			m_resolutionChoice = RESOLUTION_VIEWPORT;
			m_settings.matchViewport = true;
		}
		for (int i = 0; i < (int)std::size(RESOLUTION_PRESETS); i++) {
			if (ImGui::Selectable(RESOLUTION_PRESETS[i].label, i == m_resolutionChoice)) {
				m_resolutionChoice = i;
				m_settings.matchViewport = false;
				m_settings.width = RESOLUTION_PRESETS[i].width;
				m_settings.height = RESOLUTION_PRESETS[i].height;
			}
		}
		//picking Custom keeps whatever size was showing, so the fields below open on it rather
		//than on some unrelated default
		if (ImGui::Selectable("Custom...", m_resolutionChoice == RESOLUTION_CUSTOM)) {
			m_resolutionChoice = RESOLUTION_CUSTOM;
			m_settings.matchViewport = false;
			m_customWidth = m_settings.width;
			m_customHeight = m_settings.height;
		}
		ImGui::EndCombo();
	}

	if (m_resolutionChoice == RESOLUTION_CUSTOM) {
		//InputInt commits on Enter or on leaving the field, not per keystroke, so the "1" typed on
		//the way to "1280" never becomes a resolution of its own (and never restarts the render)
		const ImGuiInputTextFlags commitFlags = ImGuiInputTextFlags_CharsDecimal;
		const float fieldWidth = ImGui::CalcItemWidth() * 0.5f - ImGui::GetStyle().ItemInnerSpacing.x;

		ImGui::SetNextItemWidth(fieldWidth);
		if (ImGui::InputInt("##customWidth", &m_customWidth, 0, 0, commitFlags)) {
			m_customWidth = std::clamp(m_customWidth, MIN_RENDER_DIMENSION, MAX_RENDER_DIMENSION);
			m_settings.width = m_customWidth;
		}
		ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::SetNextItemWidth(fieldWidth);
		if (ImGui::InputInt("Custom size", &m_customHeight, 0, 0, commitFlags)) {
			m_customHeight = std::clamp(m_customHeight, MIN_RENDER_DIMENSION, MAX_RENDER_DIMENSION);
			m_settings.height = m_customHeight;
		}
		ImGui::TextDisabled("Width x height, %d to %d", MIN_RENDER_DIMENSION, MAX_RENDER_DIMENSION);
	}

	ImGui::Checkbox("Anti-aliasing", &m_settings.antialiasing);
	if (!m_settings.antialiasing) {
		ImGui::SameLine();
		ImGui::TextDisabled("(1 un-jittered sample)");
	}

	ImGui::BeginDisabled(!m_settings.antialiasing);
	ImGui::Checkbox("Unlimited samples", &m_settings.unlimitedSamples);
	ImGui::BeginDisabled(m_settings.unlimitedSamples);
	ImGui::SliderInt("Max samples per pixel", &m_settings.maxSamples, 1, 4096, "%d", ImGuiSliderFlags_Logarithmic);
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	ImGui::SliderInt("Samples per frame", &m_settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	ImGui::SliderInt("Depth of ray bounces", &m_settings.rayDepth, 1, CRT_MAX_DEPTH);

	ImGui::Checkbox("Russian roulette", &m_settings.russianRoulette);
	ImGui::BeginDisabled(!m_settings.russianRoulette);
	ImGui::SliderInt("Min bounces before roulette", &m_settings.minBouncesBeforeRoulette, 0, CRT_MAX_DEPTH);
	ImGui::EndDisabled();

	ImGui::Checkbox("Fixed seed", &m_settings.useFixedSeed);
	if (m_settings.useFixedSeed) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.f);
		ImGui::InputScalar("Seed", ImGuiDataType_U32, &m_settings.seed);
	}

	ImGui::Checkbox("Restart on change", &m_settings.restartOnChange);

	if (ImGui::BeginCombo("Debug view", crtDebugViewName(m_gpu.debugView))) {
		for (uint32_t i = 0; i < (uint32_t)CrtDebugView::Count; i++) {
			const CrtDebugView view = (CrtDebugView)i;
			if (ImGui::Selectable(crtDebugViewName(view), view == m_gpu.debugView)) {
				m_gpu.debugView = view;
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Replaces shading inside the path tracer. Samples taken after a change show the view, and the earlier ones stay in the average: restart the render for a clean one.");
	}
	if (m_gpu.debugView == CrtDebugView::AmbientOcclusion) {
		//the ray length is read every frame: a longer one reaches farther, and 0 is a tenth of the scene's size
		ImGui::DragFloat("AO distance", &m_gpu.aoDistance, 0.01f, 0.f, 1000.f, m_gpu.aoDistance > 0.f ? "%.3f" : "auto");
		m_gpu.aoDistance = std::max(m_gpu.aoDistance, 0.f);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("How far an ambient occlusion ray travels before it counts as unoccluded, in scene units.\n0 is automatic: a tenth of the scene's size (%.3f here). Applies from the next sample.", m_gpu.autoAoDistance());
		}
	}

	//the measurements of the film: display only, so they switch live and work on a finished render
	if (ImGui::BeginCombo("Analysis view", crtAnalysisViewName(m_gpu.analysisView))) {
		for (uint32_t i = 0; i < (uint32_t)CrtAnalysisView::Count; i++) {
			const CrtAnalysisView view = (CrtAnalysisView)i;
			if (ImGui::Selectable(crtAnalysisViewName(view), view == m_gpu.analysisView)) {
				m_gpu.analysisView = view;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", crtAnalysisViewDescription(view));
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered() && m_gpu.analysisView != CrtAnalysisView::None) {
		ImGui::SetTooltip("%s", crtAnalysisViewDescription(m_gpu.analysisView));
	}
}

double RaytraceRenderer::sceneCostPerRay() const
{
	if (m_sceneAccel == nullptr) {
		return 1.0;
	}
	//WHICH traversal is selected decides what a ray costs, and the difference is four orders of
	//magnitude on a real model. A BVH ray costs the scene's SAH (tens of steps); a brute-force ray
	//costs every primitive in the scene. Reading this off the selected traversal rather than
	//assuming a BVH is what keeps the guard honest when it is swapped - the linear scan is exactly
	//the case the guard exists for
	const TraversalVariant* traversal = m_kernels.selectedTraversal();
	const TraversalCost cost = traversal != nullptr ? traversal->cost : TraversalCost::Acceleration;
	switch (cost) {
	case TraversalCost::AllPrimitives:
		//every triangle of every placed mesh, plus one test per shape instance
		return std::max((double)m_sceneAccel->placedTriangles + (double)m_sceneAccel->instances.size(), 1.0);
	case TraversalCost::None:
		return 1.0;
	case TraversalCost::Acceleration:
		break;
	}
	return std::max((double)m_sceneAccel->tlas.sceneSahCost, 1.0);
}

double RaytraceRenderer::estimatedWorkPerFrame(const RenderSettings& settings) const
{
	//the integrator knows how many rays a frame of it traces; the traversal what each costs
	const WorkEstimate estimate {
		.settings = settings,
		.costPerRay = sceneCostPerRay(),
		.kernels = m_kernels,
		.lights = m_sceneAccel != nullptr ? &m_sceneAccel->lights.header : nullptr,
	};
	return m_gpu.schedule(m_kernels.integrator).estimatedWorkPerFrame(estimate);
}

RenderSettings RaytraceRenderer::renderSettings(VulkanEngine* engine) const
{
	RenderSettings settings = m_settings;
	if (settings.matchViewport) {
		settings.width = (int)engine->m_drawExtent.width;
		settings.height = (int)engine->m_drawExtent.height;
	}
	settings.width = std::max(settings.width, 2);
	settings.height = std::max(settings.height, 2);
	if (!settings.antialiasing) {
		settings.maxSamples = 1;
		settings.unlimitedSamples = false;
	}
	settings.maxSamples = std::max(settings.maxSamples, 1);
	settings.rayDepth = std::clamp(settings.rayDepth, 1, CRT_MAX_DEPTH);
	settings.samplesPerFrame = std::clamp(settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	settings.spectral = clampSpectralSettings(settings.spectral);
	return settings;
}

void RaytraceRenderer::startRender(VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
	const SceneCamera* camera = resolveCamera(editor);
	if (camera == nullptr) {
		return;
	}

	//the settings as this render will use them
	const RenderSettings settings = renderSettings(engine);

	//the scene first: the budget check below needs its cost per ray
	ensureSceneAccel(engine, editor);
	const double costPerRay = sceneCostPerRay();

	//the one thing that must never be started by accident. Over the budget the frame runs long
	//enough to trip the GPU watchdog, and the failure mode is not a dropped frame - it is the
	//display driver being killed. Refused here rather than in the panel so that no caller can
	//reach it another way, and acknowledged only for the exact cost that was shown
	const double work = estimatedWorkPerFrame(settings);
	if (work > WORK_PER_FRAME_BUDGET && !(m_acceptedHeavyRender && work <= m_acceptedWork)) {
		m_blockedReason = fmt::format("~{:.0f} steps per ray at {}x{}x{} is {:.1e} per frame, over the {:.0e} budget. Lower the resolution or samples per frame, hide some objects, switch the traversal back to a BVH, or choose Render anyway",
			costPerRay, settings.width, settings.height, settings.samplesPerFrame, work, WORK_PER_FRAME_BUDGET);
		fmt::println("RaytraceRenderer: render refused - {}", m_blockedReason);
		m_gpu.stop();
		return;
	}

	//the BVH geometry is one buffer on the device and one for staging. Past what the device allocates at once or holds,
	//the allocation fails and the app aborts, so the render is refused: pbrt's landscape needs 4.2 GB on a 4 GB GPU whose
	//MoltenVK allocates at most 3 GB at once
	const VkDeviceSize alignment = std::max<VkDeviceSize>(engine->m_gpuProperties.limits.minStorageBufferOffsetAlignment, 16);
	const VkDeviceSize geometryBytes = GpuRenderer::geometryUploadBytes(m_sceneAccel != nullptr ? m_sceneAccel->geometry.get() : nullptr, alignment);
	if (geometryBytes > engine->m_maxAllocationBytes || geometryBytes > engine->m_deviceLocalBytes) {
		m_blockedReason = fmt::format("The scene's BVH geometry is {} MB, more than the GPU allocates in one buffer ({} MB) or holds ({} MB). Hide some objects or remove a model",
			geometryBytes >> 20, engine->m_maxAllocationBytes >> 20, engine->m_deviceLocalBytes >> 20);
		fmt::println("RaytraceRenderer: render refused - {}", m_blockedReason);
		m_gpu.stop();
		return;
	}
	m_blockedReason.clear();

	ensureDisplayImage(engine, (uint32_t)settings.width, (uint32_t)settings.height);

	GpuRenderSnapshot snapshot;
	snapshot.width = (uint32_t)settings.width;
	snapshot.height = (uint32_t)settings.height;
	snapshot.camera = cameraSnapshot(*camera);
	//shared, not copied: the tracer compares it by pointer to decide whether it needs re-uploading
	snapshot.scene = m_sceneAccel;
	snapshot.settings = settings;
	snapshot.kernels = m_kernels;
	snapshot.seed = settings.useFixedSeed ? settings.seed : (uint32_t)std::random_device {}();
	//the tables go with the light list that names the environment: both only when the map is the
	//background, which is what environmentLight() decides for both
	snapshot.environment = m_sceneAccelEnvironment.distribution;
	//what a missed ray sees, as the tracer's three sources: a solid color (the gradient's black too), the map or the sky
	snapshot.useEnvironmentMap = engine->environmentShown();
	snapshot.environmentIntensity = engine->m_environmentIntensity;
	snapshot.environmentMapScale = engine->m_environmentMapScale;
	snapshot.environmentRotation = engine->m_environmentRotation;
	snapshot.solidBackground = engine->m_backgroundMode != BackgroundMode::Sky && !engine->environmentShown();
	snapshot.backgroundColor = engine->m_backgroundMode == BackgroundMode::Solid ? engine->m_backgroundColor : glm::vec3(0.f);
	//the light groups the render keeps sums for: every group the scene names, while the setting is on
	snapshot.lightGroupCount = settings.lightGroups ? (uint32_t)editor.lightGroupNames().size() : 0u;
	snapshot.environmentGroup = editor.environmentGroup();

	keepConvergenceCurve();
	m_curveLabel = convergenceLabel();

	m_renderKey = currentKey(engine, editor, *camera);
	m_hasRender = true;
	m_renderStart = std::chrono::steady_clock::now();
	m_throughput.clear();
	m_gpu.start(engine, std::move(snapshot));
	m_wasRunning = true;
}

void RaytraceRenderer::record(VkCommandBuffer cmd, VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
	//timestamps and header readbacks from FRAME_OVERLAP frames back, whether or not a render runs
	m_gpu.beginFrame(engine);

	if (!m_hasDisplayImage || !(m_gpu.isRunning() || m_gpu.hasImage())) {
		return;
	}

	//the render camera's exposure, live: it is display-only, so it changes the picture without
	//restarting the render
	const SceneCamera* camera = editor.findCamera(m_renderCameraId);
	const float exposure = camera != nullptr ? std::max(camera->exposure, 0.f) : 1.f;
	//Relight's gains are the camera's, display only, read every frame whether or not the panel is open
	if (camera != nullptr) {
		m_gpu.relightGains = camera->relight;
	}

	vkutil::transition_image(cmd, m_displayImage.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	m_gpu.record(cmd, engine, m_displayImage, exposure);
	vkutil::transition_image(cmd, m_displayImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void RaytraceRenderer::prepareGeometry(VulkanEngine* engine)
{
	if (engine->m_raytraceMeshData == nullptr) {
		m_blasCache.clear();
		m_blasSet.reset();
		m_geometry.reset();
	} else {
		m_blasSet = m_blasCache.build(*engine->m_raytraceMeshData, m_accelSettings);
		m_geometry = std::make_shared<const RaytraceGeometry>(packGeometry(*m_blasSet));
	}
	m_accelRevision++;
	//the instances name BLASes by their place in the set, so the top level has to follow
	m_hasSceneAccel = false;
}

void RaytraceRenderer::ensureSceneAccel(VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
	//a scene loaded before the renderer saw its models (or with BVH settings it has not built yet)
	if (m_blasSet == nullptr && engine->m_raytraceMeshData != nullptr) {
		prepareGeometry(engine);
	}

	//keyed by the editor's content revision, which a camera change does not move: a camera drag restarts the render
	//each frame, and must not rebuild the TLAS each time. The lights and the background are part of it: the light list
	//is built with the TLAS
	EnvironmentLight environment = environmentLight(engine);
	environment.group = editor.environmentGroup();
	//the working space and the design wavelength decide exact RGB's colors and indices in the records, and the spectral
	//builds' uplift table the spectra a material derives from RGB (a conductor's index)
	SpectralSceneContext spectral;
	spectral.spectra.space = m_settings.spectral.colorSpace;
	if (m_settings.spectral.representation != SpectralRepresentation::ExactRgb) {
		spectral.spectra.table = upliftTable(engine->m_rootPath, m_settings.spectral.colorSpace);
	}
	spectral.designWavelength = m_settings.spectral.designWavelength;
	//and what the optical bodies' coating tables are packed for (docs/plans/completed/physoptics-optical-materials.md)
	spectral.exactRgb = m_settings.spectral.representation == SpectralRepresentation::ExactRgb;
	spectral.lambdaMin = m_settings.spectral.lambdaMin;
	spectral.lambdaMax = m_settings.spectral.lambdaMax;
	spectral.monochromatic = m_settings.spectral.representation == SpectralRepresentation::Monochromatic ? m_settings.spectral.monochromaticWavelength : 0.f;
	spectral.surrounding = m_settings.optics.surrounding;
	spectral.libraryRevision = engine->m_opticalLibrary.revision();
	//and what the texture library holds, and whether models read their own textures from it
	SceneTextureContext textureContext;
	textureContext.library = &engine->m_textureLibrary;
	textureContext.modelTexturesFromLibrary = m_settings.textureStorage == TextureStorage::Native;
	textureContext.revision = engine->m_textureLibrary.revision();
	const bool sameBuild = m_hasSceneAccel && m_sceneAccelModelRevision == engine->m_sceneRevision && m_sceneAccelRevision == m_accelRevision
		&& m_sceneAccelEnvironment == environment && m_sceneAccelSpectral == spectral && m_sceneAccelTextures == textureContext;
	if (sameBuild && m_sceneAccelContentRevision == editor.contentRevision()) {
		return;
	}
	//an edit happened, but one that leaves the scene as it was (a slider let go where it started) still rebuilds
	//nothing. Compared by value only here, since a scene of tens of thousands of objects takes milliseconds to compare
	static const RaytraceMeshData noMeshes;
	const RaytraceMeshData& meshData = engine->m_raytraceMeshData != nullptr ? *engine->m_raytraceMeshData : noMeshes;
	//the bench's breadboard, posts and mounts (docs/plans/completed/physoptics-bench-and-mounts.md), from each optic's stack
	BenchTrace bench = engine->m_benchScene.traceInput(editor, meshData);
	if (sameBuild && m_sceneAccelObjects == editor.meshObjects() && m_sceneAccelShapes == editor.shapes() && m_sceneAccelLights == editor.lights()
		&& m_sceneAccelMaterials == editor.materials() && m_sceneAccelOptics == editor.optics() && m_sceneAccelBench == bench) {
		m_sceneAccelContentRevision = editor.contentRevision();
		return;
	}

	//which library entry each loaded glTF material became, for the surfaces shaded with their own
	ModelMaterialIds modelMaterialIds;
	for (const auto& [key, model] : engine->m_models) {
		if (model == nullptr) {
			continue;
		}
		for (uint32_t i = 0; i < model->materialsByIndex.size(); i++) {
			if (const MaterialDef* material = editor.findModelMaterial(key, i)) {
				modelMaterialIds[model->materialsByIndex[i].get()] = material->id;
			}
		}
	}
	m_sceneAccel = buildSceneAccel(m_blasSet, m_geometry, meshData, editor.meshObjects(), editor.shapes(), editor.lights(), editor.materials(), modelMaterialIds,
		environment, engine->m_raytraceTextures, spectral, editor.optics(), &engine->m_opticalLibrary, bench.empty() ? nullptr : &bench, textureContext);
	m_sceneAccelSpectral = spectral;
	m_sceneAccelTextures = textureContext;
	m_sceneAccelOptics = editor.optics();
	m_sceneAccelBench = std::move(bench);

	m_sceneAccelObjects = editor.meshObjects();
	m_sceneAccelShapes = editor.shapes();
	m_sceneAccelLights = editor.lights();
	m_sceneAccelMaterials = editor.materials();
	m_sceneAccelEnvironment = environment;
	m_sceneAccelModelRevision = engine->m_sceneRevision;
	m_sceneAccelRevision = m_accelRevision;
	m_sceneAccelContentRevision = editor.contentRevision();
	m_hasSceneAccel = true;
}

EnvironmentLight RaytraceRenderer::environmentLight(VulkanEngine* engine) const
{
	//a missed ray sees the map only when it is the background; the sky is not a light the list can sample, and is left
	//to BSDF samples at full weight
	EnvironmentLight light;
	if (engine->environmentShown()) {
		light.distribution = engine->m_environmentDistribution;
		light.intensity = engine->m_environmentIntensity;
	}
	return light;
}

void RaytraceRenderer::drawLightingSettings(VulkanEngine* engine)
{
	//kernel 06's variants ARE the strategies, so the combo is the registry's list for that slot,
	//exactly as the Raytracer Shaders window shows it
	const std::span<const KernelVariant> strategies = kernelVariants(KernelSlot::SurfaceScattering);
	const KernelVariant* selected = m_kernels.selected(KernelSlot::SurfaceScattering);
	if (ImGui::BeginCombo("Strategy", selected != nullptr ? selected->name : "none")) {
		for (uint32_t v = 0; v < (uint32_t)strategies.size(); v++) {
			if (ImGui::Selectable(strategies[v].name, m_kernels[KernelSlot::SurfaceScattering] == v)) {
				KernelSelection changed = m_kernels;
				changed[KernelSlot::SurfaceScattering] = v;
				setKernels(changed);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", strategies[v].description);
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("How direct light is found at each bounce: kernel 06 of the Raytracer Shaders window.\nEvery strategy converges to the same image; they differ in noise. Saved with the scene.");
	}

	//what there is to sample
	if (m_sceneAccel == nullptr) {
		return;
	}
	const RaytraceLightList& lights = m_sceneAccel->lights;
	if (lights.header.count == 0) {
		ImGui::TextDisabled("No light to sample: every light is found by BSDF samples");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Emissive shapes and triangles, punctual lights and an environment map are sampled.\nThe sky gradient, a solid background colour and the infinite plane are not.");
		}
		return;
	}
	std::string line = fmt::format("Light list: {} light(s) -", lights.header.count);
	if (lights.punctualLights > 0) {
		line += fmt::format(" {} punctual", lights.punctualLights);
	}
	if (lights.shapeLights > 0) {
		line += fmt::format(" {} shape surface(s)", lights.shapeLights);
	}
	if (lights.triangleLightCount > 0) {
		line += fmt::format(" {} triangle(s)", lights.triangleLightCount);
	}
	if (lights.environment) {
		line += " environment map";
	}
	ImGui::TextWrapped("%s", line.c_str());
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("One light is picked per shadow ray, in proportion to its power (pbrt-v4's power light sampler).");
	}
	(void)engine;
}

std::string RaytraceRenderer::convergenceLabel() const
{
	const KernelVariant* sampler = m_kernels.selected(KernelSlot::GenerateSamples);
	const KernelVariant* lighting = m_kernels.selected(KernelSlot::SurfaceScattering);
	return fmt::format("{} / {}", sampler != nullptr ? sampler->name : "?", lighting != nullptr ? lighting->name : "?");
}

void RaytraceRenderer::keepConvergenceCurve()
{
	//a render that got a few points against the reference is worth comparing with; one cut short is not
	const std::vector<ReferenceError>& points = m_gpu.convergence();
	if (m_gpu.hasReference() && points.size() >= 3 && !m_curveLabel.empty()) {
		m_curves.push_back({ m_curveLabel, points });
		constexpr size_t MAX_KEPT_CURVES = 8;
		if (m_curves.size() > MAX_KEPT_CURVES) {
			m_curves.erase(m_curves.begin());
		}
	}
}

IoResult RaytraceRenderer::saveReference(VulkanEngine* engine, const std::filesystem::path& path)
{
	Image image;
	if (!m_gpu.readReference(engine, image)) {
		return IoResult::failure("There is no reference to save");
	}
	const ImageIoStatus status = saveImage(path, image);
	return status.ok ? IoResult::success(status.message) : IoResult::failure(status.message);
}

IoResult RaytraceRenderer::loadReference(VulkanEngine* engine, const std::filesystem::path& path)
{
	Image image;
	const ImageIoStatus status = loadImage(path, image);
	if (!status.ok) {
		return IoResult::failure(status.message);
	}
	m_gpu.loadReference(engine, image);
	m_curves.clear();
	return IoResult::success(status.message + ", as the reference");
}

IoResult RaytraceRenderer::exportRender(VulkanEngine* engine, const RaytraceSceneEditor& editor, const std::filesystem::path& path)
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });

	//the display image as the window shows it, 8-bit: whatever the window shows, relit or an analysis view included
	if (extension == ".png") {
		if (!m_hasDisplayImage || !m_gpu.hasImage()) {
			return IoResult::failure("There is no render to save");
		}
		Image8 display;
		display.width = m_displayImage.imageExtent.width;
		display.height = m_displayImage.imageExtent.height;
		if (!readbackImage(engine, m_displayImage.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, m_displayImage.imageExtent, 1, 4, display.rgba)) {
			return IoResult::failure("There is no render to save");
		}
		const ImageIoStatus status = saveImage8(path, display);
		return status.ok ? IoResult::success(status.message) : IoResult::failure(status.message);
	}

	Image beauty;
	if (!m_gpu.readAccumulation(engine, beauty)) {
		return IoResult::failure("There is no render to save");
	}
	if (extension != ".exr") {
		//the picture alone
		const ImageIoStatus status = saveImage(path, beauty);
		return status.ok ? IoResult::success(status.message) : IoResult::failure(status.message);
	}

	RenderOutputsData outputs;
	if (!m_gpu.readOutputs(engine, outputs)) {
		return IoResult::failure("There is no render to save");
	}
	RenderExportInfo info;
	info.scenePath = engine->m_scenePath.string();
	{
		const KernelSelectionIds ids = kernelSelectionToIds(m_renderKey.kernels);
		info.kernels = fmt::format("integrator={} traversal={}", ids.integrator, ids.traversal);
		for (const auto& [slot, variant] : ids.stages) {
			info.kernels += fmt::format(" {}={}", slot, variant);
		}
	}
	const RenderSettings& r = m_renderKey.settings;
	info.settings = fmt::format(R"({{"width":{},"height":{},"samplesPerPixel":{},"rayDepth":{},"seed":{},"antialiasing":{},"pixelFilter":"{}","russianRoulette":{},"clampSamples":{},"clampValue":{}}})",
		beauty.width, beauty.height, outputs.samplesPerPixel, r.rayDepth, r.seed, r.antialiasing, pixelFilterId(r.pixelFilter.kind), r.russianRoulette, r.clampSamples, r.clampValue);
	info.renderSeconds = m_hasLastRender ? m_lastRenderMs / 1000.f : std::chrono::duration<float>(std::chrono::steady_clock::now() - m_renderStart).count();
	if (m_hasRender) {
		const RTCameraSnapshot& camera = m_renderKey.camera;
		info.cameraFromWorld = glm::lookAt(camera.lookFrom, camera.lookAt, camera.vUp);
		const float aspect = (float)beauty.width / (float)beauty.height;
		const glm::mat4 projection = glm::perspective(glm::radians(camera.vfovDegrees), aspect, 0.01f, 1000.f);
		//clip space to normalized device coordinates, x right and y down over [0, 1] as pbrt's NDC is
		const glm::mat4 toNdc(0.5f, 0.f, 0.f, 0.f, 0.f, -0.5f, 0.f, 0.f, 0.f, 0.f, 0.5f, 0.f, 0.5f, 0.5f, 0.5f, 1.f);
		info.ndcFromWorld = toNdc * projection * info.cameraFromWorld;
	}
	if (m_gpu.hasReference() && m_gpu.referenceExtent().width == beauty.width && m_gpu.referenceExtent().height == beauty.height && m_gpu.referenceError().valid) {
		info.hasMse = true;
		info.mse = (float)(m_gpu.referenceError().rmse * m_gpu.referenceError().rmse);
	}
	info.lightGroupNames = editor.lightGroupNames();

	const ChannelImage image = buildExportImage(beauty, outputs, info);
	const ImageIoStatus status = saveChannelImage(path, image, ExrOptions { m_exportSettings.halfFloat });
	return status.ok ? IoResult::success(status.message) : IoResult::failure(status.message);
}

namespace {

//the slope of log10(relative MSE) against log10(samples) over the points from `lo` to `hi` samples, by
//least squares on at most one point per eighth of an octave so the dense high end does not dominate.
//An unbiased estimator draws -1. False with fewer than three points in the window
bool convergenceSlope(const std::vector<ReferenceError>& points, double lo, double hi, double& slope)
{
	double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
	int n = 0;
	double lastBucket = -1e9;
	for (const ReferenceError& point : points) {
		if (point.samples < lo || point.samples > hi || point.relativeMse <= 0.0) {
			continue;
		}
		const double x = std::log10((double)point.samples);
		const double bucket = std::floor(std::log2((double)point.samples) * 8.0);
		if (bucket == lastBucket) {
			continue;
		}
		lastBucket = bucket;
		const double y = std::log10(point.relativeMse);
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
		n++;
	}
	if (n < 3) {
		return false;
	}
	const double denominator = n * sxx - sx * sx;
	if (std::abs(denominator) < 1e-12) {
		return false;
	}
	slope = (n * sxy - sx * sy) / denominator;
	return true;
}

//the range a curve's slope is fitted over: the last two octaves of its first 3/4 range, where the
//estimate has left its start-up and the reference's own noise has not yet bent it. For a curve that
//ends at N samples that is N/8 to N/2... the window is chosen by the caller
const ImU32 CURVE_COLORS[] = { IM_COL32(255, 200, 70, 255), IM_COL32(110, 200, 255, 255), IM_COL32(255, 120, 120, 255), IM_COL32(150, 230, 150, 255),
	IM_COL32(220, 150, 255, 255), IM_COL32(255, 160, 80, 255), IM_COL32(180, 180, 180, 255), IM_COL32(120, 255, 220, 255), IM_COL32(255, 130, 200, 255) };

}  // namespace

void RaytraceRenderer::drawReference(VulkanEngine* engine)
{
	if (!m_gpu.hasImage() && !m_gpu.hasReference()) {
		return;
	}
	if (!ImGui::CollapsingHeader("Compare to reference")) {
		return;
	}
	if (m_gpu.hasImage()) {
		if (ImGui::Button("Use this render as reference")) {
			m_gpu.captureReference();
			m_curves.clear();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Copies the render as it stands. Converge one strategy far (thousands of samples),\ncapture it, then render the others: each shows its error against it as it goes.");
		}
		ImGui::SameLine();
	}
	if (ImGui::Button("Load...")) {
		engine->m_pendingFileAction = VulkanEngine::FileAction::LoadReference;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Loads an .exr or .pfm file as the reference, such as a render from another renderer.\nIt is compared with renders of the same size, in linear radiance.");
	}
	drawIoResultLine(m_referenceIo);
	if (!m_gpu.hasReference()) {
		ImGui::TextDisabled("No reference yet");
		return;
	}
	if (ImGui::Button("Save...")) {
		engine->m_pendingFileAction = VulkanEngine::FileAction::SaveReference;
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear")) {
		m_gpu.clearReference(engine);
		m_curves.clear();
		return;
	}
	const VkExtent2D extent = m_gpu.referenceExtent();
	if (m_gpu.referenceSamples() > 0) {
		ImGui::Text("Reference: %u x %u, %u sample(s)/pixel", extent.width, extent.height, m_gpu.referenceSamples());
	} else {
		ImGui::Text("Reference: %u x %u, from a file", extent.width, extent.height);
	}
	if (extent.width != m_gpu.width() || extent.height != m_gpu.height()) {
		ImGui::TextDisabled("The render is %u x %u: not compared", m_gpu.width(), m_gpu.height());
		return;
	}
	const ReferenceError& error = m_gpu.referenceError();
	if (error.valid) {
		ImGui::Text("At %u sample(s): RMSE %.4g, relative MSE %.4g", error.samples, error.rmse, error.relativeMse);
		ImGui::Text("MAPE %.2f%%, mean difference %+.4g", error.mape * 100.0, error.meanDifference);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Against the reference, over every pixel and channel.\nRelative MSE divides each pixel's squared error by its reference value squared (+0.01), so dark regions count.\nMAPE is the mean of |difference| / max(|reference|, 0.01).\nThe mean difference is signed: a bias shows there, noise does not.\nMeaningful only for the same scene and camera the reference was rendered from.");
		}
	}

	//the convergence plot: relative MSE against samples, both logarithmic
	const std::vector<ReferenceError>& current = m_gpu.convergence();
	struct Drawn {
		const std::string* label;
		const std::vector<ReferenceError>* points;
	};
	std::vector<Drawn> drawn;
	for (const ConvergenceCurve& curve : m_curves) {
		drawn.push_back({ &curve.label, &curve.points });
	}
	const std::string currentLabel = m_curveLabel.empty() ? std::string("this render") : m_curveLabel;
	if (current.size() >= 2) {
		drawn.push_back({ &currentLabel, &current });
	}
	if (drawn.empty()) {
		return;
	}
	//the points a log axis can show, as the plot reads them
	struct Series {
		std::vector<double> samples;
		std::vector<double> error;
	};
	std::vector<Series> series(drawn.size());
	double minSamples = 1e30, maxSamples = 0.0;
	for (size_t k = 0; k < drawn.size(); k++) {
		for (const ReferenceError& point : *drawn[k].points) {
			if (point.relativeMse > 0.0 && point.samples > 0) {
				series[k].samples.push_back((double)point.samples);
				series[k].error.push_back(point.relativeMse);
				minSamples = std::min(minSamples, (double)point.samples);
				maxSamples = std::max(maxSamples, (double)point.samples);
			}
		}
	}
	if (maxSamples <= minSamples) {
		return;
	}
	if (ImPlot::BeginPlot("##convergence", ImVec2(-1.f, ImGui::GetFontSize() * 13.f))) {
		ImPlot::SetupAxes("samples per pixel", "relative MSE", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
		ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
		ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
		ImPlot::SetupLegend(ImPlotLocation_NorthEast);
		//the line of slope -1 an unbiased estimator draws, through the first curve's first point
		if (!series.front().samples.empty()) {
			const double scale = series.front().error.front() * series.front().samples.front();
			const double xs[2] = { minSamples, maxSamples };
			const double ys[2] = { scale / minSamples, scale / maxSamples };
			ImPlotSpec spec;
			spec.LineColor = ImVec4(0.6f, 0.6f, 0.6f, 0.6f);
			ImPlot::PlotLine("slope -1", xs, ys, 2, spec);
		}
		for (size_t k = 0; k < drawn.size(); k++) {
			ImPlotSpec spec;
			spec.LineColor = ImGui::ColorConvertU32ToFloat4(CURVE_COLORS[k % (sizeof(CURVE_COLORS) / sizeof(CURVE_COLORS[0]))]);
			spec.LineWeight = 1.5f;
			ImPlot::PlotLine(drawn[k].label->c_str(), series[k].samples.data(), series[k].error.data(), (int)series[k].samples.size(), spec);
		}
		ImPlot::EndPlot();
	}
	for (size_t k = 0; k < drawn.size(); k++) {
		const ImU32 color = CURVE_COLORS[k % (sizeof(CURVE_COLORS) / sizeof(CURVE_COLORS[0]))];
		const std::vector<ReferenceError>& points = *drawn[k].points;
		const double last = (double)points.back().samples;
		double slope = 0.0;
		//the window stops at a quarter of the reference's samples, where the reference's own noise
		//starts to bend the line, and spans two octaves below it
		const double top = m_gpu.referenceSamples() > 0 ? std::min(last, m_gpu.referenceSamples() / 4.0) : last;
		const bool fitted = convergenceSlope(points, std::max(top / 8.0, 4.0), top, slope);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
		if (fitted) {
			ImGui::Text("%s: slope %.2f over %.0f to %.0f spp", drawn[k].label->c_str(), slope, std::max(top / 8.0, 4.0), top);
		} else {
			ImGui::Text("%s: too few points for a slope", drawn[k].label->c_str());
		}
		ImGui::PopStyleColor();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Relative MSE against samples per pixel, log-log. An unbiased estimator falls with slope -1 (the grey line).\nA flatter line means the error stopped falling: accumulation precision (thousands of samples),\nor a bias, or the reference's own noise (past about a quarter of its samples).");
	}
}

bool RaytraceRenderer::drawIntegratorCombo()
{
	//every registered integrator; which stages it runs is the registry's, so the rest of the panels
	//follow the choice without knowing what it is
	bool changed = false;
	const IntegratorInfo& selected = m_kernels.selectedIntegrator();
	if (ImGui::BeginCombo("Integrator", selected.name)) {
		for (const IntegratorInfo& info : integrators()) {
			if (ImGui::Selectable(info.name, info.id == m_kernels.integrator)) {
				changed = info.id != m_kernels.integrator;
				m_kernels.integrator = info.id;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", info.description);
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s\nThe light transport algorithm. Saved with the scene.", selected.description);
	}
	return changed;
}

void RaytraceRenderer::drawKernelPanel(VulkanEngine* engine)
{
	if (!m_showKernelPanel) {
		return;
	}
	if (!ImGui::Begin("Raytracer Shaders", &m_showKernelPanel)) {
		ImGui::End();
		return;
	}

	ImGui::TextWrapped("The integrator, the traversal every kernel that traces is built for, then the integrator's "
		"stages in dispatch order. The numbering follows figure 15.2 of Physically Based Rendering 4ed; "
		"shaders/rt/NNVV_*.comp holds stage NN variant VV.");
	ImGui::Separator();

	const KernelSelection before = m_kernels;
	drawIntegratorCombo();

	//the traversal: not a stage but what every stage that traces walks
	const TraversalVariant* traversal = m_kernels.selectedTraversal();
	if (ImGui::BeginCombo("Traversal", traversal != nullptr ? traversal->name : "none")) {
		const std::span<const TraversalVariant> traversals = traversalVariants();
		for (uint32_t t = 0; t < (uint32_t)traversals.size(); t++) {
			if (ImGui::Selectable(traversals[t].name, m_kernels.traversal == t)) {
				m_kernels.traversal = t;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s\n\nshaders/rt/include/crt_trace.glsl: CRT_TRAVERSAL_%s", traversals[t].description, traversals[t].id);
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("How every kernel that traces searches the scene. Each such kernel is built once per traversal.");
	}
	if (traversal != nullptr) {
		ImGui::Indent();
		ImGui::TextDisabled("%s", traversal->description);
		ImGui::Unindent();
	}
	ImGui::Separator();

	//ONE LOOP FOR EVERY STAGE THE INTEGRATOR RUNS. Nothing here names a kernel or a strategy: the
	//rows, the combos and the greying-out all come from the registry, so a new variant appears in
	//this window as soon as it is registered, with no edit to this function
	const std::vector<KernelSlot>& stages = m_kernels.selectedIntegrator().stages;
	for (const KernelSlot slot : stages) {
		const std::span<const KernelVariant> variants = kernelVariants(slot);
		if (variants.empty()) {
			continue;
		}
		const KernelVariant* selected = m_kernels.selected(slot);
		const bool implemented = selected != nullptr && selected->implemented;

		ImGui::PushID((int)slot);
		ImGui::BeginDisabled(!implemented && variants.size() <= 1);

		const std::string label = fmt::format("{}  {}", kernelSlotNumber(slot), kernelSlotName(slot));
		if (ImGui::BeginCombo(label.c_str(), selected != nullptr ? selected->name : "none")) {
			for (uint32_t v = 0; v < (uint32_t)variants.size(); v++) {
				const bool isSelected = m_kernels[slot] == v;
				if (ImGui::Selectable(variants[v].name, isSelected)) {
					m_kernels[slot] = v;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s\n\nshaders/%s", variants[v].description, variants[v].shader);
				}
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();

		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", kernelSlotDescription(slot));
		}
		ImGui::Indent();
		if (!implemented) {
			ImGui::TextDisabled("not implemented - the kernel is skipped");
		} else if (selected->traces && traversal != nullptr) {
			ImGui::TextDisabled("%s Built for: %s.", selected->description, traversal->name);
		} else {
			ImGui::TextDisabled("%s", selected->description);
		}
		ImGui::Unindent();
		ImGui::PopID();
	}

	if (m_kernels != before) {
		//the traversal fixes the BLAS node layout, so selecting one may mean rebuilding every BLAS.
		//setAccelSettings() does nothing when the layout already matches, which is the common case
		//of swapping a stage's variant
		setAccelSettings(engine, m_accelSettings);
	}

	ImGui::Separator();
	if (ImGui::Button("Defaults")) {
		m_kernels = defaultKernelSelection();
		setAccelSettings(engine, m_accelSettings);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("saved with the scene");

	ImGui::End();
}

void RaytraceRenderer::drawAccelSettings(VulkanEngine* engine)
{
	//THE SECTION ONLY EXISTS WHILE A TRAVERSAL THAT USES IT IS SELECTED. Switching to the linear
	//scan makes every control below meaningless, so rather than grey them out the whole section goes
	//away - the traversal declares KernelSettings::AccelerationStructure and the panel reacts,
	//without the panel knowing which traversals those are
	const TraversalVariant* traversal = m_kernels.selectedTraversal();
	if (traversal == nullptr || traversal->settings != KernelSettings::AccelerationStructure) {
		return;
	}
	if (!ImGui::CollapsingHeader("Acceleration structure")) {
		return;
	}
	ImGui::TextDisabled("Traversal: %s", traversal->name);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The node layout follows the traversal selected in the Raytracer Shaders\nwindow, so there is no separate control for it here.");
	}

	AccelSettings& pending = m_pendingAccelSettings;
	BvhBuildOptions& options = pending.blas;

	if (ImGui::BeginCombo("Builder", bvhBuilderName(options.builder))) {
		for (uint32_t i = 0; i < (uint32_t)BvhBuilder::Count; i++) {
			if (ImGui::Selectable(bvhBuilderName((BvhBuilder)i), options.builder == (BvhBuilder)i)) {
				options.builder = (BvhBuilder)i;
			}
		}
		ImGui::EndCombo();
	}
	//the layout is not editable here: it is whatever the selected traversal can read
	pending.layout = requiredLayout();
	ImGui::LabelText("Node layout", "%s", bvhLayoutName(pending.layout));

	int maxLeafSize = (int)options.maxLeafSize;
	if (ImGui::SliderInt("Max leaf size", &maxLeafSize, 1, 16)) {
		options.maxLeafSize = (uint32_t)maxLeafSize;
	}
	if (pending.layout == BvhLayout::Cwbvh8 && options.maxLeafSize > BVH_CWBVH_MAX_LEAF_SIZE) {
		ImGui::SameLine();
		ImGui::TextDisabled("(CWBVH: %u)", BVH_CWBVH_MAX_LEAF_SIZE);
	}
	if (options.builder == BvhBuilder::BinnedSah || options.builder == BvhBuilder::SpatialSah) {
		int bins = (int)options.binCount;
		if (ImGui::SliderInt("Bins", &bins, 4, 256, "%d", ImGuiSliderFlags_Logarithmic)) {
			options.binCount = (uint32_t)bins;
		}
	}
	if (options.builder == BvhBuilder::SpatialSah) {
		ImGui::SliderFloat("Split budget", &options.spatialBudget, 0.f, 2.f, "%.2f x triangles");
		ImGui::SliderFloat("Overlap threshold", &options.spatialAlpha, 1e-7f, 1.f, "%.0e", ImGuiSliderFlags_Logarithmic);
	}
	if (options.builder != BvhBuilder::Midpoint) {
		ImGui::SliderFloat("Node cost", &options.traversalCost, 0.1f, 4.f, "%.2f");
		ImGui::SliderFloat("Triangle cost", &options.intersectionCost, 0.1f, 4.f, "%.2f");
	}

	const bool changed = !(pending == m_accelSettings);
	ImGui::BeginDisabled(!changed);
	if (ImGui::Button("Rebuild BVH")) {
		//every loaded mesh is rebuilt with the new settings, on this thread: a model's worth of BLASes
		//takes seconds at worst, and the frame waiting for it is the clearest signal it is working
		m_accelSettings = pending;
		prepareGeometry(engine);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Defaults")) {
		pending = defaultAccelSettings();
	}
	if (changed) {
		ImGui::SameLine();
		ImGui::TextDisabled("(not applied)");
	}

	//what the build produced is in Stats; a mesh the build could not trace is said here, beside the settings
	if (m_blasSet != nullptr) {
		for (const std::string& error : m_blasSet->errors) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.3f, 1.f));
			ImGui::TextWrapped("Not traced: %s", error.c_str());
			ImGui::PopStyleColor();
		}
	}
}

void RaytraceRenderer::drawSamplerSettings(VulkanEngine* engine)
{
	//kernel 01's variants ARE the samplers, so the combo is the registry's list for that slot, exactly
	//as the Raytracer Shaders window shows it
	if (!integratorRunsStage(m_kernels.integrator, KernelSlot::GenerateSamples)) {
		return;
	}
	if (!ImGui::CollapsingHeader("Sampler")) {
		return;
	}
	const std::span<const KernelVariant> samplers = kernelVariants(KernelSlot::GenerateSamples);
	const KernelVariant* selected = m_kernels.selected(KernelSlot::GenerateSamples);
	//"##variant": the section header above is also "Sampler", and the two would share an ID
	if (ImGui::BeginCombo("Sampler##variant", selected != nullptr ? selected->name : "none")) {
		for (uint32_t v = 0; v < (uint32_t)samplers.size(); v++) {
			if (ImGui::Selectable(samplers[v].name, m_kernels[KernelSlot::GenerateSamples] == v)) {
				KernelSelection changed = m_kernels;
				changed[KernelSlot::GenerateSamples] = v;
				setKernels(changed);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", samplers[v].description);
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Where each bounce's random numbers come from: kernel 01 of the Raytracer Shaders window.\nEvery sampler converges to the same image; the low-discrepancy ones get there with less noise.\nSaved with the scene. A new scene uses ZSobol, a scene saved before samplers existed PCG.");
	}
	selected = m_kernels.selected(KernelSlot::GenerateSamples);
	if (selected == nullptr) {
		return;
	}

	//the section's own settings exist only for the samplers that scramble their points
	if (selected->settings == KernelSettings::Sampler) {
		if (ImGui::BeginCombo("Randomization", samplerRandomizationName(m_settings.samplerRandomization))) {
			for (uint32_t r = 0; r < (uint32_t)SamplerRandomization::Count; r++) {
				const SamplerRandomization randomization = (SamplerRandomization)r;
				if (ImGui::Selectable(samplerRandomizationName(randomization), m_settings.samplerRandomization == randomization)) {
					m_settings.samplerRandomization = randomization;
				}
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("How the Sobol or Halton points are scrambled, from the render's seed. None keeps the bare sequence,\nwhich can alias; Owen scrambling is the most random and the slowest. Halton treats fast Owen as Owen.\nSaved with the scene.");
		}
	}

	//why the next render would run another sampler, from the settings as they are now: the decision
	//GpuRenderer::start() makes, so the two cannot disagree, and nothing left over from the last render
	const RenderSettings settings = renderSettings(engine);
	const std::string fallback = samplerFallbackReason(*selected, settings, (uint32_t)settings.width, (uint32_t)settings.height, m_gpu.samplerTablesLoaded());
	if (!fallback.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.3f, 1.f));
		ImGui::TextWrapped("%s", fallback.c_str());
		ImGui::PopStyleColor();
	}
	if (selected->writesRecord) {
		const double paths = (double)m_settings.width * (double)m_settings.height * (double)std::clamp(m_settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
		ImGui::TextDisabled("Sample record: %.0f MB", std::min(paths, (double)CRT_MAX_POOL) * SAMPLE_RECORD_BYTES / (1024.0 * 1024.0));
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Kernel 01 writes 48 bytes of random numbers per path, which kernels 00 and 06 read.\nPCG per bounce needs none.");
		}
	}
}

void RaytraceRenderer::drawSpectralSettings()
{
	if (!ImGui::CollapsingHeader("Spectral")) {
		return;
	}
	SpectralSettings& s = m_settings.spectral;
	auto tooltip = [](const char* text) {
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", text);
		}
	};
	const char* descriptions[] = {
		"R, G and B, as the renderer always transported. Bit-identical to a render made before spectral modes existed.",
		"One wavelength, every path. The film holds the spectral radiance at it, shown in gray, in the wavelength's color\nor through a colormap.",
		"Three bands with Gaussian sensitivities, fitted so sRGB colors come back close to themselves.",
		"N fixed bands over the range, box or Gaussian. Every path carries every band, so there is no wavelength noise.",
		"N wavelengths drawn per path (pbrt-v4's hero wavelengths), estimated against the CIE matching functions.\nUnbiased; dispersion follows one of them.",
	};
	if (ImGui::BeginCombo("Representation", spectralRepresentationName(s.representation))) {
		for (uint32_t r = 0; r < (uint32_t)SpectralRepresentation::Count; r++) {
			if (ImGui::Selectable(spectralRepresentationName((SpectralRepresentation)r), s.representation == (SpectralRepresentation)r)) {
				s.representation = (SpectralRepresentation)r;
			}
			tooltip(descriptions[r]);
		}
		ImGui::EndCombo();
	}
	tooltip("What a path carries. Saved with the scene; a new scene renders in exact RGB.");

	auto rangeNote = [&](float lo, float hi) {
		if (lo < SPECTRAL_LAMBDA_MIN || hi > SPECTRAL_LAMBDA_MAX) {
			ImGui::TextColored(ImVec4(1.f, 0.75f, 0.35f, 1.f), "RGB colors, lights and textures are defined from 360 to 830 nm only:\nbeyond that they hold their 360 or 830 nm values.");
		}
	};
	switch (s.representation) {
	case SpectralRepresentation::ExactRgb:
		break;
	case SpectralRepresentation::Monochromatic: {
		ImGui::DragFloat("Wavelength", &s.monochromaticWavelength, 0.5f, SPECTRAL_RANGE_FLOOR, SPECTRAL_RANGE_CEILING, "%.2f nm", ImGuiSliderFlags_Logarithmic);
		tooltip("The one wavelength every path carries.");
		ImGui::SameLine();
		if (ImGui::SmallButton("= design")) {
			s.monochromaticWavelength = s.designWavelength;
		}
		tooltip("Set it to the design wavelength below");
		rangeNote(s.monochromaticWavelength, s.monochromaticWavelength);
		SpectralDisplay& display = m_gpu.spectralDisplay;
		const char* modes[] = { "Grayscale", "Wavelength color", "Colormap" };
		int mode = (int)display.mode;
		if (ImGui::Combo("Display", &mode, modes, 3)) {
			display.mode = (MonochromeDisplay)mode;
		}
		tooltip("How the radiance at the wavelength is shown. Display only: it restarts nothing, and an EXR export holds the radiance.");
		if (display.mode == MonochromeDisplay::WavelengthColor) {
			const glm::vec3 c = wavelengthDisplayColor(s.monochromaticWavelength, display);
			ImGui::ColorButton("##wavelength", ImVec4(c.r, c.g, c.b, 1.f));
			ImGui::SameLine();
			ImGui::TextDisabled("the color of %.1f nm", s.monochromaticWavelength);
			if (ImGui::TreeNode("Colors outside the visible range")) {
				ImGui::ColorEdit3("Infrared", &display.infraredColor.x);
				ImGui::ColorEdit3("Ultraviolet", &display.ultravioletColor.x);
				ImGui::ColorEdit3("XUV (below 100 nm)", &display.xuvColor.x);
				ImGui::TreePop();
			}
		} else if (display.mode == MonochromeDisplay::Colormap) {
			const char* maps[] = { "viridis", "inferno", "turbo" };
			int map = (int)display.colormap;
			if (ImGui::Combo("Colormap", &map, maps, 3)) {
				display.colormap = (Colormap)map;
			}
			ImGui::Checkbox("Logarithmic", &display.logarithmic);
			if (display.logarithmic) {
				ImGui::SameLine();
				ImGui::SetNextItemWidth(80.f);
				ImGui::DragFloat("decades", &display.decades, 0.05f, 0.5f, 12.f, "%.1f");
			}
			tooltip("Exposure x radiance through the colormap: linear from 0 to 1, or logarithmic over this many decades below 1.");
		}
		break;
	}
	case SpectralRepresentation::RgbGaussians: {
		const char* names[] = { "Red band", "Green band", "Blue band" };
		for (int i = 0; i < 3; i++) {
			float values[2] = { s.gaussians[i].center, s.gaussians[i].fwhm };
			if (ImGui::DragFloat2(names[i], values, 0.5f, 5.f, 1000.f, "%.1f nm")) {
				s.gaussians[i] = { values[0], std::max(values[1], 5.f) };
			}
			tooltip("Center and full width at half maximum");
		}
		if (ImGui::SmallButton("Fitted bands")) {
			s.gaussians = DEFAULT_RGB_GAUSSIANS;
		}
		tooltip("The bands that bring sRGB colors back closest to themselves (bin/spectrum_test --fit-gaussians)");
		break;
	}
	case SpectralRepresentation::SpectralBands:
	case SpectralRepresentation::HeroWavelengths: {
		const uint32_t lanes[] = { 4, 8, 16, 32 };
		if (ImGui::BeginCombo(s.representation == SpectralRepresentation::SpectralBands ? "Bands" : "Wavelengths", std::to_string(s.lanes).c_str())) {
			for (const uint32_t n : lanes) {
				if (ImGui::Selectable(std::to_string(n).c_str(), s.lanes == n)) {
					s.lanes = n;
				}
			}
			ImGui::EndCombo();
		}
		tooltip("How many values a path carries: more lanes cost more per path and less noise (hero) or a finer spectrum (bands).\nEach count is its own build of the kernels, compiled the first time it is used.");
		float range[2] = { s.lambdaMin, s.lambdaMax };
		if (ImGui::DragFloat2("Range", range, 1.f, SPECTRAL_RANGE_FLOOR, SPECTRAL_RANGE_CEILING, "%.1f nm")) {
			s.lambdaMin = range[0];
			s.lambdaMax = range[1];
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Visible")) {
			s.lambdaMin = SPECTRAL_LAMBDA_MIN;
			s.lambdaMax = SPECTRAL_LAMBDA_MAX;
		}
		rangeNote(s.lambdaMin, s.lambdaMax);
		if (s.representation == SpectralRepresentation::SpectralBands) {
			int basis = (int)s.bandBasis;
			if (ImGui::Combo("Band basis", &basis, "Box\0Gaussian\0")) {
				s.bandBasis = (BandBasis)basis;
			}
			tooltip("Box bands average the spectrum over their width; Gaussian bands weigh it by a Gaussian that crosses its\nneighbours at half height. Either way a constant spectrum comes back exactly.");
		} else {
			int sampling = (int)s.sampling;
			if (ImGui::Combo("Sampling", &sampling, "Visible importance\0Uniform\0Emitters' spectra\0")) {
				s.sampling = (WavelengthSampling)sampling;
			}
			tooltip("How each path's wavelengths are drawn. Visible importance is pbrt's sech^2 density over 360 to 830 nm, and\nuniform over any other range. Emitters' spectra mixes in the scene's lights' spectra, so a narrow line\nsource is not mostly missed.");
			if (s.sampling == WavelengthSampling::Lights) {
				ImGui::SliderFloat("Emitters' share", &s.lightSamplingShare, 0.f, 0.99f, "%.2f");
			}
			if (s.sampling == WavelengthSampling::Visible && (s.lambdaMin != SPECTRAL_LAMBDA_MIN || s.lambdaMax != SPECTRAL_LAMBDA_MAX)) {
				ImGui::TextDisabled("visible importance needs 360 to 830 nm: this range samples uniformly");
			}
		}
		{
			bool images = s.imageBuckets > 0;
			if (ImGui::Checkbox("Spectral images", &images)) {
				s.imageBuckets = images ? 32u : 0u;
			}
			tooltip("Keep each pixel's mean spectral radiance in wavelength buckets over the range, beside the picture (pbrt's\nSpectralFilm). An EXR export writes them as S0.<wavelength>nm channels, the Spectral OpenEXR layout.\nThe bands are their own buckets; hero wavelengths fall into as many as you choose. Restarts the render.");
			if (images && s.representation == SpectralRepresentation::HeroWavelengths) {
				int buckets = (int)s.imageBuckets;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(90.f);
				if (ImGui::DragInt("buckets", &buckets, 1.f, 1, 256)) {
					s.imageBuckets = (uint32_t)std::clamp(buckets, 1, 256);
				}
			}
		}
		break;
	}
	case SpectralRepresentation::Count:
		break;
	}

	ImGui::Separator();
	if (ImGui::BeginCombo("RGB color space", colorSpace(s.colorSpace).name)) {
		for (uint32_t c = 0; c < (uint32_t)ColorSpaceId::Count; c++) {
			if (ImGui::Selectable(colorSpace((ColorSpaceId)c).name, s.colorSpace == (ColorSpaceId)c)) {
				s.colorSpace = (ColorSpaceId)c;
			}
		}
		ImGui::EndCombo();
	}
	tooltip("The space the scene's RGB values (material and light colors) are in. Exact RGB converts them to linear sRGB;\nthe spectral modes uplift them with that space's table. Textures and environment maps are sRGB whatever it is.");
	ImGui::DragFloat("Design wavelength", &s.designWavelength, 0.5f, SPECTRAL_RANGE_FLOOR, SPECTRAL_RANGE_CEILING, "%.2f nm", ImGuiSliderFlags_Logarithmic);
	tooltip("The wavelength exact RGB reads a dispersive glass's index at: 587.6 nm (the helium d line) by default, or a\nlaser's wavelength. Every spectral mode reads the index at the path's own wavelengths.");
	if (!m_gpu.spectralNote().empty()) {
		ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "%s", m_gpu.spectralNote().c_str());
	}
}

void RaytraceRenderer::drawFilmSettings(VulkanEngine* engine)
{
	//kernel 00's variants place a sample in its pixel and kernel 09's turn the samples into the pixel's
	//value, so the two combos are the registry's lists for those slots, as the Raytracer Shaders window shows them
	if (!integratorRunsStage(m_kernels.integrator, KernelSlot::GenerateCameraRays) || !integratorRunsStage(m_kernels.integrator, KernelSlot::UpdateFilm)) {
		return;
	}
	if (!ImGui::CollapsingHeader("Film")) {
		return;
	}
	auto variantCombo = [&](const char* label, KernelSlot slot, const char* tooltip) {
		const std::span<const KernelVariant> variants = kernelVariants(slot);
		const KernelVariant* current = m_kernels.selected(slot);
		if (ImGui::BeginCombo(label, current != nullptr ? current->name : "none")) {
			for (uint32_t v = 0; v < (uint32_t)variants.size(); v++) {
				if (ImGui::Selectable(variants[v].name, m_kernels[slot] == v)) {
					KernelSelection changed = m_kernels;
					changed[slot] = v;
					setKernels(changed);
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", variants[v].description);
				}
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", tooltip);
		}
	};

	variantCombo("Pixel sampling", KernelSlot::GenerateCameraRays,
		"Where in the image plane each sample goes: kernel 00 of the Raytracer Shaders window.\nFiltered pixels draw it from the pixel filter below (pbrt-v4's filter importance sampling), so a pixel\nis a weighted average of what lies around it; box pixels draw it uniformly inside the pixel.\nSaved with the scene. A new scene uses filtered pixels, a scene saved before filters existed box pixels.");
	const KernelVariant* camera = m_kernels.selected(KernelSlot::GenerateCameraRays);
	if (camera != nullptr && camera->settings == KernelSettings::PixelFilter) {
		PixelFilterSettings& filter = m_settings.pixelFilter;
		if (ImGui::BeginCombo("Pixel filter", pixelFilterName(filter.kind))) {
			for (uint32_t k = 0; k < (uint32_t)PixelFilterKind::Count; k++) {
				const PixelFilterKind kind = (PixelFilterKind)k;
				if (ImGui::Selectable(pixelFilterName(kind), filter.kind == kind) && filter.kind != kind) {
					//each filter has its own natural radius and parameters (pbrt-v4's defaults)
					filter = defaultPixelFilter(kind);
				}
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("The weighting a pixel gives the light around its centre. Box is the pixel itself and lets aliasing through;\nGaussian is pbrt's default; Mitchell and Lanczos sharpen and have negative lobes, which weigh some samples -1.\nSaved with the scene.");
		}
		ImGui::DragFloat("Radius", &filter.radius, 0.01f, MIN_FILTER_RADIUS, MAX_FILTER_RADIUS, "%.2f px");
		filter.radius = std::clamp(filter.radius, MIN_FILTER_RADIUS, MAX_FILTER_RADIUS);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("How far from the pixel's centre the filter reaches, in pixels. A wider filter blurs more.");
		}
		if (filter.kind == PixelFilterKind::Gaussian) {
			ImGui::DragFloat("Sigma", &filter.sigma, 0.005f, 0.05f, 4.f, "%.3f px");
			filter.sigma = std::clamp(filter.sigma, 0.05f, 4.f);
		} else if (filter.kind == PixelFilterKind::Mitchell) {
			ImGui::DragFloat("B", &filter.b, 0.005f, 0.f, 1.f, "%.3f");
			ImGui::DragFloat("C", &filter.c, 0.005f, 0.f, 1.f, "%.3f");
			filter.b = std::clamp(filter.b, 0.f, 1.f);
			filter.c = std::clamp(filter.c, 0.f, 1.f);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Mitchell and Netravali's two parameters: B blurs, C rings. B = C = 1/3 is their recommendation.");
			}
		} else if (filter.kind == PixelFilterKind::Lanczos) {
			ImGui::DragFloat("Tau", &filter.tau, 0.01f, 0.5f, 8.f, "%.2f");
			filter.tau = std::clamp(filter.tau, 0.5f, 8.f);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("The number of sinc lobes the window spans.");
			}
		}
		if (!m_settings.antialiasing) {
			ImGui::TextDisabled("Antialiasing is off: every sample is the centre of its pixel");
		}
	}

	//"##variant": the section header above is also "Film", and the two would share an ID
	variantCombo("Film##variant", KernelSlot::UpdateFilm,
		"How a pixel's samples become its value: kernel 09 of the Raytracer Shaders window.\nThe compensated mean keeps weighted float sums that stay accurate to high sample counts, a running\nvariance for the Standard error view, and the clamp; the running mean is the film scenes saved before used.\nSaved with the scene.");
	const KernelVariant* film = m_kernels.selected(KernelSlot::UpdateFilm);
	if (film != nullptr && film->settings == KernelSettings::Film) {
		ImGui::Checkbox("Clamp samples", &m_settings.clampSamples);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Scales a sample whose brightest channel exceeds the limit down to it, keeping its colour. It removes\nfireflies and the light they carried, so the image is darker than the converged one. Off by default. Saved with the scene.");
		}
		if (m_settings.clampSamples) {
			ImGui::DragFloat("Limit", &m_settings.clampValue, 0.05f, 0.001f, 1.0e6f, "%.2f", ImGuiSliderFlags_Logarithmic);
			m_settings.clampValue = std::max(m_settings.clampValue, 0.001f);
		}
	}
	if (film != nullptr && film->keepsFilmCascade) {
		ImGui::DragFloat("Outlier kappa", &m_settings.outlierKappa, 0.1f, 1.f, 1000.f, "%.1f", ImGuiSliderFlags_Logarithmic);
		m_settings.outlierKappa = std::clamp(m_settings.outlierKappa, 1.f, 1000.f);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("A sample of a brightness a pixel and its neighbors saw fewer than this many times is weighed down in\nproportion, and one nobody corroborates is dropped (Zirr et al. 2018). 1 stays closest to the true mean;\na larger value bounds the variance to 1/kappa of the squared mean, at the cost of the energy of rare bright\npaths. Keep it small against the samples per pixel: 4 suits a few hundred samples. Saved with the scene.");
		}
	}

	//why the next render would run another film, from the settings as they are now: the decision
	//GpuRenderer::start() makes, so the two cannot disagree
	const RenderSettings settings = renderSettings(engine);
	const FilmFallback fallback = filmFallback(m_kernels, settings, (uint32_t)settings.width, (uint32_t)settings.height);
	if (!fallback.reason.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.3f, 1.f));
		ImGui::TextWrapped("%s", fallback.reason.c_str());
		ImGui::PopStyleColor();
	} else if (film != nullptr && film->keepsFilmRecord) {
		const double bytesPerPixel = (double)sizeof(CrtFilmPixel) + (film->keepsFilmCascade ? (double)FILM_CASCADE_BYTES_PER_PIXEL : 0.0);
		ImGui::TextDisabled("Film record: %.0f MB", (double)settings.width * (double)settings.height * bytesPerPixel / (1024.0 * 1024.0));
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("48 bytes a pixel: the compensated weighted sums, the luminance's running variance and the samples accepted;\nthe outlier film adds 96 for its cascade of buffers by brightness.");
		}
	}
}

void RaytraceRenderer::drawIntersectionSettings()
{
	if (!ImGui::CollapsingHeader("Intersection precision")) {
		return;
	}
	ImGui::Checkbox("Error-bounded ray offsets", &m_settings.errorBoundedOffsets);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("A ray leaving a surface starts just past the rounding error of its hit point, so it cannot hit the\nsurface it left. Off, every ray skips the first 0.001 of its length instead: too much for small\nthings (a leaf 1 mm thick loses its back face), too little for large ones (a surface at 10 km\nshadows itself). Saved with the scene.");
	}
	ImGui::Checkbox("Watertight triangles", &m_settings.watertightTriangles);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("A triangle test that cannot let a ray through the edge or vertex two triangles share, and box\ntests widened so rounding cannot cull a ray grazing one. Off, Moller-Trumbore and plain box\ntests: a little faster, with rare pinholes along edges. Saved with the scene.");
	}
}

void RaytraceRenderer::drawTextureSettings(VulkanEngine* engine)
{
	if (!ImGui::CollapsingHeader("Textures")) {
		return;
	}
	int storage = (int)m_settings.textureStorage;
	const char* storages[] = { "Legacy array", "Native" };
	if (ImGui::Combo("Texture storage", &storage, storages, 2)) {
		m_settings.textureStorage = (TextureStorage)storage;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Where an imported model's own textures are read from. Native: the texture library, each texture at its\n"
			"own size with mip levels and its own sampler. Legacy array: every texture at 512 x 512 without mips, which\n"
			"reproduces renders from before the library bit for bit, and what a scene saved before it opens with.\n"
			"Textures you give a material, a shape's textures and light images always read the library. Saved with the scene.");
	}
	int filter = (int)m_settings.textureFilter;
	const char* filters[] = { "Point", "Bilinear", "Trilinear (ray cones)", "Anisotropic (ray cones)" };
	if (ImGui::Combo("Texture filtering", &filter, filters, 4)) {
		m_settings.textureFilter = (TextureFilter)filter;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("How a library texture is filtered. Point and bilinear read the finest level, so distant textures alias into\n"
			"noise that only more samples average out. Trilinear picks the level a ray cone's footprint covers, which\n"
			"carries through mirrors and glossy bounces; anisotropic samples along the footprint's long axis too.\n"
			"The alpha test always reads the finest level.");
	}
	//applied when the slider is let go: every texture is reloaded at the new size
	int maxSize = m_pendingMaxTextureSize > 0 ? m_pendingMaxTextureSize : m_settings.maxTextureSize;
	if (ImGui::SliderInt("Maximum texture size", &maxSize, 64, 16384, "%d", ImGuiSliderFlags_Logarithmic)) {
		m_pendingMaxTextureSize = maxSize;
	}
	if (ImGui::IsItemDeactivatedAfterEdit() && m_pendingMaxTextureSize > 0) {
		m_settings.maxTextureSize = m_pendingMaxTextureSize;
		m_pendingMaxTextureSize = 0;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("A larger image is stored at its first mip level within this many texels on its longer side. Changing it\n"
			"reloads every texture when you let go.");
	}
	const TextureLibrary& library = engine->m_textureLibrary;
	size_t failed = 0;
	for (const TextureEntry& entry : library.entries()) {
		failed += entry.error.empty() ? 0 : 1;
	}
	ImGui::TextDisabled("%zu texture(s) in the library, %.1f MB", library.entries().size(), (double)library.bytes() / (1024.0 * 1024.0));
	if (failed > 0) {
		ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "%zu texture(s) did not load; they read as white", failed);
	}
	//each texture's size and memory, its file's size where the cap shrank it
	if (!library.entries().empty() && ImGui::TreeNode("Each texture")) {
		for (const TextureEntry& entry : library.entries()) {
			if (!entry.error.empty()) {
				ImGui::TextDisabled("%s: %s", entry.name.c_str(), entry.error.c_str());
			} else if (entry.size != entry.sourceSize) {
				ImGui::Text("%s: %u x %u of %u x %u, %u levels, %.2f MB", entry.name.c_str(), entry.size.x, entry.size.y, entry.sourceSize.x, entry.sourceSize.y, entry.levels,
					(double)entry.bytes / (1024.0 * 1024.0));
			} else {
				ImGui::Text("%s: %u x %u, %u levels, %.2f MB", entry.name.c_str(), entry.size.x, entry.size.y, entry.levels, (double)entry.bytes / (1024.0 * 1024.0));
			}
		}
		ImGui::TreePop();
	}
}

void RaytraceRenderer::drawOutputsSettings(VulkanEngine* engine, RaytraceSceneEditor& editor)
{
	if (!ImGui::CollapsingHeader("Outputs")) {
		return;
	}
	//the size a render started now would have, for the memory each output takes
	const RenderSettings now = renderSettings(engine);
	const uint32_t width = (uint32_t)now.width;
	const uint32_t height = (uint32_t)now.height;

	ImGui::TextDisabled("What a render keeps beside its picture. Turning one on restarts the render.");
	for (uint32_t i = 0; i < (uint32_t)AovOutput::Count; i++) {
		const AovOutput output = (AovOutput)i;
		bool on = m_settings.aov.has(output);
		if (ImGui::Checkbox(aovOutputName(output), &on)) {
			m_settings.aov.set(output, on);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s\n\nEach one shows in its own window under Windows while the render keeps it.", aovOutputDescription(output));
		}
	}
	ImGui::BeginDisabled(!m_settings.aov.accumulates());
	ImGui::Checkbox("Capture through specular", &m_settings.aov.captureThroughSpecular);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("Record the first surface that is not a mirror or glass instead of the first, so a mirror or a window\nshows what it reflects. Denoisers expect that; pbrt records the first surface. Off by default.");
	}
	if (m_settings.aov.accumulates()) {
		const uint64_t bytes = aovImageBytes(m_settings.aov, width, height);
		ImGui::TextDisabled("Auxiliary outputs: %.0f MB at %u x %u", double(bytes) / (1024.0 * 1024.0), width, height);
	}

	ImGui::Separator();
	ImGui::Checkbox("Light groups", &m_settings.lightGroups);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Keep the light of each group apart, so a lamp can be dimmed or the sky tinted after the render (Relight below)\nand each group can be exported. A light is put in a group in the Scene panel, an emissive material in the\nMaterials window, and the background below. Turning it on restarts the render.");
	}
	if (m_settings.lightGroups) {
		editor.drawLightGroupEditor();
		const uint64_t bytes = lightGroupSumBytes((uint32_t)editor.lightGroupNames().size(), width, height);
		ImGui::TextDisabled("Light group sums: %.0f MB at %u x %u", double(bytes) / (1024.0 * 1024.0), width, height);
	}
	if (!m_gpu.outputsNote().empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.3f, 1.f));
		ImGui::TextWrapped("%s", m_gpu.outputsNote().c_str());
		ImGui::PopStyleColor();
	}

	//Relight: display only, like the exposure, and saved with the camera
	if (m_gpu.hasImage() && m_gpu.lightGroupCount() > 0) {
		ImGui::Separator();
		ImGui::Checkbox("Relight", &m_gpu.relight);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Show the picture the light groups make, each group's sum times its scale and tint, in place of the film.\nDisplay only: it needs no new samples, and the picture is exported as the film holds it.");
		}
		if (m_gpu.relight) {
			if (SceneCamera* camera = editor.findCamera(m_renderCameraId)) {
				for (uint32_t g = 0; g < m_gpu.lightGroupCount() && g < MAX_LIGHT_GROUPS && g < editor.lightGroupNames().size(); g++) {
					ImGui::PushID((int)g);
					ImGui::TextUnformatted(editor.lightGroupNames()[g].c_str());
					ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.6f);
					ImGui::SliderFloat("##scale", &camera->relight[g].scale, 0.f, 8.f, "scale %.2f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
					ImGui::SameLine();
					ImGui::ColorEdit3("##tint", &camera->relight[g].tint.x, ImGuiColorEditFlags_NoInputs);
					ImGui::PopID();
				}
				if (ImGui::SmallButton("Reset gains")) {
					camera->relight = SceneCamera {}.relight;
				}
			}
		}
	}
}

void RaytraceRenderer::drawExportSettings(VulkanEngine* engine, RaytraceSceneEditor& editor)
{
	if (!ImGui::CollapsingHeader("Export")) {
		return;
	}
	ImGui::Checkbox("Half floats in EXR", &m_exportSettings.halfFloat);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Store an EXR file's channels as 16-bit half floats: half the size, and a value above 65504 clamps.");
	}

	ImGui::Checkbox("Save when finished", &m_exportSettings.saveWhenFinished);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Write the render to this file when it reaches its sample target (not when it is stopped).\nThe extension picks the format: .exr with every output as named channels, .pfm, .hdr, or .png (the display image).");
	}
	char path[512];
	std::snprintf(path, sizeof(path), "%s", m_exportSettings.saveWhenFinishedPath.c_str());
	ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.75f);
	if (ImGui::InputText("##savePath", path, sizeof(path))) {
		m_exportSettings.saveWhenFinishedPath = path;
	}
	ImGui::SameLine();
	if (ImGui::Button("Browse...")) {
		const std::filesystem::path directory = engine->m_scenePath.empty() ? std::filesystem::path(engine->m_rootPath) / "assets" : engine->m_scenePath.parent_path();
		if (auto chosen = showSaveFileDialog("Save when finished", { "exr", "pfm", "hdr", "png" }, directory, "render.exr")) {
			m_exportSettings.saveWhenFinishedPath = chosen->string();
		}
	}
	if (m_exportSettings.saveWhenFinished && m_exportSettings.saveWhenFinishedPath.empty()) {
		ImGui::TextDisabled("Choose a file to save to");
	}

	ImGui::BeginDisabled(!m_gpu.hasImage());
	if (ImGui::Button("Export Render...")) {
		const std::filesystem::path directory = engine->m_scenePath.empty() ? std::filesystem::path(engine->m_rootPath) / "assets" : engine->m_scenePath.parent_path();
		if (auto chosen = showSaveFileDialog("Export Render", { "exr", "pfm", "hdr", "png" }, directory, "render.exr")) {
			m_exportIo = exportRender(engine, editor, *chosen);
		}
	}
	ImGui::EndDisabled();
	drawIoResultLine(m_exportIo);
}

void RaytraceRenderer::ensureDisplayImage(VulkanEngine* engine, uint32_t width, uint32_t height)
{
	if (m_hasDisplayImage && m_displayImage.imageExtent.width == width && m_displayImage.imageExtent.height == height) {
		return;
	}

	destroyDisplayImage(engine);

	const VkExtent3D extent { width, height, 1 };
	m_displayImage = engine->createImage(extent, TonemapPass::DISPLAY_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	m_hasDisplayImage = true;

	//black until the first tonemap writes it, and in the layout the registry reads it in
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_displayImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		const VkClearColorValue black { { 0.f, 0.f, 0.f, 1.f } };
		const VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
		vkCmdClearColorImage(cmd, m_displayImage.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
		vkutil::transition_image(cmd, m_displayImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	engine->m_displayRegistry.registerImage(OUTPUT_IMAGE_NAME, m_displayImage.imageView, { width, height });
}

void RaytraceRenderer::destroyDisplayImage(VulkanEngine* engine)
{
	if (!m_hasDisplayImage) {
		return;
	}

	//unregister first so no new frame references the view, then wait out the ones that already do
	engine->m_displayRegistry.unregisterImage(OUTPUT_IMAGE_NAME);
	vkDeviceWaitIdle(engine->m_device);
	engine->destroyImage(m_displayImage);
	m_displayImage = {};
	m_hasDisplayImage = false;
}

void RaytraceRenderer::shutdown(VulkanEngine* engine)
{
	m_gpu.stop();
	m_gpu.destroy(engine);

	if (m_hasDisplayImage) {
		engine->m_displayRegistry.unregisterImage(OUTPUT_IMAGE_NAME);
		engine->destroyImage(m_displayImage);
		m_displayImage = {};
		m_hasDisplayImage = false;
	}
}
