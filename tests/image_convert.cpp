// Converts an image file between the formats src/image_io.h reads and writes, with no Vulkan: .exr, .pfm and .hdr. For
// the verification scripts (tools/verify/), which make an OpenEXR environment map from a Radiance one with it.
//
// Run:  ./bin/image_convert in.hdr out.exr [--half]

#include <image_io.h>

#include <cstring>
#include <filesystem>

#include <fmt/format.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

int main(int argc, char** argv)
{
	if (argc < 3) {
		fmt::println("usage: image_convert in out [--half]");
		return 2;
	}
	const std::filesystem::path in = argv[1];
	const std::filesystem::path out = argv[2];
	const bool half = argc > 3 && std::strcmp(argv[3], "--half") == 0;

	Image image;
	if (in.extension() == ".hdr") {
		int width = 0;
		int height = 0;
		int channels = 0;
		float* pixels = stbi_loadf(in.string().c_str(), &width, &height, &channels, 3);
		if (pixels == nullptr) {
			fmt::println("cannot read '{}': {}", in.string(), stbi_failure_reason());
			return 1;
		}
		image.width = (uint32_t)width;
		image.height = (uint32_t)height;
		image.rgb.assign(pixels, pixels + (size_t)width * height * 3);
		stbi_image_free(pixels);
	} else {
		const ImageIoStatus status = loadImage(in, image);
		if (!status.ok) {
			fmt::println("{}", status.message);
			return 1;
		}
	}
	if (out.extension() == ".exr" && half) {
		ChannelImage channels;
		channels.width = image.width;
		channels.height = image.height;
		for (size_t c = 0; c < 3; c++) {
			ImageChannel channel;
			channel.name = std::string(1, "RGB"[c]);
			channel.values.resize(image.pixelCount());
			for (size_t i = 0; i < image.pixelCount(); i++) {
				channel.values[i] = image.rgb[i * 3 + c];
			}
			channels.channels.push_back(std::move(channel));
		}
		const ImageIoStatus status = saveChannelImage(out, channels, ExrOptions { true });
		fmt::println("{}", status.message);
		return status.ok ? 0 : 1;
	}
	const ImageIoStatus status = saveImage(out, image);
	fmt::println("{}", status.message);
	return status.ok ? 0 : 1;
}
