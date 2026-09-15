#include <rt_job.h>

#include <algorithm>
#include <chrono>

#include <rt_material.h>
#include <rt_random.h>

namespace {

//ray generation, ported from the sibling project's camera.h/.cpp. Lives here rather than in a
//header because the render loop is its only caller. The GPU backend's generate stage is a
//float port of this, so both fire the same primary rays for the same snapshot
class RTCamera {
public:
	RTCamera(const RTCameraSnapshot& snapshot, uint32_t width, uint32_t height)
	{
		const double aspect = double(width) / double(height);
		const double theta = degrees_to_radians(snapshot.vfovDegrees);
		const double h = glm::tan(theta / 2);
		const double viewportHeight = 2.0 * h;
		const double viewportWidth = aspect * viewportHeight;

		//the snapshot is float scene data; this backend's math is double
		const glm::dvec3 lookFrom(snapshot.lookFrom);
		const glm::dvec3 lookAt(snapshot.lookAt);
		const glm::dvec3 vUp(snapshot.vUp);
		const double focusDistance = snapshot.focusDistance;

		t = glm::normalize(lookFrom - lookAt);
		n = glm::normalize(glm::cross(vUp, t));
		b = glm::cross(t, n);

		m_origin = lookFrom;
		m_horizontal = focusDistance * viewportWidth * n;
		m_vertical = focusDistance * viewportHeight * b;
		m_lowerLeftCorner = m_origin - m_horizontal / 2.0 - m_vertical / 2.0 - focusDistance * t;
		m_lensRadius = (double)snapshot.aperture / 2;
	}

	ray get_ray(double u, double v) const
	{
		//a closed lens skips the lens sample entirely, which also keeps the fixed-seed stream unchanged
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
	//only scene.spheres is ever consulted; scene.meshData has no intersection routine
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
//TonemapPass, which replaced the sibling project's write_color(). u/v are the pixel's lower-left
//corner in [0,1]; a jittered sample lands anywhere in the pixel, an un-jittered one at its centre
glm::vec4 renderPerPixel(const RaytraceScene& scene, const RTCamera& camera, double u, double v, double pixelWidth, double pixelHeight)
{
	const bool jitter = scene.settings.antialiasing;
	const int samples = jitter ? std::max(scene.settings.maxSamples, 1) : 1;

	glm::dvec3 pixel_color(0, 0, 0);
	for (int i = 0; i < samples; i++) {
		const ray r = jitter
			? camera.get_ray(u + Random::random_double() * pixelWidth, v + Random::random_double() * pixelHeight)
			: camera.get_ray(u + 0.5 * pixelWidth, v + 0.5 * pixelHeight);
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

	//pixel x covers [x/W, (x+1)/W) of the image plane, the same mapping the raster pass uses, so
	//a "Match viewport" render frames exactly what the viewport shows. (The book divides by
	//W-1, which puts pixel 0's centre on the frustum's edge rather than half a pixel inside it)
	const double pixelWidth = 1.0 / width;
	const double pixelHeight = 1.0 / height;

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
	shutdown();
}

void RaytraceJob::cancel()
{
	m_cancelRequested.store(true, std::memory_order_relaxed);
}

bool RaytraceJob::update()
{
	if (!m_running || !m_finished.load(std::memory_order_acquire)) {
		return false;
	}

	m_thread.join();
	m_running = false;
	m_finished.store(false, std::memory_order_relaxed);

	//the worker-written fields are safe to read from here on
	return true;
}

void RaytraceJob::shutdown()
{
	if (m_thread.joinable()) {
		m_cancelRequested.store(true, std::memory_order_relaxed);
		m_thread.join();
	}
	m_running = false;
	m_pixels.clear();
	m_pixels.shrink_to_fit();
}

void RaytraceJob::start(RaytraceScene&& scene)
{
	shutdown();

	//the ray-generation math needs a real pixel size. The ui already keeps these well above this
	//floor; this is the backstop
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
