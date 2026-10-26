#include <render_export.h>

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

namespace {

void addChannel(ChannelImage& image, const std::string& name, std::vector<float> values)
{
	image.channels.push_back({ name, std::move(values) });
}

std::vector<float> rowMajor(const glm::mat4& m)
{
	std::vector<float> out(16);
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			out[(size_t)row * 4 + column] = m[column][row];
		}
	}
	return out;
}

}

std::string exportChannelName(const std::string& name)
{
	std::string out;
	for (const char c : name) {
		out.push_back(std::isalnum((unsigned char)c) ? c : '_');
	}
	return out.empty() ? "group" : out;
}

ChannelImage buildExportImage(const Image& beauty, const RenderOutputsData& outputs, const RenderExportInfo& info)
{
	ChannelImage image;
	image.width = beauty.width;
	image.height = beauty.height;
	const size_t pixels = beauty.pixelCount();

	//the picture
	for (size_t c = 0; c < 3; c++) {
		std::vector<float> values(pixels);
		for (size_t i = 0; i < pixels; i++) {
			values[i] = beauty.rgb[i * 3 + c];
		}
		addChannel(image, std::string(1, "RGB"[c]), std::move(values));
	}

	const AovSettings& aov = outputs.aov;
	const bool sized = outputs.width == beauty.width && outputs.height == beauty.height;
	if (sized && outputs.layout.count > 0) {
		//every pixel's layers, resolved
		std::vector<AovResolved> resolved(pixels);
		for (size_t i = 0; i < pixels; i++) {
			AovPixel pixel;
			for (uint32_t layer = 0; layer < AOV_LAYER_COUNT; layer++) {
				const uint32_t index = outputs.layout.layer[layer];
				if (index == AOV_LAYER_NONE) {
					continue;
				}
				const float* source = outputs.aovLayers.data() + ((size_t)index * pixels + i) * 4;
				pixel.layers[layer] = glm::vec4(source[0], source[1], source[2], source[3]);
			}
			resolved[i] = resolveAov(pixel);
		}
		auto channel = [&](const std::string& name, auto&& pick) {
			std::vector<float> values(pixels);
			for (size_t i = 0; i < pixels; i++) {
				values[i] = pick(resolved[i]);
			}
			addChannel(image, name, std::move(values));
		};
		if (aov.has(AovOutput::Albedo)) {
			channel("Albedo.R", [](const AovResolved& r) { return r.albedo.x; });
			channel("Albedo.G", [](const AovResolved& r) { return r.albedo.y; });
			channel("Albedo.B", [](const AovResolved& r) { return r.albedo.z; });
		}
		if (aov.has(AovOutput::Position)) {
			channel("P.X", [](const AovResolved& r) { return r.position.x; });
			channel("P.Y", [](const AovResolved& r) { return r.position.y; });
			channel("P.Z", [](const AovResolved& r) { return r.position.z; });
		}
		if (aov.has(AovOutput::Depth)) {
			channel("Z", [](const AovResolved& r) { return r.depth; });
		}
		if (aov.has(AovOutput::Normal)) {
			channel("N.X", [](const AovResolved& r) { return r.normal.x; });
			channel("N.Y", [](const AovResolved& r) { return r.normal.y; });
			channel("N.Z", [](const AovResolved& r) { return r.normal.z; });
		}
		if (aov.has(AovOutput::ShadingNormal)) {
			channel("Ns.X", [](const AovResolved& r) { return r.shadingNormal.x; });
			channel("Ns.Y", [](const AovResolved& r) { return r.shadingNormal.y; });
			channel("Ns.Z", [](const AovResolved& r) { return r.shadingNormal.z; });
		}
		if (aov.has(AovOutput::Uv)) {
			channel("u", [](const AovResolved& r) { return r.uv.x; });
			channel("v", [](const AovResolved& r) { return r.uv.y; });
		}
		if (aov.has(AovOutput::Ids)) {
			channel("id.instance", [](const AovResolved& r) { return r.instance; });
			channel("id.material", [](const AovResolved& r) { return r.material; });
		}
		if (aov.has(AovOutput::Variance)) {
			channel("Variance.R", [](const AovResolved& r) { return r.variance.x; });
			channel("Variance.G", [](const AovResolved& r) { return r.variance.y; });
			channel("Variance.B", [](const AovResolved& r) { return r.variance.z; });
			channel("RelativeVariance.R", [](const AovResolved& r) { return r.relativeVariance.x; });
			channel("RelativeVariance.G", [](const AovResolved& r) { return r.relativeVariance.y; });
			channel("RelativeVariance.B", [](const AovResolved& r) { return r.relativeVariance.z; });
		}
	}
	if (sized && aov.has(AovOutput::SampleCount) && outputs.samplesTaken.size() == pixels) {
		std::vector<float> values(pixels);
		for (size_t i = 0; i < pixels; i++) {
			values[i] = (float)outputs.samplesTaken[i];
		}
		addChannel(image, "n", std::move(values));
	}

	//each light group's share of the picture: its sums over the weight sum they were taken under
	const size_t stride = 3 * (size_t)outputs.lightGroups + 1 + outputs.spectralBuckets;
	if (sized && outputs.lightGroups > 0) {
		for (uint32_t g = 0; g < outputs.lightGroups; g++) {
			const std::string name = g < info.lightGroupNames.size() ? exportChannelName(info.lightGroupNames[g]) : fmt::format("group{}", g);
			for (size_t c = 0; c < 3; c++) {
				std::vector<float> values(pixels);
				for (size_t i = 0; i < pixels; i++) {
					const float weight = outputs.groupSums[i * stride + 3 * (size_t)outputs.lightGroups];
					values[i] = weight != 0.f ? outputs.groupSums[i * stride + 3 * (size_t)g + c] / weight : 0.f;
				}
				addChannel(image, fmt::format("LightGroup.{}.{}", name, "RGB"[c]), std::move(values));
			}
		}
	}

	//THE SPECTRAL IMAGES, in the Spectral OpenEXR layout (Fichet, Pacanowski and Wilkie 2021) pbrt-v4's SpectralFilm
	//writes: an emissive channel S0.<wavelength>nm per bucket, with a comma for the decimal point, in W / (m^2 sr nm).
	//A monochromatic render's picture is its one wavelength, and is written as that channel too
	auto spectralName = [](float lambda) {
		const double nm = std::round((double)lambda * 1e6) / 1e6;
		const double whole = std::floor(nm);
		return fmt::format("S0.{},{:06d}nm", (long long)whole, (int)std::lround((nm - whole) * 1e6));
	};
	bool spectral = false;
	if (sized && outputs.spectralBuckets > 0 && outputs.groupSums.size() == pixels * stride) {
		for (uint32_t b = 0; b < outputs.spectralBuckets; b++) {
			std::vector<float> values(pixels);
			for (size_t i = 0; i < pixels; i++) {
				const float weight = outputs.groupSums[i * stride + 3 * (size_t)outputs.lightGroups];
				values[i] = weight != 0.f ? outputs.groupSums[i * stride + 3 * (size_t)outputs.lightGroups + 1 + b] / weight : 0.f;
			}
			addChannel(image, spectralName(b < outputs.spectralWavelengths.size() ? outputs.spectralWavelengths[b] : 0.f), std::move(values));
		}
		spectral = true;
	} else if (outputs.monochromaticWavelength > 0.f) {
		std::vector<float> values(pixels);
		for (size_t i = 0; i < pixels; i++) {
			values[i] = beauty.rgb[i * 3];
		}
		addChannel(image, spectralName(outputs.monochromaticWavelength), std::move(values));
		spectral = true;
	}

	//the header, as pbrt's ImageMetadata
	image.attributes.push_back(ImageAttribute::real("renderTimeSeconds", info.renderSeconds));
	image.attributes.push_back(ImageAttribute::integer("samplesPerPixel", (int32_t)outputs.samplesPerPixel));
	{
		const std::vector<float> cameraFromWorld = rowMajor(info.cameraFromWorld);
		const std::vector<float> ndcFromWorld = rowMajor(info.ndcFromWorld);
		float a[16];
		float b[16];
		std::copy(cameraFromWorld.begin(), cameraFromWorld.end(), a);
		std::copy(ndcFromWorld.begin(), ndcFromWorld.end(), b);
		image.attributes.push_back(ImageAttribute::matrix("cameraFromWorld", a));
		image.attributes.push_back(ImageAttribute::matrix("NDCFromWorld", b));
	}
	if (info.hasMse) {
		image.attributes.push_back(ImageAttribute::real("MSE", info.mse));
	}
	//Rec. 709 primaries and the D65 white point: the working space, as sRGB's
	const float rec709[8] = { 0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f };
	image.attributes.push_back(ImageAttribute::chromaticities("chromaticities", rec709));
	image.attributes.push_back(ImageAttribute::text("vkguide:scene", info.scenePath));
	image.attributes.push_back(ImageAttribute::text("vkguide:kernels", info.kernels));
	image.attributes.push_back(ImageAttribute::text("vkguide:settings", info.settings));
	for (uint32_t g = 0; g < outputs.lightGroups && g < info.lightGroupNames.size(); g++) {
		image.attributes.push_back(ImageAttribute::text(fmt::format("vkguide:lightGroup{}", g), info.lightGroupNames[g]));
	}
	if (spectral) {
		image.attributes.push_back(ImageAttribute::text("spectralLayoutVersion", "1.0"));
		image.attributes.push_back(ImageAttribute::text("emissiveUnits", "W.m^-2.sr^-1.nm^-1"));
	}
	return image;
}
