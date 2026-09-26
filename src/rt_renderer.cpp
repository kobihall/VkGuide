#include <rt_renderer.h>

#include <algorithm>
#include <random>

#include <imgui.h>

#include <rt_scene.h>
#include <rt_scene_editor.h>
#include <vk_engine.h>
#include <vk_images.h>
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
	key.solidBackground = engine->m_solidBackground;
	key.backgroundColor = engine->m_backgroundColor;
	key.width = m_settings.matchViewport ? engine->m_drawExtent.width : (uint32_t)m_settings.width;
	key.height = m_settings.matchViewport ? engine->m_drawExtent.height : (uint32_t)m_settings.height;
	return key;
}

void RaytraceRenderer::update(VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
	//the render ends inside record(), by reaching maxSamples or by Stop; report it here
	if (m_wasRunning && !m_gpu.isRunning()) {
		m_lastRenderMs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_renderStart).count() / 1000.f;
		m_lastRenderSamples = (int)m_gpu.samplesAccumulated();
		m_hasLastRender = true;
	}
	m_wasRunning = m_gpu.isRunning();

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
	//kernel 08 follows kernel 02, whatever the caller handed over
	m_kernels = reconcileKernelSelection(selection);
	//a kernel 02 variant that names a node layout is what decides how the BLASes are packed, so a
	//selection that disagrees with the built set is reconciled by the caller through
	//requiredLayout() - see drawKernelPanel() and openScene()
}

BvhLayout RaytraceRenderer::requiredLayout() const
{
	const KernelVariant* intersect = m_kernels.selected(KernelSlot::IntersectClosest);
	return (intersect != nullptr && intersect->requiresLayout) ? intersect->layout : m_accelSettings.layout;
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

void RaytraceRenderer::drawPanel(VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
	if (!m_showPanel) {
		return;
	}

	if (!ImGui::Begin("Raytrace Render", &m_showPanel)) {
		ImGui::End();
		return;
	}

	const bool running = m_gpu.isRunning();
	const SceneCamera* camera = resolveCamera(editor);

	//which scene camera to render from; a change restarts under restart-on-change like any other
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
	if (camera == nullptr) {
		ImGui::TextDisabled("Add a camera in the Scene panel to render");
	}

	if (m_hasLastRender) {
		ImGui::Text("Last render: %.0f ms, %d sample(s)/pixel", m_lastRenderMs, m_lastRenderSamples);
		if (m_gpu.hasTimestamps()) {
			ImGui::SameLine();
			ImGui::Text("(GPU %.1f ms)", m_gpu.totalGpuMs());
		}
	} else {
		ImGui::TextDisabled("No render yet");
	}

	//what the next render would cost, from the scene as it stands. Shown before the button rather
	//than after the refusal, so the number that decides the outcome is visible first
	ensureSceneAccel(engine, editor);
	RenderSettings prospective = m_settings;
	if (prospective.matchViewport) {
		prospective.width = (int)engine->m_drawExtent.width;
		prospective.height = (int)engine->m_drawExtent.height;
	}
	const double costPerRay = sceneCostPerRay();
	const double work = estimatedWorkPerFrame(prospective, costPerRay * tracesPerBounce());
	const bool overBudget = work > WORK_PER_FRAME_BUDGET;

	if (m_sceneAccel != nullptr && m_sceneAccel->meshInstanceCount > 0) {
		ImGui::Text("%zu triangle(s) placed, ~%.0f steps/ray, %.1e steps/frame", m_sceneAccel->placedTriangles, costPerRay, work);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("The BVH's expected cost of one ray (SAH: node visits + primitive tests),\ntimes pixels x samples/frame, doubled when kernel 06 also sends shadow rays.\nWhat the render guard below is judged on.");
		}
	}

	if (overBudget) {
		//a frame this long does not fail as a slow render - it trips the GPU watchdog and takes
		//the display driver down with it, so the button is closed until it is accepted explicitly
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.3f, 1.f));
		ImGui::TextWrapped("Too heavy to render safely: over the %.0e steps/frame budget. A frame this long can hang the GPU driver. Lower the resolution or samples per frame, or hide some objects.", WORK_PER_FRAME_BUDGET);
		ImGui::PopStyleColor();
		if (ImGui::Checkbox("Render anyway (may freeze or crash the display)", &m_acceptedHeavyRender)) {
			//the acknowledgement is for this cost only; making the scene heavier withdraws it
			m_acceptedWork = work;
		}
	} else {
		m_acceptedHeavyRender = false;
	}

	const bool blocked = overBudget && !(m_acceptedHeavyRender && work <= m_acceptedWork);

	ImGui::BeginDisabled(running || camera == nullptr || blocked);
	if (ImGui::Button("Render")) {
		startRender(engine, editor);
	}
	ImGui::EndDisabled();

	if (!m_blockedReason.empty() && !running) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.3f, 1.f));
		ImGui::TextWrapped("%s", m_blockedReason.c_str());
		ImGui::PopStyleColor();
	}

	if (running) {
		ImGui::SameLine();
		if (ImGui::Button("Stop")) {
			cancelRender();
		}

		if (m_gpu.maxSamples() > 0) {
			const float fraction = (float)m_gpu.samplesAccumulated() / (float)m_gpu.maxSamples();
			const std::string overlay = fmt::format("{} / {} samples", m_gpu.samplesAccumulated(), m_gpu.maxSamples());
			ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay.c_str());
		} else {
			//no end to reach: an indeterminate bar with the count
			const std::string overlay = fmt::format("{} samples", m_gpu.samplesAccumulated());
			ImGui::ProgressBar(-1.f * (float)ImGui::GetTime(), ImVec2(-FLT_MIN, 0), overlay.c_str());
		}
		if (m_gpu.hasTimestamps()) {
			ImGui::Text("GPU: %.2f ms/frame, %u sample(s)/frame", m_gpu.lastFrameGpuMs(), m_gpu.samplesPerFrame());
		} else {
			ImGui::Text("%u sample(s)/frame (no GPU timestamps on this device)", m_gpu.samplesPerFrame());
		}
	}

	//kernel 09 leaves a NaN or infinite sample out of the mean rather than poison the pixel, which
	//hides the bug that made it: say so, running or finished
	if (m_gpu.hasImage() && m_gpu.droppedSamples() > 0) {
		const double taken = (double)m_gpu.samplesAccumulated() * (double)m_gpu.width() * (double)m_gpu.height();
		const double percent = taken > 0.0 ? 100.0 * (double)m_gpu.droppedSamples() / taken : 0.0;
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.3f, 1.f));
		ImGui::TextWrapped("%llu samples dropped as non-finite (%.3g%% of those taken)", (unsigned long long)m_gpu.droppedSamples(), percent);
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Kernel 09 leaves a sample out of the mean when any channel is NaN or infinite.\nSomething produced such a value, and the image is biased by the samples it lost.");
		}
	}

	if (running) {
		if (m_settings.restartOnChange) {
			ImGui::TextDisabled("Restarts when the camera, scene or settings change");
		} else {
			ImGui::TextDisabled("Rendering a snapshot - changes apply to the next Render");
		}
	}

	//what the BVH did for the last frame traced, running or finished
	const TraversalWork& traversal = m_gpu.traversalWork();
	if (traversal.rays > 0) {
		ImGui::Text("BVH work per ray: %.1f nodes + %.1f primitives (camera rays %.1f + %.1f)", traversal.nodesPerRay(), traversal.primitivesPerRay(), traversal.primaryNodesPerRay(), traversal.primaryPrimitivesPerRay());
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Counted by the extend stage over every bounce of the last frame. Unlike a\ntiming it does not move with the GPU's clocks, so it compares builders fairly\neven on a thermally throttling laptop. A wide node tests 8 boxes and a binary\nnode 2, so compare node counts within a layout, and time across layouts.");
		}
	}
	if (traversal.shadowRays > 0) {
		ImGui::Text("Shadow ray work: %.1f nodes + %.1f primitives each", traversal.shadowNodesPerRay(), traversal.shadowPrimitivesPerRay());
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Kernel 08's any-hit walks, which stop at the first thing in the way:\nusually cheaper than the closest-hit rays above.");
		}
	}

	//the compaction readout: the counts must fall monotonically
	const std::vector<uint32_t>& alive = m_gpu.pathsAlivePerBounce();
	if (!alive.empty()) {
		std::string line = "Paths alive per bounce:";
		for (uint32_t count : alive) {
			line += fmt::format(" {}", count);
		}
		ImGui::TextWrapped("%s", line.c_str());
	}
	const std::vector<uint32_t>& shadows = m_gpu.shadowRaysPerBounce();
	if (traversal.shadowRays > 0) {
		std::string line = "Shadow rays per bounce:";
		for (uint32_t count : shadows) {
			line += fmt::format(" {}", count);
		}
		ImGui::TextWrapped("%s", line.c_str());
	}

	drawReference(engine);

	ImGui::SeparatorText("Direct lighting");
	drawLightingSettings(engine);

	ImGui::SeparatorText("Settings");
	drawSettings(engine);

	drawAccelSettings(engine);

	if (m_hasDisplayImage) {
		ImGui::SeparatorText("Output");
		ImGui::Text("%ux%u, shown in the \"%s\" window", m_displayImage.imageExtent.width, m_displayImage.imageExtent.height, OUTPUT_IMAGE_NAME);
		ImGui::TextDisabled("Lens and exposure are the camera's, in the Scene panel");
	}

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
}

double RaytraceRenderer::sceneCostPerRay() const
{
	if (m_sceneAccel == nullptr) {
		return 1.0;
	}
	//WHICH strategy kernel 02 is running decides what a ray costs, and the difference is four
	//orders of magnitude on a real model. A BVH ray costs the scene's SAH (tens of steps); a
	//brute-force ray costs every primitive in the scene. Reading this off the selected variant
	//rather than assuming a BVH is what keeps the guard honest when the traversal is swapped -
	//the linear scan is exactly the case the guard exists for
	const KernelVariant* intersect = m_kernels.selected(KernelSlot::IntersectClosest);
	const TraversalCost cost = intersect != nullptr ? intersect->cost : TraversalCost::Acceleration;
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

double RaytraceRenderer::tracesPerBounce() const
{
	//a shadow ray is a second walk through the scene, priced like the first: the any-hit early out
	//makes it cheaper, which leaves the guard erring on the safe side
	const KernelVariant* scatter = m_kernels.selected(KernelSlot::SurfaceScattering);
	if (scatter == nullptr || m_sceneAccel == nullptr) {
		return 1.0;
	}
	const GpuLightHeader& lights = m_sceneAccel->lights.header;
	switch (scatter->nextEvent) {
	case NextEvent::AllLights:
		return lights.count > 0 ? 2.0 : 1.0;
	case NextEvent::DeltaLights:
		return lights.deltaCount > 0 ? 2.0 : 1.0;
	case NextEvent::None:
		break;
	}
	return 1.0;
}

double RaytraceRenderer::estimatedWorkPerFrame(const RenderSettings& settings, double costPerRay)
{
	const double pixels = (double)std::max(settings.width, 2) * (double)std::max(settings.height, 2);
	const double samples = (double)std::clamp(settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	return pixels * samples * costPerRay;
}

void RaytraceRenderer::startRender(VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
	const SceneCamera* camera = resolveCamera(editor);
	if (camera == nullptr) {
		return;
	}

	//the settings as this render will use them
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

	//the scene first: the budget check below needs its cost per ray
	ensureSceneAccel(engine, editor);
	const double costPerRay = sceneCostPerRay();

	//the one thing that must never be started by accident. Over the budget the frame runs long
	//enough to trip the GPU watchdog, and the failure mode is not a dropped frame - it is the
	//display driver being killed. Refused here rather than in the panel so that no caller can
	//reach it another way, and acknowledged only for the exact cost that was shown
	const double work = estimatedWorkPerFrame(settings, costPerRay * tracesPerBounce());
	if (work > WORK_PER_FRAME_BUDGET && !(m_acceptedHeavyRender && work <= m_acceptedWork)) {
		m_blockedReason = fmt::format("~{:.0f} steps per ray at {}x{}x{} is {:.1e} per frame, over the {:.0e} budget. Lower the resolution or samples per frame, hide some objects, switch kernel 02 back to a BVH, or accept it below",
			costPerRay, settings.width, settings.height, settings.samplesPerFrame, work, WORK_PER_FRAME_BUDGET);
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
	snapshot.useEnvironmentMap = engine->m_environmentMap.image != VK_NULL_HANDLE;
	snapshot.environmentIntensity = engine->m_environmentIntensity;
	snapshot.environmentMapScale = engine->m_environmentMapScale;
	snapshot.solidBackground = engine->m_solidBackground;
	snapshot.backgroundColor = engine->m_backgroundColor;

	m_renderKey = currentKey(engine, editor, *camera);
	m_hasRender = true;
	m_renderStart = std::chrono::steady_clock::now();
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

	//compared by value rather than by the editor's revision, which also moves for every camera
	//change: a camera drag restarts the render each frame, and must not rebuild the TLAS each time.
	//The lights and the background are part of it: the light list is built with the TLAS
	const EnvironmentLight environment = environmentLight(engine);
	if (m_hasSceneAccel && m_sceneAccelModelRevision == engine->m_sceneRevision && m_sceneAccelRevision == m_accelRevision
		&& m_sceneAccelObjects == editor.meshObjects() && m_sceneAccelShapes == editor.shapes() && m_sceneAccelLights == editor.lights()
		&& m_sceneAccelEnvironment == environment) {
		return;
	}

	static const RaytraceMeshData noMeshes;
	const RaytraceMeshData& meshData = engine->m_raytraceMeshData != nullptr ? *engine->m_raytraceMeshData : noMeshes;
	m_sceneAccel = buildSceneAccel(m_blasSet, m_geometry, meshData, editor.meshObjects(), editor.shapes(), editor.lights(), environment, engine->m_raytraceTextures);

	m_sceneAccelObjects = editor.meshObjects();
	m_sceneAccelShapes = editor.shapes();
	m_sceneAccelLights = editor.lights();
	m_sceneAccelEnvironment = environment;
	m_sceneAccelModelRevision = engine->m_sceneRevision;
	m_sceneAccelRevision = m_accelRevision;
	m_hasSceneAccel = true;
}

EnvironmentLight RaytraceRenderer::environmentLight(VulkanEngine* engine) const
{
	//a missed ray sees the map only when one is loaded and no solid colour replaces it; the sky
	//gradient is not a light the list can sample, and is left to BSDF samples at full weight
	EnvironmentLight light;
	if (engine->m_environmentMap.image != VK_NULL_HANDLE && !engine->m_solidBackground) {
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

void RaytraceRenderer::drawReference(VulkanEngine* engine)
{
	if (!m_gpu.hasImage()) {
		return;
	}
	if (!ImGui::CollapsingHeader("Compare to reference")) {
		return;
	}
	if (ImGui::Button("Use this render as reference")) {
		m_gpu.captureReference();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Copies the render as it stands. Converge one strategy far (thousands of samples),\ncapture it, then render the others: each shows its error against it as it goes.");
	}
	if (!m_gpu.hasReference()) {
		ImGui::TextDisabled("No reference yet");
		return;
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear")) {
		m_gpu.clearReference(engine);
		return;
	}
	const VkExtent2D extent = m_gpu.referenceExtent();
	ImGui::Text("Reference: %u x %u, %u sample(s)/pixel", extent.width, extent.height, m_gpu.referenceSamples());
	if (extent.width != m_gpu.width() || extent.height != m_gpu.height()) {
		ImGui::TextDisabled("The render is %u x %u: not compared", m_gpu.width(), m_gpu.height());
		return;
	}
	const ReferenceError& error = m_gpu.referenceError();
	if (error.valid) {
		ImGui::Text("At %u sample(s): RMSE %.4g, relative MSE %.4g", error.samples, error.rmse, error.relativeMse);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Against the reference, over every pixel and channel. Relative MSE divides each\npixel's squared error by its reference value squared (+0.01), so dark regions count.\nMeaningful only for the same scene and camera the reference was rendered from.");
		}
	}
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

	ImGui::TextWrapped("The wavefront, one kernel per row, in dispatch order. The numbering follows figure 15.2 of "
		"Physically Based Rendering 4ed; shaders/rt/NNVV_*.comp holds kernel NN variant VV.");
	ImGui::Separator();

	//ONE LOOP FOR EVERY SLOT. Nothing here names a kernel or a strategy: the rows, the combos and
	//the greying-out all come from the registry, so a new variant appears in this window as soon
	//as it is registered, with no edit to this function
	const KernelSelection before = m_kernels;
	for (uint32_t i = 0; i < (uint32_t)KernelSlot::Count; i++) {
		const KernelSlot slot = (KernelSlot)i;
		const std::span<const KernelVariant> variants = kernelVariants(slot);
		if (variants.empty()) {
			continue;
		}
		const KernelVariant* selected = m_kernels.selected(slot);
		const bool implemented = selected != nullptr && selected->implemented;
		//a following slot's variant is decided by its leader's: shown, never chosen
		const bool follows = kernelSlotFollows(slot);

		ImGui::PushID((int)i);
		ImGui::BeginDisabled((!implemented && variants.size() <= 1) || follows);

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
		} else if (follows) {
			ImGui::TextDisabled("follows kernel %s: %s", kernelSlotNumber(variants.front().followsSlot), selected->description);
		} else {
			ImGui::TextDisabled("%s", selected->description);
		}
		ImGui::Unindent();
		ImGui::PopID();
	}

	if (m_kernels != before) {
		//a new kernel 02 brings its kernel 08 partner with it
		m_kernels = reconcileKernelSelection(m_kernels);
		//a traversal variant fixes the BLAS node layout, so selecting one may mean rebuilding
		//every BLAS. setAccelSettings() does nothing when the layout already matches, which is
		//the common case of swapping something other than kernel 02
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
	//THE SECTION ONLY EXISTS WHILE A KERNEL THAT USES IT IS SELECTED. Switching kernel 02 to the
	//linear scan makes every control below meaningless, so rather than grey them out the whole
	//section goes away - the variant declares KernelSettings::AccelerationStructure and the panel
	//reacts, without the panel knowing which variants those are
	const KernelVariant* intersect = m_kernels.selected(KernelSlot::IntersectClosest);
	if (intersect == nullptr || intersect->settings != KernelSettings::AccelerationStructure) {
		return;
	}
	if (!ImGui::CollapsingHeader("Acceleration structure")) {
		return;
	}
	ImGui::TextDisabled("Kernel 02: %s", intersect->name);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The node layout follows the kernel 02 variant selected in the Raytracer Shaders\nwindow, so there is no separate control for it here.");
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
	//the layout is not editable here: it is whatever the selected kernel 02 variant can read
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

	if (m_blasSet != nullptr && !m_blasSet->blases.empty()) {
		const RaytraceBlasSet& set = *m_blasSet;
		ImGui::Text("BLAS: %zu mesh(es), %zu triangles, built in %.0f ms", set.blases.size(), set.triangles, set.buildMs);
		ImGui::Text("  %zu nodes, depth %u, %.1f MB", set.nodes, set.maxDepth, (double)set.bytes / (1024.0 * 1024.0));
		ImGui::Text("  %.1f%% spatial-split duplicates, mean SAH cost %.1f", set.triangles > 0 ? 100.0 * ((double)set.triangleRefs / (double)set.triangles - 1.0) : 0.0, set.meanSahCost);
		for (const std::string& error : set.errors) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.3f, 1.f));
			ImGui::TextWrapped("Not traced: %s", error.c_str());
			ImGui::PopStyleColor();
		}
	}
	if (m_sceneAccel != nullptr) {
		const Tlas& tlas = m_sceneAccel->tlas;
		ImGui::Text("TLAS: %u mesh instance(s) + %u shape(s), %u nodes, %.2f ms", m_sceneAccel->meshInstanceCount, m_sceneAccel->shapeCount - m_sceneAccel->unboundedCount, tlas.bvh.nodeCount, tlas.buildMs);
		if (m_sceneAccel->unboundedCount > 0) {
			ImGui::Text("+ %u infinite plane(s), tested by every ray", m_sceneAccel->unboundedCount);
		}
		ImGui::Text("  scene SAH cost %.1f per ray", tlas.sceneSahCost);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Expected node visits + primitive tests for a random ray through the whole scene.\nLower is faster; compare builders and layouts by this and by the GPU time per frame.\nThe \"BVH traversal cost\" debug view shows the real cost per pixel.");
		}
	}
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
