#pragma once

// Images in files, no Vulkan: linear floating-point images as OpenEXR (through tinyexr, with the
// system zlib), PFM and Radiance HDR, and an 8-bit display image as PNG (stb_image_write).
// The renderer's reference images, the render export and the batch mode use these
// (docs/plans/completed/pbrt-verification-harness.md, docs/plans/completed/pbrt-aovs-and-image-output.md),
// and bin/image_diff compares two files.
//
// An Image is RGB, three floats per pixel, row 0 at the top. A file with an alpha channel loses it
// on load; a file with fewer than three channels fails to load. A ChannelImage is any number of
// named float channels and the attributes of an EXR file: what the export writes beside the picture.

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

// The format is the file's extension: .exr (32-bit float, ZIP compressed), .pfm (little endian) or
// .hdr (Radiance RGBE, which keeps about 1 percent of precision per channel)
ImageIoStatus saveImage(const std::filesystem::path& path, const Image& image);
ImageIoStatus loadImage(const std::filesystem::path& path, Image& image);

// An 8-bit RGBA image, row 0 at the top: the display image as the window shows it
struct Image8 {
	uint32_t width { 0 };
	uint32_t height { 0 };
	std::vector<uint8_t> rgba;
};
// .png, 8-bit RGB: the alpha channel is left out
ImageIoStatus saveImage8(const std::filesystem::path& path, const Image8& image);

// One header attribute of an EXR file, in the file's own encoding: `type` is the attribute's type name
// ("string", "float", "int", "m44f", "chromaticities") and `value` its bytes
struct ImageAttribute {
	std::string name;
	std::string type;
	std::vector<uint8_t> value;

	static ImageAttribute text(std::string name, const std::string& text);
	static ImageAttribute real(std::string name, float value);
	static ImageAttribute integer(std::string name, int32_t value);
	// a 4 x 4 matrix, 16 floats in row-major order, as OpenEXR's m44f stores one
	static ImageAttribute matrix(std::string name, const float (&rowMajor)[16]);
	// the red, green and blue primaries and the white point, as (x, y) pairs
	static ImageAttribute chromaticities(std::string name, const float (&xy)[8]);
	// the text of a "string" attribute, empty for another type
	std::string asText() const;
};

struct ImageChannel {
	std::string name;
	// width x height floats, row 0 at the top
	std::vector<float> values;
};

struct ChannelImage {
	uint32_t width { 0 };
	uint32_t height { 0 };
	std::vector<ImageChannel> channels;
	std::vector<ImageAttribute> attributes;

	const ImageChannel* find(const std::string& name) const;
};

struct ExrOptions {
	// 16-bit half floats instead of 32-bit floats: half the size, and a pixel above 65504 clamps
	bool half { false };
};

// Writes every channel of an EXR file in ZIP compression, sorted by name as the format requires (the
// order of `channels` is not kept), with the attributes. Every channel needs width x height values
ImageIoStatus saveChannelImage(const std::filesystem::path& path, const ChannelImage& image, const ExrOptions& options = {});
// Reads every channel of an EXR file as floats, with its attributes
ImageIoStatus loadChannelImage(const std::filesystem::path& path, ChannelImage& image);

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
