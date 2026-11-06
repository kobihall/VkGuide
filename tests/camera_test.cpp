// CPU verification of the camera models (src/camera_model.h, the line-for-line mirror of
// shaders/rt/include/crt_camera.glsl). No Vulkan, no GPU.
//
// What the cameras promise (docs/plans/completed/pbrt-cameras.md):
//  - Ray Tracing in One Weekend's camera is unchanged: the polar lens mapping, a circular aperture and no tilt give the
//    rays its arithmetic gives, bit for bit
//  - each aperture samples its own shape uniformly (a circle by either mapping, a polygon), or an image by its
//    luminance: a chi-square test of the sampled points against apertureDensity(), which integrates to 1, and no
//    sample outside the shape
//  - every ray through one point of the film meets the chief ray on the plane of focus, whatever the lens sample: the
//    film's plane without a tilt, the tilted plane with one, which passes through the axis at the focus distance
//  - an orthographic camera's chief rays are parallel and its view volume is as tall as asked; the shift moves the
//    film by image heights
//  - a spherical camera's equirectangular image is the environment map's layout (equirectUvOf() reads its directions
//    back to the pixel), and its equal-area image gives every pixel the same solid angle
//  - camera importance (docs/plans/completed/pbrt-bidirectional-path-tracing.md): We integrates to 1 over the lens and
//    the directions that reach the film, as the density of those directions does, for a pinhole and for each aperture;
//    a point SampleWi() finds on the lens, and the film point it reports, give back the ray towards the scene point
// --selfcheck proves the tests can fail: it plants a polygon sampler that favors one triangle of the fan and a focus
// point a millimeter off, and fails unless both are caught.
//
// Run:  ./bin/camera_test [--selfcheck]

#include <camera_model.h>
#include <light_sampling.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <random>

#include <fmt/format.h>

#include "test_stats.h"

using namespace stats;

namespace {

int g_failures = 0;
bool g_quiet = false;

void check(bool ok, const std::string& what)
{
	if (!ok) {
		g_failures++;
		if (!g_quiet) {
			fmt::println("    FAIL: {}", what);
		}
	}
}

RTCameraSnapshot baseCamera()
{
	RTCameraSnapshot camera;
	camera.lookFrom = glm::vec3(0.3f, 1.2f, 4.f);
	camera.lookAt = glm::vec3(-0.2f, 0.9f, 0.f);
	camera.vfovDegrees = 38.f;
	camera.aperture = 0.25f;
	camera.focusDistance = 3.5f;
	return camera;
}

using ApertureSampler = std::function<glm::vec2(const CameraFrame&, const glm::vec2&, const PiecewiseConstant2D*)>;

//---------------------------------------------------------------- Ray Tracing in One Weekend's camera

void testLegacyCamera()
{
	fmt::println("Ray Tracing in One Weekend's camera");
	RTCameraSnapshot camera = baseCamera();
	camera.lensMapping = LensMapping::Polar;
	const uint32_t width = 320;
	const uint32_t height = 180;
	const CameraFrame frame = buildCameraFrame(camera, width, height);
	check(!frame.needsModel(), "the polar mapping with a circle and no tilt needs no camera model");

	//the camera as GpuRenderer::baseParams() built it before camera kinds existed
	const float aspect = (float)width / (float)height;
	const float h = glm::tan(glm::radians(camera.vfovDegrees) / 2.f);
	const float viewportHeight = 2.f * h;
	const float viewportWidth = aspect * viewportHeight;
	const glm::vec3 t = glm::normalize(camera.lookFrom - camera.lookAt);
	const glm::vec3 n = glm::normalize(glm::cross(camera.vUp, t));
	const glm::vec3 b = glm::cross(t, n);
	const glm::vec3 horizontal = camera.focusDistance * viewportWidth * n;
	const glm::vec3 vertical = camera.focusDistance * viewportHeight * b;
	const glm::vec3 lowerLeft = camera.lookFrom - horizontal / 2.f - vertical / 2.f - camera.focusDistance * t;
	std::mt19937 rng(7);
	std::uniform_real_distribution<float> uniform(0.f, 1.f);
	bool same = frame.lowerLeft == lowerLeft && frame.horizontal == horizontal && frame.vertical == vertical;
	for (int i = 0; i < 10000 && same; i++) {
		const glm::vec2 film(uniform(rng), uniform(rng));
		const glm::vec2 lens(uniform(rng), uniform(rng));
		const CameraRay ray = generateCameraRay(frame, film, lens, nullptr);
		glm::vec3 origin = camera.lookFrom;
		const glm::vec3 target = lowerLeft + film.x * horizontal + film.y * vertical;
		const glm::vec2 rd = camera.aperture / 2.f * polarDiskFromSample(lens);
		origin += rd.x * n + rd.y * b;
		same = ray.origin == origin && ray.direction == target - origin;
	}
	check(same, "the rays are the old camera's, bit for bit");
	camera.lensMapping = LensMapping::Concentric;
	check(buildCameraFrame(camera, width, height).needsModel(), "the concentric mapping with a lens needs the model");
	camera.aperture = 0.f;
	check(!buildCameraFrame(camera, width, height).needsModel(), "a pinhole's mapping does not matter");
}

//---------------------------------------------------------------- apertures

// The sampled points of the unit aperture in a `GRID` x `GRID` grid over [-1, 1]^2, against the counts
// apertureDensity() expects, integrated over each cell at 8 x 8 points. An image's grid divides its table, so that every
// cell holds whole texels
bool apertureChiSquare(const CameraFrame& frame, const PiecewiseConstant2D* table, const ApertureSampler& sampler, const std::string& name, uint32_t samples)
{
	const int GRID = table != nullptr ? (int)table->width / 2 : 32;
	constexpr int SUB = 8;
	std::vector<double> observed(GRID * GRID, 0.0);
	std::vector<double> expected(GRID * GRID, 0.0);
	std::mt19937 rng(1234);
	std::uniform_real_distribution<float> uniform(0.f, 1.f);
	uint32_t outside = 0;
	for (uint32_t i = 0; i < samples; i++) {
		const glm::vec2 p = sampler(frame, glm::vec2(uniform(rng), uniform(rng)), table);
		if (apertureDensity(frame, p, table) <= 0.f) {
			outside++;
		}
		const int x = std::clamp((int)((p.x + 1.f) * 0.5f * GRID), 0, GRID - 1);
		const int y = std::clamp((int)((p.y + 1.f) * 0.5f * GRID), 0, GRID - 1);
		observed[y * GRID + x] += 1.0;
	}
	const double cell = 2.0 / GRID;
	double integral = 0.0;
	for (int y = 0; y < GRID; y++) {
		for (int x = 0; x < GRID; x++) {
			double sum = 0.0;
			for (int sy = 0; sy < SUB; sy++) {
				for (int sx = 0; sx < SUB; sx++) {
					const glm::vec2 p((float)(-1.0 + cell * (x + (sx + 0.5) / SUB)), (float)(-1.0 + cell * (y + (sy + 0.5) / SUB)));
					sum += apertureDensity(frame, p, table);
				}
			}
			const double mass = sum / (SUB * SUB) * cell * cell;
			integral += mass;
			expected[y * GRID + x] = mass * samples;
		}
	}
	//the edge cells' integral is only as good as the 8 x 8 points
	check(std::abs(integral - 1.0) < 2e-3, fmt::format("{}: the density integrates to {:.5f}", name, integral));
	check(outside == 0, fmt::format("{}: {} sample(s) outside the aperture", name, outside));
	const ChiSquare test = chiSquareTest(observed, expected, 20.0);
	const bool passed = test.pValue > 1e-4;
	check(passed, fmt::format("{}: chi-square {:.1f} on {} dof, p = {:.3g}", name, test.statistic, test.dof, test.pValue));
	return passed && outside == 0;
}

// a ring of transmission between radii 0.5 and 0.9 of the square's half side, bright on its left half
PiecewiseConstant2D ringApertureTable()
{
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
	return buildPiecewiseConstant2D(values, SIZE, SIZE);
}

void testApertures()
{
	fmt::println("apertures");
	RTCameraSnapshot camera = baseCamera();
	for (const LensMapping mapping : { LensMapping::Polar, LensMapping::Concentric }) {
		camera.lensMapping = mapping;
		apertureChiSquare(buildCameraFrame(camera, 64, 64), nullptr, sampleAperture, fmt::format("circle, {} mapping", lensMappingId(mapping)), 400000);
	}
	camera.apertureShape = ApertureShape::Polygon;
	for (const uint32_t blades : { 3u, 5u, 6u, 9u }) {
		camera.apertureBlades = blades;
		camera.apertureRotationDegrees = 17.f * (float)blades;
		apertureChiSquare(buildCameraFrame(camera, 64, 64), nullptr, sampleAperture, fmt::format("{}-bladed polygon", blades), 400000);
	}
	//the polygon's area is the one the lens's density is measured over
	camera.apertureBlades = 6;
	const CameraFrame hexagon = buildCameraFrame(camera, 64, 64);
	const float radius = camera.aperture / 2.f;
	check(std::abs(hexagon.lensArea - 1.5f * std::sqrt(3.f) * radius * radius) < 1e-6f, "a hexagon's lens area is 3 sqrt(3) / 2 r^2");

	camera.apertureShape = ApertureShape::Image;
	const PiecewiseConstant2D ring = ringApertureTable();
	apertureChiSquare(buildCameraFrame(camera, 64, 64), &ring, sampleAperture, "ring image", 400000);
	check(std::abs(buildCameraFrame(camera, 64, 64).lensArea - 4.f * radius * radius) < 1e-6f, "an image's lens area is its square");
}

//---------------------------------------------------------------- focus

// every ray through one film point, from 64 lens points, passes the point the chief ray meets the plane of focus
bool raysMeetOnFocusPlane(const CameraFrame& frame, const glm::vec3& planeNormal, float planeOffset, const std::string& name, float plantedError = 0.f)
{
	std::mt19937 rng(99);
	std::uniform_real_distribution<float> uniform(0.f, 1.f);
	float worstMeet = 0.f;
	float worstPlane = 0.f;
	for (int f = 0; f < 200; f++) {
		const glm::vec2 film(uniform(rng), uniform(rng));
		glm::vec3 focus(0.f);
		for (int l = 0; l < 64; l++) {
			const CameraRay ray = generateCameraRay(frame, film, glm::vec2(uniform(rng), uniform(rng)), nullptr);
			const glm::vec3 point = ray.origin + ray.direction + glm::vec3(l == 0 ? plantedError : 0.f, 0.f, 0.f);
			if (l == 0) {
				focus = point;
				worstPlane = std::max(worstPlane, std::abs(glm::dot(planeNormal, point) - planeOffset));
			}
			worstMeet = std::max(worstMeet, glm::length(point - focus));
		}
	}
	const bool met = worstMeet < 1e-4f;
	check(met, fmt::format("{}: the rays of a film point meet within {:.3g}", name, worstMeet));
	check(worstPlane < 1e-4f, fmt::format("{}: the meeting points are {:.3g} off the plane of focus", name, worstPlane));
	return met;
}

void testFocus()
{
	fmt::println("focus");
	RTCameraSnapshot camera = baseCamera();
	const glm::vec3 axis = glm::normalize(camera.lookFrom - camera.lookAt);
	const glm::vec3 pivot = camera.lookFrom - camera.focusDistance * axis;
	for (const ApertureShape shape : { ApertureShape::Circle, ApertureShape::Polygon }) {
		camera.apertureShape = shape;
		raysMeetOnFocusPlane(buildCameraFrame(camera, 200, 100), axis, glm::dot(axis, pivot), fmt::format("perspective, {}", apertureShapeId(shape)));
	}
	camera.apertureShape = ApertureShape::Circle;
	camera.kind = CameraKind::Orthographic;
	camera.orthoHeight = 1.5f;
	const CameraFrame ortho = buildCameraFrame(camera, 200, 100);
	raysMeetOnFocusPlane(ortho, axis, glm::dot(axis, pivot), "orthographic");

	//a tilt turns the plane of focus about the pivot
	camera.kind = CameraKind::Perspective;
	camera.tiltDegrees = glm::vec2(12.f, -7.f);
	const CameraFrame tilted = buildCameraFrame(camera, 200, 100);
	const glm::vec3 normal(tilted.focusPlane);
	check(std::abs(glm::dot(normal, pivot) - tilted.focusPlane.w) < 1e-5f, "the tilted plane passes through the pivot");
	raysMeetOnFocusPlane(tilted, normal, tilted.focusPlane.w, "tilted perspective");
	//a positive tilt about x focuses farther at the top of the image than at the bottom
	auto depthAt = [&](float v) {
		const CameraRay ray = generateCameraRay(tilted, glm::vec2(0.5f, v), glm::vec2(0.5f), nullptr);
		return glm::dot(ray.origin + ray.direction - camera.lookFrom, -axis);
	};
	check(depthAt(0.9f) > depthAt(0.1f), fmt::format("a positive tilt recedes at the top ({:.3f} against {:.3f})", depthAt(0.9f), depthAt(0.1f)));
	camera.kind = CameraKind::Orthographic;
	const CameraFrame tiltedOrtho = buildCameraFrame(camera, 200, 100);
	raysMeetOnFocusPlane(tiltedOrtho, glm::vec3(tiltedOrtho.focusPlane), tiltedOrtho.focusPlane.w, "tilted orthographic");
}

//---------------------------------------------------------------- orthographic and shift

void testOrthographicAndShift()
{
	fmt::println("orthographic and shift");
	RTCameraSnapshot camera = baseCamera();
	camera.aperture = 0.f;
	camera.kind = CameraKind::Orthographic;
	camera.orthoHeight = 2.5f;
	const CameraFrame frame = buildCameraFrame(camera, 300, 150);
	const glm::vec3 forward = -glm::normalize(camera.lookFrom - camera.lookAt);
	float worstParallel = 0.f;
	for (const glm::vec2 film : { glm::vec2(0.f), glm::vec2(1.f), glm::vec2(0.3f, 0.8f), glm::vec2(0.9f, 0.1f) }) {
		const CameraRay ray = generateCameraRay(frame, film, glm::vec2(0.5f), nullptr);
		worstParallel = std::max(worstParallel, glm::length(glm::normalize(ray.direction) - forward));
	}
	check(worstParallel < 1e-6f, fmt::format("the chief rays are parallel to the axis ({:.3g})", worstParallel));
	const CameraRay bottom = generateCameraRay(frame, glm::vec2(0.5f, 0.f), glm::vec2(0.5f), nullptr);
	const CameraRay top = generateCameraRay(frame, glm::vec2(0.5f, 1.f), glm::vec2(0.5f), nullptr);
	check(std::abs(glm::length(top.origin - bottom.origin) - camera.orthoHeight) < 1e-5f, "the view volume is as tall as asked");
	const CameraRay left = generateCameraRay(frame, glm::vec2(0.f, 0.5f), glm::vec2(0.5f), nullptr);
	const CameraRay right = generateCameraRay(frame, glm::vec2(1.f, 0.5f), glm::vec2(0.5f), nullptr);
	check(std::abs(glm::length(right.origin - left.origin) - 2.f * camera.orthoHeight) < 1e-5f, "and as wide as the render's aspect makes it");
	check(frame.pixelWidth == camera.orthoHeight / 150.f && frame.pixelSpread == 0.f, "a ray cone starts a pixel wide and does not spread");

	//the shift moves the film by image heights on both axes
	camera.kind = CameraKind::Perspective;
	camera.shift = glm::vec2(0.f);
	const CameraFrame centered = buildCameraFrame(camera, 300, 150);
	camera.shift = glm::vec2(0.25f, -0.5f);
	const CameraFrame shifted = buildCameraFrame(camera, 300, 150);
	const glm::vec3 moved = generateCameraRay(shifted, glm::vec2(0.5f), glm::vec2(0.5f), nullptr).direction
		- generateCameraRay(centered, glm::vec2(0.5f), glm::vec2(0.5f), nullptr).direction;
	const float imageHeight = glm::length(centered.vertical);
	const glm::vec3 expected = 0.25f * imageHeight * centered.lensU - 0.5f * imageHeight * centered.lensV;
	check(glm::length(moved - expected) < 1e-5f, "the shift moves the film by image heights");
}

//---------------------------------------------------------------- spherical

void testSpherical()
{
	fmt::println("spherical");
	RTCameraSnapshot camera;
	camera.kind = CameraKind::Spherical;
	camera.lookFrom = glm::vec3(1.f, 2.f, 3.f);
	camera.lookAt = camera.lookFrom + glm::vec3(0.f, 0.f, -1.f);
	camera.mapping = SphericalMapping::Equirectangular;
	const CameraFrame frame = buildCameraFrame(camera, 256, 128);
	check(frame.lensRadius == 0.f, "a spherical camera has no lens");
	//the environment map's layout: equirectUvOf() reads the direction back to the pixel, rows top-down
	float worst = 0.f;
	for (int i = 1; i < 32; i++) {
		for (int j = 1; j < 16; j++) {
			const glm::vec2 film((float)i / 32.f, (float)j / 16.f);
			const CameraRay ray = generateCameraRay(frame, film, glm::vec2(0.5f), nullptr);
			const glm::vec2 uv = equirectUvOf(glm::normalize(ray.direction));
			worst = std::max(worst, glm::length(uv - glm::vec2(film.x, 1.f - film.y)));
		}
	}
	check(worst < 1e-5f, fmt::format("an equirectangular image is the environment map's layout ({:.3g})", worst));
	check(glm::length(generateCameraRay(frame, glm::vec2(0.5f), glm::vec2(0.5f), nullptr).direction - glm::vec3(0.f, 0.f, -1.f)) < 1e-6f,
		"its center looks down the camera's axis");

	//equal-area: random directions fall evenly over the pixels of a 16 x 16 image
	camera.mapping = SphericalMapping::EqualArea;
	const CameraFrame equalArea = buildCameraFrame(camera, 16, 16);
	constexpr int GRID = 16;
	std::vector<double> observed(GRID * GRID, 0.0);
	std::mt19937 rng(5);
	std::uniform_real_distribution<float> uniform(0.f, 1.f);
	const uint32_t samples = 200000;
	for (uint32_t i = 0; i < samples; i++) {
		const float z = 1.f - 2.f * uniform(rng);
		const float phi = 2.f * 3.14159265f * uniform(rng);
		const float r = std::sqrt(std::max(0.f, 1.f - z * z));
		const glm::vec3 d(r * std::cos(phi), r * std::sin(phi), z);
		//the camera's direction (x, y, z) is the square's (x, -z, y)
		const glm::vec2 uv = equalAreaSphereToSquare(glm::vec3(d.x, -d.z, d.y));
		const int x = std::clamp((int)(uv.x * GRID), 0, GRID - 1);
		const int y = std::clamp((int)(uv.y * GRID), 0, GRID - 1);
		observed[y * GRID + x] += 1.0;
	}
	const ChiSquare test = chiSquareTest(observed, std::vector<double>(GRID * GRID, (double)samples / (GRID * GRID)));
	check(test.pValue > 1e-4, fmt::format("equal-area pixels subtend equal solid angles (p = {:.3g})", test.pValue));
	float roundTrip = 0.f;
	for (int i = 0; i < 16; i++) {
		for (int j = 0; j < 16; j++) {
			const glm::vec2 film(((float)i + 0.5f) / 16.f, ((float)j + 0.5f) / 16.f);
			const glm::vec3 d = glm::normalize(generateCameraRay(equalArea, film, glm::vec2(0.5f), nullptr).direction);
			const glm::vec2 uv = equalAreaSphereToSquare(glm::vec3(d.x, -d.z, d.y));
			roundTrip = std::max(roundTrip, glm::length(uv - glm::vec2(film.x, 1.f - film.y)));
		}
	}
	check(roundTrip < 1e-4f, fmt::format("an equal-area pixel's direction maps back to it ({:.3g})", roundTrip));
}

//---------------------------------------------------------------- a detector's sensor

// the mean cosine of a sensor's rays to its front, and the chi-square of their cos^2, uniform for a cosine-weighted
// hemisphere: the lens sample through `warp` first, which a selfcheck bends
bool sensorCosines(const CameraFrame& frame, const glm::vec3& front, const std::function<glm::vec2(glm::vec2)>& warp, const std::string& name)
{
	std::mt19937 rng(11);
	std::uniform_real_distribution<float> uniform(0.f, 1.f);
	constexpr int BINS = 20;
	std::vector<double> observed(BINS, 0.0);
	const uint32_t samples = 200000;
	double meanCos = 0.0;
	bool behind = false;
	for (uint32_t i = 0; i < samples; i++) {
		const CameraRay ray = generateCameraRay(frame, glm::vec2(uniform(rng), uniform(rng)), warp(glm::vec2(uniform(rng), uniform(rng))), nullptr);
		const float c = glm::dot(glm::normalize(ray.direction), front);
		behind |= c < 0.f;
		meanCos += c;
		observed[std::clamp((int)(c * c * BINS), 0, BINS - 1)] += 1.0;
	}
	meanCos /= samples;
	const ChiSquare test = chiSquareTest(observed, std::vector<double>(BINS, (double)samples / BINS));
	const bool ok = !behind && std::abs(meanCos - 2.0 / 3.0) < 3e-3 && test.pValue > 1e-4;
	check(ok, fmt::format("{}: rays cosine-weighted over the front hemisphere (mean cosine {:.4f} of 2/3, p = {:.3g}{})", name, meanCos, test.pValue, behind ? ", some behind" : ""));
	return ok;
}

void testSensor()
{
	fmt::println("sensor");
	RTCameraSnapshot camera;
	camera.kind = CameraKind::Sensor;
	camera.lookFrom = glm::vec3(0.1f, 0.2f, 0.3f);
	const glm::vec3 front = glm::normalize(glm::vec3(0.3f, 0.f, -1.f));
	camera.lookAt = camera.lookFrom + front;
	camera.vUp = glm::vec3(0.f, 1.f, 0.f);
	camera.sensorSize = glm::vec2(0.012f, 0.008f);
	const CameraFrame frame = buildCameraFrame(camera, 300, 200);
	check(frame.lensRadius == 0.f && frame.needsModel() && !cameraConnects(frame), "a sensor has no lens, needs the camera model and takes no connections");
	check(!cameraTraits(CameraKind::Sensor).sceneCamera && cameraKindFromId("sensor") == CameraKind::Sensor, "a sensor is no scene camera's kind");
	//its film is the sensor: x along the sensor's +x (vUp x front, seen from behind), y up, centered on lookFrom
	const glm::vec3 x = glm::normalize(glm::cross(front, camera.vUp));
	const glm::vec3 y = glm::cross(x, front);
	float worst = 0.f;
	for (const glm::vec2 film : { glm::vec2(0.f), glm::vec2(1.f, 0.f), glm::vec2(0.f, 1.f), glm::vec2(1.f), glm::vec2(0.25f, 0.75f) }) {
		const CameraRay ray = generateCameraRay(frame, film, glm::vec2(0.5f), nullptr);
		const glm::vec3 expected = camera.lookFrom + (film.x - 0.5f) * camera.sensorSize.x * x + (film.y - 0.5f) * camera.sensorSize.y * y;
		const float lead = glm::dot(ray.origin - expected, front);
		worst = std::max(worst, glm::length(ray.origin - expected - lead * front));
		check(lead > 5e-6f && lead < 2e-5f, fmt::format("a sensor ray starts just in front of the sensor ({:.3g} m)", lead));
	}
	check(worst < 1e-6f, fmt::format("a sensor's film is the sensor, x right and y up ({:.3g} m off)", worst));
	check(glm::length(generateCameraRay(frame, glm::vec2(0.5f), glm::vec2(0.5f), nullptr).direction - front) < 1e-5f, "the lens sample's center leaves along the front's normal");
	sensorCosines(frame, front, [](glm::vec2 u) { return u; }, "sensor");
}

//---------------------------------------------------------------- camera importance

void testImportance()
{
	fmt::println("camera importance");
	RTCameraSnapshot camera = baseCamera();
	camera.shift = glm::vec2(0.1f, -0.05f);
	const PiecewiseConstant2D ring = ringApertureTable();
	struct Case {
		const char* name;
		float aperture;
		ApertureShape shape;
	};
	for (const Case& c : { Case { "pinhole", 0.f, ApertureShape::Circle }, Case { "circle", 0.25f, ApertureShape::Circle }, Case { "hexagon", 0.25f, ApertureShape::Polygon },
	                       Case { "ring image", 0.25f, ApertureShape::Image } }) {
		camera.aperture = c.aperture;
		camera.apertureShape = c.shape;
		camera.apertureBlades = 6;
		const CameraFrame frame = buildCameraFrame(camera, 200, 120);
		const PiecewiseConstant2D* aperture = c.shape == ApertureShape::Image ? &ring : nullptr;
		check(cameraConnects(frame), std::string(c.name) + ": a perspective camera connects");

		//the integral of We cos over the lens and the hemisphere in front of it, and of the directional density, by Monte
		//Carlo: lens points by the aperture's own density, directions uniform over the hemisphere
		std::mt19937 rng(42);
		std::uniform_real_distribution<float> uniform(0.f, 1.f);
		const glm::vec3 forward = -frame.backward;
		double weSum = 0.0;
		double pdfSum = 0.0;
		const int samples = 2000000;
		for (int i = 0; i < samples; i++) {
			glm::vec3 lensPoint = frame.origin;
			if (frame.lensRadius > 0.f) {
				const glm::vec2 rd = frame.lensRadius * sampleAperture(frame, glm::vec2(uniform(rng), uniform(rng)), aperture);
				lensPoint += rd.x * frame.lensU + rd.y * frame.lensV;
			}
			const float z = uniform(rng);
			const float phi = 2.f * 3.14159265f * uniform(rng);
			const float r = std::sqrt(std::max(0.f, 1.f - z * z));
			const glm::vec3 direction = r * std::cos(phi) * frame.lensU + r * std::sin(phi) * frame.lensV + z * forward;
			glm::vec2 film;
			const float we = cameraWe(frame, lensPoint, direction, film, aperture);
			float pdfPos;
			float pdfDir;
			cameraPdfWe(frame, lensPoint, direction, pdfPos, pdfDir, aperture);
			const float density = lensDensity(frame, lensPoint, aperture);
			if (density > 0.f) {
				weSum += we * z / density * 2.0 * 3.14159265;
			}
			pdfSum += pdfDir * 2.0 * 3.14159265;
		}
		const double weIntegral = weSum / samples;
		const double pdfIntegral = pdfSum / samples;
		check(std::abs(weIntegral - 1.0) < 0.01, fmt::format("{}: We integrates to {:.4f}", c.name, weIntegral));
		check(std::abs(pdfIntegral - 1.0) < 0.01, fmt::format("{}: the directional density integrates to {:.4f}", c.name, pdfIntegral));

		//SampleWi's lens point and film point give back the ray through the scene point
		float worstDirection = 0.f;
		float worstOrigin = 0.f;
		int connected = 0;
		for (int i = 0; i < 2000; i++) {
			const glm::vec2 film(uniform(rng), uniform(rng));
			const glm::vec3 ref = generateCameraRay(frame, film, glm::vec2(0.5f), nullptr).origin
				+ (0.5f + 3.f * uniform(rng)) * glm::normalize(generateCameraRay(frame, film, glm::vec2(0.5f), nullptr).direction);
			const glm::vec2 u(uniform(rng), uniform(rng));
			CameraWiSample sample;
			if (!cameraSampleWi(frame, ref, u, aperture, sample)) {
				continue;
			}
			connected++;
			const CameraRay ray = generateCameraRay(frame, sample.film, u, aperture);
			worstOrigin = std::max(worstOrigin, glm::length(ray.origin - sample.lensPoint));
			worstDirection = std::max(worstDirection, glm::length(glm::normalize(ray.direction) + sample.wi));
		}
		check(connected > 1000, fmt::format("{}: {} of 2000 scene points in view connect", c.name, connected));
		check(worstOrigin < 1e-5f && worstDirection < 1e-3f, fmt::format("{}: the film point gives back the ray (origin {:.3g}, direction {:.3g})", c.name, worstOrigin, worstDirection));
	}
	camera.tiltDegrees = glm::vec2(5.f, 0.f);
	check(!cameraConnects(buildCameraFrame(camera, 200, 120)), "a tilted camera does not connect");
	camera.tiltDegrees = glm::vec2(0.f);
	camera.kind = CameraKind::Orthographic;
	check(!cameraConnects(buildCameraFrame(camera, 200, 120)), "an orthographic camera does not connect");
}

//---------------------------------------------------------------- the planted errors

bool selfcheck()
{
	fmt::println("selfcheck");
	g_quiet = true;
	const int before = g_failures;
	RTCameraSnapshot camera = baseCamera();
	camera.apertureShape = ApertureShape::Polygon;
	camera.apertureBlades = 5;
	//a sampler that puts 10 percent too many points in the first triangle of the fan
	const ApertureSampler biased = [](const CameraFrame& frame, const glm::vec2& u, const PiecewiseConstant2D* table) {
		const glm::vec2 skewed(u.x < 0.22f ? u.x * (0.2f / 0.22f) : 0.2f + (u.x - 0.22f) * (0.8f / 0.78f), u.y);
		return sampleAperture(frame, skewed, table);
	};
	const bool polygonCaught = !apertureChiSquare(buildCameraFrame(camera, 64, 64), nullptr, biased, "planted", 400000);
	camera.apertureShape = ApertureShape::Circle;
	const glm::vec3 axis = glm::normalize(camera.lookFrom - camera.lookAt);
	const glm::vec3 pivot = camera.lookFrom - camera.focusDistance * axis;
	const bool focusCaught = !raysMeetOnFocusPlane(buildCameraFrame(camera, 64, 64), axis, glm::dot(axis, pivot), "planted", 1e-3f);
	//a sensor whose rays are uniform over the hemisphere instead of cosine-weighted (the disk's radius squared)
	RTCameraSnapshot sensor;
	sensor.kind = CameraKind::Sensor;
	sensor.lookAt = glm::vec3(0.f, 0.f, -1.f);
	sensor.sensorSize = glm::vec2(0.01f);
	const bool sensorCaught = !sensorCosines(buildCameraFrame(sensor, 64, 64), glm::vec3(0.f, 0.f, -1.f), [](glm::vec2 u) { return glm::vec2(u.x * u.x, u.y); }, "planted");
	g_quiet = false;
	g_failures = before;
	fmt::println("    a polygon sampler favoring one triangle: {}", polygonCaught ? "caught" : "NOT caught");
	fmt::println("    a focus point a millimeter off: {}", focusCaught ? "caught" : "NOT caught");
	fmt::println("    a sensor's rays not cosine-weighted: {}", sensorCaught ? "caught" : "NOT caught");
	return polygonCaught && focusCaught && sensorCaught;
}

}

int main(int argc, char** argv)
{
	bool selfcheckOnly = false;
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--selfcheck") == 0) {
			selfcheckOnly = true;
		} else {
			fmt::println("usage: camera_test [--selfcheck]");
			return 2;
		}
	}

	testLegacyCamera();
	testApertures();
	testFocus();
	testOrthographicAndShift();
	testSpherical();
	testSensor();
	testImportance();

	if (selfcheckOnly && !selfcheck()) {
		fmt::println("camera_test: --selfcheck FAILED, a planted error was not caught");
		return 1;
	}
	if (g_failures == 0) {
		fmt::println("camera_test: all checks passed");
		return 0;
	}
	fmt::println("camera_test: {} check(s) FAILED", g_failures);
	return 1;
}
