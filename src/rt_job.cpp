#include <rt_job.h>

#include <algorithm>
#include <chrono>

#include <imgui.h>

#include <rt_material.h>
#include <rt_random.h>
#include <rt_scene_editor.h>
#include <vk_engine.h>
#include <vk_images.h>
#include <vk_tonemap.h>

namespace {

constexpr const char* OUTPUT_IMAGE_NAME = "Raytraced Output";

//ray generation, ported from the sibling project's camera.h/.cpp. Lives here rather than in a
//header because the render loop is its only caller
class RTCamera {
public:
	RTCamera(const RTCameraSnapshot& snapshot, uint32_t width, uint32_t height)
	{
		const double aspect = double(width) / double(height);
		const double theta = degrees_to_radians(snapshot.vfovDegrees);
		const double h = glm::tan(theta / 2);
		const double viewportHeight = 2.0 * h;
		const double viewportWidth = aspect * viewportHeight;

		t = glm::normalize(snapshot.lookFrom - snapshot.lookAt);
		n = glm::normalize(glm::cross(snapshot.vUp, t));
		b = glm::cross(t, n);

		m_origin = snapshot.lookFrom;
		m_horizontal = snapshot.focusDistance * viewportWidth * n;
		m_vertical = snapshot.focusDistance * viewportHeight * b;
		m_lowerLeftCorner = m_origin - m_horizontal / 2.0 - m_vertical / 2.0 - snapshot.focusDistance * t;
		m_lensRadius = snapshot.aperture / 2;
	}

	ray get_ray(double u, double v) const
	{
		//no aperture control is exposed yet, so the common case skips the lens sample entirely
		if (m_lensRadius <= 0.0) {
			return ray(m_origin, m_lowerLeftCorner + u * m_horizontal + v * m_vertical - m_origin);
		}

		glm::dvec2 rd = m_lensRadius * Random::random_in_disk();
		glm::dvec3 offset = rd.x * n + rd.y * b;
		return ray(m_origin + offset, m_lowerLeftCorner + u * m_horizontal + v * m_vertical - m_origin - offset);
	}

private:
	glm::dvec3 m_origin;
	glm::dvec3 m_lowerLeftCorner;
	glm::dvec3 m_horizontal;
	glm::dvec3 m_vertical;
	glm::dvec3 t, n, b;
	double m_lensRadius;
};

glm::dvec3 ray_color(const ray& r, const RaytraceScene& scene, int depth)
{
	hit_record rec;
	if (depth <= 0)
		return glm::dvec3(0, 0, 0);
	//only scene.spheres is ever consulted; scene.meshInstances has no intersection routine
	if (scene.hit(r, 0.001, RT_INFINITY, rec)) {
		ray scattered;
		glm::dvec3 attenuation;
		if (rec.mat_ptr->scatter(r, rec, attenuation, scattered))
			return attenuation * ray_color(scattered, scene, depth - 1);
		return glm::dvec3(0, 0, 0);
	}
	glm::dvec3 unit_direction = glm::normalize(r.direction());
	auto t = 0.5 * (unit_direction.y + 1.0);
	return (1.0 - t) * glm::dvec3(1.0, 1.0, 1.0) + t * glm::dvec3(0.5, 0.7, 1.0);
}

//the pixel's linear radiance, averaged over its samples. Gamma and clamping are left to the shared
//TonemapPass, which replaced the sibling project's write_color()
glm::vec4 renderPerPixel(const RaytraceScene& scene, const RTCamera& camera, double u, double v, double pixelWidth, double pixelHeight)
{
	const bool jitter = scene.settings.antialiasing;
	const int samples = jitter ? std::max(scene.settings.samplesPerPixel, 1) : 1;

	glm::dvec3 pixel_color(0, 0, 0);
	for (int i = 0; i < samples; i++) {
		const ray r = jitter
			? camera.get_ray(u + Random::random_double() * pixelWidth, v + Random::random_double() * pixelHeight)
			: camera.get_ray(u, v);
		const glm::dvec3 sample = ray_color(r, scene, scene.settings.rayDepth);

		//one NaN or infinite sample would poison the whole pixel's average. Dropping it darkens
		//that pixel imperceptibly, where keeping it would leave a black or white speck
		if (!glm::any(glm::isnan(sample)) && !glm::any(glm::isinf(sample))) {
			pixel_color += sample;
		}
	}

	return glm::vec4(glm::vec3(pixel_color / double(samples)), 1.f);
}

//returns false if it stopped early because cancellation was requested
bool renderScene(const RaytraceScene& scene, glm::vec4* pixels, std::atomic<float>& progress, const std::atomic<bool>& cancelRequested)
{
	const uint32_t width = (uint32_t)scene.settings.width;
	const uint32_t height = (uint32_t)scene.settings.height;

	const RTCamera camera(scene.camera, width, height);

	const double pixelWidth = 1.0 / (width - 1);
	const double pixelHeight = 1.0 / (height - 1);

	for (uint32_t y = 0; y < height; y++) {
		//checked per scanline rather than once at loop entry, so Cancel takes effect promptly
		if (cancelRequested.load(std::memory_order_relaxed)) {
			return false;
		}

		//image rows run top-down; the camera's v axis runs bottom-up from the lower left corner
		const double v = double(height - 1 - y) * pixelHeight;

		for (uint32_t x = 0; x < width; x++) {
			const double u = double(x) * pixelWidth;
			pixels[(size_t)y * width + x] = renderPerPixel(scene, camera, u, v, pixelWidth, pixelHeight);
		}

		progress.store(float(y + 1) / float(height), std::memory_order_relaxed);
	}

	return true;
}

}

RaytraceJob::~RaytraceJob()
{
	//safety net only - shutdown() is what actually tears this down, and it also releases the
	//vulkan resources this destructor has no engine pointer to reach
	if (m_thread.joinable()) {
		m_cancelRequested.store(true, std::memory_order_relaxed);
		m_thread.join();
	}
}

void RaytraceJob::update(VulkanEngine* engine)
{
	retirePendingImages(engine, false);

	if (!m_running || !m_finished.load(std::memory_order_acquire)) {
		return;
	}

	m_thread.join();
	m_running = false;
	m_finished.store(false, std::memory_order_relaxed);

	//the worker-written fields are only safe to read from here on
	if (m_completed) {
		m_lastRenderMs = m_workerRenderMs;
		publishOutput(engine);
	}

	//the pixel buffer has served its purpose either way; the gpu copy is the one that is kept
	m_pixels.clear();
	m_pixels.shrink_to_fit();
}

void RaytraceJob::drawControlPanel(VulkanEngine* engine, const RaytraceSceneEditor& editor)
{
	if (!m_showPanel) {
		return;
	}

	if (!ImGui::Begin("Raytrace Render", &m_showPanel)) {
		ImGui::End();
		return;
	}

	ImGui::Text("Last render: %.1f ms", m_lastRenderMs);

	ImGui::BeginDisabled(m_running);
	if (ImGui::Button("Render")) {
		//snapshot taken here, on the main thread, before the worker exists
		start(buildRaytraceScene(engine, editor, m_settings));
	}
	ImGui::EndDisabled();

	if (m_running) {
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			m_cancelRequested.store(true, std::memory_order_relaxed);
			//otherwise the loop would immediately start another render and Cancel would look broken
			m_renderEveryFrame = false;
		}
		ImGui::ProgressBar(m_progress.load(std::memory_order_relaxed), ImVec2(-FLT_MIN, 0));
	}

	//the sibling project re-triggered its whole (synchronous) render on every ui frame. Here a
	//render is asynchronous, so the equivalent is to start the next one the moment the previous
	//finishes - a render already in flight is left to complete rather than being restarted
	ImGui::Checkbox("Render every frame", &m_renderEveryFrame);
	if (m_renderEveryFrame && !m_running) {
		start(buildRaytraceScene(engine, editor, m_settings));
	}

	ImGui::SeparatorText("Settings");

	//these only take effect on the next Render - an in-flight render is working from its own
	//copy of them and is unaffected
	if (ImGui::BeginCombo("Resolution", RESOLUTION_PRESETS[m_resolutionPreset].label)) {
		for (int i = 0; i < (int)std::size(RESOLUTION_PRESETS); i++) {
			if (ImGui::Selectable(RESOLUTION_PRESETS[i].label, i == m_resolutionPreset)) {
				m_resolutionPreset = i;
				m_settings.width = RESOLUTION_PRESETS[i].width;
				m_settings.height = RESOLUTION_PRESETS[i].height;
			}
		}
		ImGui::EndCombo();
	}

	ImGui::SliderInt("Depth of ray bounces", &m_settings.rayDepth, 1, 16);
	ImGui::Checkbox("Anti-aliasing", &m_settings.antialiasing);
	if (m_settings.antialiasing) {
		ImGui::SliderInt("Samples per pixel", &m_settings.samplesPerPixel, 1, 100);
	}
	ImGui::Checkbox("Fixed seed", &m_settings.useFixedSeed);
	if (m_settings.useFixedSeed) {
		ImGui::InputScalar("Seed", ImGuiDataType_U32, &m_settings.seed);
	}

	if (m_hasOutput) {
		ImGui::SeparatorText("Output");
		ImGui::Text("%ux%u, shown in the \"%s\" window", m_outputImage.imageExtent.width, m_outputImage.imageExtent.height, OUTPUT_IMAGE_NAME);
	}

	ImGui::End();
}

void RaytraceJob::shutdown(VulkanEngine* engine)
{
	if (m_thread.joinable()) {
		m_cancelRequested.store(true, std::memory_order_relaxed);
		m_thread.join();
	}
	m_running = false;

	engine->m_displayRegistry.unregisterImage(OUTPUT_IMAGE_NAME);

	if (m_hasOutput) {
		engine->destroyImage(m_outputImage);
		m_hasOutput = false;
	}

	retirePendingImages(engine, true);
}

void RaytraceJob::start(RaytraceScene&& scene)
{
	//the ray-generation math divides by (extent - 1), so a single-pixel axis has no valid
	//spacing. The ui already keeps these well above this floor; this is the backstop
	scene.settings.width = std::max(scene.settings.width, 2);
	scene.settings.height = std::max(scene.settings.height, 2);

	m_pixelWidth = (uint32_t)scene.settings.width;
	m_pixelHeight = (uint32_t)scene.settings.height;
	m_pixels.assign((size_t)m_pixelWidth * m_pixelHeight, glm::vec4(0.f));

	m_progress.store(0.f, std::memory_order_relaxed);
	m_cancelRequested.store(false, std::memory_order_relaxed);
	m_finished.store(false, std::memory_order_relaxed);
	m_completed = false;
	m_running = true;

	m_thread = std::thread([this, snapshot = std::move(scene)]() {
		const auto start = std::chrono::high_resolution_clock::now();

		//every render runs on a fresh thread, whose generator is otherwise seeded from
		//std::random_device on first use
		if (snapshot.settings.useFixedSeed) {
			Random::seed(snapshot.settings.seed);
		}

		const bool completed = renderScene(snapshot, m_pixels.data(), m_progress, m_cancelRequested);

		const auto elapsed = std::chrono::high_resolution_clock::now() - start;
		m_workerRenderMs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1000.f;
		m_completed = completed;

		//released last, and paired with the main thread's join, so everything above is visible
		m_finished.store(true, std::memory_order_release);
	});
}

void RaytraceJob::publishOutput(VulkanEngine* engine)
{
	//order matters: imgui has to stop referencing the old image's view before the view dies
	engine->m_displayRegistry.unregisterImage(OUTPUT_IMAGE_NAME);

	if (m_hasOutput) {
		m_pendingDestroys.push_back({ m_outputImage, (uint64_t)engine->m_frameNumber + FRAME_OVERLAP + 1 });
		m_hasOutput = false;
	}

	const VkExtent3D extent { m_pixelWidth, m_pixelHeight, 1 };

	//the linear buffer goes up through the same staging path load_image() uses for glTF textures;
	//glm::vec4 is four packed floats, matching rgba32f texel for texel. SAMPLED as well as STORAGE,
	//because createImage() leaves every upload in SHADER_READ_ONLY_OPTIMAL, a sampled-only layout
	AllocatedImage linearImage = engine->createImage(m_pixels.data(), extent, TonemapPass::LINEAR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	m_outputImage = engine->createImage(extent, TonemapPass::DISPLAY_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	m_hasOutput = true;

	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, linearImage.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, m_outputImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

		//the current frame slot's descriptor pool is not reset until the next draw(), well after
		//immediateSubmit() has waited for this command buffer to finish
		engine->m_tonemapPass.dispatch(cmd, engine->m_device, engine->getCurrentFrame().frameDescriptors, linearImage, m_outputImage, 1.f);

		vkutil::transition_image(cmd, m_outputImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	//nothing references the linear image once the synchronous submit above has returned
	engine->destroyImage(linearImage);

	//the display image is left in SHADER_READ_ONLY_OPTIMAL and nothing writes it again, so unlike
	//a per-frame render target this needs no transition in draw()
	engine->m_displayRegistry.registerImage(OUTPUT_IMAGE_NAME, m_outputImage.imageView, { m_pixelWidth, m_pixelHeight });
}

void RaytraceJob::retirePendingImages(VulkanEngine* engine, bool force)
{
	const uint64_t currentFrame = (uint64_t)engine->m_frameNumber;

	std::erase_if(m_pendingDestroys, [engine, currentFrame, force](const PendingImageDestroy& pending) {
		if (!force && currentFrame < pending.retireFrame) {
			return false;
		}

		engine->destroyImage(pending.image);
		return true;
	});
}
