// The pixel filter kernel 00's filtered variant samples: where in the pixel's neighborhood a camera
// ray goes, and the sign its sample carries. The line-for-line mirror of sampleFilterTable() in
// src/film.cpp (src/film.h): change both together and run bin/film_test.
// docs/plans/completed/pbrt-film-and-reconstruction.md.
//
// The filter is tabulated on the CPU (buildFilterTable()) as pbrt-v4's FilterSampler does, 32 cells per
// unit of radius over its whole width, and read here from binding 30 (crt_common.glsl
// FilterTableBuffer). Two uniform numbers choose a cell by inverting the table's cdfs, so a sample lands
// where the filter is large in proportion to how large it is. A filter that is never negative then
// needs no weight at all: the pixel's value is the plain mean of its samples, and each sample still
// belongs to its own pixel. A filter with negative lobes gives a sample the sign of the filter where it
// landed (pbrt's f / pdf is that sign times the integral of |f|, which the weighted mean divides out).
// Requires crt_common.glsl.

#define CRT_FILTER_ONE_MINUS_EPSILON 0.99999994

// pbrt's FindInterval() on the cdf of `count` + 1 entries at filterData[base]
uint filterFindInterval(uint base, uint count, float u)
{
	int length_ = int(count) - 1;
	int first = 1;
	while (length_ > 0) {
		const int half_ = length_ >> 1;
		const int middle = first + half_;
		if (filterData[base + uint(middle)] <= u) {
			first = middle + 1;
			length_ -= half_ + 1;
		} else {
			length_ = half_;
		}
	}
	return uint(clamp(first - 1, 0, int(count) - 1));
}

// pbrt-v4's PiecewiseConstant1D::Sample() over [0,1], on the cdf of `count` + 1 entries at filterData[base]
float filterSampleCdf(uint base, uint count, float u, out uint offset)
{
	offset = filterFindInterval(base, count, u);
	float du = u - filterData[base + offset];
	const float width = filterData[base + offset + 1u] - filterData[base + offset];
	if (width > 0.0) {
		du /= width;
	}
	return min((float(offset) + du) / float(count), CRT_FILTER_ONE_MINUS_EPSILON);
}

struct FilterSample {
	// from the pixel's center, in pixels
	vec2 offset;
	// +1, or -1 where the filter is negative
	float weight;
};

FilterSample sampleFilter(vec2 u)
{
	uint row;
	const float v = filterSampleCdf(0u, filterHeight, u.y, row);
	uint column;
	const uint rowCdfBase = filterHeight + 1u + row * (filterWidth + 1u);
	const float w = filterSampleCdf(rowCdfBase, filterWidth, u.x, column);
	// |f| follows the row cdfs, then the signed f
	const uint signedBase = filterHeight + 1u + filterHeight * (filterWidth + 1u) + filterHeight * filterWidth;

	FilterSample s;
	s.offset = vec2(filterRadius * (2.0 * w - 1.0), filterRadius * (2.0 * v - 1.0));
	s.weight = filterData[signedBase + row * filterWidth + column] < 0.0 ? -1.0 : 1.0;
	return s;
}
