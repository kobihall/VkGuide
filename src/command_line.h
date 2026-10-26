#pragma once

// The command line of ./bin/vulkan_guide, with no Vulkan. With no arguments the application opens as it always did.
// --render opens the BATCH MODE: the window opens (the swapchain and ImGui assume one), the scene renders to its
// sample target, the result is written to the --out file and the program quits, with exit code 0 when the file
// was written. The verification scripts (tools/verify/) use it in place of the smoke-test hooks they had.
// docs/plans/completed/pbrt-aovs-and-image-output.md.
//
//   ./bin/vulkan_guide --render scene.gltf --spp 4096 --out cornell.exr
//
// Run from bin/, as the application always is. A relative --render path is looked up in assets/scenes/.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct CommandLine {
	// --render <scene>: the scene to render headless
	std::filesystem::path renderScene;
	// --out <file>: where the render goes. The extension picks the format: .exr (with every output the render
	// keeps, as named channels), .pfm, .hdr, or .png (the display image as the window shows it)
	std::filesystem::path output;
	// --spp <n>: samples per pixel. 0 renders to the scene's own target
	uint32_t samples { 0 };
	// --seed <n>: the render's fixed seed, 1 by default, so the same scene gives the same image
	uint32_t seed { 1 };
	// --size <width>x<height>: overrides the scene's resolution
	std::optional<std::pair<int, int>> size;
	// --timeout <seconds>: gives up (exit code 3) when the render takes longer
	double timeoutSeconds { 1800.0 };
	// --aov <list>: auxiliary outputs to keep besides the scene's own, by id and comma separated, or `all`
	std::vector<std::string> aov;
	// --capture-through-specular: record the first non-specular surface instead of the first
	bool captureThroughSpecular { false };
	// --light-groups: keep the light groups' sums
	bool lightGroups { false };
	// --half: 16-bit half floats in an EXR file
	bool halfFloat { false };
	// --debug-view <id>: shade with a debug view (none, direction, hit, normal, bounce, samples, traversal, mis,
	// disagreement, ao), empty for the picture. --ao-distance <x>: the ambient occlusion view's ray length, 0 for the
	// default
	std::string debugView;
	float aoDistance { 0.f };
	// --relight <scale,...>: show the render relit, each light group's sums times its scale (and written so to a .png).
	// Needs --light-groups
	std::vector<float> relight;
	// THE SPECTRAL MODES (docs/plans/completed/pbrt-spectral-rendering.md), each in place of the scene's own:
	// --spectral <rgb|monochromatic|gaussians|bands|hero>, --lanes <4|8|16|32>, --wavelength <nm> (monochromatic),
	// --range <min>,<max> (nm), --sampling <visible|uniform|lights>, --color-space <srgb|dci-p3|rec2020|aces2065-1>
	// and --design-wavelength <nm>. Empty or 0 keeps the scene's
	std::string spectral;
	uint32_t lanes { 0 };
	float wavelength { 0.f };
	std::optional<std::pair<float, float>> range;
	std::string sampling;
	std::string colorSpace;
	float designWavelength { 0.f };
	// --spectral-images <n>: keep n wavelength buckets per pixel (hero) or the bands, written as S0.<l>nm channels
	uint32_t spectralImages { 0 };
	// --help
	bool help { false };

	bool spectralGiven() const
	{
		return !spectral.empty() || lanes != 0 || wavelength != 0.f || range.has_value() || !sampling.empty() || !colorSpace.empty() || designWavelength != 0.f || spectralImages != 0;
	}

	bool batch() const { return !renderScene.empty(); }
};

// false with `error` set when the arguments do not parse: an unknown flag, a missing value, --render without
// --out (or the reverse), a count that is not a number
bool parseCommandLine(int argc, char* const* argv, CommandLine& out, std::string& error);

// the text --help prints
std::string commandLineUsage();
