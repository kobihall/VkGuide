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
// For emitted rays (docs/plans/completed/pbrt-bidirectional-path-tracing.md), of every area light from one side, the other
// and both, and of point, spot and directional lights: the densities a sample reports are the pdf function's, the
// directional density integrates to 1 over the sphere, and the power the samples estimate is the light's (L A pi per
// side, 4 pi I, I times a spot's solid angle, E times the disk's area).
// For the environment map's half-float GPU copy: a map brighter than 65504 stays finite and within
// half float's precision of the file, and a map that fits is stored unscaled.
// For lasers (docs/plans/completed/physoptics-laser-sources.md), a round M^2 = 1 and M^2 = 2 beam and an astigmatic diode
// beam: 10^6 emitted rays, carried to the waist, one Rayleigh range and ten past it, have second-moment radii within
// 0.5 % of w(z); a sample's densities are the pdf function's; the position density integrates to 1 over the plane and the
// direction density to 1 over the sphere; the samples follow the beam's phase-space density (chi-square per axis); the
// radiance over the densities gives the power.
//
// Run:  ./bin/light_test [--samples N]

#include <ies_loader.h>
#include <light_sampling.h>

#include "test_stats.h"

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

// `degrees` turns the map about +y (the scene's environment rotation): the samples and the pdf are of world directions,
// and the map is read in its own frame, as the shaders read it
void testEnvironment(uint32_t width, uint32_t height, uint32_t maxWidth, size_t samples, float degrees)
{
	const std::vector<float> rgba = syntheticEnvironment(width, height);
	const EnvironmentDistribution distribution = buildEnvironmentDistribution(rgba.data(), width, height, maxWidth);
	const glm::vec2 rotation = environmentRotation(degrees);
	fmt::println("  {} x {} map, table {} x {}, turned {} degrees", width, height, distribution.full.width, distribution.full.height, degrees);

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
			if (!sampleEnvironment(table, glm::vec2(uniform(), uniform()), rotation, direction, pdf)) {
				continue;
			}
			const float lookup = environmentPdf(table, direction, rotation);
			if (!(std::abs(lookup - pdf) <= 1e-3f * pdf)) {
				mismatches++;
			}
			estimate += lookupLuminance(rgba, width, height, environmentToMap(direction, rotation)) / pdf;
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
				normalisation += environmentPdf(table, equirectDirectionOf(glm::vec2(u, v)), rotation) * cosLatitude;
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

// the turn itself: a quarter turn takes the map's +x to the world's -z, counterclockwise seen from above, and the two
// directions undo each other at any angle
void testEnvironmentRotation()
{
	const glm::vec3 quarter = environmentFromMap(glm::vec3(1.f, 0.f, 0.f), environmentRotation(90.f));
	check(glm::length(quarter - glm::vec3(0.f, 0.f, -1.f)) < 1e-6f, "environment rotation: a quarter turn does not take +x to -z");
	const glm::vec3 direction = glm::normalize(glm::vec3(0.3f, 0.5f, -0.8f));
	const glm::vec2 rotation = environmentRotation(217.f);
	check(glm::length(environmentFromMap(environmentToMap(direction, rotation), rotation) - direction) < 1e-6f, "environment rotation: to the map and back is not the identity");
	check(glm::length(environmentFromMap(direction, environmentRotation(0.f)) - direction) < 1e-7f, "environment rotation: no turn moves the direction");
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

// docs/plans/completed/pbrt-light-types.md: each spot falloff's solid angle against the falloff integrated over the sphere
void testSpotFalloffs()
{
	const float cosOuter = std::cos(35.f * LIGHT_PI / 180.f);
	const float cosInner = std::cos(20.f * LIGHT_PI / 180.f);
	for (const bool smooth : { false, true }) {
		double integral = 0.0;
		const int steps = 200000;
		for (int i = 0; i < steps; i++) {
			const float cosTheta = -1.f + 2.f * ((float)i + 0.5f) / (float)steps;
			const float falloff = smooth ? spotFalloffSmooth(cosTheta, cosOuter, cosInner) : spotFalloff(cosTheta, cosOuter, cosInner);
			integral += 2.0 * LIGHT_PI * falloff * (2.0 / steps);
		}
		const float expected = spotSolidAngle(cosOuter, cosInner, smooth);
		fmt::println("  {} falloff: {:.5f} sr integrated, {:.5f} sr priced", smooth ? "pbrt smoothstep" : "glTF squared ramp", integral, expected);
		check(std::abs(integral - expected) < 1e-4 * expected, fmt::format("{} spot falloff: the power's solid angle disagrees with the falloff", smooth ? "smoothstep" : "glTF"));
	}
	check(spotFalloffSmooth(cosInner, cosOuter, cosInner) == 1.f && spotFalloffSmooth(cosOuter, cosOuter, cosInner) == 0.f, "smoothstep falloff: not 1 at the inner cone and 0 at the outer");
}

// pbrt's equal-area square: the round trip, unit directions, and equal areas of the square as equal solid angles
void testEqualArea()
{
	double worst = 0.0;
	double worstLength = 0.0;
	for (int i = 0; i < 100000; i++) {
		const glm::vec2 p(uniform(), uniform());
		const glm::vec3 d = equalAreaSquareToSphere(p);
		worstLength = std::max(worstLength, (double)std::abs(glm::length(d) - 1.f));
		const glm::vec2 back = equalAreaSphereToSquare(d);
		worst = std::max(worst, (double)glm::length(back - p));
	}
	fmt::println("  equal-area square: round trip within {:.2e}, unit within {:.2e}", worst, worstLength);
	check(worst < 2e-5 && worstLength < 1e-5, "equal-area square: the mapping and its inverse disagree");
	//a cap about +z of solid angle 2 pi (1 - cos theta) takes that share of 4 pi of the square's points
	size_t inside = 0;
	const size_t count = 200000;
	for (size_t i = 0; i < count; i++) {
		inside += equalAreaSquareToSphere(glm::vec2(uniform(), uniform())).z > 0.5f ? 1 : 0;
	}
	const double share = (double)inside / (double)count;
	check(std::abs(share - 0.25) < 0.005, fmt::format("equal-area square: the cap above z = 0.5 takes {:.4f} of it, not 0.25", share));
}

// an isotropic IES file (every candela the same) becomes a flat image of that candela over 683, and a typical one's
// symmetry expands: the quadrant file reads the same in all four quadrants
void testIes()
{
	const std::string isotropic = "IESNA:LM-63-2002\n[TEST] isotropic\nTILT=NONE\n1 -1 1 5 1 1 2 0 0 0\n1 1 100\n0 45 90 135 180\n0\n500 500 500 500 500\n";
	IesData ies;
	std::string error;
	check(parseIes(isotropic, ies, &error), "IES: the isotropic file does not parse: " + error);
	const std::vector<float> image = iesEqualAreaImage(ies, 32);
	float lo = 1e30f;
	float hi = 0.f;
	for (const float v : image) {
		lo = std::min(lo, v);
		hi = std::max(hi, v);
	}
	fmt::println("  IES isotropic: {:.6f} to {:.6f} W/sr (expected {:.6f})", lo, hi, 500.f / 683.f);
	check(std::abs(lo - 500.f / 683.f) < 1e-5f && std::abs(hi - 500.f / 683.f) < 1e-5f, "IES: an isotropic file's image is not flat at candela / 683");
	const float power = lightImageSolidAngle(false, 0.f, 32, 32, image.data());
	check(std::abs(power - 4.f * LIGHT_PI * 500.f / 683.f) < 1e-3f * power, "IES: an isotropic file's power is not 4 pi I");

	const std::string quadrant = "IESNA:LM-63-2002\nTILT=NONE\n1 -1 2 3 3 1 2 0 0 0\n1 1 100\n0 45 90\n0 45 90\n100 80 0\n200 150 0\n300 250 0\n";
	check(parseIes(quadrant, ies, &error), "IES: the quadrant file does not parse: " + error);
	bool symmetric = true;
	for (const float h : { 10.f, 30.f, 80.f }) {
		const float a = iesCandela(ies, 30.f, h);
		symmetric &= std::abs(a - iesCandela(ies, 30.f, 180.f - h)) < 1e-4f && std::abs(a - iesCandela(ies, 30.f, 180.f + h)) < 1e-4f && std::abs(a - iesCandela(ies, 30.f, 360.f - h)) < 1e-4f;
	}
	check(symmetric, "IES: a quadrant file is not mirrored into the other three");
	check(std::abs(iesCandela(ies, 0.f, 45.f) - 2.f * 200.f) < 1e-3f, "IES: the candela multiplier is not applied");
	check(iesCandela(ies, 120.f, 0.f) == 0.f, "IES: a direction past the file's vertical angles is not dark");
	check(!parseIes("not an ies file", ies, &error), "IES: a file without TILT= parses");
}

// a white projection image's power is the frustum's solid angle, 4 asin(sin a sin b) for half-angles a and b
void testProjectionPower()
{
	for (const float fov : { 30.f, 90.f, 140.f }) {
		const uint32_t width = 200;
		const uint32_t height = 100;
		const std::vector<float> white((size_t)width * height, 1.f);
		const float measured = lightImageSolidAngle(true, fov, width, height, white.data());
		const glm::vec2 extents = projectionExtents(fov, (float)width / (float)height);
		const double a = std::atan(extents.x);
		const double b = std::atan(extents.y);
		const double exact = 4.0 * std::asin(std::sin(a) * std::sin(b));
		fmt::println("  projection {:.0f} deg: {:.5f} sr measured, {:.5f} sr exact", fov, measured, exact);
		check(std::abs(measured - exact) < 2e-3 * exact, fmt::format("projection {} deg: a white image's solid angle is not the frustum's", fov));
	}
}

// emitted rays: the densities and the power, per kind
void testEmittedRays(size_t samples)
{
	for (const ShapeKind kind : { ShapeKind::Sphere, ShapeKind::Quad, ShapeKind::Cylinder }) {
		const Emitter emitter = shapeEmitter(kind);
		for (const EmitSides sides : { EmitSides::Both, EmitSides::Front, EmitSides::Back }) {
			const AreaLightGeometry& light = emitter.lights[0];
			double power = 0.0;
			double directionIntegral = 0.0;
			float worstPdf = 0.f;
			for (size_t i = 0; i < samples; i++) {
				LightLeSample s;
				if (sampleAreaLe(light, sides, glm::vec2(uniform(), uniform()), glm::vec2(uniform(), uniform()), s)) {
					//unit radiance: L |cos| / (pdfPos pdfDir) estimates the power
					power += std::abs(glm::dot(s.normal, s.direction)) / (s.pdfPos * s.pdfDir);
					float pdfPos;
					float pdfDir;
					areaPdfLe(light, sides, s.normal, s.direction, pdfPos, pdfDir);
					worstPdf = std::max({ worstPdf, std::abs(pdfPos - s.pdfPos) / s.pdfPos, std::abs(pdfDir - s.pdfDir) / s.pdfDir });
				}
				float pdfPos;
				float pdfDir;
				areaPdfLe(light, sides, uniformSphere(), uniformSphere(), pdfPos, pdfDir);
				directionIntegral += pdfDir * 4.0 * LIGHT_PI;
			}
			power /= (double)samples;
			directionIntegral /= (double)samples;
			const double expected = areaLightArea(light) * LIGHT_PI * (sides == EmitSides::Both ? 2.0 : 1.0);
			const char* sideName = sides == EmitSides::Both ? "both sides" : sides == EmitSides::Front ? "front" : "back";
			fmt::println("  {} {}: power {:.4f} of L A pi per side, directions integrate to {:.4f}", emitter.name, sideName, power / expected, directionIntegral);
			check(std::abs(power / expected - 1.0) < 1e-3, fmt::format("{} {}: emitted power is {:.5f} of the light's", emitter.name, sideName, power / expected));
			check(std::abs(directionIntegral - 1.0) < 0.02, fmt::format("{} {}: the directional density integrates to {:.4f}", emitter.name, sideName, directionIntegral));
			check(worstPdf < 1e-4f, fmt::format("{} {}: a sample's densities are {:.3g} off the pdf function's", emitter.name, sideName, worstPdf));
		}
	}
	{
		const Emitter emitter = triangleEmitter();
		double power = 0.0;
		for (size_t i = 0; i < samples; i++) {
			LightLeSample s;
			if (sampleAreaLe(emitter.lights[0], EmitSides::Front, glm::vec2(uniform(), uniform()), glm::vec2(uniform(), uniform()), s)) {
				power += std::abs(glm::dot(s.normal, s.direction)) / (s.pdfPos * s.pdfDir);
				const glm::vec3 point = s.barycentrics.x * emitter.lights[0].a + s.barycentrics.y * emitter.lights[0].b + s.barycentrics.z * emitter.lights[0].c;
				check(glm::length(point - s.origin) < 1e-5f, "triangle: the barycentrics are not the origin's");
			}
		}
		power /= (double)samples;
		const double expected = areaLightArea(emitter.lights[0]) * LIGHT_PI;
		fmt::println("  triangle front: power {:.4f} of L A pi", power / expected);
		check(std::abs(power / expected - 1.0) < 1e-3, fmt::format("triangle: emitted power is {:.5f} of the light's", power / expected));
	}

	//point: 4 pi I; spot: I times the solid angle of its falloff; directional: E times the disk's area
	double pointPower = 0.0;
	double spotPower = 0.0;
	double spotIntegral = 0.0;
	const glm::vec3 axis = glm::normalize(glm::vec3(0.3f, -1.f, 0.2f));
	const float cosOuter = std::cos(35.f * LIGHT_PI / 180.f);
	const float cosInner = std::cos(20.f * LIGHT_PI / 180.f);
	for (size_t i = 0; i < samples; i++) {
		LightLeSample s;
		samplePointLe(glm::vec3(1.f, 2.f, 3.f), glm::vec2(uniform(), uniform()), s);
		pointPower += 1.0 / (s.pdfPos * s.pdfDir);
		if (sampleSpotLe(glm::vec3(0.f), axis, cosOuter, glm::vec2(uniform(), uniform()), s)) {
			spotPower += spotFalloffSmooth(glm::dot(s.direction, axis), cosOuter, cosInner) / (s.pdfPos * s.pdfDir);
			check(std::abs(spotPdfDir(axis, cosOuter, s.direction) - s.pdfDir) < 1e-4f * s.pdfDir, "spot: a sample's density is not the pdf function's");
		}
		spotIntegral += spotPdfDir(axis, cosOuter, uniformSphere()) * 4.0 * LIGHT_PI;
	}
	pointPower /= (double)samples;
	spotPower /= (double)samples;
	spotIntegral /= (double)samples;
	const double spotExpected = spotSolidAngle(cosOuter, cosInner, true);
	fmt::println("  point: power {:.4f} of 4 pi I; spot: {:.4f} of I times its solid angle, directions integrate to {:.4f}", pointPower / (4.0 * LIGHT_PI), spotPower / spotExpected, spotIntegral);
	check(std::abs(pointPower / (4.0 * LIGHT_PI) - 1.0) < 1e-4, "point: emitted power is not 4 pi I");
	check(std::abs(spotPower / spotExpected - 1.0) < 0.01, fmt::format("spot: emitted power is {:.4f} of I times its solid angle", spotPower / spotExpected));
	check(std::abs(spotIntegral - 1.0) < 0.03, fmt::format("spot: the directional density integrates to {:.4f}", spotIntegral));

	const glm::vec4 sceneSphere(1.f, -2.f, 0.5f, 3.f);
	const glm::vec3 toward = glm::normalize(glm::vec3(-0.4f, 1.f, 0.3f));
	float worstPlane = 0.f;
	float worstDisk = 0.f;
	for (size_t i = 0; i < 1000; i++) {
		const glm::vec3 origin = infiniteLightOrigin(sceneSphere, toward, glm::vec2(uniform(), uniform()));
		const glm::vec3 offset = origin - glm::vec3(sceneSphere);
		worstPlane = std::max(worstPlane, std::abs(glm::dot(offset, toward) - sceneSphere.w));
		worstDisk = std::max(worstDisk, glm::length(offset - glm::dot(offset, toward) * toward) - sceneSphere.w);
	}
	check(worstPlane < 1e-5f && worstDisk < 1e-5f, "infinite lights: an origin is off the disk behind the scene");
	check(std::abs(infiniteLightPdfPos(sceneSphere) * LIGHT_PI * 9.f - 1.f) < 1e-6f, "infinite lights: the positional density is not one over the disk's area");
}

// a laser beam whose frame is turned off the world's axes and whose center is away from the origin
LaserEmitter testLaser(glm::vec2 waist, glm::vec2 waistDistance, glm::vec2 m2, float wavelength)
{
	LaserEmitter laser;
	laser.center = glm::vec3(0.3f, 0.1f, -0.2f);
	laser.axis = glm::normalize(glm::vec3(0.4f, 0.1f, 1.f));
	laser.xAxis = glm::normalize(glm::cross(glm::vec3(0.f, 1.f, 0.f), laser.axis));
	laser.waist = waist;
	laser.waistDistance = waistDistance;
	laser.halfAngle = m2 * wavelength / (LIGHT_PI * waist);
	return laser;
}

void testLaser(const std::string& name, const LaserEmitter& laser, size_t samples)
{
	const glm::vec3 yAxis = glm::cross(laser.axis, laser.xAxis);
	// each axis's 1/e^2 radius at `fromWaist` past its waist
	auto beamRadius = [&](int axis, double fromWaist) {
		const double w0 = laser.waist[axis];
		const double theta = laser.halfAngle[axis];
		return std::sqrt(w0 * w0 + theta * theta * fromWaist * fromWaist);
	};
	// the planes, per axis, at 0, 1 and 10 Rayleigh ranges past its own waist (along the axis from the reference plane)
	const double multiples[3] = { 0.0, 1.0, 10.0 };
	double secondMoment[2][3] = {};
	double worstDensity = 0.0;
	double power = 0.0;
	// the x axis's (position, slope) and the y axis's on 20 x 20 cells of +-4 standard deviations, the rest one cell
	constexpr int BINS = 20;
	std::vector<double> observed[2];
	observed[0].assign(BINS * BINS + 1, 0.0);
	observed[1].assign(BINS * BINS + 1, 0.0);
	const glm::dvec2 sigmaW = 0.5 * glm::dvec2(laser.waist);
	const glm::dvec2 sigmaS = 0.5 * glm::dvec2(laser.halfAngle);
	const glm::dvec2 d = glm::dvec2(laser.waistDistance);
	const glm::dvec2 sigmaX = glm::sqrt(sigmaW * sigmaW + d * d * sigmaS * sigmaS);
	auto cellOf = [&](int axis, double x, double slope) {
		const int i = (int)std::floor((x / sigmaX[axis] + 4.0) / 8.0 * BINS);
		const int j = (int)std::floor((slope / sigmaS[axis] + 4.0) / 8.0 * BINS);
		return i >= 0 && i < BINS && j >= 0 && j < BINS ? i * BINS + j : BINS * BINS;
	};
	for (size_t n = 0; n < samples; n++) {
		LightLeSample s;
		if (!sampleLaserLe(laser, glm::vec2(uniform(), uniform()), glm::vec2(uniform(), uniform()), s)) {
			check(false, name + ": a sample was refused");
			continue;
		}
		const glm::dvec3 origin(s.origin - laser.center);
		const glm::dvec3 direction(s.direction);
		const glm::dvec3 axis(laser.axis);
		const glm::dvec3 xs[2] = { glm::dvec3(laser.xAxis), glm::dvec3(yAxis) };
		for (int a = 0; a < 2; a++) {
			const double rayleigh = laser.waist[a] / laser.halfAngle[a];
			for (int k = 0; k < 3; k++) {
				// the plane `fromWaist` past the waist, which is waistDistance behind the reference plane
				const double plane = multiples[k] * rayleigh - laser.waistDistance[a];
				const double t = (plane - glm::dot(origin, axis)) / glm::dot(direction, axis);
				const double x = glm::dot(origin + t * direction, xs[a]);
				secondMoment[a][k] += x * x;
			}
			const double x = glm::dot(origin, xs[a]);
			const double slope = glm::dot(direction, xs[a]) / glm::dot(direction, axis);
			observed[a][(size_t)cellOf(a, x, slope)] += 1.0;
		}
		float pdfPos;
		float pdfDir;
		laserPdfLe(laser, s.origin, s.direction, pdfPos, pdfDir);
		worstDensity = std::max({ worstDensity, std::abs((double)pdfPos / s.pdfPos - 1.0), std::abs((double)pdfDir / s.pdfDir - 1.0) });
		// a watt's radiance, its cosine and the densities: 1 per sample
		power += laserRadiance(laser, s.origin, s.direction) * glm::dot(s.direction, laser.axis) / ((double)s.pdfPos * s.pdfDir);
	}
	power /= (double)samples;
	double worstRadius = 0.0;
	for (int a = 0; a < 2; a++) {
		const double rayleigh = laser.waist[a] / laser.halfAngle[a];
		for (int k = 0; k < 3; k++) {
			const double measured = 2.0 * std::sqrt(secondMoment[a][k] / (double)samples);
			const double expected = beamRadius(a, multiples[k] * rayleigh);
			worstRadius = std::max(worstRadius, std::abs(measured / expected - 1.0));
		}
	}

	// the joint density of one axis's position in the plane and slope, integrated over a cell by a 6 x 6 midpoint rule
	auto cellIntegral = [&](int axis, int i, int j) {
		const double x0 = (8.0 * i / BINS - 4.0) * sigmaX[axis];
		const double s0 = (8.0 * j / BINS - 4.0) * sigmaS[axis];
		const double dx = 8.0 / BINS * sigmaX[axis];
		const double ds = 8.0 / BINS * sigmaS[axis];
		double sum = 0.0;
		for (int u = 0; u < 6; u++) {
			for (int v = 0; v < 6; v++) {
				const double x = x0 + (u + 0.5) / 6.0 * dx;
				const double slope = s0 + (v + 0.5) / 6.0 * ds;
				const double a = (x - d[axis] * slope) / sigmaW[axis];
				const double b = slope / sigmaS[axis];
				sum += std::exp(-0.5 * (a * a + b * b)) / (2.0 * LIGHT_PI * sigmaW[axis] * sigmaS[axis]);
			}
		}
		return sum / 36.0 * dx * ds;
	};
	double worstP = 1.0;
	for (int a = 0; a < 2; a++) {
		std::vector<double> expected(BINS * BINS + 1, 0.0);
		double inside = 0.0;
		for (int i = 0; i < BINS; i++) {
			for (int j = 0; j < BINS; j++) {
				expected[(size_t)(i * BINS + j)] = cellIntegral(a, i, j) * (double)samples;
				inside += expected[(size_t)(i * BINS + j)];
			}
		}
		expected[BINS * BINS] = std::max((double)samples - inside, 0.0);
		worstP = std::min(worstP, stats::chiSquareTest(observed[a], expected).pValue);
	}

	// the position density over the plane, and the direction density over the slopes (d omega = cos^3 d slope), from
	// a point half a beam radius off the axis, by the midpoint rule over +-8 standard deviations
	const glm::vec3 point = laser.center + 0.5f * laser.waist.x * laser.xAxis;
	double positionIntegral = 0.0;
	double directionIntegral = 0.0;
	constexpr int GRID = 160;
	const glm::dvec2 mean = d * sigmaS * sigmaS * glm::dvec2(0.5 * laser.waist.x, 0.0) / (sigmaX * sigmaX);
	const glm::dvec2 sigmaGiven = sigmaW * sigmaS / sigmaX;
	for (int i = 0; i < GRID; i++) {
		for (int j = 0; j < GRID; j++) {
			const glm::dvec2 u((i + 0.5) / GRID * 16.0 - 8.0, (j + 0.5) / GRID * 16.0 - 8.0);
			const glm::dvec2 x = u * sigmaX;
			float pdfPos;
			float pdfDir;
			laserPdfLe(laser, laser.center + (float)x.x * laser.xAxis + (float)x.y * yAxis, laser.axis, pdfPos, pdfDir);
			positionIntegral += pdfPos * (16.0 / GRID) * (16.0 / GRID) * sigmaX.x * sigmaX.y;
			const glm::dvec2 slope = mean + u * sigmaGiven;
			const glm::dvec3 direction = glm::normalize(slope.x * glm::dvec3(laser.xAxis) + slope.y * glm::dvec3(yAxis) + glm::dvec3(laser.axis));
			laserPdfLe(laser, point, glm::vec3(direction), pdfPos, pdfDir);
			const double cosTheta = glm::dot(direction, glm::dvec3(laser.axis));
			directionIntegral += pdfDir * cosTheta * cosTheta * cosTheta * (16.0 / GRID) * (16.0 / GRID) * sigmaGiven.x * sigmaGiven.y;
		}
	}
	fmt::println("  {}: radii within {:.3f} % of w(z), densities within {:.2g}, chi-square p {:.3f}, position {:.5f} and direction {:.5f} integrals, power {:.5f}", name,
		100.0 * worstRadius, worstDensity, worstP, positionIntegral, directionIntegral, power);
	check(worstRadius < 0.005, fmt::format("{}: a second-moment radius is {:.3f} % off w(z)", name, 100.0 * worstRadius));
	// in float, a slope of a few 1e-4 read back from a unit direction, and a beam radius from a point tenths of a meter
	// out, lose about 1e-4 of a standard deviation, which moves a density in the beam's far tail by a few 1e-3. Nothing
	// weighs by the recomputed densities: light tracing divides by the sample's own
	check(worstDensity < 1e-2, fmt::format("{}: a sample's densities are {:.3g} off the pdf function's", name, worstDensity));
	check(worstP > 1e-3, fmt::format("{}: the samples do not follow the beam (chi-square p = {:.2g})", name, worstP));
	check(std::abs(positionIntegral - 1.0) < 1e-3, fmt::format("{}: the position density integrates to {:.5f}", name, positionIntegral));
	check(std::abs(directionIntegral - 1.0) < 1e-3, fmt::format("{}: the direction density integrates to {:.5f}", name, directionIntegral));
	check(std::abs(power - 1.0) < 1e-3, fmt::format("{}: the radiance over the densities gives {:.5f} W per watt", name, power));
	// nothing leaves backward
	check(laserRadiance(laser, laser.center, -laser.axis) == 0.f, name + ": radiance leaves backward");
}

void testLasers(size_t samples)
{
	// a HeNe's waist at the output; the same beam at M^2 = 2 with its waist 30 mm inside the housing; a diode's elliptical
	// beam with its x and y waists 50 mm apart, in front of the output and behind it
	testLaser("M^2 = 1", testLaser(glm::vec2(0.405e-3f), glm::vec2(0.f), glm::vec2(1.f), 632.8e-9f), samples);
	testLaser("M^2 = 2", testLaser(glm::vec2(0.405e-3f), glm::vec2(0.03f), glm::vec2(2.f), 632.8e-9f), samples);
	testLaser("astigmatic diode", testLaser(glm::vec2(1.2e-3f, 0.4e-3f), glm::vec2(-0.02f, 0.03f), glm::vec2(1.3f, 1.8f), 635e-9f), samples);
	// a stratified sampler's strata stay apart: Box-Muller maps a corner of the square to the tails, never the middle
	const glm::vec2 n = sampleStandardNormal2(glm::vec2(0.f, 0.25f));
	check(std::abs(n.x) < 1e-6f && std::abs(n.y) < 1e-6f, "Box-Muller: u = (0, 1/4) is not the center");
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
	testEnvironment(256, 128, 2048, samples * 10, 0.f);
	testEnvironment(512, 256, 128, samples * 10, 0.f);
	testEnvironment(256, 128, 2048, samples * 10, 73.f);
	testEnvironmentRotation();

	fmt::println("alias table and MIS weights:");
	testAliasTable(samples * 50);
	testMisWeights();

	fmt::println("environment map storage:");
	testEnvironmentStorage();

	fmt::println("light types:");
	testSpotFalloffs();
	testEqualArea();
	testIes();
	testProjectionPower();

	fmt::println("emitted rays:");
	testEmittedRays(samples * 25);

	fmt::println("lasers ({} rays each):", samples * 50);
	testLasers(samples * 50);

	fmt::println("\n{}", g_failures == 0 ? "PASS" : fmt::format("FAIL ({} problem(s))", g_failures));
	return g_failures == 0 ? 0 : 2;
}
