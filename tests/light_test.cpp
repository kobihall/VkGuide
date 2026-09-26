// CPU verification of the light sampling that next-event estimation and MIS rest on
// (src/light_sampling.h, the line-for-line mirror of shaders/rt/include/crt_light.glsl). No Vulkan,
// no GPU.
//
// For every kind of area light, over random placements and reference points:
//  - the pdf a sample reports equals the pdf function evaluated at the sampled point. Kernel 06
//    uses the first, kernel 04 the second, and MIS is only unbiased when they agree
//  - the solid angle estimated through light samples (the visible ones, each weighted 1 / pdf)
//    matches an independent reference. That holds only if the pdf is the true density of the
//    visible points, which is what the light-sampling estimator needs
// For the environment map's tables: sample and pdf agree, both integrate to 1 over the sphere, and
// the radiance estimate through light samples matches quadrature (the full table only: the
// compensated one covers the bright directions alone, by design).
// For the alias table: sampled frequencies against the weights.
// For the environment map's half-float GPU copy: a map brighter than 65504 stays finite and within
// half float's precision of the file, and a map that fits is stored unscaled.
//
// Run:  ./bin/light_test [--samples N]

#include <light_sampling.h>

#include <bvh_layout.h>
#include <shape.h>

#include <cmath>
#include <cstring>
#include <random>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/packing.hpp>

namespace {

std::mt19937 g_rng(20260925);
std::uniform_real_distribution<float> g_unit(0.f, 1.f);

float uniform()
{
	return g_unit(g_rng);
}

glm::vec3 uniformSphere()
{
	const float z = 1.f - 2.f * uniform();
	const float r = std::sqrt(std::max(0.f, 1.f - z * z));
	const float phi = 2.f * LIGHT_PI * uniform();
	return glm::vec3(r * std::cos(phi), r * std::sin(phi), z);
}

int g_failures = 0;

void check(bool ok, const std::string& what)
{
	if (!ok) {
		fmt::println("    FAIL: {}", what);
		g_failures++;
	}
}

// One placed emitter as the tests see it: its light records, and the shape or triangle that a ray
// can hit, for the visibility test and the reference solid angle
struct Emitter {
	std::string name;
	std::vector<AreaLightGeometry> lights;
	bool isTriangle { false };
	ShapeKind shape { ShapeKind::Sphere };
	glm::mat4 objectToWorld { 1.f };
	glm::mat4 worldToObject { 1.f };
	BvhTriangle triangle {};
	// a sphere around the emitter, for the reference solid angle
	glm::vec3 boundCenter { 0.f };
	float boundRadius { 0.f };
};

// the nearest hit of a world-space ray on the emitter, or false
bool hitEmitter(const Emitter& emitter, const glm::vec3& origin, const glm::vec3& direction, float& t)
{
	if (emitter.isTriangle) {
		glm::vec2 barycentrics;
		return intersectBvhTriangle(emitter.triangle, origin, direction, 1e-6f, 1e30f, t, barycentrics);
	}
	const glm::vec3 localOrigin = glm::vec3(emitter.worldToObject * glm::vec4(origin, 1.f));
	const glm::vec3 localDirection = glm::mat3(emitter.worldToObject) * direction;
	return intersectShape(emitter.shape, localOrigin, localDirection, 1e-6f, 1e30f, t);
}

glm::mat4 randomPlacement(ShapeKind kind)
{
	const glm::vec3 position(uniform() * 4.f - 2.f, uniform() * 4.f - 2.f, uniform() * 4.f - 2.f);
	const glm::vec3 axis = glm::normalize(uniformSphere() + glm::vec3(1e-3f));
	glm::mat4 rotation = glm::rotate(glm::mat4(1.f), uniform() * 6.28f, axis);
	glm::vec3 size(0.1f + uniform() * 1.5f, 0.1f + uniform() * 1.5f, 0.1f + uniform() * 1.5f);
	//the same constraints the editor imposes (rt_scene_types.h SHAPE_TRAITS)
	if (kind == ShapeKind::Sphere) {
		rotation = glm::mat4(1.f);
		size = glm::vec3(size.x);
	} else if (kind == ShapeKind::Cylinder) {
		size.z = size.x;
	} else if (kind == ShapeKind::Quad) {
		size.y = 1.f;
	}
	return glm::translate(glm::mat4(1.f), position) * rotation * glm::scale(glm::mat4(1.f), size);
}

const char* shapeName(ShapeKind kind)
{
	switch (kind) {
	case ShapeKind::Sphere:
		return "sphere";
	case ShapeKind::Plane:
		return "plane";
	case ShapeKind::Quad:
		return "quad";
	case ShapeKind::Box:
		return "box";
	case ShapeKind::Cylinder:
		return "cylinder";
	}
	return "shape";
}

Emitter shapeEmitter(ShapeKind kind)
{
	Emitter emitter;
	emitter.name = shapeName(kind);
	emitter.shape = kind;
	emitter.objectToWorld = randomPlacement(kind);
	emitter.worldToObject = glm::inverse(emitter.objectToWorld);
	emitter.lights = shapeLightGeometry(kind, emitter.objectToWorld);
	const Aabb box = shapeObjectBounds(kind);
	emitter.boundCenter = glm::vec3(emitter.objectToWorld * glm::vec4(box.center(), 1.f));
	for (int corner = 0; corner < 8; corner++) {
		const glm::vec3 p((corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y, (corner & 4) ? box.max.z : box.min.z);
		emitter.boundRadius = std::max(emitter.boundRadius, glm::length(glm::vec3(emitter.objectToWorld * glm::vec4(p, 1.f)) - emitter.boundCenter));
	}
	return emitter;
}

Emitter triangleEmitter()
{
	Emitter emitter;
	emitter.name = "triangle";
	emitter.isTriangle = true;
	const glm::vec3 center(uniform() * 4.f - 2.f, uniform() * 4.f - 2.f, uniform() * 4.f - 2.f);
	const float scale = 0.2f + uniform() * 2.f;
	AreaLightGeometry light;
	light.kind = AreaLightKind::Triangle;
	light.a = center + scale * uniformSphere();
	light.b = center + scale * uniformSphere();
	light.c = center + scale * uniformSphere();
	light.area = 0.5f * glm::length(glm::cross(light.b - light.a, light.c - light.a));
	emitter.lights.push_back(light);
	emitter.triangle = makeBvhTriangle(light.a, light.b, light.c, 0);
	emitter.boundCenter = (light.a + light.b + light.c) / 3.f;
	emitter.boundRadius = std::max({ glm::length(light.a - emitter.boundCenter), glm::length(light.b - emitter.boundCenter), glm::length(light.c - emitter.boundCenter) });
	return emitter;
}

// a reference point: usually somewhere around the emitter, sometimes right against it
glm::vec3 referencePoint(const Emitter& emitter)
{
	const float distance = emitter.boundRadius * (uniform() < 0.25f ? 1.02f + 0.2f * uniform() : 1.2f + 4.f * uniform());
	return emitter.boundCenter + distance * uniformSphere();
}

// the solid angle the emitter covers from `ref`, by directions uniform in the cone around its bounding
// sphere - an estimate that knows nothing of the light sampling
double referenceSolidAngle(const Emitter& emitter, const glm::vec3& ref, size_t samples, double& standardError)
{
	const glm::vec3 axis = glm::normalize(emitter.boundCenter - ref);
	const float sinThetaMax = std::min(emitter.boundRadius / glm::length(emitter.boundCenter - ref), 1.f);
	const float cosThetaMax = std::sqrt(std::max(0.f, 1.f - sinThetaMax * sinThetaMax));
	const double coneSolidAngle = 2.0 * LIGHT_PI * (1.0 - cosThetaMax);
	glm::vec3 frameX;
	glm::vec3 frameY;
	lightFrameFromZ(axis, frameX, frameY);
	size_t hits = 0;
	for (size_t i = 0; i < samples; i++) {
		const float cosTheta = 1.f - uniform() * (1.f - cosThetaMax);
		const float sinTheta = std::sqrt(std::max(0.f, 1.f - cosTheta * cosTheta));
		const float phi = 2.f * LIGHT_PI * uniform();
		const glm::vec3 direction = sinTheta * std::cos(phi) * frameX + sinTheta * std::sin(phi) * frameY + cosTheta * axis;
		float t;
		hits += hitEmitter(emitter, ref, direction, t) ? 1 : 0;
	}
	const double fraction = (double)hits / (double)samples;
	standardError = coneSolidAngle * std::sqrt(fraction * (1.0 - fraction) / (double)samples);
	return coneSolidAngle * fraction;
}

void testEmitter(const Emitter& emitter, size_t samples)
{
	for (int trial = 0; trial < 6; trial++) {
		const glm::vec3 ref = referencePoint(emitter);

		//every light record of the emitter; a box's six faces together cover what the box covers
		double lightSolidAngle = 0.0;
		double lightVariance = 0.0;
		size_t pdfMismatches = 0;
		size_t valid = 0;
		for (const AreaLightGeometry& light : emitter.lights) {
			double sum = 0.0;
			double sumSquares = 0.0;
			for (size_t i = 0; i < samples; i++) {
				LightSample sample;
				glm::vec3 barycentrics;
				if (!sampleAreaLight(light, ref, glm::vec2(uniform(), uniform()), sample, barycentrics)) {
					continue;
				}
				valid++;
				const float pdf = areaLightPdf(light, ref, sample.point);
				if (!(std::abs(pdf - sample.pdf) <= 2e-3f * sample.pdf)) {
					if (pdfMismatches < 3) {
						fmt::println("    {} trial {}: sample pdf {} but pdf() {}", emitter.name, trial, sample.pdf, pdf);
					}
					pdfMismatches++;
				}
				//only a sample the ray reaches unobstructed by the emitter itself counts
				float t;
				const bool visible = hitEmitter(emitter, ref, sample.direction, t) && std::abs(t - sample.distance) <= 1e-3f * std::max(1.f, sample.distance);
				const double value = visible ? 1.0 / (double)sample.pdf : 0.0;
				sum += value;
				sumSquares += value * value;
			}
			const double mean = sum / (double)samples;
			lightSolidAngle += mean;
			lightVariance += std::max(0.0, sumSquares / (double)samples - mean * mean) / (double)samples;
		}

		double referenceError;
		const double reference = referenceSolidAngle(emitter, ref, samples * 4, referenceError);
		const double tolerance = 4.0 * std::sqrt(lightVariance + referenceError * referenceError) + 1e-4 * reference;
		const bool agrees = std::abs(lightSolidAngle - reference) <= tolerance;
		if (!agrees || pdfMismatches > samples / 10000) {
			fmt::println("    {} trial {}: solid angle by light samples {:.5f} vs reference {:.5f} (+-{:.5f}), {} pdf mismatches of {}",
				emitter.name, trial, lightSolidAngle, reference, tolerance, pdfMismatches, valid);
		}
		check(agrees, fmt::format("{}: light-sampled solid angle disagrees with the reference", emitter.name));
		check(pdfMismatches <= samples / 10000, fmt::format("{}: sample pdf disagrees with the pdf function", emitter.name));
	}
}

// a synthetic map: a sky gradient, a black band, and a small very bright sun
std::vector<float> syntheticEnvironment(uint32_t width, uint32_t height)
{
	std::vector<float> rgba((size_t)width * height * 4);
	const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, 0.5f, -0.8f));
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			const glm::vec2 uv(((float)x + 0.5f) / (float)width, ((float)y + 0.5f) / (float)height);
			const glm::vec3 direction = equirectDirectionOf(uv);
			glm::vec3 color = glm::mix(glm::vec3(1.f), glm::vec3(0.4f, 0.6f, 1.f), 0.5f * (direction.y + 1.f));
			if (direction.y < -0.2f && direction.y > -0.5f) {
				color = glm::vec3(0.f);
			}
			if (glm::dot(direction, sun) > 0.9995f) {
				color = glm::vec3(20000.f, 18000.f, 15000.f);
			}
			float* texel = &rgba[((size_t)y * width + x) * 4];
			texel[0] = color.r;
			texel[1] = color.g;
			texel[2] = color.b;
			texel[3] = 1.f;
		}
	}
	return rgba;
}

// the map's luminance by bilinear filtering with wrapping on both axes, as the renderer samples it
float lookupLuminance(const std::vector<float>& rgba, uint32_t width, uint32_t height, const glm::vec3& direction)
{
	const glm::vec2 uv = equirectUvOf(direction);
	const float fx = uv.x * (float)width - 0.5f;
	const float fy = uv.y * (float)height - 0.5f;
	const int x0 = (int)std::floor(fx);
	const int y0 = (int)std::floor(fy);
	const float tx = fx - (float)x0;
	const float ty = fy - (float)y0;
	auto texel = [&](int x, int y) {
		const int wx = ((x % (int)width) + (int)width) % (int)width;
		const int wy = ((y % (int)height) + (int)height) % (int)height;
		const float* t = &rgba[((size_t)wy * width + wx) * 4];
		return 0.2126f * t[0] + 0.7152f * t[1] + 0.0722f * t[2];
	};
	return (1.f - ty) * ((1.f - tx) * texel(x0, y0) + tx * texel(x0 + 1, y0)) + ty * ((1.f - tx) * texel(x0, y0 + 1) + tx * texel(x0 + 1, y0 + 1));
}

void testEnvironment(uint32_t width, uint32_t height, uint32_t maxWidth, size_t samples)
{
	const std::vector<float> rgba = syntheticEnvironment(width, height);
	const EnvironmentDistribution distribution = buildEnvironmentDistribution(rgba.data(), width, height, maxWidth);
	fmt::println("  {} x {} map, table {} x {}", width, height, distribution.full.width, distribution.full.height);

	//the reference integral of the luminance over the sphere, by stratified quadrature on the map
	double reference = 0.0;
	const uint32_t steps = 2048;
	for (uint32_t j = 0; j < steps; j++) {
		const float v = ((float)j + 0.5f) / (float)steps;
		const double cosLatitude = std::cos((0.5 - v) * LIGHT_PI);
		for (uint32_t i = 0; i < steps * 2; i++) {
			const float u = ((float)i + 0.5f) / (float)(steps * 2);
			reference += lookupLuminance(rgba, width, height, equirectDirectionOf(glm::vec2(u, v))) * cosLatitude;
		}
	}
	reference *= (2.0 * LIGHT_PI / (steps * 2)) * (LIGHT_PI / steps);

	for (int which = 0; which < 2; which++) {
		const PiecewiseConstant2D& table = which == 0 ? distribution.full : distribution.compensated;
		const char* name = which == 0 ? "full" : "compensated";

		size_t mismatches = 0;
		double estimate = 0.0;
		for (size_t i = 0; i < samples; i++) {
			glm::vec3 direction;
			float pdf;
			if (!sampleEnvironment(table, glm::vec2(uniform(), uniform()), direction, pdf)) {
				continue;
			}
			const float lookup = environmentPdf(table, direction);
			if (!(std::abs(lookup - pdf) <= 1e-3f * pdf)) {
				mismatches++;
			}
			estimate += lookupLuminance(rgba, width, height, direction) / pdf;
		}
		estimate /= (double)samples;

		//integrates to 1 over the sphere, by the same quadrature: uniformly random directions would
		//almost never land in the sun, which holds half of the table's probability
		double normalisation = 0.0;
		for (uint32_t j = 0; j < steps; j++) {
			const float v = ((float)j + 0.5f) / (float)steps;
			const double cosLatitude = std::cos((0.5 - v) * LIGHT_PI);
			for (uint32_t i = 0; i < steps * 2; i++) {
				const float u = ((float)i + 0.5f) / (float)(steps * 2);
				normalisation += environmentPdf(table, equirectDirectionOf(glm::vec2(u, v))) * cosLatitude;
			}
		}
		normalisation *= (2.0 * LIGHT_PI / (steps * 2)) * (LIGHT_PI / steps);

		fmt::println("    {:<12} pdf mismatches {:>4}, integral of pdf {:.4f}, luminance by light samples {:.2f} vs quadrature {:.2f}", name, mismatches, normalisation, estimate, reference);
		check(mismatches <= samples / 10000, fmt::format("environment {}: sample pdf disagrees with the pdf function", name));
		check(std::abs(normalisation - 1.0) < 2e-3, fmt::format("environment {}: pdf does not integrate to 1", name));
		if (which == 0) {
			check(std::abs(estimate - reference) < 0.02 * reference, "environment full: the light-sampled estimate is biased");
		}
	}
}

void testAliasTable(size_t samples)
{
	std::vector<float> weights { 0.f, 1.f, 5.f, 0.25f, 0.f, 30.f, 2.f, 2.f, 0.001f };
	const std::vector<AliasEntry> table = buildAliasTable(weights);
	std::vector<size_t> counts(weights.size(), 0);
	for (size_t i = 0; i < samples; i++) {
		counts[sampleAliasTable(table, uniform())]++;
	}
	double total = 0.0;
	for (float w : weights) {
		total += w;
	}
	bool ok = true;
	for (size_t i = 0; i < weights.size(); i++) {
		const double p = weights[i] / total;
		const double expected = p * (double)samples;
		const double sigma = std::sqrt(std::max(expected * (1.0 - p), 1.0));
		if (std::abs((double)counts[i] - expected) > 5.0 * sigma) {
			fmt::println("    alias outcome {}: {} samples, expected {:.0f}", i, counts[i], expected);
			ok = false;
		}
	}
	check(ok, "alias table frequencies do not follow the weights");
	check(buildAliasTable(std::vector<float> { 0.f, 0.f }).empty(), "alias table over zero weights is not empty");
}

void testMisWeights()
{
	bool ok = true;
	for (int i = 0; i < 1000; i++) {
		const float a = std::exp(uniform() * 20.f - 10.f);
		const float b = std::exp(uniform() * 20.f - 10.f);
		for (const float exponent : { 1.f, 2.f }) {
			ok &= std::abs(misWeight(a, b, exponent) + misWeight(b, a, exponent) - 1.f) < 1e-5f;
		}
	}
	ok &= misWeight(1.f, 0.f, 2.f) == 1.f && misWeight(0.f, 1.f, 1.f) == 0.f;
	check(ok, "MIS weights of the two techniques do not sum to 1");
}

// The storage scale is the smallest power of two that brings a peak within half float, and 1 for
// a map that already fits. A map past the range, like moon_lab_4k.hdr's lamp at 466944, once
// uploaded infinities that dropped a third of all samples
void testEnvironmentStorage()
{
	for (const float peak : { 0.f, 1.f, 65504.f, 65505.f, 466944.f, 1e8f, 3e38f }) {
		const float scale = environmentStorageScale(peak);
		int exponent = 0;
		const bool powerOfTwo = std::frexp(scale, &exponent) == 0.5f;
		const bool fits = peak / scale <= HALF_FLOAT_MAX;
		const bool smallest = scale == 1.f || peak / (scale * 0.5f) > HALF_FLOAT_MAX;
		check(powerOfTwo && fits && smallest, fmt::format("environment storage: scale {} for a peak of {}", scale, peak));
	}
	check(environmentStorageScale(65504.f) == 1.f, "environment storage: a map that fits in half float is scaled");

	//the lamp's texel, a dim one and an ordinary one, alpha 1 as stbi_loadf gives it
	const float map[] = {
		397312.f, 409600.f, 466944.f, 1.f,
		0.001f, 0.002f, 0.004f, 1.f,
		0.5f, 1.5f, 3.f, 1.f,
	};
	const float peak = environmentPeak(map, 3);
	const float scale = environmentStorageScale(peak);
	const std::vector<uint32_t> packed = packEnvironmentTexels(map, 3, scale);
	bool finite = true;
	float worst = 0.f;
	for (size_t i = 0; i < 3; i++) {
		const glm::vec2 rg = glm::unpackHalf2x16(packed[i * 2 + 0]);
		const glm::vec2 ba = glm::unpackHalf2x16(packed[i * 2 + 1]);
		const float stored[4] = { rg.x * scale, rg.y * scale, ba.x * scale, ba.y };
		for (int c = 0; c < 4; c++) {
			finite &= std::isfinite(stored[c]);
			worst = std::max(worst, std::abs(stored[c] - map[i * 4 + c]) / map[i * 4 + c]);
		}
	}
	fmt::println("  half-float copy: peak {}, stored at 1/{}, worst relative error {:.2e}", peak, scale, worst);
	check(peak == 466944.f && scale == 8.f, "environment storage: the lamp's peak or scale is wrong");
	check(finite, "environment storage: the half-float copy has a non-finite texel");
	//half float keeps 11 significant bits: 2^-11 relative, for values above its subnormal range
	check(worst <= 1.f / 2048.f, "environment storage: the half-float copy strays from the map");
}

}

int main(int argc, char* argv[])
{
	size_t samples = 20000;
	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--samples") && i + 1 < argc) {
			samples = std::stoul(argv[++i]);
		}
	}

	fmt::println("area lights ({} samples per light per trial):", samples);
	for (int placement = 0; placement < 8; placement++) {
		for (const ShapeKind kind : { ShapeKind::Sphere, ShapeKind::Quad, ShapeKind::Box, ShapeKind::Cylinder }) {
			testEmitter(shapeEmitter(kind), samples);
		}
		testEmitter(triangleEmitter(), samples);
	}
	//from inside a sphere every direction reaches it, from the far side of its own centre and all
	{
		Emitter sphere = shapeEmitter(ShapeKind::Sphere);
		const float radius = sphere.lights[0].radius;
		double sum = 0.0;
		for (size_t i = 0; i < samples; i++) {
			const glm::vec3 ref = sphere.boundCenter + 0.9f * radius * uniformSphere();
			LightSample sample;
			glm::vec3 barycentrics;
			if (sampleAreaLight(sphere.lights[0], ref, glm::vec2(uniform(), uniform()), sample, barycentrics)) {
				sum += 1.0 / sample.pdf;
			}
		}
		const double solidAngle = sum / (double)samples;
		fmt::println("  inside a sphere: {:.3f} sr (expected 4 pi = {:.3f})", solidAngle, 4.0 * LIGHT_PI);
		check(std::abs(solidAngle - 4.0 * LIGHT_PI) < 0.05 * 4.0 * LIGHT_PI, "sphere: solid angle from inside is not 4 pi");
	}

	fmt::println("environment map:");
	testEnvironment(256, 128, 2048, samples * 10);
	testEnvironment(512, 256, 128, samples * 10);

	fmt::println("alias table and MIS weights:");
	testAliasTable(samples * 50);
	testMisWeights();

	fmt::println("environment map storage:");
	testEnvironmentStorage();

	fmt::println("\n{}", g_failures == 0 ? "PASS" : fmt::format("FAIL ({} problem(s))", g_failures));
	return g_failures == 0 ? 0 : 2;
}
