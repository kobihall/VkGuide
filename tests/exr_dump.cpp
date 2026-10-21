// Lists the channels and header attributes of an OpenEXR file, and optionally writes each channel as a NumPy .npy
// file, for the verification scripts (tools/verify/), which read the render's auxiliary outputs through it.
//
// Run:  ./bin/exr_dump file.exr [--npy <directory>]
//   prints each channel's name with its minimum, mean and maximum, and each attribute
//   --npy writes <directory>/<channel name>.npy, float32 of shape (height, width), row 0 at the top

#include <image_io.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <fmt/format.h>

namespace {

bool writeNpy(const std::filesystem::path& path, uint32_t width, uint32_t height, const std::vector<float>& values)
{
	FILE* file = std::fopen(path.string().c_str(), "wb");
	if (file == nullptr) {
		return false;
	}
	std::string header = fmt::format("{{'descr': '<f4', 'fortran_order': False, 'shape': ({}, {}), }}", height, width);
	//version 1.0: magic, two version bytes, a 2-byte header length, then the header padded to a multiple of 64 bytes
	//that ends in a newline
	while ((10 + header.size() + 1) % 64 != 0) {
		header.push_back(' ');
	}
	header.push_back('\n');
	const uint8_t magic[8] = { 0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0 };
	const uint16_t length = (uint16_t)header.size();
	bool ok = std::fwrite(magic, 1, 8, file) == 8 && std::fwrite(&length, 2, 1, file) == 1 && std::fwrite(header.data(), 1, header.size(), file) == header.size();
	ok = ok && std::fwrite(values.data(), sizeof(float), values.size(), file) == values.size();
	return std::fclose(file) == 0 && ok;
}

}

int main(int argc, char** argv)
{
	if (argc < 2) {
		fmt::println("usage: exr_dump file.exr [--npy <directory>]");
		return 2;
	}
	std::filesystem::path npyDirectory;
	for (int i = 2; i < argc; i++) {
		if (std::strcmp(argv[i], "--npy") == 0 && i + 1 < argc) {
			npyDirectory = argv[++i];
		} else {
			fmt::println("usage: exr_dump file.exr [--npy <directory>]");
			return 2;
		}
	}

	ChannelImage image;
	const ImageIoStatus status = loadChannelImage(argv[1], image);
	fmt::println("{}", status.message);
	if (!status.ok) {
		return 1;
	}
	for (const ImageChannel& channel : image.channels) {
		double sum = 0.0;
		float lowest = INFINITY;
		float highest = -INFINITY;
		size_t nonFinite = 0;
		for (const float v : channel.values) {
			if (!std::isfinite(v)) {
				nonFinite++;
				continue;
			}
			sum += v;
			lowest = std::min(lowest, v);
			highest = std::max(highest, v);
		}
		const size_t finite = channel.values.size() - nonFinite;
		fmt::println("  {:24} min {:12.6g} mean {:12.6g} max {:12.6g}{}", channel.name, lowest, finite > 0 ? sum / (double)finite : 0.0, highest, nonFinite > 0 ? fmt::format("  ({} non-finite)", nonFinite) : "");
		if (!npyDirectory.empty()) {
			std::filesystem::create_directories(npyDirectory);
			if (!writeNpy(npyDirectory / (channel.name + ".npy"), image.width, image.height, channel.values)) {
				fmt::println("cannot write {}", (npyDirectory / (channel.name + ".npy")).string());
				return 1;
			}
		}
	}
	for (const ImageAttribute& attribute : image.attributes) {
		if (attribute.type == "string") {
			fmt::println("  attribute {} (string): {}", attribute.name, attribute.asText());
		} else if (attribute.type == "float" && attribute.value.size() == 4) {
			float v;
			std::memcpy(&v, attribute.value.data(), 4);
			fmt::println("  attribute {} (float): {}", attribute.name, v);
		} else if (attribute.type == "int" && attribute.value.size() == 4) {
			int32_t v;
			std::memcpy(&v, attribute.value.data(), 4);
			fmt::println("  attribute {} (int): {}", attribute.name, v);
		} else {
			fmt::println("  attribute {} ({}, {} bytes)", attribute.name, attribute.type, attribute.value.size());
		}
	}
	return 0;
}
