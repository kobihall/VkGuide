#include <film.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr float FILM_PI = 3.14159265358979323846f;
// the largest float below 1: a sampled position never reaches the end of the domain
constexpr float ONE_MINUS_EPSILON = 0.99999994f;
// the table has this many cells per unit of radius, along each axis over the filter's whole width
// (pbrt-v4's FilterSampler)
constexpr float TABLE_CELLS_PER_RADIUS = 32.f;

struct FilterInfo {
	const char* id;
	const char* name;
	bool negativeLobes;
	PixelFilterSettings defaults;
};

const std::array<FilterInfo, (size_t)PixelFilterKind::Count>& filterInfo()
{
	static const std::array<FilterInfo, (size_t)PixelFilterKind::Count> info { {
		{ "box", "Box", false, { PixelFilterKind::Box, 0.5f, 0.5f, 1.f / 3.f, 1.f / 3.f, 3.f } },
		{ "triangle", "Triangle", false, { PixelFilterKind::Triangle, 2.f, 0.5f, 1.f / 3.f, 1.f / 3.f, 3.f } },
		{ "gaussian", "Gaussian", false, { PixelFilterKind::Gaussian, 1.5f, 0.5f, 1.f / 3.f, 1.f / 3.f, 3.f } },
		{ "mitchell", "Mitchell-Netravali", true, { PixelFilterKind::Mitchell, 2.f, 0.5f, 1.f / 3.f, 1.f / 3.f, 3.f } },
		{ "lanczos", "Lanczos (windowed sinc)", true, { PixelFilterKind::Lanczos, 4.f, 0.5f, 1.f / 3.f, 1.f / 3.f, 3.f } },
	} };
	return info;
}

float gaussian(float x, float sigma)
{
	return 1.f / std::sqrt(2.f * FILM_PI * sigma * sigma) * std::exp(-(x * x) / (2.f * sigma * sigma));
}

// pbrt's Sinc() through SinXOverX(): 1 where sin(x) / x rounds to it
float sinc(float x)
{
	const float pix = FILM_PI * x;
	if (1.f - pix * pix == 1.f) {
		return 1.f;
	}
	return std::sin(pix) / pix;
}

float windowedSinc(float x, float radius, float tau)
{
	if (std::abs(x) > radius) {
		return 0.f;
	}
	return sinc(x) * sinc(x / tau);
}

// pbrt-v4's Mitchell1D on the filter's [-2, 2] domain
float mitchell1D(float x, float b, float c)
{
	x = std::abs(x);
	if (x <= 1.f) {
		return ((12.f - 9.f * b - 6.f * c) * x * x * x + (-18.f + 12.f * b + 6.f * c) * x * x + (6.f - 2.f * b)) * (1.f / 6.f);
	}
	if (x <= 2.f) {
		return ((-b - 6.f * c) * x * x * x + (6.f * b + 30.f * c) * x * x + (-12.f * b - 48.f * c) * x + (8.f * b + 24.f * c)) * (1.f / 6.f);
	}
	return 0.f;
}

// pbrt's FindInterval() on a cdf of `size` entries: the last entry at or below u, clamped to a cell
uint32_t findInterval(const float* cdf, uint32_t size, float u)
{
	int length = (int)size - 2;
	int first = 1;
	while (length > 0) {
		const int half = length >> 1;
		const int middle = first + half;
		if (cdf[middle] <= u) {
			first = middle + 1;
			length -= half + 1;
		} else {
			length = half;
		}
	}
	return (uint32_t)std::clamp(first - 1, 0, (int)size - 2);
}

// pbrt-v4's PiecewiseConstant1D::Sample() over [0,1], on one cdf of `count` + 1 entries
float sampleCdf(const float* cdf, uint32_t count, float u, uint32_t& offset)
{
	offset = findInterval(cdf, count + 1, u);
	float du = u - cdf[offset];
	const float width = cdf[offset + 1] - cdf[offset];
	if (width > 0.f) {
		du /= width;
	}
	return std::min(((float)offset + du) / (float)count, ONE_MINUS_EPSILON);
}

}

//---------------------------------------------------------------- pixel filters

const char* pixelFilterId(PixelFilterKind kind)
{
	return kind < PixelFilterKind::Count ? filterInfo()[(size_t)kind].id : "";
}

const char* pixelFilterName(PixelFilterKind kind)
{
	return kind < PixelFilterKind::Count ? filterInfo()[(size_t)kind].name : "unknown";
}

PixelFilterKind pixelFilterFromId(const char* id)
{
	for (size_t i = 0; i < filterInfo().size(); i++) {
		if (std::strcmp(id, filterInfo()[i].id) == 0) {
			return (PixelFilterKind)i;
		}
	}
	return PixelFilterKind::Count;
}

bool pixelFilterHasNegativeLobes(PixelFilterKind kind)
{
	return kind < PixelFilterKind::Count && filterInfo()[(size_t)kind].negativeLobes;
}

PixelFilterSettings defaultPixelFilter(PixelFilterKind kind)
{
	return kind < PixelFilterKind::Count ? filterInfo()[(size_t)kind].defaults : PixelFilterSettings {};
}

PixelFilterSettings boxPixelFilter()
{
	return defaultPixelFilter(PixelFilterKind::Box);
}

float evaluatePixelFilter(const PixelFilterSettings& filter, const glm::vec2& offset)
{
	const float radius = filter.radius;
	switch (filter.kind) {
	case PixelFilterKind::Box:
		return (std::abs(offset.x) <= radius && std::abs(offset.y) <= radius) ? 1.f : 0.f;
	case PixelFilterKind::Triangle:
		return std::max(0.f, radius - std::abs(offset.x)) * std::max(0.f, radius - std::abs(offset.y));
	case PixelFilterKind::Gaussian: {
		const float edge = gaussian(radius, filter.sigma);
		return std::max(0.f, gaussian(offset.x, filter.sigma) - edge) * std::max(0.f, gaussian(offset.y, filter.sigma) - edge);
	}
	case PixelFilterKind::Mitchell:
		return mitchell1D(2.f * offset.x / radius, filter.b, filter.c) * mitchell1D(2.f * offset.y / radius, filter.b, filter.c);
	case PixelFilterKind::Lanczos:
		return windowedSinc(offset.x, radius, filter.tau) * windowedSinc(offset.y, radius, filter.tau);
	case PixelFilterKind::Count:
		break;
	}
	return 0.f;
}

FilterTable buildFilterTable(const PixelFilterSettings& filter)
{
	FilterTable table;
	table.radius = filter.radius;
	table.width = std::max<uint32_t>(1, (uint32_t)(TABLE_CELLS_PER_RADIUS * filter.radius));
	table.height = table.width;
	table.values.resize((size_t)table.width * table.height);
	for (uint32_t y = 0; y < table.height; y++) {
		for (uint32_t x = 0; x < table.width; x++) {
			//the cell's center on the domain [-radius, radius]
			const float tx = ((float)x + 0.5f) / (float)table.width;
			const float ty = ((float)y + 0.5f) / (float)table.height;
			const glm::vec2 p(filter.radius * (2.f * tx - 1.f), filter.radius * (2.f * ty - 1.f));
			table.values[(size_t)y * table.width + x] = evaluatePixelFilter(filter, p);
		}
	}
	table.distribution = buildPiecewiseConstant2D(table.values, table.width, table.height);
	return table;
}

std::vector<uint32_t> packFilterTable(const FilterTable& table)
{
	const PiecewiseConstant2D& distribution = table.distribution;
	std::vector<uint32_t> words;
	words.reserve(4 + distribution.marginalCdf.size() + distribution.rowCdf.size() + distribution.func.size() + table.values.size());
	auto pushFloat = [&](float value) {
		uint32_t word;
		std::memcpy(&word, &value, sizeof(word));
		words.push_back(word);
	};
	words.push_back(table.width);
	words.push_back(table.height);
	pushFloat(table.radius);
	pushFloat(distribution.integral);
	for (const float value : distribution.marginalCdf) {
		pushFloat(value);
	}
	for (const float value : distribution.rowCdf) {
		pushFloat(value);
	}
	for (const float value : distribution.func) {
		pushFloat(value);
	}
	for (const float value : table.values) {
		pushFloat(value);
	}
	return words;
}

FilterSample sampleFilterTable(const FilterTable& table, const glm::vec2& u)
{
	const PiecewiseConstant2D& distribution = table.distribution;
	uint32_t row;
	const float v = sampleCdf(distribution.marginalCdf.data(), table.height, u.y, row);
	uint32_t column;
	const float w = sampleCdf(&distribution.rowCdf[(size_t)row * (table.width + 1)], table.width, u.x, column);

	FilterSample sample;
	sample.offset = glm::vec2(table.radius * (2.f * w - 1.f), table.radius * (2.f * v - 1.f));
	sample.weight = table.values[(size_t)row * table.width + column] < 0.f ? -1.f : 1.f;
	return sample;
}

//---------------------------------------------------------------- the sums

void compensatedAdd(CompensatedFloat& total, float x)
{
	const float t = total.sum + x;
	if (std::abs(total.sum) >= std::abs(x)) {
		total.compensation += (total.sum - t) + x;
	} else {
		total.compensation += (x - t) + total.sum;
	}
	total.sum = t;
}

float compensatedValue(const CompensatedFloat& total)
{
	return total.sum + total.compensation;
}

void welfordAdd(WelfordState& state, float x)
{
	state.count++;
	const float delta = x - state.mean;
	state.mean += delta / (float)state.count;
	const float delta2 = x - state.mean;
	state.m2 += delta * delta2;
}

float relativeStandardError(const WelfordState& state, float floor)
{
	if (state.count < 2) {
		return 0.f;
	}
	const float n = (float)state.count;
	const float standardError = std::sqrt(std::max(state.m2, 0.f) / (n - 1.f) / n);
	return standardError / std::max(std::abs(state.mean), floor);
}

float filmLuminance(const glm::vec3& rgb)
{
	return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

glm::vec3 clampSampleComponent(const glm::vec3& rgb, float maxComponent)
{
	const float peak = std::max(rgb.x, std::max(rgb.y, rgb.z));
	if (maxComponent > 0.f && peak > maxComponent) {
		return rgb * (maxComponent / peak);
	}
	return rgb;
}

void filmAddSample(FilmPixel& pixel, const glm::vec3& rgb, float weight, float clampComponent)
{
	const glm::vec3 clamped = clampSampleComponent(rgb, clampComponent);
	//the weight is +1 or -1, so the products are exact
	compensatedAdd(pixel.sums[0], weight * clamped.x);
	compensatedAdd(pixel.sums[1], weight * clamped.y);
	compensatedAdd(pixel.sums[2], weight * clamped.z);
	compensatedAdd(pixel.sums[3], weight);
	welfordAdd(pixel.welford, filmLuminance(clamped));
}

glm::vec3 filmResolve(const FilmPixel& pixel)
{
	const float weightSum = compensatedValue(pixel.sums[3]);
	if (weightSum == 0.f) {
		return glm::vec3(0.f);
	}
	return glm::vec3(compensatedValue(pixel.sums[0]), compensatedValue(pixel.sums[1]), compensatedValue(pixel.sums[2])) / weightSum;
}

//---------------------------------------------------------------- outlier reweighting

namespace {

// b^level, exact for the base 8
float cascadeLevelValue(uint32_t level)
{
	return std::exp2(3.f * (float)level);
}

}

void cascadeAddSample(CascadePixel& pixel, const glm::vec3& rgb, float weight)
{
	const float luminance = std::max(filmLuminance(rgb), 0.f);
	uint32_t level = 0;
	float alpha = 1.f;
	//a sample dimmer than 1 goes whole to the first buffer, and one beyond the top whole to the last
	if (luminance >= 1.f) {
		const float exponent = std::floor(std::log2(luminance) / 3.f);
		if (exponent >= (float)(OUTLIER_BUFFERS - 1)) {
			level = OUTLIER_BUFFERS - 1;
		} else {
			level = (uint32_t)exponent;
			alpha = std::clamp((cascadeLevelValue(level) / luminance - 1.f / OUTLIER_BASE) / (1.f - 1.f / OUTLIER_BASE), 0.f, 1.f);
		}
	}
	pixel.buffers[level].rgb += alpha * weight * rgb;
	pixel.buffers[level].luminance += alpha * luminance;
	if (level + 1 < OUTLIER_BUFFERS) {
		pixel.buffers[level + 1].rgb += (1.f - alpha) * weight * rgb;
		pixel.buffers[level + 1].luminance += (1.f - alpha) * luminance;
	}
}

float cascadeCount(const CascadePixel& pixel, uint32_t level)
{
	float count = 0.f;
	for (uint32_t k = (level > 0 ? level - 1 : 0); k <= std::min(level + 1, OUTLIER_BUFFERS - 1); k++) {
		count += pixel.buffers[k].luminance / cascadeLevelValue(k);
	}
	return count;
}

glm::vec3 outlierResolve(const CascadePixel& pixel, const std::array<float, OUTLIER_BUFFERS>& neighborhoodCount, uint32_t accepted, float weightSum, float kappa)
{
	if (accepted == 0 || weightSum == 0.f) {
		return glm::vec3(0.f);
	}
	glm::vec3 result(0.f);
	//the expected value of the buffers below, as the weighted composition so far: a conservative lower bound
	float expected = 0.f;
	for (uint32_t level = 0; level < OUTLIER_BUFFERS; level++) {
		//the count criterion: well sampled brightness keeps its weight, rare brightness loses it in proportion,
		//and a brightness nobody nearby has seen is rejected
		float countWeight = 0.f;
		if (neighborhoodCount[level] >= OUTLIER_MIN_COUNT) {
			countWeight = std::min(1.f, cascadeCount(pixel, level) / kappa);
		}
		//the value criterion: a sample under N E / kappa cannot be a firefly, whatever its count
		const float valueWeight = std::min(1.f, (float)accepted * expected / (kappa * cascadeLevelValue(level)));
		const float weight = std::max(countWeight, valueWeight);
		result += weight * pixel.buffers[level].rgb / weightSum;
		expected = filmLuminance(result);
	}
	return result;
}
