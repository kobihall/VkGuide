// CPU verification of the film (src/film.h, the line-for-line mirror of shaders/rt/include/crt_filter.glsl
// and crt_film.glsl). No Vulkan, no GPU.
//
// What the film promises:
//  - each filter is the function pbrt-v4 defines: zero outside its radius, the known value at its
//    center, negative lobes exactly where the kind says so, and the integral Mitchell's construction
//    gives
//  - the table samples its own distribution: a chi-square test of the sampled offsets against the
//    cells' |f|, and a sample's weight is the sign of the filter in the cell it landed in
//  - the weighted mean sum(w g) / sum(w) of a function over filtered samples converges to the filter
//    weighted average of it, which is the estimator PBR 4ed equation 5.13 uses, and a constant
//    function comes out exactly itself
//  - the compensated sum keeps float's twelve extra digits where a plain float sum stalls
//  - Welford's mean and M2 match a two-pass computation in double, and the standard error follows
//  - the clamp keeps a sample's color and limits its largest component
//  - outlier reweighting (Zirr et al. 2018): the cascade conserves the samples and counts each as one,
//    a well-sampled bright value keeps its energy, and a lone firefly is mostly discarded
// --selfcheck proves the tests can fail: it plants a shifted sampler and a compensation that does
// nothing, and fails unless both are caught.
//
// Run:  ./bin/film_test [--selfcheck]

#include <film.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>

#include <fmt/format.h>

#include "test_stats.h"

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

using SampleFunction = std::function<FilterSample(const FilterTable&, const glm::vec2&)>;
using AddFunction = std::function<void(CompensatedFloat&, float)>;

//---------------------------------------------------------------- the filters

void testFilterShapes()
{
	fmt::println("filters");
	for (uint32_t k = 0; k < (uint32_t)PixelFilterKind::Count; k++) {
		const PixelFilterKind kind = (PixelFilterKind)k;
		const PixelFilterSettings filter = defaultPixelFilter(kind);
		const std::string name = pixelFilterName(kind);
		check(std::string(pixelFilterId(kind)) != "" && pixelFilterFromId(pixelFilterId(kind)) == kind, name + ": id round trip");

		//zero beyond the radius (the Gaussian is shifted to reach zero exactly there)
		check(evaluatePixelFilter(filter, glm::vec2(filter.radius * 1.001f, 0.f)) == 0.f, name + ": zero outside the radius");
		check(evaluatePixelFilter(filter, glm::vec2(0.f, -filter.radius * 1.5f)) == 0.f, name + ": zero outside the radius (y)");
		check(evaluatePixelFilter(filter, glm::vec2(0.f)) > 0.f, name + ": positive at the center");

		//symmetric and separable in x and y
		const glm::vec2 p(0.31f * filter.radius, -0.17f * filter.radius);
		check(std::abs(evaluatePixelFilter(filter, p) - evaluatePixelFilter(filter, -p)) < 1e-6f, name + ": symmetric");
		check(std::abs(evaluatePixelFilter(filter, p) - evaluatePixelFilter(filter, glm::vec2(p.y, p.x))) < 1e-6f, name + ": same along both axes");

		//a negative lobe exactly where the kind says so
		float lowest = 0.f;
		for (int i = 0; i <= 400; i++) {
			lowest = std::min(lowest, evaluatePixelFilter(filter, glm::vec2(filter.radius * (float)i / 400.f, 0.f)));
		}
		check((lowest < -1e-4f) == pixelFilterHasNegativeLobes(kind), fmt::format("{}: negative lobes are {} (lowest value {})", name, pixelFilterHasNegativeLobes(kind) ? "claimed" : "not claimed", lowest));
	}

	//known values
	const float third = 1.f / 3.f;
	check(std::abs(evaluatePixelFilter(defaultPixelFilter(PixelFilterKind::Box), glm::vec2(0.49f, -0.49f)) - 1.f) < 1e-6f, "box is 1 inside");
	check(std::abs(evaluatePixelFilter(defaultPixelFilter(PixelFilterKind::Triangle), glm::vec2(0.f)) - 4.f) < 1e-6f, "triangle peaks at radius^2");
	check(std::abs(evaluatePixelFilter(defaultPixelFilter(PixelFilterKind::Triangle), glm::vec2(1.f, 0.f)) - 2.f) < 1e-6f, "triangle falls linearly");
	//Mitchell1D(0) = (6 - 2B) / 6, so the 2D filter at its center is the square
	const float mitchellCenter = (6.f - 2.f * third) / 6.f;
	check(std::abs(evaluatePixelFilter(defaultPixelFilter(PixelFilterKind::Mitchell), glm::vec2(0.f)) - mitchellCenter * mitchellCenter) < 1e-5f, "Mitchell center");
	//Lanczos is 1 at its center and 0 at the integers
	check(std::abs(evaluatePixelFilter(defaultPixelFilter(PixelFilterKind::Lanczos), glm::vec2(0.f)) - 1.f) < 1e-6f, "Lanczos center is 1");
	check(std::abs(evaluatePixelFilter(defaultPixelFilter(PixelFilterKind::Lanczos), glm::vec2(1.f, 0.f))) < 1e-6f, "Lanczos is 0 at 1");
	const PixelFilterSettings gaussian = defaultPixelFilter(PixelFilterKind::Gaussian);
	const float gaussianCenter = 1.f / std::sqrt(2.f * 3.14159265f * 0.25f);
	const float gaussianEdge = gaussianCenter * std::exp(-(1.5f * 1.5f) / (2.f * 0.25f));
	check(std::abs(evaluatePixelFilter(gaussian, glm::vec2(0.f)) - (gaussianCenter - gaussianEdge) * (gaussianCenter - gaussianEdge)) < 1e-5f, "Gaussian center");

	//Mitchell-Netravali integrates to 1 over [-2, 2] in its own units, so the 2D filter integrates
	//to (radius / 2)^2 over its domain
	const FilterTable mitchell = buildFilterTable(defaultPixelFilter(PixelFilterKind::Mitchell));
	double integral = 0.0;
	for (const float value : mitchell.values) {
		integral += value;
	}
	const double cell = (2.0 * mitchell.radius / mitchell.width) * (2.0 * mitchell.radius / mitchell.height);
	check(std::abs(integral * cell - 1.0) < 0.01, fmt::format("Mitchell integrates to 1 (got {:.4f})", integral * cell));

	//the table has pbrt's resolution: 32 cells per unit of radius, along each axis
	check(buildFilterTable(defaultPixelFilter(PixelFilterKind::Gaussian)).width == 48, "Gaussian table is 48 cells wide");
	check(buildFilterTable(defaultPixelFilter(PixelFilterKind::Box)).width == 16, "box table is 16 cells wide");
}

//---------------------------------------------------------------- sampling the table

uint32_t cellOf(const FilterTable& table, const glm::vec2& offset, uint32_t& row)
{
	const int column = std::clamp((int)((offset.x / table.radius + 1.f) * 0.5f * (float)table.width), 0, (int)table.width - 1);
	row = (uint32_t)std::clamp((int)((offset.y / table.radius + 1.f) * 0.5f * (float)table.height), 0, (int)table.height - 1);
	return (uint32_t)column;
}

// a chi-square test of the sampled offsets against the table's |f|, and the sign of each weight
void testSampling(const SampleFunction& sample, int familySize, bool report)
{
	if (report) {
		fmt::println("sampling the table");
	}
	const double alpha = stats::sidakThreshold(1e-3, familySize);
	std::mt19937 engine(12345);
	std::uniform_real_distribution<float> uniform(0.f, 0.99999994f);

	for (uint32_t k = 0; k < (uint32_t)PixelFilterKind::Count; k++) {
		const PixelFilterKind kind = (PixelFilterKind)k;
		const FilterTable table = buildFilterTable(defaultPixelFilter(kind));
		const size_t cells = (size_t)table.width * table.height;
		const int samples = 2'000'000;

		std::vector<double> observed(cells, 0.0);
		double wrongSigns = 0.0;
		double outside = 0.0;
		for (int i = 0; i < samples; i++) {
			const FilterSample s = sample(table, glm::vec2(uniform(engine), uniform(engine)));
			if (std::abs(s.offset.x) > table.radius || std::abs(s.offset.y) > table.radius) {
				outside++;
				continue;
			}
			uint32_t row;
			const uint32_t column = cellOf(table, s.offset, row);
			observed[(size_t)row * table.width + column] += 1.0;
			const float f = table.values[(size_t)row * table.width + column];
			if (s.weight != (f < 0.f ? -1.f : 1.f)) {
				wrongSigns++;
			}
		}
		double total = 0.0;
		for (const float value : table.distribution.func) {
			total += value;
		}
		std::vector<double> expected(cells);
		for (size_t i = 0; i < cells; i++) {
			expected[i] = samples * (double)table.distribution.func[i] / total;
		}
		const stats::ChiSquare result = stats::chiSquareTest(observed, expected);
		const std::string name = pixelFilterName(kind);
		if (report) {
			fmt::println("  {:26s} chi-square {:.1f} on {} bins, p = {:.3g}, {} wrong signs", name, result.statistic, result.bins, result.pValue, (int)wrongSigns);
		}
		check(result.pValue > alpha, fmt::format("{}: the sampled offsets follow |f| (p = {:.3g}, need above {:.3g})", name, result.pValue, alpha));
		check(wrongSigns == 0.0 && outside == 0.0, fmt::format("{}: {} wrong weight signs, {} samples outside the domain", name, (int)wrongSigns, (int)outside));
	}
}

// sum(w g) / sum(w) over filtered samples against the filter-weighted average of g, for g the
// indicator of the pixel [-0.5, 0.5]^2 and for a constant
void testWeightedMean()
{
	fmt::println("the weighted mean");
	std::mt19937 engine(777);
	std::uniform_real_distribution<float> uniform(0.f, 0.99999994f);

	for (uint32_t k = 0; k < (uint32_t)PixelFilterKind::Count; k++) {
		const PixelFilterKind kind = (PixelFilterKind)k;
		const FilterTable table = buildFilterTable(defaultPixelFilter(kind));
		const std::string name = pixelFilterName(kind);

		//the filter-weighted average of the pixel's indicator, integrated cell by cell
		const double cellWidth = 2.0 * table.radius / table.width;
		double numerator = 0.0;
		double denominator = 0.0;
		for (uint32_t row = 0; row < table.height; row++) {
			for (uint32_t column = 0; column < table.width; column++) {
				auto overlap = [&](uint32_t index) {
					const double low = -table.radius + index * cellWidth;
					return std::max(0.0, std::min(low + cellWidth, 0.5) - std::max(low, -0.5)) / cellWidth;
				};
				const double f = table.values[(size_t)row * table.width + column];
				numerator += f * overlap(row) * overlap(column);
				denominator += f;
			}
		}
		const double expected = numerator / denominator;

		const int batches = 40;
		const int perBatch = 60000;
		stats::Running means;
		bool constantExact = true;
		for (int b = 0; b < batches; b++) {
			double sumG = 0.0;
			double sumOne = 0.0;
			double sumW = 0.0;
			for (int i = 0; i < perBatch; i++) {
				const FilterSample s = sampleFilterTable(table, glm::vec2(uniform(engine), uniform(engine)));
				const double g = (std::abs(s.offset.x) <= 0.5f && std::abs(s.offset.y) <= 0.5f) ? 1.0 : 0.0;
				sumG += s.weight * g;
				sumOne += s.weight * 1.0;
				sumW += s.weight;
			}
			means.add(sumG / sumW);
			constantExact &= sumOne / sumW == 1.0;
		}
		const double error = std::abs(means.mean - expected);
		fmt::println("  {:26s} estimate {:.5f} against {:.5f} (standard error {:.5f})", name, means.mean, expected, means.standardError());
		check(error < 5.0 * means.standardError() + 2e-4, fmt::format("{}: weighted mean {:.5f} differs from {:.5f}", name, means.mean, expected));
		check(constantExact, name + ": the weighted mean of a constant is exactly the constant");
	}
}

//---------------------------------------------------------------- the sums

void testCompensatedSum(const AddFunction& add, bool report)
{
	if (report) {
		fmt::println("the compensated sum");
	}
	//copies of 0.1: a plain float sum stalls once the total outgrows the addend's last bit. Neumaier's
	//compensation is itself a float sum, so it holds only while n times float's epsilon is well below 1:
	//exact to 2^17 terms here, and still two orders of magnitude better than plain at 2^20 (a pixel
	//takes thousands of samples; 2^24 would reach n x epsilon = 1 and not be compensated at all)
	const float value = 0.1f;
	for (const uint32_t count : { 1u << 16, 1u << 20 }) {
		CompensatedFloat compensated;
		float plain = 0.f;
		for (uint32_t i = 0; i < count; i++) {
			add(compensated, value);
			plain += value;
		}
		const double exact = (double)count * (double)value;
		const double compensatedError = std::abs((double)compensatedValue(compensated) - exact) / exact;
		const double plainError = std::abs((double)plain - exact) / exact;
		if (report) {
			fmt::println("  {} x 0.1: compensated relative error {:.3g}, plain float {:.3g}", count, compensatedError, plainError);
		}
		check(compensatedError < (count <= (1u << 16) ? 1e-6 : 2e-4), fmt::format("compensated sum of {} x 0.1 has relative error {:.3g}", count, compensatedError));
		check(plainError > 100.0 * compensatedError, fmt::format("a plain float sum should be far worse (relative error {:.3g} against {:.3g}); the test would not tell the difference", plainError, compensatedError));
	}

	//a large term, many small ones and the large one back out: the small ones survive
	CompensatedFloat cancelling;
	add(cancelling, 1e8f);
	for (int i = 0; i < 1000; i++) {
		add(cancelling, 1.f);
	}
	add(cancelling, -1e8f);
	check(std::abs(compensatedValue(cancelling) - 1000.f) < 1e-3f, fmt::format("1e8 + 1000 x 1 - 1e8 = {}", compensatedValue(cancelling)));

	//random signed values over a wide range against double
	std::mt19937 engine(99);
	std::uniform_real_distribution<float> uniform(-1.f, 1.f);
	std::uniform_real_distribution<float> exponent(-10.f, 10.f);
	CompensatedFloat random;
	double reference = 0.0;
	double magnitude = 0.0;
	for (int i = 0; i < 1'000'000; i++) {
		const float x = uniform(engine) * std::exp2(exponent(engine));
		add(random, x);
		reference += (double)x;
		magnitude += std::abs((double)x);
	}
	const double randomError = std::abs((double)compensatedValue(random) - reference) / magnitude;
	if (report) {
		fmt::println("  1e6 random terms over 2^-10..2^10: error {:.3g} of the sum of magnitudes", randomError);
	}
	check(randomError < 2e-7, fmt::format("random terms: error {:.3g} of the sum of magnitudes", randomError));
}

void testWelford()
{
	fmt::println("Welford");
	std::mt19937 engine(31337);
	std::normal_distribution<float> normal(3.f, 2.f);
	WelfordState state;
	std::vector<double> values;
	for (int i = 0; i < 100000; i++) {
		const float x = normal(engine);
		welfordAdd(state, x);
		values.push_back(x);
	}
	double mean = 0.0;
	for (const double x : values) {
		mean += x;
	}
	mean /= (double)values.size();
	double m2 = 0.0;
	for (const double x : values) {
		m2 += (x - mean) * (x - mean);
	}
	check(std::abs(state.mean - mean) < 1e-4, fmt::format("mean {} against {}", state.mean, mean));
	check(std::abs(state.m2 - m2) / m2 < 1e-4, fmt::format("M2 {} against {}", state.m2, m2));
	const double error = std::sqrt(m2 / (values.size() - 1) / values.size()) / mean;
	check(std::abs(relativeStandardError(state, 1e-3f) - error) / error < 1e-3, "relative standard error");

	WelfordState one;
	welfordAdd(one, 5.f);
	check(relativeStandardError(one, 1e-3f) == 0.f, "no error estimate from one sample");
	WelfordState dark;
	welfordAdd(dark, 0.f);
	welfordAdd(dark, 0.f);
	welfordAdd(dark, 1e-6f);
	check(std::isfinite(relativeStandardError(dark, 1e-3f)), "the floor keeps a dark pixel's error finite");
}

void testClamp()
{
	fmt::println("the clamp");
	const glm::vec3 bright(40.f, 20.f, 10.f);
	const glm::vec3 clamped = clampSampleComponent(bright, 10.f);
	check(std::abs(std::max(clamped.x, std::max(clamped.y, clamped.z)) - 10.f) < 1e-5f, "the largest component is limited to the threshold");
	check(std::abs(clamped.y / clamped.x - 0.5f) < 1e-6f && std::abs(clamped.z / clamped.x - 0.25f) < 1e-6f, "the color is kept");
	check(clampSampleComponent(glm::vec3(1.f, 2.f, 3.f), 10.f) == glm::vec3(1.f, 2.f, 3.f), "a sample under the threshold is untouched");
	check(clampSampleComponent(bright, 0.f) == bright, "a threshold of 0 clamps nothing");
}

void testFilmPixel()
{
	fmt::println("one pixel");
	//a pixel fed -1 and +1 weights: the value is sum(w L) / sum(w)
	FilmPixel pixel;
	filmAddSample(pixel, glm::vec3(2.f, 4.f, 8.f), 1.f, 0.f);
	filmAddSample(pixel, glm::vec3(1.f, 1.f, 1.f), -1.f, 0.f);
	filmAddSample(pixel, glm::vec3(2.f, 4.f, 8.f), 1.f, 0.f);
	const glm::vec3 value = filmResolve(pixel);
	check(std::abs(value.x - 3.f) < 1e-6f && std::abs(value.y - 7.f) < 1e-6f && std::abs(value.z - 15.f) < 1e-6f, fmt::format("signed weights: ({}, {}, {})", value.x, value.y, value.z));
	check(pixel.welford.count == 3, "the variance counts every accepted sample");

	//weights that sum to zero give black rather than a division by zero
	FilmPixel cancelled;
	filmAddSample(cancelled, glm::vec3(1.f), 1.f, 0.f);
	filmAddSample(cancelled, glm::vec3(1.f), -1.f, 0.f);
	check(filmResolve(cancelled) == glm::vec3(0.f), "weights that cancel resolve to black");

	//a constant radiance resolves to itself, exactly, whatever the signs
	FilmPixel constant;
	std::mt19937 engine(5);
	std::uniform_int_distribution<int> sign(0, 1);
	for (int i = 0; i < 9999; i++) {
		filmAddSample(constant, glm::vec3(0.5f), sign(engine) ? 1.f : -1.f, 0.f);
	}
	filmAddSample(constant, glm::vec3(0.5f), 1.f, 0.f);
	const glm::vec3 half = filmResolve(constant);
	check(half == glm::vec3(0.5f), fmt::format("a constant resolves to itself ({})", half.x));

	//the clamp is applied before the sums and the variance
	FilmPixel clamped;
	filmAddSample(clamped, glm::vec3(100.f, 0.f, 0.f), 1.f, 4.f);
	check(filmResolve(clamped).x == 4.f && std::abs(clamped.welford.mean - 0.2126f * 4.f) < 1e-6f, "the clamp feeds the sums and the variance");
}

//---------------------------------------------------------------- outlier reweighting

struct CascadeRun {
	CascadePixel cascade;
	glm::vec3 plain { 0.f };
	uint32_t count { 0 };
};

CascadeRun fillCascade(const std::vector<float>& values)
{
	CascadeRun run;
	for (const float v : values) {
		cascadeAddSample(run.cascade, glm::vec3(v, v, v), 1.f);
		run.plain += glm::vec3(v);
		run.count++;
	}
	run.plain /= (float)run.count;
	return run;
}

// The reweighted value of a pixel whose 3 x 3 neighborhood holds `neighborScale` times the pixel's own
// count on average: 1 for neighbors that look like it, 1/9 for a pixel whose eight neighbors have none of
// that brightness, which is what a firefly in a quiet region is
glm::vec3 resolveWith(const CascadeRun& run, float kappa, float neighborScale)
{
	std::array<float, OUTLIER_BUFFERS> counts;
	for (uint32_t j = 0; j < OUTLIER_BUFFERS; j++) {
		counts[j] = cascadeCount(run.cascade, j) * neighborScale;
	}
	return outlierResolve(run.cascade, counts, run.count, (float)run.count, kappa);
}

void testOutliers()
{
	fmt::println("outlier reweighting");
	std::mt19937 engine(2018);
	std::uniform_real_distribution<float> unit(0.f, 1.f);

	//one sample is one count, split over its two buffers, wherever it falls in the cascade
	for (const float value : { 1.f, 1.7f, 3.7f, 7.99f, 8.f, 64.f, 1000.f, 20000.f }) {
		CascadePixel pixel;
		cascadeAddSample(pixel, glm::vec3(value), 1.f);
		double counted = 0.0;
		glm::vec3 rgb(0.f);
		for (uint32_t k = 0; k < OUTLIER_BUFFERS; k++) {
			counted += pixel.buffers[k].luminance / std::exp2(3.0 * k);
			rgb += pixel.buffers[k].rgb;
		}
		check(std::abs(counted - 1.0) < 1e-4, fmt::format("a sample of {} counts {:.5f} times over its buffers", value, counted));
		check(std::abs(rgb.x - value) < 1e-3f * value, fmt::format("a sample of {} keeps its color over its buffers ({})", value, rgb.x));
	}
	//a sample beyond the top buffer, and one dimmer than 1, go whole to one
	CascadePixel bright;
	cascadeAddSample(bright, glm::vec3(1.0e6f), 1.f);
	check(bright.buffers[OUTLIER_BUFFERS - 1].rgb.x == 1.0e6f, "a sample beyond the cascade lands in its last buffer");
	CascadePixel dim;
	cascadeAddSample(dim, glm::vec3(0.1f), 1.f);
	check(dim.buffers[0].rgb.x == 0.1f, "a sample dimmer than 1 lands in the first buffer");

	//the buffers' rgb sums add to the samples' (signed) sum
	{
		CascadePixel pixel;
		glm::vec3 total(0.f);
		for (int i = 0; i < 5000; i++) {
			const glm::vec3 v(std::exp2(-3.f + 14.f * unit(engine)));
			const float w = unit(engine) < 0.2f ? -1.f : 1.f;
			cascadeAddSample(pixel, v, w);
			total += w * v;
		}
		glm::vec3 sum(0.f);
		for (const CascadeBuffer& buffer : pixel.buffers) {
			sum += buffer.rgb;
		}
		check(std::abs(sum.x - total.x) < 1e-4 * std::abs(total.x) + 1.f, fmt::format("the buffers hold the signed sum of the samples ({} against {})", sum.x, total.x));
	}

	//a smooth distribution loses little: every brightness is well sampled
	{
		std::vector<float> values;
		for (int i = 0; i < 4000; i++) {
			values.push_back(std::exp(0.8f * (unit(engine) + unit(engine) + unit(engine) - 1.5f) * 2.f));
		}
		const CascadeRun run = fillCascade(values);
		const glm::vec3 value = resolveWith(run, 8.f, 1.f);
		check(std::abs(value.x - run.plain.x) < 0.02f * run.plain.x && value.x <= run.plain.x * 1.00001f, fmt::format("a smooth distribution keeps its mean ({:.4f} against {:.4f})", value.x, run.plain.x));
	}

	//a bright value seen often enough keeps its energy: 5 percent of 2000 samples at 20, the rest 0.5
	{
		std::vector<float> values;
		for (int i = 0; i < 2000; i++) {
			values.push_back(unit(engine) < 0.05f ? 20.f : 0.5f);
		}
		const CascadeRun run = fillCascade(values);
		const glm::vec3 value = resolveWith(run, 8.f, 1.f);
		check(std::abs(value.x - run.plain.x) < 0.01f * run.plain.x, fmt::format("a well-sampled bright value keeps its energy ({:.4f} against {:.4f})", value.x, run.plain.x));
	}

	//one firefly among 1999 samples around 1: the plain mean doubles, the reweighted mean mostly does not
	{
		std::vector<float> values;
		for (int i = 0; i < 1999; i++) {
			values.push_back(0.5f + unit(engine));
		}
		values.push_back(4000.f);
		const CascadeRun run = fillCascade(values);
		const glm::vec3 value = resolveWith(run, 8.f, 1.f / 9.f);
		check(run.plain.x > 2.9f && value.x < 1.25f && value.x > 0.9f, fmt::format("a lone firefly is mostly discarded (plain mean {:.3f}, reweighted {:.3f})", run.plain.x, value.x));
		//a larger kappa discards more, and the weights never exceed 1
		check(resolveWith(run, 64.f, 1.f / 9.f).x <= value.x + 1e-6f, "a larger kappa never keeps more");
		check(value.x <= run.plain.x, "the reweighted mean is at most the plain mean");
	}

	//a neighborhood that corroborates the brightness keeps it even where this pixel saw it once
	{
		std::vector<float> values(500, 1.f);
		values.push_back(4000.f);
		const CascadeRun run = fillCascade(values);
		std::array<float, OUTLIER_BUFFERS> counts;
		for (uint32_t j = 0; j < OUTLIER_BUFFERS; j++) {
			counts[j] = 3.f;
		}
		const glm::vec3 corroborated = outlierResolve(run.cascade, counts, run.count, (float)run.count, 8.f);
		const glm::vec3 alone = resolveWith(run, 8.f, 1.f / 9.f);
		check(corroborated.x > alone.x, fmt::format("a corroborated brightness keeps more ({:.4f} against {:.4f})", corroborated.x, alone.x));
	}
}

//---------------------------------------------------------------- the planted errors

// a sampler that is 2% off in x, and a compensated add that compensates nothing: the tests above
// must catch both
bool selfcheck()
{
	fmt::println("selfcheck: planting errors");
	g_quiet = true;
	const int before = g_failures;
	testSampling([](const FilterTable& table, const glm::vec2& u) { return sampleFilterTable(table, glm::vec2(u.x * 0.98f, u.y)); }, 5, false);
	const bool caughtSampling = g_failures > before;
	const int middle = g_failures;
	testCompensatedSum([](CompensatedFloat& total, float x) { total.sum += x; }, false);
	const bool caughtSum = g_failures > middle;
	g_quiet = false;
	g_failures = before;
	fmt::println("  a sampler shifted by 2%: {}", caughtSampling ? "caught" : "NOT CAUGHT");
	fmt::println("  a sum without its compensation: {}", caughtSum ? "caught" : "NOT CAUGHT");
	return caughtSampling && caughtSum;
}

}

int main(int argc, char** argv)
{
	bool selfcheckOnly = false;
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--selfcheck") == 0) {
			selfcheckOnly = true;
		} else {
			fmt::println("usage: film_test [--selfcheck]");
			return 2;
		}
	}

	testFilterShapes();
	testSampling(sampleFilterTable, (int)PixelFilterKind::Count, true);
	testWeightedMean();
	testCompensatedSum(compensatedAdd, true);
	testWelford();
	testClamp();
	testFilmPixel();
	testOutliers();

	if (selfcheckOnly && !selfcheck()) {
		fmt::println("film_test: --selfcheck FAILED, a planted error was not caught");
		return 1;
	}
	if (g_failures == 0) {
		fmt::println("film_test: all checks passed");
		return 0;
	}
	fmt::println("film_test: {} check(s) FAILED", g_failures);
	return 1;
}
