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
	key.sceneRevision = editor.revision();
	key.modelRevision = engine->m_sceneRevision;
	key.accelRevision = m_accelRevision;
	key.environmentMapPath = engine->m_environmentMapPath;
	key.environmentIntensity = engine->m_environmentIntensity;
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

void RaytraceRenderer::setSettings(const RenderSettings& settings)
{
	m_settings = settings;

	//the combo's selection follows the settings, not the other way round
	if (m_settings.matchViewport) {
		m_resolutionChoice = 0;
		return;
	}
	m_resolutionChoice = -1;
	for (int i = 0; i < (int)std::size(RESOLUTION_PRESETS); i++) {
		if (RESOLUTION_PRESETS[i].width == m_settings.width && RESOLUTION_PRESETS[i].height == m_settings.height) {
			m_resolutionChoice = i + 1;
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
	const double costPerRay = m_sceneAccel != nullptr ? std::max((double)m_sceneAccel->tlas.sceneSahCost, 1.0) : 1.0;
	const double work = estimatedWorkPerFrame(prospective, costPerRay);
	const bool overBudget = work > WORK_PER_FRAME_BUDGET;

	if (m_sceneAccel != nullptr && m_sceneAccel->meshInstanceCount > 0) {
		ImGui::Text("%zu triangle(s) placed, ~%.0f steps/ray, %.1e steps/frame", m_sceneAccel->placedTriangles, costPerRay, work);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("The BVH's expected cost of one ray (SAH: node visits + primitive tests),\ntimes pixels x samples/frame. What the render guard below is judged on.");
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

	//the compaction readout: the counts must fall monotonically
	const std::vector<uint32_t>& alive = m_gpu.pathsAlivePerBounce();
	if (!alive.empty()) {
		std::string line = "Paths alive per bounce:";
		for (uint32_t count : alive) {
			line += fmt::format(" {}", count);
		}
		ImGui::TextWrapped("%s", line.c_str());
	}

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
	if (m_resolutionChoice == 0) {
		resolutionLabel = fmt::format("Match viewport ({} x {})", engine->m_drawExtent.width, engine->m_drawExtent.height);
	} else if (m_resolutionChoice > 0) {
		resolutionLabel = RESOLUTION_PRESETS[m_resolutionChoice - 1].label;
	} else {
		resolutionLabel = fmt::format("{} x {} (from file)", m_settings.width, m_settings.height);
	}
	if (ImGui::BeginCombo("Resolution", resolutionLabel.c_str())) {
		if (ImGui::Selectable("Match viewport", m_resolutionChoice == 0)) {
			m_resolutionChoice = 0;
			m_settings.matchViewport = true;
		}
		for (int i = 0; i < (int)std::size(RESOLUTION_PRESETS); i++) {
			if (ImGui::Selectable(RESOLUTION_PRESETS[i].label, i + 1 == m_resolutionChoice)) {
				m_resolutionChoice = i + 1;
				m_settings.matchViewport = false;
				m_settings.width = RESOLUTION_PRESETS[i].width;
				m_settings.height = RESOLUTION_PRESETS[i].height;
			}
		}
		ImGui::EndCombo();
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
	const double costPerRay = m_sceneAccel != nullptr ? std::max((double)m_sceneAccel->tlas.sceneSahCost, 1.0) : 1.0;

	//the one thing that must never be started by accident. Over the budget the frame runs long
	//enough to trip the GPU watchdog, and the failure mode is not a dropped frame - it is the
	//display driver being killed. Refused here rather than in the panel so that no caller can
	//reach it another way, and acknowledged only for the exact cost that was shown
	const double work = estimatedWorkPerFrame(settings, costPerRay);
	if (work > WORK_PER_FRAME_BUDGET && !(m_acceptedHeavyRender && work <= m_acceptedWork)) {
		m_blockedReason = fmt::format("~{:.0f} BVH steps per ray at {}x{}x{} is {:.1e} per frame, over the {:.0e} budget. Lower the resolution or samples per frame, hide some objects, or accept it below",
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
	snapshot.seed = settings.useFixedSeed ? settings.seed : (uint32_t)std::random_device {}();
	snapshot.useEnvironmentMap = engine->m_environmentMap.image != VK_NULL_HANDLE;
	snapshot.environmentIntensity = engine->m_environmentIntensity;

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
	//change: a camera drag restarts the render each frame, and must not rebuild the TLAS each time
	if (m_hasSceneAccel && m_sceneAccelModelRevision == engine->m_sceneRevision && m_sceneAccelRevision == m_accelRevision
		&& m_sceneAccelObjects == editor.meshObjects() && m_sceneAccelSpheres == editor.spheres()) {
		return;
	}

	static const RaytraceMeshData noMeshes;
	const RaytraceMeshData& meshData = engine->m_raytraceMeshData != nullptr ? *engine->m_raytraceMeshData : noMeshes;
	m_sceneAccel = buildSceneAccel(m_blasSet, m_geometry, meshData, editor.meshObjects(), editor.spheres(), engine->m_raytraceTextures);

	m_sceneAccelObjects = editor.meshObjects();
	m_sceneAccelSpheres = editor.spheres();
	m_sceneAccelModelRevision = engine->m_sceneRevision;
	m_sceneAccelRevision = m_accelRevision;
	m_hasSceneAccel = true;
}

void RaytraceRenderer::drawAccelSettings(VulkanEngine* engine)
{
	if (!ImGui::CollapsingHeader("Acceleration structure")) {
		return;
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
	if (ImGui::BeginCombo("Node layout", bvhLayoutName(pending.layout))) {
		for (uint32_t i = 0; i < (uint32_t)BvhLayout::Count; i++) {
			if (ImGui::Selectable(bvhLayoutName((BvhLayout)i), pending.layout == (BvhLayout)i)) {
				pending.layout = (BvhLayout)i;
			}
		}
		ImGui::EndCombo();
	}

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
		ImGui::Text("TLAS: %u mesh instance(s) + %u sphere(s), %u nodes, %.2f ms", m_sceneAccel->meshInstanceCount, m_sceneAccel->sphereCount, tlas.bvh.nodeCount, tlas.buildMs);
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
