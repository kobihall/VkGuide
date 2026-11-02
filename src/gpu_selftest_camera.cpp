// The cameras' GPU self-test (docs/plans/completed/pbrt-cameras.md).
//
// Camera parity runs generateCameraRay() of shaders/rt/include/crt_camera.glsl (shaders/test/camera_parity.comp) on a
// fixed batch of film and lens points for every kind of camera, each aperture shape, a tilt and a shift, and compares
// the rays with the C++ mirror in src/camera_model.cpp; for a camera light tracing connects to, also the importance of
// each ray and SampleWi() from a point along it. Ray Tracing in One Weekend's camera, built without
// CRT_CAMERA_MODEL as a render builds it, must agree to the float; the model, under Metal's fast math, to a few ulps.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include <fmt/format.h>

#include <camera_model.h>
#include <gpu_selftest.h>
#include <light_sampling.h>
#include <rt_gpu_types.h>
#include <vk_compute.h>
#include <vk_engine.h>

namespace {

struct CameraCase {
	const char* label;
	RTCameraSnapshot camera;
	// what a render of it is built with
	bool model;
};

std::vector<CameraCase> cameraCases()
{
	RTCameraSnapshot base;
	base.lookFrom = glm::vec3(0.3f, 1.2f, 4.f);
	base.lookAt = glm::vec3(-0.2f, 0.9f, 0.f);
	base.vfovDegrees = 38.f;
	base.aperture = 0.25f;
	base.focusDistance = 3.5f;
	std::vector<CameraCase> cases;
	RTCameraSnapshot c = base;
	c.lensMapping = LensMapping::Polar;
	cases.push_back({ "Ray Tracing in One Weekend's camera", c, false });
	c.lensMapping = LensMapping::Concentric;
	cases.push_back({ "perspective, concentric", c, true });
	c.apertureShape = ApertureShape::Polygon;
	c.apertureBlades = 7;
	c.apertureRotationDegrees = 20.f;
	cases.push_back({ "perspective, seven blades", c, true });
	c.apertureShape = ApertureShape::Image;
	cases.push_back({ "perspective, aperture image", c, true });
	c.apertureShape = ApertureShape::Circle;
	c.tiltDegrees = glm::vec2(15.f, -8.f);
	c.shift = glm::vec2(0.2f, -0.1f);
	cases.push_back({ "perspective, tilted and shifted", c, true });
	c.kind = CameraKind::Orthographic;
	c.orthoHeight = 1.7f;
	cases.push_back({ "orthographic, tilted and shifted", c, true });
	c.tiltDegrees = glm::vec2(0.f);
	cases.push_back({ "orthographic", c, true });
	c = base;
	c.kind = CameraKind::Spherical;
	cases.push_back({ "spherical, equirectangular", c, true });
	c.mapping = SphericalMapping::EqualArea;
	cases.push_back({ "spherical, equal-area", c, true });
	return cases;
}

}

SelfTestResult runCameraParityTest(VulkanEngine* engine)
{
	// relative to the larger of the vector's length and 1: the model's arithmetic under Metal's fast math
	constexpr double TOLERANCE = 2e-5;
	constexpr uint32_t CASES = 8192;

	SelfTestResult result;
	bool passed = true;

	//the points: film and lens samples away from the open interval's edges
	std::mt19937 rng(20261101u);
	std::uniform_real_distribution<float> unit(0.f, 0.99999994f);
	std::vector<glm::vec4> points(CASES);
	for (glm::vec4& p : points) {
		p = glm::vec4(unit(rng), unit(rng), unit(rng), unit(rng));
	}
	const VkDeviceSize pointBytes = sizeof(glm::vec4) * points.size();
	const VkDeviceSize resultBytes = 4 * pointBytes;
	const AllocatedBuffer pointBuffer = engine->createBuffer(pointBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	const AllocatedBuffer resultBuffer = engine->createBuffer(resultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	std::memcpy(pointBuffer.info.pMappedData, points.data(), pointBytes);
	vmaFlushAllocation(engine->m_memAllocator, pointBuffer.allocation, 0, pointBytes);

	//an aperture image: a ring, brighter on its left, as bin/camera_test's
	constexpr uint32_t SIZE = 48;
	std::vector<float> values(SIZE * SIZE);
	for (uint32_t y = 0; y < SIZE; y++) {
		for (uint32_t x = 0; x < SIZE; x++) {
			const float px = 2.f * ((float)x + 0.5f) / SIZE - 1.f;
			const float py = 1.f - 2.f * ((float)y + 0.5f) / SIZE;
			const float r = std::sqrt(px * px + py * py);
			values[y * SIZE + x] = (r > 0.5f && r < 0.9f) ? (px < 0.f ? 1.f : 0.25f) : 0.f;
		}
	}
	const PiecewiseConstant2D ring = buildPiecewiseConstant2D(values, SIZE, SIZE);
	std::vector<float> tableData;
	const uint32_t tableBase = packPiecewiseConstant2D(ring, tableData);
	const VkDeviceSize tableBytes = sizeof(float) * tableData.size();
	const AllocatedBuffer tableBuffer = engine->createBuffer(tableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	std::memcpy(tableBuffer.info.pMappedData, tableData.data(), tableBytes);
	vmaFlushAllocation(engine->m_memAllocator, tableBuffer.allocation, 0, tableBytes);
	//crt_samples.glsl's sample record, which nothing here reads: a kernel built from raw bindings binds a stub
	const AllocatedBuffer stub = engine->createBuffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	for (const bool model : { false, true }) {
		ComputePass pass = ComputePassBuilder(engine->m_rootPath + "shaders/test/camera_parity.comp.spv")
			.addBinding(23, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addBinding(65, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addBinding(90, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addBinding(91, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addSpecialization(7, model ? 1u : 0u)
			.setPushConstants<CrtParams>()
			.setWorkgroupSize(64, 1, 1)
			.build(engine->m_device);
		for (const CameraCase& c : cameraCases()) {
			if (c.model != model) {
				continue;
			}
			const CameraFrame frame = buildCameraFrame(c.camera, 320, 180, tableBase);
			if (frame.needsModel() != model) {
				passed = false;
				result.lines.push_back(fmt::format("{}: a render would build it {} the camera model - FAILED", c.label, frame.needsModel() ? "with" : "without"));
				continue;
			}
			CrtParams params {};
			packCameraParams(frame, params);

			const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, pass.setLayout);
			DescriptorWriter writer;
			writer.writeBuffer(23, stub.buffer, 64, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
			writer.writeBuffer(65, tableBuffer.buffer, tableBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
			writer.writeBuffer(90, pointBuffer.buffer, pointBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
			writer.writeBuffer(91, resultBuffer.buffer, resultBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
			writer.updateSet(engine->m_device, set);
			std::memset(resultBuffer.info.pMappedData, 0, resultBytes);
			engine->immediateSubmit([&](VkCommandBuffer cmd) {
				dispatchComputePassOver(cmd, pass, set, &params, { CASES, 1, 1 });
			});
			vmaInvalidateAllocation(engine->m_memAllocator, resultBuffer.allocation, 0, resultBytes);
			const glm::vec4* gpu = (const glm::vec4*)resultBuffer.info.pMappedData;

			const PiecewiseConstant2D* aperture = c.camera.apertureShape == ApertureShape::Image ? &ring : nullptr;
			double worst = 0.0;
			size_t bad = 0;
			size_t exact = 0;
			for (uint32_t i = 0; i < CASES; i++) {
				const CameraRay mirror = generateCameraRay(frame, glm::vec2(points[i].x, points[i].y), glm::vec2(points[i].z, points[i].w), aperture);
				const glm::vec3 origin(gpu[4 * i]);
				const glm::vec3 direction(gpu[4 * i + 1]);
				auto difference = [](const glm::vec3& a, const glm::vec3& b) {
					return (double)glm::length(a - b) / std::max({ (double)glm::length(a), (double)glm::length(b), 1.0 });
				};
				double d = std::max(difference(mirror.origin, origin), difference(mirror.direction, direction));
				//the importance of the GPU's own ray, and SampleWi() from a point along it, for a camera that connects. A
				//film point within the tolerance of the film's edge may fall either side of it
				if (cameraConnects(frame)) {
					auto relative = [](double a, double b) { return std::abs(a - b) / std::max({ std::abs(a), std::abs(b), 1e-6 }); };
					const glm::vec3 unit = glm::normalize(direction);
					glm::vec2 film;
					const float we = cameraWe(frame, origin, unit, film, aperture);
					float pdfPos;
					float pdfDir;
					cameraPdfWe(frame, origin, unit, pdfPos, pdfDir, aperture);
					const glm::vec4 gpuWe = gpu[4 * i + 2];
					const bool edge = std::min({ film.x, film.y, 1.f - film.x, 1.f - film.y }) < 1e-4f;
					if (!edge) {
						d = std::max({ d, relative(we, gpuWe.x), relative(pdfDir, gpuWe.w), (double)glm::length(film - glm::vec2(gpuWe.y, gpuWe.z)) });
					}
					CameraWiSample sample;
					cameraSampleWi(frame, origin + 2.5f * unit, glm::vec2(points[i].z, points[i].w), aperture, sample);
					const glm::vec4 gpuWi = gpu[4 * i + 3];
					const bool wiEdge = std::min({ sample.film.x, sample.film.y, 1.f - sample.film.x, 1.f - sample.film.y }) < 1e-4f;
					if (!wiEdge) {
						d = std::max({ d, relative(sample.pdf, gpuWi.x), relative(sample.we, gpuWi.y), (double)glm::length(sample.film - glm::vec2(gpuWi.z, gpuWi.w)) });
					}
				}
				worst = std::max(worst, d);
				bad += d > TOLERANCE ? 1 : 0;
				exact += (mirror.origin == origin && mirror.direction == direction) ? 1 : 0;
			}
			const bool casePassed = bad == 0;
			passed &= casePassed;
			result.lines.push_back(fmt::format("{}: {} rays, {} identical to the float, worst difference {:.2g} (limit {:g}){}", c.label, CASES, exact, worst, TOLERANCE,
				casePassed ? "" : fmt::format(", {} over - FAILED", bad)));
		}
		pass.destroy(engine->m_device);
	}
	engine->destroyBuffer(stub);
	engine->destroyBuffer(tableBuffer);
	engine->destroyBuffer(pointBuffer);
	engine->destroyBuffer(resultBuffer);

	result.passed = passed;
	result.lines.insert(result.lines.begin(), passed ? "the shaders and the mirror agree" : "the shaders and the mirror disagree");
	return result;
}
