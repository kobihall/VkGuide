#include <rt_renderer.h>

#include <algorithm>
#include <random>

#include <imgui.h>

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

	ImGui::BeginDisabled(running || camera == nullptr);
	if (ImGui::Button("Render")) {
		startRender(engine, editor);
	}
	ImGui::EndDisabled();

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

	ensureDisplayImage(engine, (uint32_t)settings.width, (uint32_t)settings.height);

	GpuRenderSnapshot snapshot;
	snapshot.width = (uint32_t)settings.width;
	snapshot.height = (uint32_t)settings.height;
	snapshot.camera = cameraSnapshot(*camera);
	snapshot.spheres = editor.spheres();
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

