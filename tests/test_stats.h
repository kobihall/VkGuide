#pragma once

// The statistics the CPU tests share (bin/bsdf_test, and the light and sampler tests as they take
// on chi-square checks): Pearson's chi-square test with pooled cells and a p-value, the Sidak
// correction for the number of configurations a test runs, and a histogram of directions over the
// hemisphere. Header only, no Vulkan.
//
// Pearson's test (PBR 4ed chapter 2 further reading; pbrt-v4 src/pbrt/bsdfs_test.cpp, after Jung
// et al. 2020): draw N samples from a routine, count them in cells, integrate the density the
// routine claims over each cell, and ask how likely counts that far from N * (cell integral) are
// if the claim is true. A p-value below a threshold means the routine and its density disagree.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

namespace stats {

inline constexpr double PI = 3.14159265358979323846;

// Q(a, x) = Gamma(a, x) / Gamma(a), the regularized upper incomplete gamma function: the survival
// function of a chi-square variable with 2a degrees of freedom at 2x (Numerical Recipes 6.2: the
// series for x < a + 1, the continued fraction beyond)
inline double upperIncompleteGammaQ(double a, double x)
{
	if (x <= 0.0) {
		return 1.0;
	}
	const double logPrefix = -x + a * std::log(x) - std::lgamma(a);
	if (x < a + 1.0) {
		double sum = 1.0 / a;
		double term = sum;
		for (int n = 1; n < 100000; n++) {
			term *= x / (a + n);
			sum += term;
			if (std::abs(term) < std::abs(sum) * 1e-16) {
				break;
			}
		}
		return std::max(0.0, 1.0 - sum * std::exp(logPrefix));
	}
	// Lentz's algorithm for the continued fraction
	const double tiny = 1e-300;
	double b = x + 1.0 - a;
	double c = 1.0 / tiny;
	double d = 1.0 / b;
	double h = d;
	for (int n = 1; n < 100000; n++) {
		const double an = -n * (n - a);
		b += 2.0;
		d = an * d + b;
		if (std::abs(d) < tiny) {
			d = tiny;
		}
		c = b + an / c;
		if (std::abs(c) < tiny) {
			c = tiny;
		}
		d = 1.0 / d;
		const double delta = d * c;
		h *= delta;
		if (std::abs(delta - 1.0) < 1e-16) {
			break;
		}
	}
	return std::exp(logPrefix) * h;
}

// the probability that a chi-square variable with `dof` degrees of freedom is at least `statistic`
inline double chiSquarePValue(double statistic, int dof)
{
	if (dof <= 0) {
		return 1.0;
	}
	return upperIncompleteGammaQ(0.5 * dof, 0.5 * statistic);
}

// the per-test threshold that keeps the chance of any of `tests` independent true nulls failing at
// `familyAlpha` (Sidak): 1 - (1 - alpha)^(1/n)
inline double sidakThreshold(double familyAlpha, int tests)
{
	return -std::expm1(std::log1p(-familyAlpha) / std::max(tests, 1));
}

struct ChiSquare {
	double statistic { 0.0 };
	int dof { 0 };
	double pValue { 1.0 };
	// the cells given, and how many bins they pooled into
	int cells { 0 };
	int bins { 0 };
	double impossible { 0.0 };
};

// Pearson's statistic over `observed` counts against `expected` counts. A cell whose expected count
// is below `minExpected` (5, the usual rule) is pooled with the other small ones, and the pool is
// merged into the smallest large cell if it is still below the rule. A cell of zero expected count
// joins the pool too, so a sample the claimed density says is impossible weighs in with the other
// small cells (`impossible` counts those samples, for a caller that wants to report them)
inline ChiSquare chiSquareTest(const std::vector<double>& observed, const std::vector<double>& expected, double minExpected = 5.0)
{
	ChiSquare result;
	result.cells = (int)observed.size();
	double poolObserved = 0.0;
	double poolExpected = 0.0;
	struct Bin {
		double observed;
		double expected;
	};
	std::vector<Bin> bins;
	for (size_t i = 0; i < observed.size(); i++) {
		if (expected[i] <= 0.0) {
			result.impossible += observed[i];
			poolObserved += observed[i];
			continue;
		}
		if (expected[i] < minExpected) {
			poolObserved += observed[i];
			poolExpected += expected[i];
		} else {
			bins.push_back({ observed[i], expected[i] });
		}
	}
	if (poolExpected > 0.0 || poolObserved > 0.0) {
		if (poolExpected >= minExpected || bins.empty()) {
			bins.push_back({ poolObserved, poolExpected });
		} else {
			auto smallest = std::min_element(bins.begin(), bins.end(), [](const Bin& a, const Bin& b) { return a.expected < b.expected; });
			smallest->observed += poolObserved;
			smallest->expected += poolExpected;
		}
	}
	for (const Bin& bin : bins) {
		const double difference = bin.observed - bin.expected;
		result.statistic += difference * difference / bin.expected;
	}
	result.bins = (int)bins.size();
	result.dof = result.bins - 1;
	result.pValue = chiSquarePValue(result.statistic, result.dof);
	return result;
}

// A grid over directions in a local frame (z up): `rings` rings of equal azimuthal width
// 2 pi / `azimuths`. The ring edges in z run from the top of the grid to its bottom as
// z = top - (top - bottom) (k / rings)^power: power 1 gives cells of equal solid angle, a larger
// power rings that thicken away from the pole, which is what resolves a lobe centered on it.
// `full` extends the grid from the pole to the opposite pole; otherwise it ends at the equator.
// A direction outside the grid belongs to no cell
struct DirectionGrid {
	int rings { 64 };
	int azimuths { 128 };
	double power { 1.0 };
	bool full { false };

	int cellCount() const { return rings * azimuths; }
	double bottom() const { return full ? -1.0 : 0.0; }
	double ringEdge(int k) const { return 1.0 - (1.0 - bottom()) * std::pow((double)k / rings, power); }
	// the solid angle of a cell: its ring's height in z times its azimuthal width
	double cellSolidAngle(int cell) const
	{
		const int ring = cell / azimuths;
		return (ringEdge(ring) - ringEdge(ring + 1)) * 2.0 * PI / azimuths;
	}

	// -1 for a direction below the grid
	int cellOf(const glm::dvec3& w) const
	{
		if (w.z < bottom() || (!full && w.z <= 0.0)) {
			return -1;
		}
		double phi = std::atan2(w.y, w.x);
		if (phi < 0.0) {
			phi += 2.0 * PI;
		}
		int ring = std::clamp((int)(rings * std::pow((1.0 - w.z) / (1.0 - bottom()), 1.0 / power)), 0, rings - 1);
		// the inverse of ringEdge() to rounding: settle on the ring whose edges bracket z
		while (ring > 0 && w.z > ringEdge(ring)) {
			ring--;
		}
		while (ring < rings - 1 && w.z < ringEdge(ring + 1)) {
			ring++;
		}
		const int azimuth = std::min((int)(phi / (2.0 * PI) * azimuths), azimuths - 1);
		return ring * azimuths + azimuth;
	}

	// the direction at the point (u, v) in [0, 1)^2 of cell `cell`; uniform in (z, phi) within it
	glm::dvec3 pointIn(int cell, double u, double v) const
	{
		const int ring = cell / azimuths;
		const int azimuth = cell % azimuths;
		const double z = ringEdge(ring) + u * (ringEdge(ring + 1) - ringEdge(ring));
		const double phi = 2.0 * PI * (azimuth + v) / azimuths;
		const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
		return glm::dvec3(r * std::cos(phi), r * std::sin(phi), z);
	}
};

// The integral over one cell of a vector-valued density (per solid angle, over directions in the
// grid's frame). The cell starts as `initial` x `initial` subcells, each integrated by comparing a
// one-point midpoint rule with the sum of its four quarters and splitting it further, to `maxDepth`
// levels, wherever they differ by more than `relativeTolerance` of the value. The starting grid is
// what keeps a lobe narrower than a cell from slipping between the sample points
template <typename Density>
glm::dvec4 integrateCell(const DirectionGrid& grid, int cell, int initial, int maxDepth, double relativeTolerance, const Density& density)
{
	using glm::dvec4;
	auto value = [&](double u, double v) { return density(grid.pointIn(cell, u, v)); };
	auto close = [&](const dvec4& coarse, const dvec4& fine) {
		for (int k = 0; k < 4; k++) {
			if (std::abs(coarse[k] - fine[k]) > relativeTolerance * std::abs(fine[k]) + 1e-9) {
				return false;
			}
		}
		return true;
	};
	std::function<dvec4(double, double, double, double, int, const dvec4&)> refine = [&](double u0, double u1, double v0, double v1, int depth, const dvec4& centre) {
		const double um = 0.5 * (u0 + u1);
		const double vm = 0.5 * (v0 + v1);
		const double area = (u1 - u0) * (v1 - v0);
		const dvec4 quarters[4] = {
			value(0.5 * (u0 + um), 0.5 * (v0 + vm)),
			value(0.5 * (um + u1), 0.5 * (v0 + vm)),
			value(0.5 * (u0 + um), 0.5 * (vm + v1)),
			value(0.5 * (um + u1), 0.5 * (vm + v1)),
		};
		const dvec4 fine = (quarters[0] + quarters[1] + quarters[2] + quarters[3]) * (area / 4.0);
		if (depth >= maxDepth || close(centre * area, fine)) {
			return fine;
		}
		return refine(u0, um, v0, vm, depth + 1, quarters[0]) + refine(um, u1, v0, vm, depth + 1, quarters[1]) + refine(u0, um, vm, v1, depth + 1, quarters[2]) + refine(um, u1, vm, v1, depth + 1, quarters[3]);
	};
	dvec4 sum(0.0);
	for (int i = 0; i < initial; i++) {
		for (int j = 0; j < initial; j++) {
			const double u0 = (double)i / initial;
			const double u1 = (double)(i + 1) / initial;
			const double v0 = (double)j / initial;
			const double v1 = (double)(j + 1) / initial;
			sum += refine(u0, u1, v0, v1, 0, value(0.5 * (u0 + u1), 0.5 * (v0 + v1)));
		}
	}
	return sum * grid.cellSolidAngle(cell);
}

// the mean and the standard error of the mean of a stream of values, by Welford's update
struct Running {
	double count { 0.0 };
	double mean { 0.0 };
	double m2 { 0.0 };

	void add(double x)
	{
		count += 1.0;
		const double delta = x - mean;
		mean += delta / count;
		m2 += delta * (x - mean);
	}
	double variance() const { return count > 1.0 ? m2 / (count - 1.0) : 0.0; }
	double standardError() const { return count > 0.0 ? std::sqrt(variance() / count) : 0.0; }
};

}  // namespace stats
