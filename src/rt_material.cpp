#include <rt_material.h>

#include <algorithm>
#include <cstring>

#include <bxdf.h>
#include <rt_material_record.h>

namespace {

//---------------------------------------------------------------- parameters, per type

// the defaults are what SceneMaterial held before parameters were data, so a material made today
// starts where one did then
const SpectrumRef DEFAULT_ALBEDO = rgbSpectrum(glm::vec3(0.7f));

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
	{ "albedo", "colour", ParamKind::Spectrum, DEFAULT_ALBEDO, 0.f, 1.f, PARAM_COLOR, nullptr, SpectrumRole::Illuminant },
	{ "strength", "strength", ParamKind::Float, 1.f, 0.f, 1000.f, PARAM_LOGARITHMIC, "The radiance is colour x strength, so a light can be far brighter than white" },
};

const MaterialParam PBR_PARAMS[] = {
	{ "albedo", "base colour", ParamKind::Spectrum, DEFAULT_ALBEDO, 0.f, 1.f, PARAM_COLOR | PARAM_TEXTURABLE, nullptr },
	{ "baseColorTexture", "base colour texture", ParamKind::Texture, TexRef {}, 0.f, 0.f, PARAM_READ_ONLY, "sRGB, multiplies the base colour; its alpha is the coverage the alpha test reads" },
	{ "metallic", "metallic", ParamKind::Float, 0.f, 0.f, 1.f, PARAM_TEXTURABLE, nullptr },
	{ "f0", "dielectric F0", ParamKind::Float, 0.04f, 0.f, 0.5f, 0,
		"The reflectance at normal incidence of the non-metal part: glTF's 0.04, or ((ior - 1) / (ior + 1))^2 times the specular factor when a glTF material sets KHR_materials_ior or KHR_materials_specular" },
	{ "roughness", "roughness", ParamKind::Float, 0.5f, 0.f, 1.f, PARAM_TEXTURABLE, "Perceptual roughness: 0 is a mirror, and GGX's alpha is its square" },
	{ "metallicRoughnessTexture", "metal/rough texture", ParamKind::Texture, TexRef {}, 0.f, 0.f, PARAM_READ_ONLY, "Green multiplies roughness, blue multiplies metallic" },
	{ "normalTexture", "normal texture", ParamKind::Texture, TexRef {}, 0.f, 0.f, PARAM_READ_ONLY, "Tangent space; applied on meshes with tangents" },
	{ "normalScale", "normal scale", ParamKind::Float, 1.f, 0.f, 2.f, 0, "Scales the normal texture's tangent-plane components" },
	{ "emission", "emission", ParamKind::Spectrum, rgbSpectrum(glm::vec3(0.f)), 0.f, 1.f, PARAM_COLOR | PARAM_TEXTURABLE, nullptr, SpectrumRole::Illuminant },
	{ "strength", "emission strength", ParamKind::Float, 1.f, 0.f, 1000.f, PARAM_LOGARITHMIC, "The emitted radiance is emission x strength" },
	{ "emissiveTexture", "emission texture", ParamKind::Texture, TexRef {}, 0.f, 0.f, PARAM_READ_ONLY, "sRGB, multiplies the emission" },
	{ "alphaCutoff", "alpha cutoff", ParamKind::Float, 0.f, 0.f, 2.f, PARAM_READ_ONLY,
		"The base colour texel's alpha below which the surface is cut away: glTF's alphaCutoff divided by the base colour factor's alpha. 0 for never" },
};

// pbrt-v4's glass and conductor (docs/plans/completed/pbrt-bsdf-library.md). Roughness is pbrt's: remapped to alpha by its
// square root unless remapRoughness is off, and 0 is a perfect specular surface
const MaterialParam GLASS_PARAMS[] = {
	{ "eta", "index of refraction", ParamKind::Spectrum, constantRef(1.5f), 1.f, 3.f, 0,
		"A constant, a named glass, a dispersion formula or a table. An index that varies with wavelength disperses in the\nspectral modes; exact RGB reads it at the design wavelength", SpectrumRole::Scalar },
	{ "uRoughness", "roughness u", ParamKind::Float, 0.f, 0.f, 1.f, 0, "Along dp/du. 0 is smooth glass" },
	{ "vRoughness", "roughness v", ParamKind::Float, 0.f, 0.f, 1.f, 0, "Along dp/dv" },
	{ "remapRoughness", "remap roughness", ParamKind::Bool, true, 0.f, 0.f, 0, "Map roughness to GGX's alpha by its square root, as pbrt does" },
};

const MaterialParam CONDUCTOR_PARAMS[] = {
	{ "eta", "eta", ParamKind::Spectrum, *namedRef("metal-Cu-eta"), 0.f, 10.f, 0, "The real part of the metal's index; pbrt's default is copper", SpectrumRole::Scalar },
	{ "k", "k", ParamKind::Spectrum, *namedRef("metal-Cu-k"), 0.f, 10.f, 0, "The imaginary part, the absorption", SpectrumRole::Scalar },
	{ "useReflectance", "from reflectance", ParamKind::Bool, false, 0.f, 0.f, 0, "Derive eta and k from a normal-incidence reflectance instead, as pbrt does" },
	{ "albedo", "reflectance", ParamKind::Spectrum, DEFAULT_ALBEDO, 0.f, 1.f, PARAM_COLOR, "The reflectance at normal incidence, when 'from reflectance' is on" },
	{ "uRoughness", "roughness u", ParamKind::Float, 0.f, 0.f, 1.f, 0, "Along dp/du. 0 is a mirror" },
	{ "vRoughness", "roughness v", ParamKind::Float, 0.f, 0.f, 1.f, 0, "Along dp/dv" },
	{ "remapRoughness", "remap roughness", ParamKind::Bool, true, 0.f, 0.f, 0, "Map roughness to GGX's alpha by its square root, as pbrt does" },
};

// pbrt-v4's diffuse: reflectance and Oren-Nayar's roughness. pbrt-v4 has no Oren-Nayar; PBR 3ed 8.4.1 does
const MaterialParam DIFFUSE_PARAMS[] = {
	{ "albedo", "reflectance", ParamKind::Spectrum, DEFAULT_ALBEDO, 0.f, 1.f, PARAM_COLOR, "Clamped to 0 to 1" },
	{ "sigma", "roughness sigma", ParamKind::Float, 0.f, 0.f, 90.f, 0,
		"Oren and Nayar's microfacet slope in degrees. 0 is a Lambertian surface; rough clay is about 20 and the moon about 30" },
};

const MaterialParam THIN_GLASS_PARAMS[] = {
	{ "eta", "index of refraction", ParamKind::Spectrum, constantRef(1.5f), 1.f, 3.f, 0,
		"A thin pane: light goes straight through, and the pane reflects what two parallel interfaces and their inter-reflections do. A constant, a named glass, a dispersion formula or a table",
		SpectrumRole::Scalar },
};

const MaterialParam DIFFUSE_TRANSMISSION_PARAMS[] = {
	{ "albedo", "reflectance", ParamKind::Spectrum, rgbSpectrum(glm::vec3(0.25f)), 0.f, 1.f, PARAM_COLOR, "Clamped to 0 to 1 after the scale" },
	{ "transmittance", "transmittance", ParamKind::Spectrum, rgbSpectrum(glm::vec3(0.25f)), 0.f, 1.f, PARAM_COLOR, "Clamped to 0 to 1 after the scale" },
	{ "scale", "scale", ParamKind::Float, 1.f, 0.f, 10.f, 0, "Multiplies both" },
};

const MaterialParam MIX_PARAMS[] = {
	{ "materialA", "first material", ParamKind::Material, MaterialLink {}, 0.f, 0.f, 0,
		"A hit takes this material with the chance 1 - amount. The material must scatter; a child's emission and alpha cutoff are not seen through the mix" },
	{ "materialB", "second material", ParamKind::Material, MaterialLink {}, 0.f, 0.f, 0, "A hit takes this one with the chance amount" },
	{ "amount", "amount", ParamKind::Float, 0.5f, 0.f, 1.f, 0,
		"The chance of the second material. The choice is a hash of the hit point and the direction, so a hit chooses the same child each time it is evaluated" },
};

//---------------------------------------------------------------- packing

// The six original types all pack the 64-byte record they had before the header existed, exactly as
// the renderer's gpuMaterial() filled it, so the shaders compute the same values bit for bit
void packClassic(const MaterialDef& material, const PackContext& context, MaterialRecordWriter& writer)
{
	CrtClassicMaterial out {};
	out.albedo = materialColor(material, "albedo", context.spectra);
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
		//the record's param is the dielectric F0, 0 for glTF's 0.04 so a record that never set it is what it was
		out.param = materialFloat(material, "f0") == 0.04f ? 0.f : std::max(materialFloat(material, "f0"), 1e-6f);
		out.metallic = std::clamp(materialFloat(material, "metallic"), 0.f, 1.f);
		out.roughness = std::clamp(materialFloat(material, "roughness"), 0.f, 1.f);
		out.emission = glm::max(materialColor(material, "emission", context.spectra), glm::vec3(0.f)) * std::max(materialFloat(material, "strength"), 0.f);
		out.normalScale = materialFloat(material, "normalScale");
		if (context.textures) {
			out.albedoLayer = materialTextureLayer(material, "baseColorTexture");
			out.normalLayer = materialTextureLayer(material, "normalTexture");
			out.metalRoughLayer = materialTextureLayer(material, "metallicRoughnessTexture");
			out.emissiveLayer = materialTextureLayer(material, "emissiveTexture");
			out.alphaCutoff = materialFloat(material, "alphaCutoff");
		}
		break;
	default:
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

	//the spectral builds' albedo and emission: the same parameters, as spectra
	PackedMaterial& packed = writer.out;
	packed.spectra[0] = std::make_pair(materialSpectrum(material, "albedo"), SpectrumRole::Albedo);
	if (material.type == MaterialType::Emissive) {
		packed.spectra[1] = std::make_pair(materialSpectrum(material, "albedo"), SpectrumRole::Illuminant);
		packed.emissionScale = out.param;
	} else if (material.type == MaterialType::Pbr) {
		packed.spectra[1] = std::make_pair(materialSpectrum(material, "emission"), SpectrumRole::Illuminant);
		packed.emissionScale = std::max(materialFloat(material, "strength"), 0.f);
	}
	if (packed.spectra[1].has_value() && packed.spectra[1]->first.kind != SpectrumKind::Rgb) {
		const SpectrumRef& emission = packed.spectra[1]->first;
		const glm::vec3 rgb = exactRgb(emission, SpectrumRole::Illuminant, context.spectra);
		if (!(0.2126f * rgb.r + 0.7152f * rgb.g + 0.0722f * rgb.b > 0.f)) {
			packed.emissionPrice = spectrumRadiometricPrice(emission) * packed.emissionScale;
			//an emitter only a spectral build sees still emits, so kernel 02 queues it for kernel 04
			if (packed.emissionPrice > 0.f) {
				flags |= CRT_MATERIAL_FLAG_EMITS;
			}
		}
	}
}

// the representative wavelengths exact RGB reads a conductor's eta and k at (docs/plans/completed/pbrt-bsdf-library.md)
constexpr double RGB_WAVELENGTHS[3] = { 630.0, 532.0, 465.0 };

float materialBool(const MaterialDef& material, std::string_view name)
{
	const ParamValue value = materialValue(material, name);
	const bool* b = std::get_if<bool>(&value);
	return b != nullptr && *b;
}

// pbrt reads an RGB eta or k as an unbounded spectrum (RGBUnboundedSpectrum), not as one number: exact RGB takes its
// channels, and the spectral builds its uplift, tabulated every 5 nm so the shaders' scalar path reads it as it reads
// a measured table (and holds its 360 and 830 nm values beyond, as every RGB value does)
glm::vec3 conductorRgb(const SpectrumRef& ref, const SpectrumContext& context)
{
	if (ref.kind == SpectrumKind::Rgb) {
		return exactRgb(ref, SpectrumRole::Unbounded, context);
	}
	glm::vec3 rgb;
	for (int c = 0; c < 3; c++) {
		rgb[c] = (float)scalarAt(ref, RGB_WAVELENGTHS[c]);
	}
	return rgb;
}

SpectrumRef conductorSpectrum(const SpectrumRef& ref, const SpectrumContext& context)
{
	if (ref.kind != SpectrumKind::Rgb || context.table == nullptr) {
		return ref;
	}
	std::vector<float> samples;
	for (int lambda = 360; lambda <= 830; lambda += 5) {
		samples.push_back((float)lambda);
		samples.push_back((float)evalSpectrumRef(ref, SpectrumRole::Unbounded, context, lambda));
	}
	return tabulatedRef(std::move(samples));
}

void packBxdf(const MaterialDef& material, const PackContext& context, MaterialRecordWriter& writer)
{
	CrtBxdfMaterial out {};
	out.type = (uint32_t)material.type;
	out.albedoLayer = -1;
	out.normalLayer = -1;
	out.metalRoughLayer = -1;
	out.emissiveLayer = -1;
	PackedMaterial& packed = writer.out;
	switch (material.type) {
	case MaterialType::Diffuse: {
		//a reflectance is 0 to 1, as pbrt clamps it
		out.rgbA = glm::clamp(materialColor(material, "albedo", context.spectra), glm::vec3(0.f), glm::vec3(1.f));
		const DiffuseBxdf bxdf = DiffuseBxdf::make(out.rgbA, std::max(materialFloat(material, "sigma"), 0.f));
		//Oren-Nayar's A and B ride in the alphas' place: a diffuse surface has no microfacets
		out.alphaX = bxdf.orenA;
		out.alphaY = bxdf.orenB;
		packed.spectra[0] = std::make_pair(materialSpectrum(material, "albedo"), SpectrumRole::Albedo);
		break;
	}
	case MaterialType::DiffuseTransmission: {
		//pbrt: both are scaled, then clamped to 0 to 1. The spectral builds scale and clamp their lanes the same way
		const float scale = std::max(materialFloat(material, "scale"), 0.f);
		out.rgbA = glm::clamp(materialColor(material, "albedo", context.spectra) * scale, glm::vec3(0.f), glm::vec3(1.f));
		out.rgbB = glm::clamp(materialColor(material, "transmittance", context.spectra) * scale, glm::vec3(0.f), glm::vec3(1.f));
		out.alphaX = scale;
		packed.spectra[0] = std::make_pair(materialSpectrum(material, "albedo"), SpectrumRole::Albedo);
		packed.spectra[2] = std::make_pair(materialSpectrum(material, "transmittance"), SpectrumRole::Albedo);
		break;
	}
	case MaterialType::Glass:
	case MaterialType::ThinGlass: {
		if (material.type == MaterialType::Glass) {
			const bool remap = materialBool(material, "remapRoughness");
			auto alpha = [&](const char* name) {
				const float roughness = std::clamp(materialFloat(material, name), 0.f, 1.f);
				return remap ? std::sqrt(roughness) : roughness;
			};
			out.alphaX = alpha("uRoughness");
			out.alphaY = alpha("vRoughness");
		}
		const SpectrumRef eta = materialSpectrum(material, "eta");
		out.etaDesign = (float)scalarAt(eta, context.designWavelength);
		if (!(out.etaDesign > 0.f)) {
			out.etaDesign = 1.f;
		}
		if (!isConstantSpectrum(eta)) {
			out.bxdfFlags |= CRT_BXDF_FLAG_DISPERSIVE;
		}
		packed.spectra[2] = std::make_pair(eta, SpectrumRole::Scalar);
		break;
	}
	default: {
		const bool remap = materialBool(material, "remapRoughness");
		auto alpha = [&](const char* name) {
			const float roughness = std::clamp(materialFloat(material, name), 0.f, 1.f);
			return remap ? std::sqrt(roughness) : roughness;
		};
		out.alphaX = alpha("uRoughness");
		out.alphaY = alpha("vRoughness");
		const SpectrumRef eta = materialSpectrum(material, "eta");
		const SpectrumRef k = materialSpectrum(material, "k");
		if (materialBool(material, "useReflectance")) {
			out.bxdfFlags |= CRT_BXDF_FLAG_REFLECTANCE;
			out.rgbB = glm::clamp(materialColor(material, "albedo", context.spectra), glm::vec3(0.f), glm::vec3(1.f));
			packed.spectra[0] = std::make_pair(materialSpectrum(material, "albedo"), SpectrumRole::Albedo);
		} else {
			out.rgbA = conductorRgb(eta, context.spectra);
			out.rgbB = conductorRgb(k, context.spectra);
		}
		packed.spectra[2] = std::make_pair(conductorSpectrum(eta, context.spectra), SpectrumRole::Scalar);
		packed.spectra[3] = std::make_pair(conductorSpectrum(k, context.spectra), SpectrumRole::Scalar);
		break;
	}
	}
	writer.write(&out, sizeof(out));
	writer.out.flags |= CRT_MATERIAL_FLAG_SCATTERS;
}

MaterialLink materialLink(const MaterialDef& material, std::string_view name)
{
	const ParamValue value = materialValue(material, name);
	const MaterialLink* link = std::get_if<MaterialLink>(&value);
	return link != nullptr ? *link : MaterialLink {};
}

void packMix(const MaterialDef& material, const PackContext& context, MaterialRecordWriter& writer)
{
	CrtMixMaterial out {};
	auto child = [&](const char* name) -> uint32_t {
		const MaterialLink link = materialLink(material, name);
		return context.materialIndex && link.id != 0 ? context.materialIndex(link.id) : 0u;
	};
	out.childA = child("materialA");
	out.childB = child("materialB");
	out.amount = std::clamp(materialFloat(material, "amount"), 0.f, 1.f);
	out.type = (uint32_t)MaterialType::Mix;
	out.albedoLayer = -1;
	out.normalLayer = -1;
	out.metalRoughLayer = -1;
	out.emissiveLayer = -1;
	writer.write(&out, sizeof(out));
	writer.out.flags |= CRT_MATERIAL_FLAG_SCATTERS;
}

// a smooth metal reads as its normal-incidence reflectance; glass as the classic dielectric's tint
glm::vec3 conductorPreview(const MaterialDef& material)
{
	if (materialBool(material, "useReflectance")) {
		return materialColor(material, "albedo");
	}
	const glm::vec3 eta = conductorRgb(materialSpectrum(material, "eta"), {});
	const glm::vec3 k = conductorRgb(materialSpectrum(material, "k"), {});
	glm::vec3 rgb;
	for (int c = 0; c < 3; c++) {
		rgb[c] = frComplex(1.f, eta[c], k[c]);
	}
	return rgb;
}

// a mix has no appearance of its own in the raster preview, which has no library to look its children up in
glm::vec3 mixPreview(const MaterialDef&)
{
	return glm::vec3(0.7f);
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
	{ "glass", "Glass (pbrt)", GLASS_PARAMS, MATERIAL_TYPE_SCATTERS | MATERIAL_TYPE_COMPLEX, packBxdf, noEmission, glassPreview },
	{ "conductor", "Conductor (pbrt)", CONDUCTOR_PARAMS, MATERIAL_TYPE_SCATTERS | MATERIAL_TYPE_COMPLEX, packBxdf, noEmission, conductorPreview },
	{ "diffuse", "Diffuse (pbrt)", DIFFUSE_PARAMS, MATERIAL_TYPE_SCATTERS | MATERIAL_TYPE_COMPLEX, packBxdf, noEmission, albedoPreview },
	{ "thin_glass", "Thin glass (pbrt)", THIN_GLASS_PARAMS, MATERIAL_TYPE_SCATTERS | MATERIAL_TYPE_COMPLEX | MATERIAL_TYPE_DELTA, packBxdf, noEmission, glassPreview },
	{ "diffuse_transmission", "Diffuse transmission (pbrt)", DIFFUSE_TRANSMISSION_PARAMS, MATERIAL_TYPE_SCATTERS | MATERIAL_TYPE_COMPLEX, packBxdf, noEmission, albedoPreview },
	{ "mix", "Mix (pbrt)", MIX_PARAMS, MATERIAL_TYPE_SCATTERS | MATERIAL_TYPE_COMPLEX, packMix, noEmission, mixPreview },
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
	material.params["albedo"] = rgbSpectrum(albedo);
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

SpectrumRole materialSpectrumRole(MaterialType type, std::string_view name)
{
	const MaterialParam* param = findMaterialParam(type, name);
	return param != nullptr ? param->role : SpectrumRole::Albedo;
}

SpectrumRef materialSpectrum(const MaterialDef& material, std::string_view name)
{
	const ParamValue value = materialValue(material, name);
	const SpectrumRef* s = std::get_if<SpectrumRef>(&value);
	return s != nullptr ? *s : rgbSpectrum(glm::vec3(0.f));
}

glm::vec3 materialColor(const MaterialDef& material, std::string_view name, const SpectrumContext& context)
{
	const ParamValue value = materialValue(material, name);
	const SpectrumRef* s = std::get_if<SpectrumRef>(&value);
	if (s == nullptr) {
		return glm::vec3(0.f);
	}
	//an RGB value in an sRGB scene is the authored triple, bit for bit, as it always was
	if (s->kind == SpectrumKind::Rgb && context.space == ColorSpaceId::Srgb) {
		return s->rgb;
	}
	return exactRgb(*s, materialSpectrumRole(material.type, name), context);
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
