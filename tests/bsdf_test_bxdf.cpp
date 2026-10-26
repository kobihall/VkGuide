// bin/bsdf_test's checks of pbrt-v4's BxDFs (src/bxdf.h, the mirror of crt_fresnel.glsl, crt_microfacet.glsl and
// crt_bxdf.glsl): the glass and conductor material types. They work in the local shading frame and, for glass, over
// the whole sphere, so they have their own section rather than rows in the hemisphere suite of bsdf_test.cpp.
//
//  1. FRESNEL. Normal incidence gives ((n - 1) / (n + 1))^2; at Brewster's angle the p reflectance vanishes, so the
//     unpolarized one is half the s one; past the critical angle the reflectance is 1; frComplex() with k = 0 is
//     frDielectric(); pbrt's silver reflects 0.95 to 0.97 at 633 nm at normal incidence.
//  2. SAMPLES AND DENSITY. A million directions from sample(), counted in a 64 x 128 grid over the sphere, against the
//     integral of pdf() over each cell (Pearson's chi-square, Sidak-corrected), the failed samples one more cell.
//  3. WEIGHTS. The mean of f |cos| / pdf over the samples estimates the integral of f |cos| over the sphere; the two
//     must agree within the estimate's error.
//  4. ENERGY. In importance mode a dielectric loses nothing to the eta^2 factor: its albedo is 1 when smooth and at
//     most 1 when rough, and a conductor reflects at most 1.
//  5. RECIPROCITY. A conductor and a dielectric's reflection are symmetric; a dielectric's transmission satisfies
//     eta_i^2 f(wo, wi) = eta_o^2 f(wi, wo) in radiance mode (PBR 4ed 9.5.2).
//  6. DELTA LOBES. A smooth surface reflects to the mirror direction and refracts by Snell's law, with the weight the
//     Fresnel term sets.
// --selfcheck plants a dielectric density 5% high and a conductor evaluation 5% high, and expects them caught.

#include <bxdf.h>
#include <spectrum.h>

#include "test_stats.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

namespace {

using glm::dvec3;
using glm::vec3;

constexpr float PI_F = 3.14159265358979323846f;

enum class Plant {
	None,
	DielectricPdf,
	ConductorEval,
};
Plant g_plant = Plant::None;

// one BxDF under test, as a dielectric or a conductor
struct BxdfCase {
	std::string label;
	bool conductor { false };
	DielectricBxdf dielectric;
	ConductorBxdf metal;
	// the cosine of wo with the outward normal; negative is from inside the glass
	float cosO { 0.5f };
	TransportMode mode { TransportMode::Radiance };
};

bool caseSample(const BxdfCase& c, vec3 wo, float uc, glm::vec2 u, BxdfSample& s)
{
	return c.conductor ? c.metal.sample(wo, uc, u, c.mode, s) : c.dielectric.sample(wo, uc, u, c.mode, s);
}

vec3 caseF(const BxdfCase& c, vec3 wo, vec3 wi)
{
	const vec3 f = c.conductor ? c.metal.f(wo, wi, c.mode) : c.dielectric.f(wo, wi, c.mode);
	return g_plant == Plant::ConductorEval && c.conductor ? f * 1.05f : f;
}

float casePdf(const BxdfCase& c, vec3 wo, vec3 wi)
{
	const float p = c.conductor ? c.metal.pdf(wo, wi, c.mode) : c.dielectric.pdf(wo, wi, c.mode);
	return g_plant == Plant::DielectricPdf && !c.conductor ? p * 1.05f : p;
}

vec3 woFor(float cosO)
{
	//a little azimuth, so an anisotropic distribution is not seen along its axes
	const float sinO = std::sqrt(std::max(0.f, 1.f - cosO * cosO));
	return glm::normalize(vec3(sinO * std::cos(0.7f), sinO * std::sin(0.7f), cosO));
}

std::vector<BxdfCase> buildCases()
{
	std::vector<BxdfCase> cases;
	auto addDielectric = [&](float eta, float ax, float ay, float cosO, TransportMode mode) {
		BxdfCase c;
		c.dielectric.eta = eta;
		c.dielectric.distribution = TrowbridgeReitz(ax, ay);
		c.cosO = cosO;
		c.mode = mode;
		c.label = fmt::format("glass eta={} alpha=({}, {}) cos={} {}", eta, ax, ay, cosO, mode == TransportMode::Radiance ? "radiance" : "importance");
		cases.push_back(c);
	};
	for (const float eta : { 1.5f, 1.33f, 2.4f }) {
		for (const float alpha : { 0.1f, 0.4f, 0.8f }) {
			for (const float cosO : { 0.9f, 0.4f, -0.7f }) {
				addDielectric(eta, alpha, alpha, cosO, TransportMode::Radiance);
			}
		}
	}
	addDielectric(1.5f, 0.15f, 0.6f, 0.6f, TransportMode::Radiance);
	addDielectric(1.5f, 0.3f, 0.3f, 0.5f, TransportMode::Importance);
	addDielectric(1.5f, 0.3f, 0.3f, -0.5f, TransportMode::Importance);
	auto addConductor = [&](const char* metal, vec3 eta, vec3 k, float ax, float ay, float cosO) {
		BxdfCase c;
		c.conductor = true;
		c.metal.distribution = TrowbridgeReitz(ax, ay);
		c.metal.eta = eta;
		c.metal.k = k;
		c.cosO = cosO;
		c.label = fmt::format("conductor {} alpha=({}, {}) cos={}", metal, ax, ay, cosO);
		cases.push_back(c);
	};
	//pbrt's copper and gold at 630, 532 and 465 nm, as exact RGB reads them
	auto at = [](const char* name) {
		const DenseSpectrum s = *namedSpectrum(name);
		return vec3((float)s(630), (float)s(532), (float)s(465));
	};
	const vec3 cuEta = at("metal-Cu-eta"), cuK = at("metal-Cu-k");
	const vec3 auEta = at("metal-Au-eta"), auK = at("metal-Au-k");
	for (const float alpha : { 0.05f, 0.3f, 0.7f }) {
		for (const float cosO : { 0.9f, 0.3f }) {
			addConductor("Cu", cuEta, cuK, alpha, alpha, cosO);
		}
	}
	addConductor("Au", auEta, auK, 0.2f, 0.5f, 0.6f);
	//pbrt's reflectance mode at its limit: the most a conductor can reflect, which must stay at or below 1
	const ConductorBxdf white = ConductorBxdf::fromReflectance(TrowbridgeReitz(0.4f, 0.4f), vec3(1.f));
	addConductor("reflectance 1", white.eta, white.k, 0.4f, 0.4f, 0.7f);
	return cases;
}

struct Result {
	std::vector<std::string> failures;
	std::vector<std::pair<std::string, double>> pValues;
	std::string line;
};

void runCase(const BxdfCase& c, uint64_t samples, int quadrature, Result& out)
{
	const vec3 wo = woFor(c.cosO);
	const stats::DirectionGrid grid { 64, 128, 1.0, true };
	const int cells = grid.cellCount();

	//the density's and the weighted evaluation's integrals over each cell
	std::vector<double> expected(cells + 1, 0.0);
	std::vector<dvec3> fCos(cells, dvec3(0.0));
	double kept = 0.0;
	dvec3 albedoQuadrature(0.0);
	for (int cell = 0; cell < cells; cell++) {
		const glm::dvec4 sum = stats::integrateCell(grid, cell, quadrature, 5, 2e-3, [&](const dvec3& w) {
			const vec3 wi(w);
			const vec3 f = caseF(c, wo, wi) * std::abs(wi.z);
			return glm::dvec4(f.x, f.y, f.z, casePdf(c, wo, wi));
		});
		expected[cell] = sum.w * (double)samples;
		kept += sum.w;
		fCos[cell] = dvec3(sum);
		albedoQuadrature += dvec3(sum);
	}
	expected[cells] = std::max(0.0, 1.0 - kept) * (double)samples;

	//the samples, counted, and their weights summed
	std::vector<double> observed(cells + 1, 0.0);
	stats::Running weight[3];
	std::mt19937 engine(1234);
	std::uniform_real_distribution<float> uniform(0.f, 1.f);
	bool finite = true;
	for (uint64_t i = 0; i < samples; i++) {
		const float uc = uniform(engine);
		const glm::vec2 u(uniform(engine), uniform(engine));
		BxdfSample s;
		vec3 w(0.f);
		if (caseSample(c, wo, uc, u, s) && s.pdf > 0.f) {
			const int cell = grid.cellOf(dvec3(s.wi));
			observed[cell >= 0 ? cell : cells] += 1.0;
			w = s.f * std::abs(s.wi.z) / s.pdf;
			finite = finite && std::isfinite(w.x) && std::isfinite(w.y) && std::isfinite(w.z);
		} else {
			observed[cells] += 1.0;
		}
		for (int k = 0; k < 3; k++) {
			weight[k].add(w[k]);
		}
	}
	if (!finite) {
		out.failures.push_back(c.label + ": a sample's weight is not finite");
	}
	const stats::ChiSquare test = stats::chiSquareTest(observed, expected);
	out.pValues.push_back({ c.label, test.pValue });

	//weights against the quadrature, per channel
	for (int k = 0; k < 3; k++) {
		const double tolerance = 5.0 * weight[k].standardError() + 2e-3 * std::max(1.0, albedoQuadrature[k]);
		if (std::abs(weight[k].mean - albedoQuadrature[k]) > tolerance) {
			out.failures.push_back(fmt::format("{}: the samples' mean weight {:.5f} is not the integral of f cos {:.5f} (channel {})", c.label, weight[k].mean, albedoQuadrature[k], k));
		}
	}
	//energy: a conductor and an importance-mode dielectric reflect and transmit at most what arrives
	if (c.conductor || c.mode == TransportMode::Importance) {
		for (int k = 0; k < 3; k++) {
			if (albedoQuadrature[k] > 1.0 + 3e-3) {
				out.failures.push_back(fmt::format("{}: the albedo is {:.4f}, more than 1", c.label, albedoQuadrature[k]));
			}
		}
	}
	//reciprocity, over random direction pairs: a reflection is symmetric; a radiance-mode transmission satisfies
	//eta_i^2 f(wo, wi) = eta_o^2 f(wi, wo), eta the index on each direction's side; an importance-mode BxDF is the
	//adjoint of the radiance-mode one, f_importance(wo, wi) = f_radiance(wi, wo)
	double reflectAsymmetry = 0.0;
	double transmitAsymmetry = 0.0;
	BxdfCase radiance = c;
	radiance.mode = TransportMode::Radiance;
	for (int i = 0; i < 4000; i++) {
		const dvec3 a = grid.pointIn((int)(uniform(engine) * (cells - 1)), uniform(engine), uniform(engine));
		const vec3 wi(a);
		const bool transmits = wo.z * wi.z < 0.f;
		float lhs = caseF(c, wo, wi).x;
		float rhs = caseF(c.mode == TransportMode::Importance ? radiance : c, wi, wo).x;
		if (transmits && !c.conductor && c.mode == TransportMode::Radiance) {
			const float etaO = wo.z > 0.f ? 1.f : c.dielectric.eta;
			const float etaI = wi.z > 0.f ? 1.f : c.dielectric.eta;
			lhs *= (etaI * etaI) / (etaO * etaO);
		}
		if (lhs + rhs > 1e-4f) {
			double& worst = transmits ? transmitAsymmetry : reflectAsymmetry;
			worst = std::max(worst, (double)std::abs(lhs - rhs) / (lhs + rhs));
		}
	}
	const double asymmetry = std::max(reflectAsymmetry, transmitAsymmetry);
	//float rounding in the generalized half vector, where dot(wi, wm) + dot(wo, wm) / eta nearly cancels, leaves
	//transmission a few parts in a thousand off; a reflection must be exact to float precision
	if (reflectAsymmetry > 1e-4 || transmitAsymmetry > 2e-2) {
		out.failures.push_back(fmt::format("{}: not reciprocal, relative asymmetry {:.3g} in reflection, {:.3g} in transmission", c.label, reflectAsymmetry, transmitAsymmetry));
	}
	out.line = fmt::format("  {:<58} albedo {:.4f} {:.4f} {:.4f}  sampled {:.4f}  asymmetry {:.2g}  p {:.3g}", c.label, albedoQuadrature.x, albedoQuadrature.y, albedoQuadrature.z, weight[0].mean,
		asymmetry, test.pValue);
}

int checkFresnel()
{
	int failures = 0;
	auto check = [&](bool ok, const std::string& what) {
		if (!ok) {
			fmt::println("    FAIL: {}", what);
			failures++;
		}
	};
	fmt::println("Fresnel");
	for (const float n : { 1.33f, 1.5f, 2.4f }) {
		const float r0 = (n - 1.f) / (n + 1.f);
		check(std::abs(frDielectric(1.f, n) - r0 * r0) < 1e-6f, fmt::format("normal incidence on n = {} reflects {:.6f}, expected {:.6f}", n, frDielectric(1.f, n), r0 * r0));
		//Brewster's angle: tan theta = n, where the p reflectance is zero and the unpolarized is half the s one
		const float thetaB = std::atan(n);
		const float cosB = std::cos(thetaB);
		const float cosT = std::sqrt(1.f - std::sin(thetaB) * std::sin(thetaB) / (n * n));
		const float rs = (cosB - n * cosT) / (cosB + n * cosT);
		check(std::abs(frDielectric(cosB, n) - 0.5f * rs * rs) < 1e-5f, fmt::format("at Brewster's angle on n = {} the reflectance is half the s one", n));
		//from inside, past the critical angle
		const float cosCritical = std::sqrt(1.f - 1.f / (n * n));
		check(frDielectric(-(cosCritical * 0.98f), n) == 1.f, fmt::format("inside n = {}, past the critical angle, everything reflects", n));
		check(frDielectric(-(cosCritical * 1.02f), n) < 1.f, fmt::format("inside n = {}, before the critical angle, some transmits", n));
		for (const float cosI : { 1.f, 0.7f, 0.3f, 0.05f }) {
			check(std::abs(frComplex(cosI, n, 0.f) - frDielectric(cosI, n)) < 2e-6f, fmt::format("frComplex() with k = 0 is frDielectric() at cos {} on n = {}", cosI, n));
		}
	}
	const DenseSpectrum agEta = *namedSpectrum("metal-Ag-eta"), agK = *namedSpectrum("metal-Ag-k");
	const float silver = frComplex(1.f, (float)agEta(633), (float)agK(633));
	check(silver > 0.95f && silver < 0.97f, fmt::format("silver reflects {:.4f} at 633 nm, expected 0.95 to 0.97", silver));
	//the smooth lobes: mirror and Snell, and their weights
	DielectricBxdf glass;
	glass.eta = 1.5f;
	glass.distribution = TrowbridgeReitz(0.f, 0.f);
	const vec3 wo = woFor(0.8f);
	BxdfSample s;
	check(glass.sample(wo, 0.f, glm::vec2(0.5f), TransportMode::Importance, s) && (s.flags & BXDF_REFLECTION) && glm::length(s.wi - vec3(-wo.x, -wo.y, wo.z)) < 1e-6f,
		"smooth glass reflects to the mirror direction");
	check(std::abs(s.f.x * std::abs(s.wi.z) / s.pdf - 1.f) < 1e-5f, "smooth glass's reflection weighs 1 in importance mode");
	check(glass.sample(wo, 0.999f, glm::vec2(0.5f), TransportMode::Radiance, s) && (s.flags & BXDF_TRANSMISSION), "smooth glass transmits");
	const float sinI = std::sqrt(1.f - wo.z * wo.z);
	const float sinT = std::sqrt(1.f - s.wi.z * s.wi.z);
	check(std::abs(sinI - 1.5f * sinT) < 1e-5f && s.wi.z < 0.f, fmt::format("smooth glass refracts by Snell's law: sin {:.6f} = 1.5 x {:.6f}", sinI, sinT));
	check(std::abs(s.f.x * std::abs(s.wi.z) / s.pdf - 1.f / (1.5f * 1.5f)) < 1e-5f, "smooth glass's transmission weighs 1 / eta^2 in radiance mode");
	ConductorBxdf mirror;
	mirror.distribution = TrowbridgeReitz(0.f, 0.f);
	mirror.eta = vec3(0.2f);
	mirror.k = vec3(3.f);
	check(mirror.sample(wo, 0.f, glm::vec2(0.5f), TransportMode::Radiance, s) && std::abs(s.f.x * std::abs(s.wi.z) / s.pdf - frComplex(wo.z, 0.2f, 3.f)) < 1e-6f,
		"a smooth conductor reflects its Fresnel term");
	return failures;
}

}

// bin/bsdf_test calls this after its hemisphere suite: the failures, printed as it goes
int runBxdfTests(uint64_t samples, int quadrature, bool selfcheck, const std::string& filter)
{
	std::vector<BxdfCase> cases;
	for (const BxdfCase& c : buildCases()) {
		if (filter.empty() || c.label.find(filter) != std::string::npos) {
			cases.push_back(c);
		}
	}
	auto suite = [&](bool print) {
		std::vector<Result> results(cases.size());
		std::atomic<size_t> next { 0 };
		std::vector<std::thread> threads;
		for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); t++) {
			threads.emplace_back([&] {
				for (size_t i = next++; i < cases.size(); i = next++) {
					runCase(cases[i], samples, quadrature, results[i]);
				}
			});
		}
		for (std::thread& t : threads) {
			t.join();
		}
		int failures = 0;
		std::vector<double> ps;
		for (const Result& r : results) {
			if (print) {
				fmt::println("{}", r.line);
			}
			for (const std::string& f : r.failures) {
				if (print) {
					fmt::println("    FAIL: {}", f);
				}
				failures++;
			}
			for (const auto& p : r.pValues) {
				ps.push_back(p.second);
			}
		}
		const double threshold = stats::sidakThreshold(0.01, (int)ps.size());
		for (size_t i = 0; i < ps.size(); i++) {
			if (ps[i] < threshold) {
				if (print) {
					fmt::println("    FAIL: {}: samples and density disagree, p = {:.3g}", results[i].pValues[0].first, ps[i]);
				}
				failures++;
			}
		}
		return failures;
	};

	if (selfcheck) {
		int missed = 0;
		const int clean = suite(false);
		for (const auto& [plant, name] : { std::pair { Plant::DielectricPdf, "dielectric pdf x 1.05" }, std::pair { Plant::ConductorEval, "conductor evaluation x 1.05" } }) {
			//a plant in a kind --material left out has nothing to be caught by
			const bool conductor = plant == Plant::ConductorEval;
			if (std::none_of(cases.begin(), cases.end(), [&](const BxdfCase& c) { return c.conductor == conductor; })) {
				continue;
			}
			g_plant = plant;
			const int caught = suite(false);
			fmt::println("self-check: {} -> {} check(s) failed{}", name, caught, caught > 0 ? "" : "  <-- NOT CAUGHT");
			missed += caught == 0;
		}
		g_plant = Plant::None;
		fmt::println("self-check (BxDFs): {}", clean == 0 && missed == 0 ? "ok" : "FAILED");
		return clean + missed;
	}
	int failures = checkFresnel();
	fmt::println("pbrt-v4's BxDFs (glass, conductor)");
	failures += suite(true);
	return failures;
}
