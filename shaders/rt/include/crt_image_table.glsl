// PIECEWISE-CONSTANT 2D DISTRIBUTIONS OVER IMAGES, as binding 65 (ImageDistributions) holds them: pbrt-v4's
// PiecewiseConstant2D, packed by src/light_sampling.cpp packPiecewiseConstant2D() and mirrored by its
// samplePiecewiseConstant2D() and piecewiseConstant2DPdf(). A table at `base` is four words (its width and height as uint
// bits, its integral, 0), its marginal cdf, its row cdfs and its function. The camera's aperture image is one.
// Requires crt_common.glsl, with the including kernel declaring binding 65.

#ifndef CRT_IMAGE_TABLE_GLSL
#define CRT_IMAGE_TABLE_GLSL

uint imageTableWidth(uint base)
{
	return floatBitsToUint(imageDistributions[base]);
}

uint imageTableHeight(uint base)
{
	return floatBitsToUint(imageDistributions[base + 1u]);
}

// pbrt's FindInterval() on the cdf of `count` + 1 entries at imageDistributions[at]
uint imageTableFindInterval(uint at, uint count, float u)
{
	int length_ = int(count) - 1;
	int first = 1;
	while (length_ > 0) {
		const int half_ = length_ >> 1;
		const int middle = first + half_;
		if (imageDistributions[at + uint(middle)] <= u) {
			first = middle + 1;
			length_ -= half_ + 1;
		} else {
			length_ = half_;
		}
	}
	return uint(clamp(first - 1, 0, int(count) - 1));
}

float imageTableSampleCdf(uint at, uint count, float u, out uint offset)
{
	offset = imageTableFindInterval(at, count, u);
	float du = u - imageDistributions[at + offset];
	const float width = imageDistributions[at + offset + 1u] - imageDistributions[at + offset];
	if (width > 0.0) {
		du /= width;
	}
	return min((float(offset) + du) / float(count), 0.99999994);
}

// a point of the unit square with its density in `pdf`, 0 for a table that is all zero
vec2 sampleImageTable(uint base, vec2 u, out float pdf)
{
	const uint width = imageTableWidth(base);
	const uint height = imageTableHeight(base);
	const float integral = imageDistributions[base + 2u];
	const uint marginal = base + 4u;
	uint row;
	const float v = imageTableSampleCdf(marginal, height, u.y, row);
	uint column;
	const uint rowCdf = marginal + height + 1u + row * (width + 1u);
	const float uu = imageTableSampleCdf(rowCdf, width, u.x, column);
	const uint func = marginal + height + 1u + height * (width + 1u);
	pdf = integral > 0.0 ? imageDistributions[func + row * width + column] / integral : 0.0;
	return vec2(uu, v);
}

float imageTablePdf(uint base, vec2 uv)
{
	const uint width = imageTableWidth(base);
	const uint height = imageTableHeight(base);
	const float integral = imageDistributions[base + 2u];
	const uint column = uint(clamp(int(uv.x * float(width)), 0, int(width) - 1));
	const uint row = uint(clamp(int(uv.y * float(height)), 0, int(height) - 1));
	const uint func = base + 4u + height + 1u + height * (width + 1u);
	return integral > 0.0 ? imageDistributions[func + row * width + column] / integral : 0.0;
}

#endif
