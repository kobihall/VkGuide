#include <aov.h>

namespace {

struct OutputInfo {
	const char* name;
	const char* description;
};

constexpr OutputInfo OUTPUT_INFO[(size_t)AovOutput::Count] = {
	{ "Position", "The first visible surface's world-space position, P.X P.Y P.Z in the file." },
	{ "Depth", "The distance of that surface along the camera's forward axis, Z in the file." },
	{ "Normal", "The geometric normal, N.X N.Y N.Z, averaged over the pixel's samples and normalized." },
	{ "Shading normal", "The normal the material shaded with, after the normal map, Ns.X Ns.Y Ns.Z. It faces the same side as the geometric normal." },
	{ "UV", "The surface's texture coordinates, u and v." },
	{ "Albedo", "The BSDF's directional albedo towards the camera, estimated from 16 fixed samples as pbrt does: Albedo.R Albedo.G Albedo.B." },
	{ "Ids", "The instance and the material of the first surface sample, id.instance and id.material. Never averaged, so an edge pixel keeps one object's id." },
	{ "Variance", "The variance of the pixel's samples per channel, Variance.R .G .B, and the variance over the mean, RelativeVariance.R .G .B." },
	{ "Sample count", "The samples each pixel has taken, n." },
};

}

const char* aovOutputName(AovOutput output)
{
	return output < AovOutput::Count ? OUTPUT_INFO[(size_t)output].name : "?";
}

const char* aovOutputId(AovOutput output)
{
	static const char* const ids[(size_t)AovOutput::Count] = { "position", "depth", "normal", "shadingNormal", "uv", "albedo", "ids", "variance", "sampleCount" };
	return output < AovOutput::Count ? ids[(size_t)output] : "";
}

AovOutput aovOutputFromId(const std::string& id)
{
	for (uint32_t i = 0; i < (uint32_t)AovOutput::Count; i++) {
		if (id == aovOutputId((AovOutput)i)) {
			return (AovOutput)i;
		}
	}
	return AovOutput::Count;
}

const char* aovOutputDescription(AovOutput output)
{
	return output < AovOutput::Count ? OUTPUT_INFO[(size_t)output].description : "";
}

uint32_t AovLayout::pack() const
{
	uint32_t packed = 0;
	for (uint32_t i = 0; i < AOV_LAYER_COUNT; i++) {
		packed |= (layer[i] & 0xFu) << (4u * i);
	}
	return packed;
}

uint32_t unpackAovLayer(uint32_t packed, AovLayer which)
{
	return (packed >> (4u * (uint32_t)which)) & 0xFu;
}

AovLayout makeAovLayout(const AovSettings& settings)
{
	AovLayout layout;
	layout.layer.fill(AOV_LAYER_NONE);
	if (!settings.accumulates()) {
		return layout;
	}
	bool wanted[AOV_LAYER_COUNT] = {};
	wanted[(size_t)AovLayer::Weights] = true;
	wanted[(size_t)AovLayer::PositionDepth] = settings.has(AovOutput::Position) || settings.has(AovOutput::Depth);
	wanted[(size_t)AovLayer::Normal] = settings.has(AovOutput::Normal);
	wanted[(size_t)AovLayer::ShadingNormal] = settings.has(AovOutput::ShadingNormal);
	wanted[(size_t)AovLayer::Uv] = settings.has(AovOutput::Uv);
	wanted[(size_t)AovLayer::Albedo] = settings.has(AovOutput::Albedo);
	wanted[(size_t)AovLayer::VarianceMean] = settings.has(AovOutput::Variance);
	wanted[(size_t)AovLayer::VarianceM2] = settings.has(AovOutput::Variance);
	for (uint32_t i = 0; i < AOV_LAYER_COUNT; i++) {
		if (wanted[i]) {
			layout.layer[i] = layout.count++;
		}
	}
	return layout;
}

void AovPixel::add(const AovSample& sample, float weight, const glm::vec3& radiance)
{
	glm::vec4& weights = layers[(size_t)AovLayer::Weights];
	weights.y += weight;
	if (sample.hit) {
		weights.x += weight;
		if (weights.z == 0.f) {
			weights.z = float(sample.instance) + 1.f;
			weights.w = float(sample.material) + 1.f;
		}
		glm::vec4& position = layers[(size_t)AovLayer::PositionDepth];
		position.x += weight * sample.position.x;
		position.y += weight * sample.position.y;
		position.z += weight * sample.position.z;
		position.w += weight * sample.depth;
		glm::vec4& normal = layers[(size_t)AovLayer::Normal];
		normal.x += weight * sample.normal.x;
		normal.y += weight * sample.normal.y;
		normal.z += weight * sample.normal.z;
		glm::vec4& shading = layers[(size_t)AovLayer::ShadingNormal];
		shading.x += weight * sample.shadingNormal.x;
		shading.y += weight * sample.shadingNormal.y;
		shading.z += weight * sample.shadingNormal.z;
		glm::vec4& uv = layers[(size_t)AovLayer::Uv];
		uv.x += weight * sample.uv.x;
		uv.y += weight * sample.uv.y;
	}
	glm::vec4& albedo = layers[(size_t)AovLayer::Albedo];
	albedo.x += weight * sample.albedo.x;
	albedo.y += weight * sample.albedo.y;
	albedo.z += weight * sample.albedo.z;

	//Welford's update per channel, pbrt's VarianceEstimator::Add
	glm::vec4& mean = layers[(size_t)AovLayer::VarianceMean];
	glm::vec4& m2 = layers[(size_t)AovLayer::VarianceM2];
	mean.w += 1.f;
	const float n = mean.w;
	for (int c = 0; c < 3; c++) {
		const float delta = radiance[c] - mean[c];
		mean[c] += delta / n;
		const float delta2 = radiance[c] - mean[c];
		m2[c] += delta * delta2;
	}
}

AovResolved resolveAov(const AovPixel& pixel)
{
	const glm::vec4& weights = pixel.layers[(size_t)AovLayer::Weights];
	const float surfaceWeight = weights.x;
	const float totalWeight = weights.y;
	AovResolved out;

	const glm::vec4& position = pixel.layers[(size_t)AovLayer::PositionDepth];
	const glm::vec4& uv = pixel.layers[(size_t)AovLayer::Uv];
	if (surfaceWeight != 0.f) {
		out.position = glm::vec3(position.x, position.y, position.z) / surfaceWeight;
		out.depth = position.w / surfaceWeight;
		out.uv = glm::vec2(uv.x, uv.y) / surfaceWeight;
	}
	auto normalized = [](const glm::vec4& sum) {
		const glm::vec3 v(sum.x, sum.y, sum.z);
		return glm::dot(v, v) > 0.f ? glm::normalize(v) : glm::vec3(0.f);
	};
	out.normal = normalized(pixel.layers[(size_t)AovLayer::Normal]);
	out.shadingNormal = normalized(pixel.layers[(size_t)AovLayer::ShadingNormal]);

	const glm::vec4& albedo = pixel.layers[(size_t)AovLayer::Albedo];
	if (totalWeight != 0.f) {
		out.albedo = glm::vec3(albedo.x, albedo.y, albedo.z) / totalWeight;
	}
	if (weights.z > 0.f) {
		out.instance = weights.z - 1.f;
		out.material = weights.w - 1.f;
	}

	const glm::vec4& mean = pixel.layers[(size_t)AovLayer::VarianceMean];
	const glm::vec4& m2 = pixel.layers[(size_t)AovLayer::VarianceM2];
	out.varianceSamples = mean.w;
	for (int c = 0; c < 3; c++) {
		out.variance[c] = mean.w > 1.f ? m2[c] / (mean.w - 1.f) : 0.f;
		out.relativeVariance[c] = (mean.w < 1.f || mean[c] == 0.f) ? 0.f : out.variance[c] / mean[c];
	}
	return out;
}

glm::vec3 aovIdColor(uint32_t id)
{
	//PCG's output permutation, so neighboring ids get unrelated colors; kept off black so every id shows
	uint32_t h = id * 747796405u + 2891336453u;
	h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
	h = (h >> 22u) ^ h;
	const glm::vec3 bytes(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u));
	return bytes * (0.75f / 255.f) + glm::vec3(0.25f);
}
