#include <rt_material.h>

#include <algorithm>
#include <cstring>

#include <rt_material_record.h>

namespace {

//---------------------------------------------------------------- parameters, per type

// the defaults are what SceneMaterial held before parameters were data, so a material made today
// starts where one did then
const SpectrumRef DEFAULT_ALBEDO { glm::vec3(0.7f) };

// the textures only an imported model's material sets (a pbrt "diffuse" with an image), as on pbr
const MaterialParam LAMBERTIAN_PARAMS[] = {
	{ "albedo", "albedo", ParamKind::Spectrum, DEFAULT_ALBEDO, 0.f, 1.f, PARAM_COLOR | PARAM_TEXTURABLE, nullptr },
	{ "baseColorTexture", "albedo texture", ParamKind::Texture, TexRef {}, 0.f, 0.f, PARAM_READ_ONLY, "sRGB, multiplies the albedo" },
	{ "normalTexture", "normal texture", ParamKind::Texture, TexRef {}, 0.f, 0.f, PARAM_READ_ONLY, "Tangent space; applied on meshes with tangents" },
};

const MaterialParam METAL_PARAMS[] = {
	{ "albedo", "albedo", ParamKind::Spectrum, DEFAULT_ALBEDO, 0.f, 1.f, PARAM_COLOR | PARAM_TEXTURABLE, nullptr },
	// the scatter clamps fuzz to at most 1, so the slider stops there too
	{ "fuzz", "fuzz", ParamKind::Float, 0.3f, 0.f, 1.f, 0, "The radius of the ball the mirror direction is perturbed within. Below 1e-4 the metal is a perfect mirror" },
};

const MaterialParam PHONG_PARAMS[] = {
	{ "albedo", "albedo", ParamKind::Spectrum, DEFAULT_ALBEDO, 0.f, 1.f, PARAM_COLOR | PARAM_TEXTURABLE, nullptr },
	{ "smoothness", "smoothness", ParamKind::Float, 0.5f, 0.f, 1.f, 0, "Mapped to a lobe exponent of 1 to 1000" },
};

const MaterialParam DIELECTRIC_PARAMS[] = {
	{ "ir", "index of refraction", ParamKind::Float, 1.5f, 1.f, 3.f, 0, "1.0 vacuum, 1.33 water, 1.5 glass, 2.42 diamond" },
};

// a light's color is what it emits, but it is the same value an albedo is, so switching a surface
// to emissive and back keeps it, as it always has
const MaterialParam EMISSIVE_PARAMS[] = {
	{ "albedo", "colour", ParamKind::Spectrum, DEFAULT_ALBEDO, 0.f, 1.f, PARAM_COLOR, nullptr },
	{ "strength", "strength", ParamKind::Float, 1.f, 0.f, 1000.f, PARAM_LOGARITHMIC, "The radiance is colour x strength, so a light can be far brighter than white" },
};

const MaterialParam PBR_PARAMS[] = {
	{ "albedo", "base colour", ParamKind::Spectrum, DEFAULT_ALBEDO, 0.f, 1.f, PARAM_COLOR | PARAM_TEXTURABLE, nullptr },
	{ "baseColorTexture", "base colour texture", ParamKind::Texture, TexRef {}, 0.f, 0.f, PARAM_READ_ONLY, "sRGB, multiplies the base colour; its alpha is the coverage the alpha test reads" },
	{ "metallic", "metallic", ParamKind::Float, 0.f, 0.f, 1.f, PARAM_TEXTURABLE, nullptr },
	{ "roughness", "roughness", ParamKind::Float, 0.5f, 0.f, 1.f, PARAM_TEXTURABLE, "Perceptual roughness: 0 is a mirror, and GGX's alpha is its square" },
	{ "metallicRoughnessTexture", "metal/rough texture", ParamKind::Texture, TexRef {}, 0.f, 0.f, PARAM_READ_ONLY, "Green multiplies roughness, blue multiplies metallic" },
	{ "normalTexture", "normal texture", ParamKind::Texture, TexRef {}, 0.f, 0.f, PARAM_READ_ONLY, "Tangent space; applied on meshes with tangents" },
	{ "normalScale", "normal scale", ParamKind::Float, 1.f, 0.f, 2.f, 0, "Scales the normal texture's tangent-plane components" },
	{ "emission", "emission", ParamKind::Spectrum, SpectrumRef { glm::vec3(0.f) }, 0.f, 1.f, PARAM_COLOR | PARAM_TEXTURABLE, nullptr },
	{ "strength", "emission strength", ParamKind::Float, 1.f, 0.f, 1000.f, PARAM_LOGARITHMIC, "The emitted radiance is emission x strength" },
	{ "emissiveTexture", "emission texture", ParamKind::Texture, TexRef {}, 0.f, 0.f, PARAM_READ_ONLY, "sRGB, multiplies the emission" },
	{ "alphaCutoff", "alpha cutoff", ParamKind::Float, 0.f, 0.f, 2.f, PARAM_READ_ONLY,
		"The base colour texel's alpha below which the surface is cut away: glTF's alphaCutoff divided by the base colour factor's alpha. 0 for never" },
};

//---------------------------------------------------------------- packing

// The six original types all pack the 64-byte record they had before the header existed, exactly as
// the renderer's gpuMaterial() filled it, so the shaders compute the same values bit for bit
void packClassic(const MaterialDef& material, const PackContext& context, MaterialRecordWriter& writer)
{
	CrtClassicMaterial out {};
	out.albedo = materialColor(material, "albedo");
	out.type = (uint32_t)material.type;
	out.albedoLayer = -1;
	out.normalLayer = -1;
	out.metalRoughLayer = -1;
	out.emissiveLayer = -1;
	out.normalScale = 1.f;
	out.emission = glm::vec3(0.f);
	switch (material.type) {
	case MaterialType::Lambertian:
		out.param = 0.f;
		//-1 unless an imported model's material names a texture, so every other material packs as it always did
		if (context.textures) {
			out.albedoLayer = materialTextureLayer(material, "baseColorTexture");
			out.normalLayer = materialTextureLayer(material, "normalTexture");
		}
		break;
	case MaterialType::Metal:
		out.param = std::min(materialFloat(material, "fuzz"), 1.f);
		break;
	case MaterialType::Phong:
		out.param = materialFloat(material, "smoothness");
		break;
	case MaterialType::Dielectric:
		out.param = materialFloat(material, "ir");
		break;
	case MaterialType::Emissive:
		out.param = materialFloat(material, "strength");
		break;
	case MaterialType::Pbr:
		out.metallic = std::clamp(materialFloat(material, "metallic"), 0.f, 1.f);
		out.roughness = std::clamp(materialFloat(material, "roughness"), 0.f, 1.f);
		out.emission = glm::max(materialColor(material, "emission"), glm::vec3(0.f)) * std::max(materialFloat(material, "strength"), 0.f);
		out.normalScale = materialFloat(material, "normalScale");
		if (context.textures) {
			out.albedoLayer = materialTextureLayer(material, "baseColorTexture");
			out.normalLayer = materialTextureLayer(material, "normalTexture");
			out.metalRoughLayer = materialTextureLayer(material, "metallicRoughnessTexture");
			out.emissiveLayer = materialTextureLayer(material, "emissiveTexture");
			out.alphaCutoff = materialFloat(material, "alphaCutoff");
		}
		break;
	}
	writer.write(&out, sizeof(out));

	//what kernel 02 sorts by and the traversal tests, decided from the record the shaders will read
	uint32_t& flags = writer.out.flags;
	const bool emits = material.type == MaterialType::Emissive || (material.type == MaterialType::Pbr && (out.emission.x > 0.f || out.emission.y > 0.f || out.emission.z > 0.f));
	flags |= emits ? CRT_MATERIAL_FLAG_EMITS : 0u;
	flags |= material.type != MaterialType::Emissive ? CRT_MATERIAL_FLAG_SCATTERS : 0u;
	flags |= out.alphaCutoff > 0.f ? CRT_MATERIAL_FLAG_CUTOUT : 0u;
	flags |= out.normalLayer >= 0 ? CRT_MATERIAL_FLAG_NORMAL_MAP : 0u;
	writer.out.emissiveLayer = out.emissiveLayer;
}

glm::vec3 noEmission(const MaterialDef&)
{
	return glm::vec3(0.f);
}

glm::vec3 emissiveEmission(const MaterialDef& material)
{
	return materialColor(material, "albedo") * materialFloat(material, "strength");
}

glm::vec3 pbrEmission(const MaterialDef& material)
{
	return glm::max(materialColor(material, "emission"), glm::vec3(0.f)) * std::max(materialFloat(material, "strength"), 0.f);
}

glm::vec3 albedoPreview(const MaterialDef& material)
{
	return materialColor(material, "albedo");
}

// a dielectric has no albedo of its own (it attenuates by white), so a pale tint reads as glass
// rather than as a plain white diffuse surface
glm::vec3 glassPreview(const MaterialDef&)
{
	return glm::vec3(0.75f, 0.85f, 1.f);
}

//---------------------------------------------------------------- the table

// by MaterialType
const MaterialTraits MATERIAL_TRAITS[] = {
	{ "lambertian", "Lambertian", LAMBERTIAN_PARAMS, MATERIAL_TYPE_SCATTERS, packClassic, noEmission, albedoPreview },
	{ "metal", "Metal", METAL_PARAMS, MATERIAL_TYPE_SCATTERS, packClassic, noEmission, albedoPreview },
	{ "phong", "Phong", PHONG_PARAMS, MATERIAL_TYPE_SCATTERS, packClassic, noEmission, albedoPreview },
	{ "dielectric", "Dielectric", DIELECTRIC_PARAMS, MATERIAL_TYPE_SCATTERS | MATERIAL_TYPE_DELTA, packClassic, noEmission, glassPreview },
	{ "emissive", "Emissive", EMISSIVE_PARAMS, MATERIAL_TYPE_EMISSIVE, packClassic, emissiveEmission, albedoPreview },
	{ "pbr", "glTF PBR", PBR_PARAMS, MATERIAL_TYPE_SCATTERS | MATERIAL_TYPE_EMISSIVE, packClassic, pbrEmission, albedoPreview },
};
static_assert(sizeof(MATERIAL_TRAITS) / sizeof(MATERIAL_TRAITS[0]) == MATERIAL_TYPE_COUNT);

// the default some type gives `name`, for a value the material's own type does not have
const ParamValue* anyTypeDefault(std::string_view name)
{
	for (const MaterialTraits& traits : MATERIAL_TRAITS) {
		for (const MaterialParam& param : traits.params) {
			if (name == param.name) {
				return &param.defaultValue;
			}
		}
	}
	return nullptr;
}

}

const MaterialTraits& materialTraits(MaterialType type)
{
	return MATERIAL_TRAITS[(size_t)type];
}

std::optional<MaterialType> findMaterialType(std::string_view name)
{
	for (const MaterialType type : MATERIAL_TYPES) {
		if (name == materialTraits(type).name) {
			return type;
		}
	}
	return std::nullopt;
}

const MaterialParam* findMaterialParam(MaterialType type, std::string_view name)
{
	for (const MaterialParam& param : materialTraits(type).params) {
		if (name == param.name) {
			return &param;
		}
	}
	return nullptr;
}

MaterialDef makeMaterial(MaterialType type, const glm::vec3& albedo)
{
	MaterialDef material;
	material.type = type;
	material.params["albedo"] = SpectrumRef { albedo };
	return material;
}

ParamValue materialValue(const MaterialDef& material, std::string_view name)
{
	if (const auto found = material.params.find(name); found != material.params.end()) {
		return found->second;
	}
	if (const MaterialParam* param = findMaterialParam(material.type, name)) {
		return param->defaultValue;
	}
	if (const ParamValue* fallback = anyTypeDefault(name)) {
		return *fallback;
	}
	return 0.f;
}

float materialFloat(const MaterialDef& material, std::string_view name)
{
	const ParamValue value = materialValue(material, name);
	const float* f = std::get_if<float>(&value);
	return f != nullptr ? *f : 0.f;
}

glm::vec3 materialColor(const MaterialDef& material, std::string_view name)
{
	const ParamValue value = materialValue(material, name);
	const SpectrumRef* s = std::get_if<SpectrumRef>(&value);
	return s != nullptr ? s->rgb : glm::vec3(0.f);
}

int materialTextureLayer(const MaterialDef& material, std::string_view name)
{
	const ParamValue value = materialValue(material, name);
	const TexRef* t = std::get_if<TexRef>(&value);
	return t != nullptr ? t->layer : -1;
}

void MaterialRecordWriter::write(const void* data, size_t bytes)
{
	const size_t first = out.params.size();
	out.params.resize(first + (bytes + sizeof(glm::vec4) - 1) / sizeof(glm::vec4), glm::vec4(0.f));
	std::memcpy(&out.params[first], data, bytes);
}

PackedMaterial packMaterial(const MaterialDef& material, const PackContext& context)
{
	const MaterialTraits& traits = materialTraits(material.type);
	PackedMaterial packed;
	packed.type = (uint32_t)material.type;
	packed.flags = (traits.flags & MATERIAL_TYPE_COMPLEX) != 0 ? CRT_MATERIAL_FLAG_COMPLEX : 0u;
	MaterialRecordWriter writer { packed };
	traits.pack(material, context, writer);
	packed.emission = traits.emission(material);
	return packed;
}

glm::vec3 materialEmission(const MaterialDef& material)
{
	return materialTraits(material.type).emission(material);
}

glm::vec3 materialPreviewColor(const MaterialDef& material)
{
	return materialTraits(material.type).previewColor(material);
}
