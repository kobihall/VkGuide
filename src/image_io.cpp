#include <image_io.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

// tinyexr takes a zlib-compatible header from its includer when it is not built with miniz
#include <zlib.h>
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_NANOZLIB 0
#define TINYEXR_USE_OPENMP 0
#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>

namespace {

std::string lowerExtension(const std::filesystem::path& path)
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return extension;
}

ImageIoStatus failure(std::string message)
{
	return { false, std::move(message) };
}

ImageIoStatus savePfm(const std::filesystem::path& path, const Image& image)
{
	FILE* file = std::fopen(path.string().c_str(), "wb");
	if (file == nullptr) {
		return failure(fmt::format("Cannot open '{}' for writing", path.string()));
	}
	// "PF" is three channels; a negative scale means little endian; rows run bottom to top
	std::fprintf(file, "PF\n%u %u\n-1.0\n", image.width, image.height);
	bool ok = true;
	for (uint32_t row = 0; row < image.height && ok; row++) {
		const float* source = image.rgb.data() + (size_t)(image.height - 1 - row) * image.width * 3;
		ok = std::fwrite(source, sizeof(float), (size_t)image.width * 3, file) == (size_t)image.width * 3;
	}
	ok = std::fclose(file) == 0 && ok;
	return ok ? ImageIoStatus { true, fmt::format("Saved '{}' ({} x {})", path.filename().string(), image.width, image.height) } : failure(fmt::format("Writing '{}' failed", path.string()));
}

ImageIoStatus loadPfm(const std::filesystem::path& path, Image& image)
{
	FILE* file = std::fopen(path.string().c_str(), "rb");
	if (file == nullptr) {
		return failure(fmt::format("Cannot open '{}'", path.string()));
	}
	char magic[3] = {};
	unsigned width = 0;
	unsigned height = 0;
	float scale = 0.f;
	if (std::fscanf(file, "%2s %u %u %f", magic, &width, &height, &scale) != 4 || (std::strcmp(magic, "PF") != 0 && std::strcmp(magic, "Pf") != 0) || width == 0 || height == 0) {
		std::fclose(file);
		return failure(fmt::format("'{}' is not a PFM file", path.filename().string()));
	}
	// one whitespace character ends the header
	std::fgetc(file);
	const size_t channels = std::strcmp(magic, "PF") == 0 ? 3 : 1;
	std::vector<float> raw((size_t)width * height * channels);
	const size_t read = std::fread(raw.data(), sizeof(float), raw.size(), file);
	std::fclose(file);
	if (read != raw.size()) {
		return failure(fmt::format("'{}' ends before its pixels do", path.filename().string()));
	}
	if (scale > 0.f) {
		// big endian
		for (float& value : raw) {
			uint32_t bits;
			std::memcpy(&bits, &value, sizeof(bits));
			bits = __builtin_bswap32(bits);
			std::memcpy(&value, &bits, sizeof(bits));
		}
	}
	image.width = width;
	image.height = height;
	image.rgb.resize((size_t)width * height * 3);
	for (uint32_t row = 0; row < height; row++) {
		for (uint32_t x = 0; x < width; x++) {
			for (size_t c = 0; c < 3; c++) {
				image.rgb[((size_t)row * width + x) * 3 + c] = raw[((size_t)(height - 1 - row) * width + x) * channels + (channels == 3 ? c : 0)];
			}
		}
	}
	return { true, fmt::format("Loaded '{}' ({} x {})", path.filename().string(), width, height) };
}

}  // namespace

ImageIoStatus saveImage(const std::filesystem::path& path, const Image& image)
{
	if (image.width == 0 || image.height == 0 || image.rgb.size() != image.pixelCount() * 3) {
		return failure("There is no image to save");
	}
	const std::string extension = lowerExtension(path);
	if (extension == ".pfm") {
		return savePfm(path, image);
	}
	if (extension != ".exr") {
		return failure(fmt::format("Cannot save '{}': the extension must be .exr or .pfm", path.filename().string()));
	}
	const char* error = nullptr;
	const int status = SaveEXR(image.rgb.data(), (int)image.width, (int)image.height, 3, 0, path.string().c_str(), &error);
	if (status != TINYEXR_SUCCESS) {
		std::string message = fmt::format("Saving '{}' failed: {}", path.filename().string(), error != nullptr ? error : "unknown error");
		if (error != nullptr) {
			FreeEXRErrorMessage(error);
		}
		return failure(std::move(message));
	}
	return { true, fmt::format("Saved '{}' ({} x {})", path.filename().string(), image.width, image.height) };
}

ImageIoStatus loadImage(const std::filesystem::path& path, Image& image)
{
	const std::string extension = lowerExtension(path);
	if (extension == ".pfm") {
		return loadPfm(path, image);
	}
	if (extension != ".exr") {
		return failure(fmt::format("Cannot load '{}': the extension must be .exr or .pfm", path.filename().string()));
	}
	float* rgba = nullptr;
	int width = 0;
	int height = 0;
	const char* error = nullptr;
	const int status = LoadEXR(&rgba, &width, &height, path.string().c_str(), &error);
	if (status != TINYEXR_SUCCESS) {
		std::string message = fmt::format("Loading '{}' failed: {}", path.filename().string(), error != nullptr ? error : "unknown error");
		if (error != nullptr) {
			FreeEXRErrorMessage(error);
		}
		return failure(std::move(message));
	}
	image.width = (uint32_t)width;
	image.height = (uint32_t)height;
	image.rgb.resize(image.pixelCount() * 3);
	for (size_t i = 0; i < image.pixelCount(); i++) {
		image.rgb[i * 3 + 0] = rgba[i * 4 + 0];
		image.rgb[i * 3 + 1] = rgba[i * 4 + 1];
		image.rgb[i * 3 + 2] = rgba[i * 4 + 2];
	}
	std::free(rgba);
	return { true, fmt::format("Loaded '{}' ({} x {})", path.filename().string(), width, height) };
}

bool compareImages(const Image& image, const Image& reference, ImageError& error)
{
	if (image.width != reference.width || image.height != reference.height || image.rgb.size() != reference.rgb.size()) {
		return false;
	}
	error = {};
	double squared = 0.0;
	double relative = 0.0;
	double absolutePercent = 0.0;
	double difference = 0.0;
	size_t counted = 0;
	for (size_t i = 0; i < image.pixelCount(); i++) {
		double pixelSquared = 0.0;
		double pixelRelative = 0.0;
		double pixelPercent = 0.0;
		double pixelDifference = 0.0;
		bool finite = true;
		for (size_t c = 0; c < 3; c++) {
			const double x = image.rgb[i * 3 + c];
			const double r = reference.rgb[i * 3 + c];
			if (!std::isfinite(x) || !std::isfinite(r)) {
				finite = false;
				break;
			}
			const double d = x - r;
			pixelSquared += d * d;
			pixelRelative += d * d / (r * r + 0.01) / 3.0;
			pixelPercent += std::abs(d) / std::max(std::abs(r), 0.01) / 3.0;
			pixelDifference += d;
		}
		if (!finite) {
			error.nonFinite++;
			continue;
		}
		squared += pixelSquared;
		relative += pixelRelative;
		absolutePercent += pixelPercent;
		difference += pixelDifference;
		counted++;
	}
	if (counted == 0) {
		return true;
	}
	error.rmse = std::sqrt(squared / (3.0 * counted));
	error.relativeMse = relative / counted;
	error.mape = absolutePercent / counted;
	error.meanDifference = difference / (3.0 * counted);
	return true;
}
