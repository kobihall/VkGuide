#pragma once

// Linear floating-point images in files: OpenEXR (through tinyexr, with the system zlib) and PFM,
// no Vulkan. The renderer's reference images and the render dump of a verification run use these
// (docs/plans/completed/pbrt-verification-harness.md), and bin/image_diff compares two files.
//
// An Image is RGB, three floats per pixel, row 0 at the top. A file with an alpha channel loses it
// on load; a file with fewer than three channels fails to load.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct Image {
	uint32_t width { 0 };
	uint32_t height { 0 };
	std::vector<float> rgb;

	size_t pixelCount() const { return (size_t)width * height; }
};

struct ImageIoStatus {
	bool ok { false };
	std::string message;
};

// The format is the file's extension: .exr (32-bit float, ZIP compressed) or .pfm (little endian)
ImageIoStatus saveImage(const std::filesystem::path& path, const Image& image);
ImageIoStatus loadImage(const std::filesystem::path& path, Image& image);

// How far an image is from a reference of the same size, over every pixel and channel. The measures
// are the ones the render panel reports against a reference (shaders/image_error.comp), which the
// GPU sums in a different order and so matches only to rounding
struct ImageError {
	// the square root of the mean of (image - reference)^2
	double rmse { 0.0 };
	// the mean of (image - reference)^2 / (reference^2 + 0.01), averaged over the channels of a
	// pixel first: a dark pixel's noise weighs as much as a bright one's
	double relativeMse { 0.0 };
	// the mean of |image - reference| / max(|reference|, 0.01), as a fraction (0.05 is 5 percent).
	// Pixels darker than 1 percent of white count as if they were that bright
	double mape { 0.0 };
	// the mean over pixels and channels of image - reference: a signed bias
	double meanDifference { 0.0 };
	// non-finite values in either image, which the sums leave out
	uint64_t nonFinite { 0 };
};

// false when the sizes differ
bool compareImages(const Image& image, const Image& reference, ImageError& error);
