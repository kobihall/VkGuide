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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

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


bool sortByName(const ImageChannel* a, const ImageChannel* b)
{
	return a->name < b->name;
}

void appendBytes(std::vector<uint8_t>& bytes, const void* data, size_t count)
{
	const uint8_t* source = (const uint8_t*)data;
	bytes.insert(bytes.end(), source, source + count);
}

ImageIoStatus exrFailure(const std::filesystem::path& path, const char* verb, const char* error)
{
	std::string message = fmt::format("{} '{}' failed: {}", verb, path.filename().string(), error != nullptr ? error : "unknown error");
	if (error != nullptr) {
		FreeEXRErrorMessage(error);
	}
	return failure(std::move(message));
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
	if (extension == ".hdr") {
		if (stbi_write_hdr(path.string().c_str(), (int)image.width, (int)image.height, 3, image.rgb.data()) == 0) {
			return failure(fmt::format("Writing '{}' failed", path.string()));
		}
		return { true, fmt::format("Saved '{}' ({} x {})", path.filename().string(), image.width, image.height) };
	}
	if (extension != ".exr") {
		return failure(fmt::format("Cannot save '{}': the extension must be .exr, .pfm or .hdr", path.filename().string()));
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

ImageIoStatus saveImage8(const std::filesystem::path& path, const Image8& image)
{
	if (image.width == 0 || image.height == 0 || image.rgba.size() != (size_t)image.width * image.height * 4) {
		return failure("There is no image to save");
	}
	if (lowerExtension(path) != ".png") {
		return failure(fmt::format("Cannot save '{}': the extension must be .png", path.filename().string()));
	}
	//the alpha channel is always 1 in a display image, so a PNG of three channels says as much
	std::vector<uint8_t> rgb((size_t)image.width * image.height * 3);
	for (size_t i = 0; i < (size_t)image.width * image.height; i++) {
		rgb[i * 3 + 0] = image.rgba[i * 4 + 0];
		rgb[i * 3 + 1] = image.rgba[i * 4 + 1];
		rgb[i * 3 + 2] = image.rgba[i * 4 + 2];
	}
	if (stbi_write_png(path.string().c_str(), (int)image.width, (int)image.height, 3, rgb.data(), (int)image.width * 3) == 0) {
		return failure(fmt::format("Writing '{}' failed", path.string()));
	}
	return { true, fmt::format("Saved '{}' ({} x {})", path.filename().string(), image.width, image.height) };
}

ImageAttribute ImageAttribute::text(std::string name, const std::string& text)
{
	ImageAttribute attribute;
	attribute.name = std::move(name);
	attribute.type = "string";
	attribute.value.assign(text.begin(), text.end());
	return attribute;
}

ImageAttribute ImageAttribute::real(std::string name, float value)
{
	ImageAttribute attribute;
	attribute.name = std::move(name);
	attribute.type = "float";
	appendBytes(attribute.value, &value, sizeof(value));
	return attribute;
}

ImageAttribute ImageAttribute::integer(std::string name, int32_t value)
{
	ImageAttribute attribute;
	attribute.name = std::move(name);
	attribute.type = "int";
	appendBytes(attribute.value, &value, sizeof(value));
	return attribute;
}

ImageAttribute ImageAttribute::matrix(std::string name, const float (&rowMajor)[16])
{
	ImageAttribute attribute;
	attribute.name = std::move(name);
	attribute.type = "m44f";
	appendBytes(attribute.value, rowMajor, sizeof(rowMajor));
	return attribute;
}

ImageAttribute ImageAttribute::chromaticities(std::string name, const float (&xy)[8])
{
	ImageAttribute attribute;
	attribute.name = std::move(name);
	attribute.type = "chromaticities";
	appendBytes(attribute.value, xy, sizeof(xy));
	return attribute;
}

std::string ImageAttribute::asText() const
{
	return type == "string" ? std::string(value.begin(), value.end()) : std::string();
}

const ImageChannel* ChannelImage::find(const std::string& name) const
{
	for (const ImageChannel& channel : channels) {
		if (channel.name == name) {
			return &channel;
		}
	}
	return nullptr;
}

ImageIoStatus saveChannelImage(const std::filesystem::path& path, const ChannelImage& image, const ExrOptions& options)
{
	const size_t pixels = (size_t)image.width * image.height;
	if (pixels == 0 || image.channels.empty()) {
		return failure("There is no image to save");
	}
	std::vector<const ImageChannel*> sorted;
	for (const ImageChannel& channel : image.channels) {
		if (channel.values.size() != pixels || channel.name.empty() || channel.name.size() > 255) {
			return failure(fmt::format("Cannot save '{}': channel '{}' is not {} x {} values with a name of up to 255 characters", path.filename().string(), channel.name, image.width, image.height));
		}
		sorted.push_back(&channel);
	}
	//the format keeps its channels sorted by name, and the readers rely on it
	std::sort(sorted.begin(), sorted.end(), sortByName);

	EXRHeader header;
	InitEXRHeader(&header);
	EXRImage exrImage;
	InitEXRImage(&exrImage);
	const int count = (int)sorted.size();
	std::vector<float*> pointers;
	for (const ImageChannel* channel : sorted) {
		pointers.push_back(const_cast<float*>(channel->values.data()));
	}
	exrImage.images = (unsigned char**)pointers.data();
	exrImage.width = (int)image.width;
	exrImage.height = (int)image.height;
	exrImage.num_channels = count;

	header.num_channels = count;
	header.channels = (EXRChannelInfo*)std::calloc((size_t)count, sizeof(EXRChannelInfo));
	header.pixel_types = (int*)std::calloc((size_t)count, sizeof(int));
	header.requested_pixel_types = (int*)std::calloc((size_t)count, sizeof(int));
	for (int i = 0; i < count; i++) {
		std::strncpy(header.channels[i].name, sorted[i]->name.c_str(), 255);
		header.pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
		header.requested_pixel_types[i] = options.half ? TINYEXR_PIXELTYPE_HALF : TINYEXR_PIXELTYPE_FLOAT;
	}
	header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

	std::vector<EXRAttribute> attributes(std::min<size_t>(image.attributes.size(), TINYEXR_MAX_CUSTOM_ATTRIBUTES));
	for (size_t i = 0; i < attributes.size(); i++) {
		const ImageAttribute& source = image.attributes[i];
		std::memset(&attributes[i], 0, sizeof(EXRAttribute));
		std::strncpy(attributes[i].name, source.name.c_str(), 255);
		std::strncpy(attributes[i].type, source.type.c_str(), 255);
		attributes[i].value = const_cast<unsigned char*>(source.value.data());
		attributes[i].size = (int)source.value.size();
	}
	header.custom_attributes = attributes.empty() ? nullptr : attributes.data();
	header.num_custom_attributes = (int)attributes.size();

	const char* error = nullptr;
	const int status = SaveEXRImageToFile(&exrImage, &header, path.string().c_str(), &error);

	//the arrays above are ours, not tinyexr's allocation, so FreeEXRHeader would free them wrongly
	std::free(header.channels);
	std::free(header.pixel_types);
	std::free(header.requested_pixel_types);
	if (status != TINYEXR_SUCCESS) {
		return exrFailure(path, "Saving", error);
	}
	return { true, fmt::format("Saved '{}' ({} x {}, {} channels)", path.filename().string(), image.width, image.height, count) };
}

ImageIoStatus loadChannelImage(const std::filesystem::path& path, ChannelImage& image)
{
	EXRVersion version;
	if (ParseEXRVersionFromFile(&version, path.string().c_str()) != TINYEXR_SUCCESS || version.multipart || version.tiled) {
		return failure(fmt::format("'{}' is not a single-part scanline EXR file", path.filename().string()));
	}
	EXRHeader header;
	InitEXRHeader(&header);
	const char* error = nullptr;
	if (ParseEXRHeaderFromFile(&header, &version, path.string().c_str(), &error) != TINYEXR_SUCCESS) {
		return exrFailure(path, "Loading", error);
	}
	//every channel comes back as a float whatever the file stores
	for (int i = 0; i < header.num_channels; i++) {
		header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
	}
	EXRImage exrImage;
	InitEXRImage(&exrImage);
	if (LoadEXRImageFromFile(&exrImage, &header, path.string().c_str(), &error) != TINYEXR_SUCCESS) {
		FreeEXRHeader(&header);
		return exrFailure(path, "Loading", error);
	}

	image = {};
	image.width = (uint32_t)exrImage.width;
	image.height = (uint32_t)exrImage.height;
	const size_t pixels = (size_t)image.width * image.height;
	for (int i = 0; i < header.num_channels; i++) {
		ImageChannel channel;
		channel.name = header.channels[i].name;
		channel.values.assign((const float*)exrImage.images[i], (const float*)exrImage.images[i] + pixels);
		image.channels.push_back(std::move(channel));
	}
	for (int i = 0; i < header.num_custom_attributes; i++) {
		const EXRAttribute& source = header.custom_attributes[i];
		ImageAttribute attribute;
		attribute.name = source.name;
		attribute.type = source.type;
		attribute.value.assign(source.value, source.value + source.size);
		image.attributes.push_back(std::move(attribute));
	}
	const std::string name = path.filename().string();
	const int channelCount = header.num_channels;
	FreeEXRImage(&exrImage);
	FreeEXRHeader(&header);
	return { true, fmt::format("Loaded '{}' ({} x {}, {} channels)", name, image.width, image.height, channelCount) };
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
