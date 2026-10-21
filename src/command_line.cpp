#include <command_line.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include <aov.h>

namespace {

bool parseUnsigned(const char* text, uint32_t& out)
{
	const char* end = text + std::strlen(text);
	uint32_t value = 0;
	const auto result = std::from_chars(text, end, value);
	if (result.ec != std::errc() || result.ptr != end) {
		return false;
	}
	out = value;
	return true;
}

}

std::string commandLineUsage()
{
	std::string outputs;
	for (uint32_t i = 0; i < (uint32_t)AovOutput::Count; i++) {
		outputs += fmt::format("{}{}", i > 0 ? ", " : "", aovOutputId((AovOutput)i));
	}
	return fmt::format(
		"usage: vulkan_guide [--render <scene> --out <file> [options]]\n"
		"\n"
		"With no arguments the application opens as usual. --render renders a scene headless (the window opens behind the\n"
		"active app), writes the result and quits. Run it from bin/. A relative scene path is looked up in assets/scenes/.\n"
		"\n"
		"  --render <scene>            the scene file to render, or a pbrt-v4 scene (.pbrt, .pbrt.gz) rendered with its own\n"
		"                              camera, resolution, samples, sampler, filter and depth\n"
		"  --out <file>                where the render goes: .exr (every output as named channels), .pfm, .hdr or .png\n"
		"                              (the display image as the window shows it)\n"
		"  --spp <n>                   samples per pixel; the scene's own target when omitted\n"
		"  --seed <n>                  fixed seed, 1 when omitted\n"
		"  --size <width>x<height>     the resolution, in place of the scene's\n"
		"  --timeout <seconds>         give up with exit code 3 after this long (1800 when omitted)\n"
		"  --aov <list>                auxiliary outputs to keep, comma separated, or `all`: {}\n"
		"  --capture-through-specular  record the first non-specular surface for the auxiliary outputs\n"
		"  --light-groups              keep the light groups' sums, written as LightGroup.<name>.R in an EXR file\n"
		"  --half                      16-bit half floats in an EXR file\n"
		"  --debug-view <id>           shade with a debug view: none, direction, hit, normal, bounce, samples, traversal, mis,\n"
		"                              disagreement or ao\n"
		"  --ao-distance <x>           the ambient occlusion view's ray length; 0 (the default) is a tenth of the scene's size\n"
		"  --relight <scale,...>       show the render relit: each light group's sums times its scale (needs --light-groups)\n"
		"  --help                      this text\n"
		"\n"
		"exit code: 0 the file was written, 2 the scene or the options were refused, 3 a timeout\n",
		outputs);
}

bool parseCommandLine(int argc, char* const* argv, CommandLine& out, std::string& error)
{
	out = {};
	for (int i = 1; i < argc; i++) {
		const std::string flag = argv[i];
		//the value of a flag that takes one
		auto value = [&](const char*& text) {
			if (i + 1 >= argc) {
				error = fmt::format("{} needs a value", flag);
				return false;
			}
			text = argv[++i];
			return true;
		};
		const char* text = nullptr;
		if (flag == "--help" || flag == "-h") {
			out.help = true;
		} else if (flag == "--render") {
			if (!value(text)) {
				return false;
			}
			out.renderScene = text;
		} else if (flag == "--out") {
			if (!value(text)) {
				return false;
			}
			out.output = text;
		} else if (flag == "--spp") {
			if (!value(text)) {
				return false;
			}
			if (!parseUnsigned(text, out.samples) || out.samples == 0) {
				error = fmt::format("--spp needs a positive whole number, not '{}'", text);
				return false;
			}
		} else if (flag == "--seed") {
			if (!value(text)) {
				return false;
			}
			if (!parseUnsigned(text, out.seed)) {
				error = fmt::format("--seed needs a whole number, not '{}'", text);
				return false;
			}
		} else if (flag == "--size") {
			if (!value(text)) {
				return false;
			}
			int width = 0;
			int height = 0;
			char extra = 0;
			if (std::sscanf(text, "%dx%d%c", &width, &height, &extra) != 2 || width <= 0 || height <= 0) {
				error = fmt::format("--size needs <width>x<height>, not '{}'", text);
				return false;
			}
			out.size = std::make_pair(width, height);
		} else if (flag == "--timeout") {
			if (!value(text)) {
				return false;
			}
			char* end = nullptr;
			const double seconds = std::strtod(text, &end);
			if (end == text || *end != '\0' || !(seconds > 0.0)) {
				error = fmt::format("--timeout needs a positive number of seconds, not '{}'", text);
				return false;
			}
			out.timeoutSeconds = seconds;
		} else if (flag == "--aov") {
			if (!value(text)) {
				return false;
			}
			//a list of ids, or `all`; an id this build does not have is refused rather than skipped, since a script
			//that asks for an output it never gets would read a file without it
			std::string list = text;
			size_t start = 0;
			while (start <= list.size()) {
				const size_t comma = list.find(',', start);
				const std::string id = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
				if (id == "all") {
					for (uint32_t k = 0; k < (uint32_t)AovOutput::Count; k++) {
						out.aov.push_back(aovOutputId((AovOutput)k));
					}
				} else if (aovOutputFromId(id) != AovOutput::Count) {
					out.aov.push_back(id);
				} else {
					error = fmt::format("--aov: '{}' is not an output", id);
					return false;
				}
				if (comma == std::string::npos) {
					break;
				}
				start = comma + 1;
			}
		} else if (flag == "--capture-through-specular") {
			out.captureThroughSpecular = true;
		} else if (flag == "--light-groups") {
			out.lightGroups = true;
		} else if (flag == "--half") {
			out.halfFloat = true;
		} else if (flag == "--debug-view") {
			if (!value(text)) {
				return false;
			}
			out.debugView = text;
		} else if (flag == "--ao-distance") {
			if (!value(text)) {
				return false;
			}
			char* end = nullptr;
			const double distance = std::strtod(text, &end);
			if (end == text || *end != '\0' || !(distance >= 0.0)) {
				error = fmt::format("--ao-distance needs a distance of 0 or more, not '{}'", text);
				return false;
			}
			out.aoDistance = (float)distance;
		} else if (flag == "--relight") {
			if (!value(text)) {
				return false;
			}
			std::string list = text;
			size_t start = 0;
			while (start <= list.size()) {
				const size_t comma = list.find(',', start);
				const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
				char* end = nullptr;
				const double scale = std::strtod(item.c_str(), &end);
				if (item.empty() || *end != '\0' || !(scale >= 0.0)) {
					error = fmt::format("--relight needs scales of 0 or more, comma separated, not '{}'", text);
					return false;
				}
				out.relight.push_back((float)scale);
				if (comma == std::string::npos) {
					break;
				}
				start = comma + 1;
			}
		} else {
			error = fmt::format("unknown option '{}'", flag);
			return false;
		}
	}
	if (out.help) {
		return true;
	}
	if (out.batch() != !out.output.empty()) {
		error = out.batch() ? "--render needs --out" : "--out needs --render";
		return false;
	}
	if (!out.relight.empty() && !out.lightGroups) {
		error = "--relight needs --light-groups";
		return false;
	}
	if (!out.batch() && (!out.debugView.empty() || out.aoDistance != 0.f || !out.relight.empty() || out.samples != 0 || !out.aov.empty() || out.size.has_value() || out.lightGroups || out.captureThroughSpecular || out.halfFloat)) {
		error = "this option only applies to --render";
		return false;
	}
	return true;
}
