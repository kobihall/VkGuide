// CPU verification of the surface scattering (src/bsdf.h, the line-for-line mirror of
// shaders/rt/include/crt_scatter.glsl). No Vulkan, no GPU.
//
// MIS is unbiased only if a material's sampling routine, its evaluation and its density agree. They
// are written separately, by hand, so for every material over a grid of parameters and incident
// angles this checks:
//  1. SAMPLES AND DENSITY. About a million directions from scatterSurface() are counted in a
//     64 x 128 grid over the hemisphere and compared with the integral of pdfSurface() over each
//     cell by Pearson's chi-square test (pooled cells, Sidak-corrected over every configuration).
//     The samples a material absorbs are one more cell, so a density that does not integrate to the
//     probability of keeping a sample fails too. Run for the PCG stream and for a sample record.
//  2. WEIGHTS. The attenuation scatterSurface() returns, summed over the samples that fell in a
//     region, estimates the integral of evalSurface() over it: that is what makes the estimator
//     unbiased. For a material whose weight is exactly f cos / pdf per sample (lambertian, phong,
//     metal) it is also compared sample by sample.
//  3. ENERGY. The directional albedo of a white material, from cosine-weighted directions through
//     evalSurface() and from the sampler, must agree and must not exceed 1.
//  4. RECIPROCITY. f(wo, wi) = f(wi, wo) for the materials that should have it; the others are
//     reported with their asymmetry.
// For a delta material (a dielectric, a metal without fuzz) the sampled direction is checked
// against the mirror or Snell's law instead. Every material is also run at corner samples (0 and 1
// in each field) and at grazing angles, which must give finite numbers.
//
// --selfcheck proves the test can fail: it plants an error in the model (a density 5% too high, an
// evaluation 5% too high, a sampling roughness 10% off) and expects checks 1 and 2 to catch it.
//
// Run:  ./bin/bsdf_test [--samples N] [--quick] [--material NAME] [--selfcheck] [--strict] [--verbose]
//       --strict also fails on the known findings the test prints as KNOWN

#include <bsdf.h>
#include <sampler.h>

#include "test_stats.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>

#include <fmt/format.h>

// tests/bsdf_test_bxdf.cpp: pbrt-v4's glass and conductor
int runBxdfTests(uint64_t samples, int quadrature, bool selfcheck, const std::string& filter);

namespace {

using glm::dvec3;
using glm::vec3;

//---------------------------------------------------------------- the model under test

// A planted error, for --selfcheck
enum class Mutation {
	None,
	// pdfSurface() of a pbr material 5% too high
	PdfScale,
	// evalSurface() of a pbr material 5% too high
	EvalScale,
	// scatterSurface() of a pbr material samples with roughness 10% above the one it evaluates with
	SampleRoughness,
};

Mutation g_mutation = Mutation::None;

bool scatter(const BsdfMaterial& m, const vec3& inDir, const vec3& normal, bool frontFace, BsdfDraws& draws, vec3& attenuation, vec3& outDir)
{
	BsdfMaterial sampled = m;
	if (g_mutation == Mutation::SampleRoughness && m.type == BsdfType::Pbr) {
		sampled.roughness *= 1.1f;
	}
	return scatterSurface(sampled, inDir, normal, frontFace, draws, attenuation, outDir);
}

vec3 eval(const BsdfMaterial& m, const vec3& inDir, const vec3& normal, const vec3& wi)
{
	const vec3 f = evalSurface(m, inDir, normal, wi);
	return g_mutation == Mutation::EvalScale && m.type == BsdfType::Pbr ? f * 1.05f : f;
}

float pdf(const BsdfMaterial& m, const vec3& inDir, const vec3& normal, const vec3& wi)
{
	const float p = pdfSurface(m, inDir, normal, wi);
	return g_mutation == Mutation::PdfScale && m.type == BsdfType::Pbr ? p * 1.05f : p;
}

bool finite(const vec3& v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

//---------------------------------------------------------------- results

struct Options {
	uint64_t samples { 1000000 };
	uint64_t energySamples { 65536 };
	std::string material;
	// the starting subcells per cell side of the quadrature
	int quadrature { 8 };
	bool verbose { false };
	bool selfcheck { false };
};

// one configuration's result of one check, judged against the Sidak threshold at the end
struct PValue {
	std::string what;
	double p;
};

struct Report {
	std::vector<PValue> pValues;
	int hardFailures { 0 };
	// --selfcheck counts the failures of a planted error without printing them
	bool quiet { false };

	// kept for the merge when the report belongs to a worker thread
	std::vector<std::string> messages;
	// verbose detail, printed in order with the rest of the configuration's output
	std::vector<std::string> notes;

	void fail(const std::string& what)
	{
		if (!quiet) {
			fmt::println("    FAIL: {}", what);
		}
		messages.push_back(what);
		hardFailures++;
	}
	// A finding the harness reports and the build tolerates until it is decided what to do about it
	// (--strict makes it a failure). See docs/plans/completed/pbrt-verification-harness.md
	void known(bool ok, const std::string& what);
	void check(bool ok, const std::string& what)
	{
		if (!ok) {
			fail(what);
		}
	}
};

bool g_strict = false;

void Report::known(bool ok, const std::string& what)
{
	if (ok) {
		return;
	}
	if (g_strict) {
		fail(what);
	} else if (!quiet) {
		fmt::println("    KNOWN: {}", what);
	}
}

//---------------------------------------------------------------- configurations

struct Case {
	std::string label;
	BsdfMaterial material;
	float angleDegrees { 0.f };
	// the material's weight is exactly f cos / pdf for every sample
	bool exactWeight { false };
	// the cells are laid out around the mirror direction, thickening away from it, instead of
	// around the normal: for a lobe too narrow for the uniform grid to resolve
	bool lobeFrame { false };
	// the material is expected to be reciprocal (checked) or not (reported)
	bool reciprocal { false };
};

// the unit incoming direction at `angleDegrees` from the normal (+z), in the xz plane
vec3 incomingDirection(float angleDegrees)
{
	const float a = angleDegrees * BSDF_PI / 180.f;
	return -vec3(std::sin(a), 0.f, std::cos(a));
}

std::vector<Case> buildCases()
{
	const vec3 colour(0.9f, 0.6f, 0.3f);
	const float angles[] = { 0.f, 30.f, 60.f, 80.f, 88.f };
	std::vector<Case> cases;
	auto add = [&](const std::string& name, BsdfMaterial m, bool exactWeight, bool reciprocal, bool lobeFrame = false) {
		for (float angle : angles) {
			Case c;
			c.label = fmt::format("{} @{:g}deg", name, angle);
			c.material = m;
			c.angleDegrees = angle;
			c.exactWeight = exactWeight;
			c.lobeFrame = lobeFrame;
			c.reciprocal = reciprocal;
			cases.push_back(c);
		}
	};
	auto material = [&](BsdfType type, float param, float metallic = 0.f, float roughness = 0.5f) {
		BsdfMaterial m;
		m.type = type;
		m.albedo = colour;
		m.param = param;
		m.metallic = metallic;
		m.roughness = roughness;
		return m;
	};

	add("lambertian", material(BsdfType::Lambertian, 0.f), true, true);
	// phong: alpha = 1000^(smoothness^2), a lobe from broad to about 0.045 rad
	for (float smoothness : { 0.1f, 0.4f, 0.7f, 1.0f }) {
		add(fmt::format("phong smooth={:g}", smoothness), material(BsdfType::Phong, smoothness), true, false, true);
	}
	// metal: a ball of radius `fuzz` around the mirror direction
	for (float fuzz : { 0.03f, 0.1f, 0.3f, 0.6f, 1.0f }) {
		add(fmt::format("metal fuzz={:g}", fuzz), material(BsdfType::Metal, fuzz), true, false, fuzz < 0.5f);
	}
	add("metal fuzz=0 (mirror)", material(BsdfType::Metal, 0.f), false, false);
	add("metal fuzz=5e-5 (mirror)", material(BsdfType::Metal, 5e-5f), false, false);
	add("dielectric ior=1.5", material(BsdfType::Dielectric, 1.5f), false, false);
	add("dielectric ior=1.33", material(BsdfType::Dielectric, 1.33f), false, false);
	// pbr: GGX over a Lambertian lobe. Reciprocal only where the Lambertian lobe is absent
	for (float roughness : { 0.2f, 0.4f, 0.7f, 1.0f }) {
		for (float metallic : { 0.f, 0.5f, 1.f }) {
			add(fmt::format("pbr rough={:g} metal={:g}", roughness, metallic), material(BsdfType::Pbr, 0.f, metallic, roughness), false, metallic >= 1.f, roughness < 0.35f);
		}
	}
	// pbr with the dielectric F0 a KHR_materials_ior or _specular import sets in `param` (ior 1.9 is 0.0963; a specular factor of 0.2 is 0.008)
	for (float f0 : { 0.0963f, 0.008f }) {
		for (float metallic : { 0.f, 0.5f }) {
			add(fmt::format("pbr rough=0.4 metal={:g} f0={:g}", metallic, f0), material(BsdfType::Pbr, f0, metallic, 0.4f), false, false);
		}
	}
	return cases;
}

//---------------------------------------------------------------- check 1 and 2: sampling

struct SampleOutcome {
	double expectedKept { 0.0 };
	double observedKept { 0.0 };
};

// the per-cell integrals of the density and of f cos, which do not depend on the sampling mode
// where a case counts its directions: a grid and the frame it is laid out in. Directions go from
// world space to the frame with toLocal, and back with its transpose
struct Layout {
	stats::DirectionGrid grid;
	glm::mat3 frame { 1.f };
	glm::mat3 toLocal { 1.f };
};

Layout layoutFor(const Case& c, int rings, int azimuths)
{
	Layout layout;
	if (!c.lobeFrame) {
		layout.grid = { rings, azimuths, 1.0, false };
		return layout;
	}
	// the mirror direction is the pole; thickening rings with a cube law give a lobe a few rings
	const vec3 normal(0.f, 0.f, 1.f);
	layout.grid = { rings, azimuths, 3.0, true };
	layout.frame = bsdfTangentSpace(glm::reflect(incomingDirection(c.angleDegrees), normal));
	layout.toLocal = glm::transpose(layout.frame);
	return layout;
}

// the per-cell integrals of the density and of f cos, which do not depend on the sampling mode
struct CellIntegrals {
	std::vector<double> pdf;
	std::vector<dvec3> eval;
};

// `initial` x `initial` starting subcells per cell: more for a lobe narrower than a cell
CellIntegrals integrate(const BsdfMaterial& material, float angleDegrees, const Layout& layout, int initial, int maxDepth)
{
	const vec3 inDir = incomingDirection(angleDegrees);
	const vec3 normal(0.f, 0.f, 1.f);
	CellIntegrals out;
	out.pdf.resize(layout.grid.cellCount());
	out.eval.resize(layout.grid.cellCount());
	for (int cell = 0; cell < layout.grid.cellCount(); cell++) {
		const glm::dvec4 sum = stats::integrateCell(layout.grid, cell, initial, maxDepth, 2e-3, [&](const dvec3& local) {
			const vec3 wi = layout.frame * vec3(local);
			return glm::dvec4((double)pdf(material, inDir, normal, wi), dvec3(eval(material, inDir, normal, wi)));
		});
		out.pdf[cell] = sum.x;
		out.eval[cell] = dvec3(sum.y, sum.z, sum.w);
	}
	return out;
}

void testSampling(const Case& c, bool record, const Layout& layout, const CellIntegrals& integrals, const Options& options, Report& report)
{
	const stats::DirectionGrid& grid = layout.grid;
	const vec3 inDir = incomingDirection(c.angleDegrees);
	const vec3 normal(0.f, 0.f, 1.f);
	const int cells = grid.cellCount();
	const double n = (double)options.samples;
	const char* mode = record ? "record" : "stream";

	std::vector<double> counts(cells, 0.0);
	std::vector<dvec3> weightSum(cells, dvec3(0.0));
	std::vector<dvec3> weightSquares(cells, dvec3(0.0));
	double absorbed = 0.0;
	double belowHorizon = 0.0;
	double nonFinite = 0.0;
	double exactChecked = 0.0;
	double exactBad = 0.0;
	double zeroPdf = 0.0;
	std::mt19937 fieldRng(12345u + (uint32_t)record);
	std::uniform_real_distribution<float> unit(0.f, 1.f);

	for (uint64_t i = 0; i < options.samples; i++) {
		BsdfDraws draws;
		draws.rng = samplerPcgHash((uint32_t)i * 2654435761u + 0x9e3779b9u);
		draws.record = record;
		if (record) {
			for (float& f : draws.fields) {
				f = unit(fieldRng);
			}
		}
		vec3 attenuation;
		vec3 outDir;
		if (!scatter(c.material, inDir, normal, true, draws, attenuation, outDir)) {
			absorbed += 1.0;
			continue;
		}
		if (!finite(attenuation) || !finite(outDir) || glm::dot(outDir, outDir) == 0.f) {
			nonFinite += 1.0;
			continue;
		}
		const vec3 wi = glm::normalize(outDir);
		const int cell = wi.z > 0.f ? grid.cellOf(dvec3(layout.toLocal * wi)) : -1;
		if (cell < 0) {
			belowHorizon += 1.0;
			continue;
		}
		counts[cell] += 1.0;
		weightSum[cell] += dvec3(attenuation);
		weightSquares[cell] += dvec3(attenuation) * dvec3(attenuation);

		if (c.exactWeight) {
			const float p = pdf(c.material, inDir, normal, wi);
			if (p <= 0.f) {
				zeroPdf += 1.0;
				continue;
			}
			const vec3 ratio = eval(c.material, inDir, normal, wi) / p;
			exactChecked += 1.0;
			for (int k = 0; k < 3; k++) {
				if (std::abs(ratio[k] - attenuation[k]) > 1e-4f * std::max(std::abs(ratio[k]), std::abs(attenuation[k])) + 1e-6f) {
					exactBad += 1.0;
					break;
				}
			}
		}
	}

	report.check(nonFinite == 0.0, fmt::format("{} [{}]: {} non-finite sample(s)", c.label, mode, nonFinite));
	report.check(belowHorizon == 0.0, fmt::format("{} [{}]: {} accepted sample(s) below the horizon", c.label, mode, belowHorizon));

	// check 1: the counts against the density
	std::vector<double> observed = counts;
	std::vector<double> expected(cells);
	double kept = 0.0;
	for (int cell = 0; cell < cells; cell++) {
		expected[cell] = n * integrals.pdf[cell];
		kept += integrals.pdf[cell];
	}
	observed.push_back(absorbed + belowHorizon + nonFinite);
	expected.push_back(n * std::max(0.0, 1.0 - kept));
	const stats::ChiSquare chi = stats::chiSquareTest(observed, expected);
	report.pValues.push_back({ fmt::format("{} [{}] samples vs pdf", c.label, mode), chi.pValue });
	if (options.verbose) {
		report.notes.push_back(fmt::format("{} [{}]: chi2 {:.1f} on {} dof, p {:.3g}; pdf integrates to {:.5f}, {:.5f} of the samples kept", c.label, mode, chi.statistic, chi.dof, chi.pValue, kept, 1.0 - absorbed / n));
	}
	if (options.verbose && chi.pValue < 1e-6) {
		// the cells that contribute most, to tell a quadrature artifact from a real disagreement
		std::vector<std::pair<double, int>> worst;
		for (int cell = 0; cell < cells; cell++) {
			if (expected[cell] > 1.0) {
				const double d = observed[cell] - expected[cell];
				worst.push_back({ d * d / expected[cell], cell });
			}
		}
		std::sort(worst.rbegin(), worst.rend());
		for (size_t k = 0; k < std::min<size_t>(6, worst.size()); k++) {
			const int cell = worst[k].second;
			const dvec3 centre = dvec3(layout.frame * vec3(grid.pointIn(cell, 0.5, 0.5)));
			report.notes.push_back(fmt::format("    cell {} at world ({:.3f}, {:.3f}, {:.3f}): observed {:.0f}, expected {:.1f}, chi2 term {:.1f}", cell, centre.x, centre.y, centre.z, observed[cell], expected[cell], worst[k].first));
		}
		report.notes.push_back(fmt::format("    absorbed cell: observed {:.0f}, expected {:.1f}", observed.back(), expected.back()));
	}
	if (kept > 1.0 + 1e-3) {
		report.fail(fmt::format("{} [{}]: the pdf integrates to {:.5f} over the hemisphere", c.label, mode, kept));
	}

	// check 2: the weights. Cells are pooled 8 x 16 to 8 x 8 coarse cells, each with enough samples
	// for a normal approximation to the mean weight
	if (c.exactWeight) {
		report.check(zeroPdf == 0.0, fmt::format("{} [{}]: {} sample(s) where the pdf is zero", c.label, mode, zeroPdf));
		report.check(exactBad == 0.0, fmt::format("{} [{}]: attenuation differs from f cos / pdf in {} of {} samples", c.label, mode, exactBad, exactChecked));
	}
	constexpr int COARSE_RINGS = 8;
	constexpr int COARSE_AZIMUTHS = 8;
	const int fineAzimuths = grid.azimuths;
	for (int channel = 0; channel < 3; channel++) {
		double statistic = 0.0;
		int terms = 0;
		for (int cc = 0; cc < COARSE_RINGS; cc++) {
			for (int ca = 0; ca < COARSE_AZIMUTHS; ca++) {
				double count = 0.0;
				double sum = 0.0;
				double squares = 0.0;
				double truth = 0.0;
				for (int fc = cc * (grid.rings / COARSE_RINGS); fc < (cc + 1) * (grid.rings / COARSE_RINGS); fc++) {
					for (int fa = ca * (fineAzimuths / COARSE_AZIMUTHS); fa < (ca + 1) * (fineAzimuths / COARSE_AZIMUTHS); fa++) {
						const int cell = fc * fineAzimuths + fa;
						count += counts[cell];
						sum += weightSum[cell][channel];
						squares += weightSquares[cell][channel];
						truth += integrals.eval[cell][channel];
					}
				}
				// the coarse cell's contribution to the mean weight, E[w 1_cell] = truth
				if (count < 100.0) {
					continue;
				}
				const double mean = sum / n;
				const double variance = std::max(0.0, squares / n - mean * mean) / n;
				const double sigma = std::max(std::sqrt(variance), 1e-3 * std::abs(truth));
				if (sigma <= 0.0) {
					continue;
				}
				const double z = (mean - truth) / sigma;
				statistic += z * z;
				terms++;
			}
		}
		if (terms > 0) {
			const double p = stats::chiSquarePValue(statistic, terms);
			report.pValues.push_back({ fmt::format("{} [{}] mean weight vs f cos, channel {}", c.label, mode, channel), p });
		}
	}
}

//---------------------------------------------------------------- check 3: energy

struct Albedo {
	// the integral of evalSurface() over the hemisphere, by quadrature: exact to the quadrature's
	// tolerance, whatever the lobe
	double evaluated { 0.0 };
	// the mean attenuation scatterSurface() returns, with its standard error
	double sampled { 0.0 };
	double sampledError { 0.0 };
};

Albedo directionalAlbedo(const Case& c, uint64_t samples)
{
	BsdfMaterial white = c.material;
	white.albedo = vec3(1.f);
	const vec3 inDir = incomingDirection(c.angleDegrees);
	const vec3 normal(0.f, 0.f, 1.f);
	stats::Running bySampling;
	for (uint64_t i = 0; i < samples; i++) {
		BsdfDraws draws;
		draws.rng = samplerPcgHash((uint32_t)i * 2654435761u + 0x1234567u);
		vec3 attenuation;
		vec3 outDir;
		bySampling.add(scatter(white, inDir, normal, true, draws, attenuation, outDir) ? attenuation.g : 0.f);
	}
	const CellIntegrals integrals = integrate(white, c.angleDegrees, layoutFor(c, 32, 64), 4, 6);
	double evaluated = 0.0;
	for (const dvec3& cell : integrals.eval) {
		evaluated += cell.g;
	}
	return { evaluated, bySampling.mean, bySampling.standardError() };
}

//---------------------------------------------------------------- check 4: reciprocity

// the largest relative difference of f(wo, wi) and f(wi, wo) over random pairs
double reciprocityAsymmetry(const Case& c, uint64_t pairs)
{
	const vec3 normal(0.f, 0.f, 1.f);
	std::mt19937 rng(4242u);
	std::uniform_real_distribution<double> unit(0.0, 1.0);
	auto direction = [&]() {
		// away from the horizon, where the cosine in f cos / cos loses its precision
		const double z = 0.05 + 0.95 * unit(rng);
		const double r = std::sqrt(1.0 - z * z);
		const double phi = 2.0 * stats::PI * unit(rng);
		return vec3((float)(r * std::cos(phi)), (float)(r * std::sin(phi)), (float)z);
	};
	double worst = 0.0;
	for (uint64_t i = 0; i < pairs; i++) {
		const vec3 a = direction();
		const vec3 b = direction();
		// f(wo = a, wi = b) = eval(inDir = -a, b) / cos(b)
		const vec3 fab = eval(c.material, -a, normal, b) / b.z;
		const vec3 fba = eval(c.material, -b, normal, a) / a.z;
		for (int k = 0; k < 3; k++) {
			const double scale = std::max(std::abs(fab[k]), std::abs(fba[k]));
			if (scale > 1e-4) {
				worst = std::max(worst, std::abs(fab[k] - fba[k]) / scale);
			}
		}
	}
	return worst;
}

//---------------------------------------------------------------- delta materials

void testDelta(const Case& c, const Options& options, Report& report)
{
	const vec3 inDir = incomingDirection(c.angleDegrees);
	const vec3 normal(0.f, 0.f, 1.f);
	const BsdfMaterial& m = c.material;
	report.check(isSpecularMaterial(m), c.label + ": isSpecularMaterial() is false for a delta material");
	std::mt19937 rng(99u);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	for (int i = 0; i < 64; i++) {
		const vec3 wi = glm::normalize(vec3(unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) + 0.05f));
		report.check(glm::all(glm::equal(eval(m, inDir, normal, wi), vec3(0.f))) && pdf(m, inDir, normal, wi) == 0.f, c.label + ": a delta material has a density");
	}

	if (m.type == BsdfType::Metal) {
		BsdfDraws draws;
		draws.rng = 5;
		vec3 attenuation;
		vec3 outDir;
		const bool kept = scatter(m, inDir, normal, true, draws, attenuation, outDir);
		const vec3 mirror = glm::reflect(inDir, normal);
		const bool ok = kept && glm::length(glm::normalize(outDir) - mirror) < 1e-3f && glm::all(glm::equal(attenuation, m.albedo));
		report.check(ok, fmt::format("{}: sampled direction ({:.4f}, {:.4f}, {:.4f}) is not the mirror direction", c.label, outDir.x, outDir.y, outDir.z));
		return;
	}

	// a dielectric: the refracted direction obeys Snell's law and lies in the plane of incidence,
	// the reflected one is the mirror direction, and the chance of reflecting follows the
	// approximation's own reflectance
	const float ratio = 1.f / m.param;
	const float cosTheta = std::min(glm::dot(-inDir, normal), 1.f);
	const float sinTheta = std::sqrt(1.f - cosTheta * cosTheta);
	const double expectedReflect = ratio * sinTheta > 1.f ? 1.0 : (double)bsdfSchlick(cosTheta, ratio);
	double reflected = 0.0;
	const uint64_t n = std::min<uint64_t>(options.samples, 200000);
	for (uint64_t i = 0; i < n; i++) {
		BsdfDraws draws;
		draws.rng = samplerPcgHash((uint32_t)i + 17u);
		vec3 attenuation;
		vec3 outDir;
		const bool kept = scatter(m, inDir, normal, true, draws, attenuation, outDir);
		if (!kept || !glm::all(glm::equal(attenuation, vec3(1.f)))) {
			report.fail(c.label + ": a dielectric absorbed or tinted a sample");
			return;
		}
		if (outDir.z > 0.f) {
			reflected += 1.0;
			report.check(glm::length(outDir - glm::reflect(inDir, normal)) < 1e-4f, c.label + ": reflected direction is not the mirror direction");
		} else {
			const float sinOut = std::sqrt(outDir.x * outDir.x + outDir.y * outDir.y) / glm::length(outDir);
			report.check(std::abs(sinOut - ratio * sinTheta) < 2e-4f && std::abs(outDir.y) < 1e-5f, fmt::format("{}: refracted direction breaks Snell's law (sin {:.5f}, expected {:.5f})", c.label, sinOut, ratio * sinTheta));
			report.check(std::abs(glm::length(outDir) - 1.f) < 1e-4f, c.label + ": refracted direction is not a unit vector");
		}
	}
	const double sigma = std::sqrt(expectedReflect * (1.0 - expectedReflect) / (double)n);
	const double measured = reflected / (double)n;
	const double z = sigma > 0.0 ? (measured - expectedReflect) / sigma : (measured == expectedReflect ? 0.0 : 1e9);
	report.check(std::abs(z) < 5.0, fmt::format("{}: reflects {:.5f} of samples, Schlick says {:.5f}", c.label, measured, expectedReflect));
	// the exact Fresnel reflectance, for the record: Schlick with the incident cosine overestimates
	// transmission going from dense to thin (RTIOW's form)
	if (options.verbose) {
		const double eta = m.param;
		const double ci = cosTheta;
		const double st2 = (1.0 / (eta * eta)) * (1.0 - ci * ci);
		double exact = 1.0;
		if (st2 < 1.0) {
			const double ct = std::sqrt(1.0 - st2);
			const double rpar = (eta * ci - ct) / (eta * ci + ct);
			const double rperp = (ci - eta * ct) / (ci + eta * ct);
			exact = 0.5 * (rpar * rpar + rperp * rperp);
		}
		report.notes.push_back(fmt::format("{}: reflects {:.5f} of samples (exact Fresnel from outside: {:.5f})", c.label, measured, exact));
	}
}

//---------------------------------------------------------------- finite output at the corners

void testCorners(const BsdfMaterial& base, const std::string& name, Report& report)
{
	const vec3 normal(0.f, 0.f, 1.f);
	const float values[] = { 0.f, 1.f, 0.5f, 1e-7f, 0.99999994f };
	int bad = 0;
	int runs = 0;
	for (float angle : { 0.f, 45.f, 85.f, 89.9f, 89.999f }) {
		for (float a : values) {
			for (float b : values) {
				for (float c : values) {
					for (bool frontFace : { true, false }) {
						BsdfDraws draws;
						draws.rng = 0xffffffffu;
						draws.record = true;
						draws.fields[4] = a;
						draws.fields[5] = b;
						draws.fields[6] = c;
						vec3 attenuation;
						vec3 outDir;
						runs++;
						// a dielectric seen from inside leaves through the other side: the normal faces the ray
						const vec3 inDir = incomingDirection(angle);
						if (scatterSurface(base, inDir, normal, frontFace, draws, attenuation, outDir)) {
							if (!finite(attenuation) || !finite(outDir) || glm::dot(outDir, outDir) == 0.f) {
								bad++;
							}
						}
					}
				}
			}
		}
	}
	// A fully metallic pbr material once divided 0 by 0 here: its diffuse lobe has weight 0 and
	// probability 0, and a lobe draw of exactly 1.0 reached it (docs/plans/completed/pbrt-verification-harness.md)
	report.check(bad == 0, fmt::format("{}: {} of {} corner samples gave a non-finite attenuation or direction", name, bad, runs));
}

//---------------------------------------------------------------- the random numbers

void testRandom(Report& report)
{
	// randomFloat() converts the whole 32-bit word to a float, which rounds the top 128 words up to
	// exactly 2^32. It clamps to the largest float below 1, so a draw is in [0, 1)
	report.check(bsdfWordToFloat(0xffffffffu) < 1.0f && bsdfWordToFloat(0xffffff80u) < 1.0f && bsdfWordToFloat(0u) == 0.0f, "randomFloat() reaches 1.0 for a top word");
	// a word just under the clamp's reach converts as it always did, so the stream is otherwise unchanged
	report.check(bsdfWordToFloat(0xffffff00u) == float(0xffffff00u) * (1.0f / 4294967296.0f), "randomFloat() changed a word below the top 128");
	uint32_t state = 0;
	double maxDraw = 0.0;
	double minDraw = 1.0;
	for (int i = 0; i < 4000000; i++) {
		const double draw = bsdfRandomFloat(state);
		maxDraw = std::max(maxDraw, draw);
		minDraw = std::min(minDraw, draw);
	}
	report.check(minDraw >= 0.0 && maxDraw < 1.0, "randomFloat() leaves [0, 1)");
	fmt::println("  randomFloat(): largest of 4M draws {:.9f}; the top 128 of 2^32 words, one in {:.3g}, are clamped below 1", maxDraw, 4294967296.0 / 128.0);

	// Drive every material with PCG states whose first draw is one of the top words: the lobe choice of
	// pbr, the reflect-or-refract choice of a dielectric, the ball of a fuzzy metal. Each must give a
	// finite result
	std::vector<uint32_t> states;
	for (uint32_t candidate = 0; states.size() < 6; candidate += 1) {
		uint32_t probe = candidate;
		const float draw = bsdfRandomFloat(probe);
		if (draw >= 0.99999994f) {
			states.push_back(candidate);
		}
		if (candidate == 0xffffffffu) {
			break;
		}
	}
	report.check(!states.empty(), "no PCG state with a top word as its first draw was found");
	const std::pair<const char*, BsdfMaterial> materials[] = {
		{ "pbr metal=1", [] { BsdfMaterial m; m.type = BsdfType::Pbr; m.albedo = glm::vec3(0.9f, 0.6f, 0.3f); m.metallic = 1.f; m.roughness = 0.4f; return m; }() },
		{ "pbr metal=0.5", [] { BsdfMaterial m; m.type = BsdfType::Pbr; m.albedo = glm::vec3(0.9f, 0.6f, 0.3f); m.metallic = 0.5f; m.roughness = 0.4f; return m; }() },
		{ "dielectric", [] { BsdfMaterial m; m.type = BsdfType::Dielectric; m.param = 1.5f; return m; }() },
		{ "fuzzy metal", [] { BsdfMaterial m; m.type = BsdfType::Metal; m.param = 0.3f; return m; }() },
		{ "phong", [] { BsdfMaterial m; m.type = BsdfType::Phong; m.param = 0.5f; return m; }() },
	};
	const glm::vec3 normal(0.f, 0.f, 1.f);
	for (const auto& [name, material] : materials) {
		int bad = 0;
		int runs = 0;
		for (uint32_t start : states) {
			// the first draw, the second and the third are each the top word for some state; scanning
			// states covers the first, and stepping the stream covers the later ones
			for (int skip = 0; skip < 3; skip++) {
				BsdfDraws draws;
				draws.rng = start;
				for (int k = 0; k < skip; k++) {
					bsdfRandomFloat(draws.rng);
				}
				glm::vec3 attenuation;
				glm::vec3 outDir;
				runs++;
				if (scatterSurface(material, incomingDirection(30.f), normal, true, draws, attenuation, outDir) && (!finite(attenuation) || !finite(outDir))) {
					bad++;
				}
			}
		}
		report.check(bad == 0, fmt::format("{}: {} of {} samples were non-finite after a top-word draw", name, bad, runs));
	}
}

}  // namespace

int main(int argc, char** argv)
{
	Options options;
	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--samples") && i + 1 < argc) {
			options.samples = std::strtoull(argv[++i], nullptr, 10);
		} else if (!std::strcmp(argv[i], "--quick")) {
			options.samples = 100000;
			options.energySamples = 16384;
			options.quadrature = 4;
		} else if (!std::strcmp(argv[i], "--material") && i + 1 < argc) {
			options.material = argv[++i];
		} else if (!std::strcmp(argv[i], "--selfcheck")) {
			options.selfcheck = true;
		} else if (!std::strcmp(argv[i], "--strict")) {
			g_strict = true;
		} else if (!std::strcmp(argv[i], "--verbose")) {
			options.verbose = true;
		} else {
			fmt::println("usage: bsdf_test [--samples N] [--quick] [--material NAME] [--selfcheck] [--strict] [--verbose]");
			return 2;
		}
	}

	const std::vector<Case> allCases = buildCases();
	std::vector<Case> cases;
	for (const Case& c : allCases) {
		if (options.material.empty() || c.label.find(options.material) != std::string::npos) {
			cases.push_back(c);
		}
	}
	//pbrt-v4's BxDFs have their own section, which --material glass or conductor selects alone
	const bool bxdfsOnly = !options.material.empty() && (options.material.starts_with("glass") || options.material.starts_with("conductor") || options.material.starts_with("diffuse") || options.material.starts_with("thin") || options.material.starts_with("mix"));
	if (bxdfsOnly) {
		const int failures = runBxdfTests(options.samples, options.quadrature, options.selfcheck, options.material);
		fmt::println("{}", failures == 0 ? "bsdf_test: ok" : fmt::format("bsdf_test: {} failure(s)", failures));
		return failures == 0 ? 0 : 1;
	}
	if (cases.empty()) {
		fmt::println("no configuration matches '{}'", options.material);
		return 2;
	}

	// Each configuration runs on its own thread with its own report, and the reports are merged in
	// the order of the list, so the output does not depend on the thread count
	auto runSuite = [&](Report& report, bool printTables) {
		struct CaseResult {
			Report report;
			std::string line;
		};
		std::vector<CaseResult> results(cases.size());
		std::atomic<size_t> next { 0 };
		const unsigned threadCount = std::max(1u, std::thread::hardware_concurrency());
		auto work = [&]() {
			for (size_t index = next++; index < cases.size(); index = next++) {
				const Case& c = cases[index];
				Report& local = results[index].report;
				local.quiet = true;
				if (isSpecularMaterial(c.material)) {
					testDelta(c, options, local);
					continue;
				}
				const Layout layout = layoutFor(c, 64, 128);
				const CellIntegrals integrals = integrate(c.material, c.angleDegrees, layout, options.quadrature, 6);
				testSampling(c, false, layout, integrals, options, local);
				testSampling(c, true, layout, integrals, options, local);

				const Albedo albedo = directionalAlbedo(c, options.energySamples);
				const double slack = 4.0 * albedo.sampledError + 1e-3;
				// the quadrature of a narrow lobe is good to about 1%: this compares the two, the cell-by-cell
				// weight test is the sharp one
				const bool agree = std::abs(albedo.evaluated - albedo.sampled) <= 4.0 * albedo.sampledError + 1e-2;
				const bool conserves = albedo.evaluated <= 1.0 + 2e-3 && albedo.sampled <= 1.0 + slack;
				const double asymmetry = reciprocityAsymmetry(c, 20000);
				results[index].line = fmt::format("  {:<34} albedo eval {:.4f}  sampled {:.4f} +- {:.4f}  asymmetry {:.2g}", c.label.substr(c.label.find('@')), albedo.evaluated, albedo.sampled, albedo.sampledError, asymmetry);
				local.check(agree, fmt::format("{}: the albedo from evalSurface() ({:.4f}) and from sampling ({:.4f}) disagree", c.label, albedo.evaluated, albedo.sampled));
				local.check(conserves, fmt::format("{}: a white material reflects {:.4f} / {:.4f} of the light (more than 1)", c.label, albedo.evaluated, albedo.sampled));
				if (c.reciprocal) {
					local.check(asymmetry < 1e-3, fmt::format("{}: f(wo, wi) and f(wi, wo) differ by {:.3g} (relative)", c.label, asymmetry));
				}
			}
		};
		std::vector<std::thread> threads;
		for (unsigned t = 0; t < threadCount; t++) {
			threads.emplace_back(work);
		}
		for (std::thread& t : threads) {
			t.join();
		}
		std::string lastGroup;
		for (size_t index = 0; index < cases.size(); index++) {
			const Case& c = cases[index];
			const std::string group = c.label.substr(0, c.label.find('@'));
			if (printTables && group != lastGroup) {
				fmt::println("{}", group);
				lastGroup = group;
			}
			if (printTables && !results[index].line.empty()) {
				fmt::println("{}", results[index].line);
			}
			if (printTables) {
				for (const std::string& note : results[index].report.notes) {
					fmt::println("    {}", note);
				}
			}
			for (const std::string& message : results[index].report.messages) {
				report.fail(message);
			}
			report.pValues.insert(report.pValues.end(), results[index].report.pValues.begin(), results[index].report.pValues.end());
		}
	};

	auto judge = [&](Report& report, bool print) -> int {
		const int tests = (int)report.pValues.size();
		const double threshold = stats::sidakThreshold(0.01, tests);
		int failed = 0;
		double smallest = 1.0;
		for (const PValue& p : report.pValues) {
			smallest = std::min(smallest, p.p);
			if (p.p < threshold) {
				failed++;
				if (print) {
					fmt::println("    FAIL: {}: p = {:.3g}", p.what, p.p);
				}
			}
		}
		if (print) {
			fmt::println("{} chi-square tests, Sidak threshold {:.3g}, smallest p {:.3g}, {} below it", tests, threshold, smallest, failed);
		}
		return failed;
	};

	if (options.selfcheck) {
		// the unplanted suite passes, and each planted error is caught
		fmt::println("self-check: the model as it is");
		Report clean;
		runSuite(clean, false);
		const int cleanFailures = clean.hardFailures + judge(clean, true);
		int missed = 0;
		const struct {
			Mutation mutation;
			const char* name;
		} mutants[] = {
			{ Mutation::PdfScale, "pbr pdf x 1.05" },
			{ Mutation::EvalScale, "pbr evaluation x 1.05" },
			{ Mutation::SampleRoughness, "pbr sampling roughness x 1.1" },
		};
		std::vector<Case> pbrOnly;
		for (const Case& c : cases) {
			if (c.material.type == BsdfType::Pbr) {
				pbrOnly.push_back(c);
			}
		}
		cases = pbrOnly;
		for (const auto& mutant : mutants) {
			g_mutation = mutant.mutation;
			Report planted;
			planted.quiet = true;
			runSuite(planted, false);
			const int caught = planted.hardFailures + judge(planted, false);
			fmt::println("self-check: {} -> {} check(s) failed{}", mutant.name, caught, caught > 0 ? "" : "  <-- NOT CAUGHT");
			if (caught == 0) {
				missed++;
			}
		}
		g_mutation = Mutation::None;
		const int bxdfMissed = options.material.empty() ? runBxdfTests(options.samples, options.quadrature, true, "") : 0;
		fmt::println("self-check: {}", (cleanFailures == 0 && missed == 0 && bxdfMissed == 0) ? "ok" : "FAILED");
		return (cleanFailures == 0 && missed == 0 && bxdfMissed == 0) ? 0 : 1;
	}

	Report report;
	testRandom(report);
	std::vector<std::pair<std::string, BsdfMaterial>> cornerMaterials;
	for (const Case& c : cases) {
		if (c.angleDegrees == 0.f) {
			cornerMaterials.push_back({ c.label.substr(0, c.label.find('@')), c.material });
		}
	}
	for (const auto& [name, material] : cornerMaterials) {
		testCorners(material, name, report);
	}
	runSuite(report, true);
	const int failedP = judge(report, true);
	const int bxdfFailures = options.material.empty() ? runBxdfTests(options.samples, options.quadrature, false, "") : 0;
	const int failures = report.hardFailures + failedP + bxdfFailures;
	fmt::println("{}", failures == 0 ? "bsdf_test: ok" : fmt::format("bsdf_test: {} failure(s)", failures));
	return failures == 0 ? 0 : 1;
}
