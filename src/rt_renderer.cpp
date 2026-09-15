#include <rt_renderer.h>

#include <algorithm>
#include <chrono>
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

const char* backendName(RaytraceBackend backend)
{
	switch (backend) {
	case RaytraceBackend::Gpu:
		return "GPU path tracer";
	case RaytraceBackend::CpuLegacy:
		return "CPU (legacy)";
	}
	return "unknown";
}

}

void RaytraceRenderer::init(VulkanEngine* engine)
{
	m_gpu.init(engine);
}

void RaytraceRenderer::update(VulkanEngine* engine)
{
	if (m_cpu.update()) {
		//the worker has been joined; its buffer is readable
		if (m_cpu.completed()) {
			m_lastRenderMs = m_cpu.renderMs();
			m_lastRenderSamples = m_cpuRenderSamples;
			m_lastRenderBackend = RaytraceBackend::CpuLegacy;
			m_hasLastRender = true;
			publishCpuImage(engine);
		}
	}

	//the GPU render ends inside record(), by reaching maxSamples or by Stop; report it here
	if (m_gpuWasRunning && !m_gpu.isRunning()) {
		m_lastRenderMs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_renderStart).count() / 1000.f;
		m_lastRenderSamples = (int)m_gpu.samplesAccumulated();
		m_lastRenderBackend = RaytraceBackend::Gpu;
		m_hasLastRender = true;
	}
	m_gpuWasRunning = m_gpu.isRunning();
}

bool RaytraceRenderer::isRunning() const
{
	return m_cpu.isRunning() || m_gpu.isRunning();
}

void RaytraceRenderer::cancelRender()
{
	m_cpu.cancel();
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

	const bool running = isRunning();

	//the backend is part of the snapshot; switching mid-render would only confuse the readouts
	ImGui::BeginDisabled(running);
	if (ImGui::BeginCombo("Backend", backendName(m_backend))) {
		for (RaytraceBackend candidate : { RaytraceBackend::Gpu, RaytraceBackend::CpuLegacy }) {
			if (ImGui::Selectable(backendName(candidate), candidate == m_backend)) {
				m_backend = candidate;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();

	if (m_hasLastRender) {
		ImGui::Text("Last render: %.0f ms, %d sample(s)/pixel, %s", m_lastRenderMs, m_lastRenderSamples, backendName(m_lastRenderBackend));
		if (m_lastRenderBackend == RaytraceBackend::Gpu && m_gpu.hasTimestamps()) {
			ImGui::Text("GPU time: %.1f ms", m_gpu.totalGpuMs());
		}
	} else {
		ImGui::TextDisabled("No render yet");
	}

	ImGui::BeginDisabled(running);
	if (ImGui::Button("Render")) {
		startRender(engine, editor);
	}
	ImGui::EndDisabled();

	if (running) {
		ImGui::SameLine();
		if (ImGui::Button("Stop")) {
			cancelRender();
		}

		if (m_gpu.isRunning()) {
			const float fraction = m_gpu.maxSamples() > 0 ? (float)m_gpu.samplesAccumulated() / (float)m_gpu.maxSamples() : 0.f;
			const std::string overlay = fmt::format("{} / {} samples", m_gpu.samplesAccumulated(), m_gpu.maxSamples());
			ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay.c_str());
			if (m_gpu.hasTimestamps()) {
				ImGui::Text("GPU: %.2f ms/frame, %u sample(s)/frame", m_gpu.lastFrameGpuMs(), m_gpu.samplesPerFrame());
			} else {
				ImGui::Text("%u sample(s)/frame (no GPU timestamps on this device)", m_gpu.samplesPerFrame());
			}
		} else {
			ImGui::ProgressBar(m_cpu.progress(), ImVec2(-FLT_MIN, 0));
		}

		//the render is of the scene as it was at the click; nothing done since reaches it
		ImGui::TextDisabled("Rendering a snapshot - edits and camera moves apply to the next Render");
	}

	//the compaction readout: the counts must fall monotonically. Shown after a GPU render too,
	//since the last frame's counts are the interesting ones
	const std::vector<uint32_t>& alive = m_gpu.pathsAlivePerBounce();
	if (!alive.empty() && (m_gpu.isRunning() || m_lastRenderBackend == RaytraceBackend::Gpu)) {
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
	}

	ImGui::End();
}

void RaytraceRenderer::drawSettings(VulkanEngine* engine)
{
	//these only take effect on the next Render - an in-flight render is working from its own
	//copy of them - except exposure and the debug view, which the GPU backend applies live
	const bool gpu = m_backend == RaytraceBackend::Gpu;

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
	ImGui::BeginDisabled(!m_settings.antialiasing);
	ImGui::SliderInt("Max samples per pixel", &m_settings.maxSamples, 1, 4096, "%d", ImGuiSliderFlags_Logarithmic);
	ImGui::EndDisabled();
	if (!m_settings.antialiasing) {
		ImGui::SameLine();
		ImGui::TextDisabled("(1 un-jittered sample)");
	}

	ImGui::BeginDisabled(!gpu);
	ImGui::SliderInt("Samples per frame", &m_settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);
	ImGui::EndDisabled();

	ImGui::SliderInt("Depth of ray bounces", &m_settings.rayDepth, 1, CRT_MAX_DEPTH);

	ImGui::BeginDisabled(!gpu);
	ImGui::Checkbox("Russian roulette", &m_settings.russianRoulette);
	ImGui::BeginDisabled(!m_settings.russianRoulette);
	ImGui::SliderInt("Min bounces before roulette", &m_settings.minBouncesBeforeRoulette, 0, CRT_MAX_DEPTH);
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	ImGui::SliderFloat("Aperture", &m_settings.aperture, 0.f, 1.f, "%.3f");
	ImGui::SliderFloat("Focus distance", &m_settings.focusDistance, 0.1f, 50.f, "%.2f", ImGuiSliderFlags_Logarithmic);

	ImGui::Checkbox("Fixed seed", &m_settings.useFixedSeed);
	if (m_settings.useFixedSeed) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.f);
		ImGui::InputScalar("Seed", ImGuiDataType_U32, &m_settings.seed);
	}

	ImGui::SliderFloat("Exposure", &m_settings.exposure, 0.f, 4.f);

	ImGui::BeginDisabled(!gpu);
	if (ImGui::BeginCombo("Debug view", crtDebugViewName(m_gpu.debugView))) {
		for (uint32_t i = 0; i < (uint32_t)CrtDebugView::Count; i++) {
			const CrtDebugView view = (CrtDebugView)i;
			if (ImGui::Selectable(crtDebugViewName(view), view == m_gpu.debugView)) {
				m_gpu.debugView = view;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
}

void RaytraceRenderer::startRender(VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
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
	}
	settings.maxSamples = std::max(settings.maxSamples, 1);
	settings.rayDepth = std::clamp(settings.rayDepth, 1, CRT_MAX_DEPTH);
	settings.samplesPerFrame = std::clamp(settings.samplesPerFrame, 1, CRT_MAX_SAMPLES_PER_FRAME);

	ensureDisplayImage(engine, (uint32_t)settings.width, (uint32_t)settings.height);

	m_renderStart = std::chrono::steady_clock::now();

	if (m_backend == RaytraceBackend::CpuLegacy) {
		m_cpuRenderSamples = settings.maxSamples;
		m_cpuRenderExposure = settings.exposure;
		m_cpu.start(buildRaytraceScene(engine, editor, settings));
		return;
	}

	GpuRenderSnapshot snapshot;
	snapshot.width = (uint32_t)settings.width;
	snapshot.height = (uint32_t)settings.height;
	snapshot.camera = captureCameraSnapshot(engine, settings);
	snapshot.spheres = editor.spheres();
	snapshot.settings = settings;
	snapshot.seed = settings.useFixedSeed ? settings.seed : (uint32_t)std::random_device {}();
	snapshot.useEnvironmentMap = engine->m_environmentMap.image != VK_NULL_HANDLE;
	snapshot.environmentIntensity = engine->m_environmentIntensity;

	m_lastRenderBackend = RaytraceBackend::Gpu;
	m_gpu.start(engine, std::move(snapshot));
	m_gpuWasRunning = true;
}

void RaytraceRenderer::record(VkCommandBuffer cmd, VulkanEngine* engine)
{
	//timestamps and header readbacks from FRAME_OVERLAP frames back, whether or not a render runs
	m_gpu.beginFrame(engine);

	//the display image belongs to whichever backend rendered last. The GPU backend re-tonemaps
	//its accumulation every frame it owns the image, so exposure is live for it even after the
	//render has stopped; a CPU image was tonemapped once at publish and is left alone
	if (!m_hasDisplayImage || m_lastRenderBackend != RaytraceBackend::Gpu || !(m_gpu.isRunning() || m_gpu.hasImage())) {
		return;
	}

	vkutil::transition_image(cmd, m_displayImage.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	m_gpu.record(cmd, engine, m_displayImage, m_settings.exposure);
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

void RaytraceRenderer::publishCpuImage(VulkanEngine* engine)
{
	ensureDisplayImage(engine, m_cpu.width(), m_cpu.height());

	const VkExtent3D extent { m_cpu.width(), m_cpu.height(), 1 };

	//the linear buffer goes up through the same staging path load_image() uses for glTF textures;
	//glm::vec4 is four packed floats, matching rgba32f texel for texel. SAMPLED as well as STORAGE,
	//because createImage() leaves every upload in SHADER_READ_ONLY_OPTIMAL, a sampled-only layout
	AllocatedImage linearImage = engine->createImage((void*)m_cpu.pixels().data(), extent, TonemapPass::LINEAR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, linearImage.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, m_displayImage.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

		//the current frame slot's descriptor pool is not reset until the next draw(), well after
		//immediateSubmit() has waited for this command buffer to finish
		engine->m_tonemapPass.dispatch(cmd, engine->m_device, engine->getCurrentFrame().frameDescriptors, linearImage, m_displayImage, m_cpuRenderExposure);

		vkutil::transition_image(cmd, m_displayImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	//nothing references the linear image once the synchronous submit above has returned
	engine->destroyImage(linearImage);
}

void RaytraceRenderer::shutdown(VulkanEngine* engine)
{
	m_cpu.shutdown();
	m_gpu.stop();
	m_gpu.destroy(engine);

	if (m_hasDisplayImage) {
		engine->m_displayRegistry.unregisterImage(OUTPUT_IMAGE_NAME);
		engine->destroyImage(m_displayImage);
		m_displayImage = {};
		m_hasDisplayImage = false;
	}
}

