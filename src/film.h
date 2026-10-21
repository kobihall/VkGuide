#pragma once

// THE FILM: how a pixel's samples become its value, as plain C++ with no Vulkan. The line-for-line
// mirror of shaders/rt/include/crt_filter.glsl (kernel 00's pixel filter) and crt_film.glsl (kernel
// 09's sums, variance and clamp), checked on its own by tests/film_test.cpp and against the GPU by
// the self-test in the Windows menu. Change both sides together.
// Design: docs/plans/completed/pbrt-film-and-reconstruction.md.
//
// A pixel's value is the weighted mean of its samples, sum(w L) / sum(w) (PBR 4ed equation 5.13). A
// sample's position in the pixel is drawn from the filter itself (PBR 4ed 8.8.3), so it needs no
// weight for a positive filter, and a filter with negative lobes (Mitchell, Lanczos) gives a sample
// the sign of the filter where it landed. Each sample touches only its own pixel.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include <light_sampling.h>

//---------------------------------------------------------------- pixel filters

// pbrt-v4's filters. The ids are saved in the scene file; never rename
enum class PixelFilterKind : uint32_t {
	Box,
	Triangle,
	Gaussian,
	Mitchell,
	Lanczos,
	Count
};

const char* pixelFilterId(PixelFilterKind kind);
const char* pixelFilterName(PixelFilterKind kind);
// Count for an id this build does not have
PixelFilterKind pixelFilterFromId(const char* id);
// Mitchell and Lanczos go below zero, which a sample's weight then carries
bool pixelFilterHasNegativeLobes(PixelFilterKind kind);

struct PixelFilterSettings {
	PixelFilterKind kind { PixelFilterKind::Gaussian };
	// the filter's extent from its center, in pixels, on both axes
	float radius { 1.5f };
	// Gaussian
	float sigma { 0.5f };
	// Mitchell-Netravali
	float b { 1.f / 3.f };
	float c { 1.f / 3.f };
	// Lanczos windowed sinc
	float tau { 3.f };

	bool operator==(const PixelFilterSettings&) const = default;
};

// the radius the panel and the scene file allow: the table has 32 cells per unit of it, along each axis
inline constexpr float MIN_FILTER_RADIUS = 0.25f;
inline constexpr float MAX_FILTER_RADIUS = 6.f;

// pbrt-v4's defaults for a kind: box 0.5, triangle 2, Gaussian 1.5 with sigma 0.5, Mitchell 2 with
// B = C = 1/3, Lanczos 4 with tau 3
PixelFilterSettings defaultPixelFilter(PixelFilterKind kind);
// what an older scene file means: a sample uniform in its pixel
PixelFilterSettings boxPixelFilter();

// the filter at an offset from its center, in pixels. 0 outside the radius
float evaluatePixelFilter(const PixelFilterSettings& filter, const glm::vec2& offset);

// pbrt's FilterSampler: the filter tabulated 32 times per unit of radius, as a piecewise-constant 2D
// distribution of its absolute value, with the signed values beside it for the weight
struct FilterTable {
	uint32_t width { 0 };
	uint32_t height { 0 };
	float radius { 0.f };
	// the table's cdfs, and func = |f| per cell
	PiecewiseConstant2D distribution;
	// f per cell, by row
	std::vector<float> values;
};

FilterTable buildFilterTable(const PixelFilterSettings& filter);

// the table as binding 30 (crt_common.glsl FilterTableBuffer): width, height, radius and integral,
// then the marginal cdf (height + 1), the row cdfs (height x (width + 1)), |f| (height x width) and
// the signed f (height x width), all float but the first two words
std::vector<uint32_t> packFilterTable(const FilterTable& table);

struct FilterSample {
	// from the pixel's center, in pixels
	glm::vec2 offset { 0.f };
	// +1, or -1 where the filter is negative. pbrt's f / pdf is this times the integral of |f|, which
	// the weighted mean divides out
	float weight { 1.f };
};

// the sample two uniform numbers choose: pbrt's PiecewiseConstant2D::Sample() on the table, mapped
// onto the filter's domain
FilterSample sampleFilterTable(const FilterTable& table, const glm::vec2& u);

//---------------------------------------------------------------- the sums

// A float sum with Neumaier's compensation (PBR 4ed keeps double-precision pixels, which this GPU has
// no shaderFloat64 for): sum + compensation holds roughly twice float's precision. The compensation
// term must not be reassociated away, so neither side may be compiled with fast math
struct CompensatedFloat {
	float sum { 0.f };
	float compensation { 0.f };
};

void compensatedAdd(CompensatedFloat& total, float x);
float compensatedValue(const CompensatedFloat& total);

// Welford's running mean and sum of squared deviations (PBR 4ed B.2.11's VarianceEstimator)
struct WelfordState {
	float mean { 0.f };
	float m2 { 0.f };
	uint32_t count { 0 };
};

void welfordAdd(WelfordState& state, float x);
// the standard error of the mean over the mean itself: sqrt(M2 / (n - 1) / n) / max(|mean|, floor).
// 0 below two samples
float relativeStandardError(const WelfordState& state, float floor);

float filmLuminance(const glm::vec3& rgb);
// pbrt's maxComponentValue: a sample whose largest component exceeds `maxComponent` is scaled down
// to it, keeping its color. A `maxComponent` of 0 or less clamps nothing
glm::vec3 clampSampleComponent(const glm::vec3& rgb, float maxComponent);

// One pixel's film record, as kernel 09's variants keep it in binding 31 (crt_common.glsl FilmPixel)
struct FilmPixel {
	// sum(w L) per channel, and sum(w)
	std::array<CompensatedFloat, 4> sums;
	// the luminance's mean and M2, of the samples after the clamp
	WelfordState welford;
};

// one accepted sample: the clamp, the weighted sums, the variance
void filmAddSample(FilmPixel& pixel, const glm::vec3& rgb, float weight, float clampComponent);
// the pixel's value, sum(w L) / sum(w); black where the weights sum to zero
glm::vec3 filmResolve(const FilmPixel& pixel);

//---------------------------------------------------------------- outlier reweighting

// Zirr, Hanika and Dachsbacher 2018, "Re-weighting firefly samples for improved finite-sample Monte Carlo
// estimates": a pixel keeps its samples in a small cascade of buffers by brightness, and the estimate
// weighs each buffer by how well its brightness was sampled. A sample of luminance S lands in the two
// buffers B_j and B_j+1 around it, j = floor(log_b S), split by a hyperbolic weight so that dividing each
// buffer's luminance sum by b^j counts the samples of similar brightness (equation 19), with no sample
// stored. A buffer whose samples were found fewer than kappa times is weighed down by n / kappa, and one
// nobody else in the pixel's neighborhood corroborates is dropped. Samples dimmer than N E / kappa, with E
// the expected value so far, cannot be fireflies and keep their full weight. The result is darker than the
// plain mean only by the energy of samples too rare to trust, which a clamp would cut regardless.
inline constexpr uint32_t OUTLIER_BUFFERS = 6;
// the buffers' brightness ratio: 8 as in the paper, which puts the top buffer at 8^5 = 32768
inline constexpr float OUTLIER_BASE = 8.f;
// a buffer whose neighborhood averages fewer samples than this is a lone outlier and is dropped
inline constexpr float OUTLIER_MIN_COUNT = 1.f;

struct CascadeBuffer {
	// sum of alpha w rgb: the samples split into this buffer, with the filter weight's sign
	glm::vec3 rgb { 0.f };
	// sum of alpha S: the luminance the same samples carry, unsigned
	float luminance { 0.f };
};

struct CascadePixel {
	std::array<CascadeBuffer, OUTLIER_BUFFERS> buffers;
};

// one accepted sample (after the clamp): split into the two buffers that bracket its luminance
void cascadeAddSample(CascadePixel& pixel, const glm::vec3& rgb, float weight);
// the number of samples of brightness around b^level that the pixel holds (equation 19), fractional
float cascadeCount(const CascadePixel& pixel, uint32_t level);
// The pixel's reweighted value. `neighborhoodCount[j]` is cascadeCount(j) averaged over the pixel and its
// existing neighbors in a 3 x 3 block, the criterion for dropping a buffer; `accepted` is the samples the pixel
// took, and `weightSum` the sum of their filter weights, which divides the sum as in filmResolve()
glm::vec3 outlierResolve(const CascadePixel& pixel, const std::array<float, OUTLIER_BUFFERS>& neighborhoodCount, uint32_t accepted, float weightSum, float kappa);
