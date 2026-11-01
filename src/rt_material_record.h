#pragma once

// The material records the GPU reads, as plain C++ with no Vulkan: binding 7's header and the parameter record of
// the six original types in binding 26, mirroring crt_common.glsl and crt_material.glsl. rt_material.cpp packs
// them and rt_gpu_types.h, which holds the renderer's other GPU mirrors, includes them.

#include <cstdint>

#include <glm/glm.hpp>

// binding 7, one per material: what kind it is, what kernel 02 and the traversal need to know of it
// without reading its parameters, and where its parameters are in binding 26 (crt_common.glsl
// GpuMaterial)
struct CrtMaterialHeader {
	uint32_t type;
	// CRT_MATERIAL_FLAG_*
	uint32_t flags;
	// into the parameter buffer, in vec4s
	uint32_t paramOffset;
	uint32_t paramCount;
};
static_assert(sizeof(CrtMaterialHeader) == 16);

// crt_common.glsl CRT_MATERIAL_FLAG_*: what a material does, decided on the CPU when it is packed
inline constexpr uint32_t CRT_MATERIAL_FLAG_EMITS = 1u << 0;
inline constexpr uint32_t CRT_MATERIAL_FLAG_SCATTERS = 1u << 1;
// evaluated by the complex build of kernel 06, over its own surface queue
inline constexpr uint32_t CRT_MATERIAL_FLAG_COMPLEX = 1u << 2;
// the traversal alpha-tests a flagged triangle against this material
inline constexpr uint32_t CRT_MATERIAL_FLAG_CUTOUT = 1u << 3;
inline constexpr uint32_t CRT_MATERIAL_FLAG_NORMAL_MAP = 1u << 4;
// the record's last vec4 is its extension header (CrtMaterialExtension): the emission block and the texture table
inline constexpr uint32_t CRT_MATERIAL_FLAG_EXTENDED = 1u << 5;
// it emits only on the side its geometric normal faces, and with REVERSED, only on the back
inline constexpr uint32_t CRT_MATERIAL_FLAG_ONE_SIDED = 1u << 6;
inline constexpr uint32_t CRT_MATERIAL_FLAG_REVERSED = 1u << 7;
// the traversal's alpha test reads the library texture targeting the coverage (TextureTarget::Alpha)
inline constexpr uint32_t CRT_MATERIAL_FLAG_ALPHA_TEXTURE = 1u << 8;

// The extension header, the last vec4 of a record flagged CRT_MATERIAL_FLAG_EXTENDED: where the emission block and the
// texture table are, in vec4s from the record's start (0 for none), and how many entries the table has
struct CrtMaterialExtension {
	uint32_t emissionOffset;
	uint32_t textureOffset;
	uint32_t textureCount;
	uint32_t unused;
};
static_assert(sizeof(CrtMaterialExtension) == 16);

// The emission block of a type that has no emission of its own (all but emissive and pbr): its radiance before any
// texture, color x strength, and the strength alone
struct CrtEmissionBlock {
	glm::vec3 radiance;
	float strength;
};
static_assert(sizeof(CrtEmissionBlock) == 16);

// what a texture reference modulates (crt_texture.glsl CRT_TEX_TARGET_*)
inline constexpr uint32_t CRT_TEX_TARGET_NONE = 0;
inline constexpr uint32_t CRT_TEX_TARGET_ALBEDO = 1;
inline constexpr uint32_t CRT_TEX_TARGET_PARAM = 2;
inline constexpr uint32_t CRT_TEX_TARGET_METALLIC = 3;
inline constexpr uint32_t CRT_TEX_TARGET_ROUGHNESS = 4;
inline constexpr uint32_t CRT_TEX_TARGET_EMISSION = 5;
inline constexpr uint32_t CRT_TEX_TARGET_NORMAL_SCALE = 6;
inline constexpr uint32_t CRT_TEX_TARGET_RGB_A = 7;
inline constexpr uint32_t CRT_TEX_TARGET_RGB_B = 8;
inline constexpr uint32_t CRT_TEX_TARGET_ETA = 9;
inline constexpr uint32_t CRT_TEX_TARGET_ALPHA_X = 10;
inline constexpr uint32_t CRT_TEX_TARGET_ALPHA_Y = 11;
inline constexpr uint32_t CRT_TEX_TARGET_SIGMA = 12;
inline constexpr uint32_t CRT_TEX_TARGET_SCALE = 13;
inline constexpr uint32_t CRT_TEX_TARGET_AMOUNT = 14;
inline constexpr uint32_t CRT_TEX_TARGET_TIS = 15;
inline constexpr uint32_t CRT_TEX_TARGET_EMIT_COLOR = 16;
inline constexpr uint32_t CRT_TEX_TARGET_EMIT_STRENGTH = 17;
inline constexpr uint32_t CRT_TEX_TARGET_NORMAL_MAP = 18;
inline constexpr uint32_t CRT_TEX_TARGET_ALPHA = 19;
// a conductor's reflectance in its reflectance mode
inline constexpr uint32_t CRT_TEX_TARGET_REFLECTANCE = 20;
// how a value is applied (crt_texture.glsl CRT_TEX_OP_*)
inline constexpr uint32_t CRT_TEX_OP_MULTIPLY = 0;
inline constexpr uint32_t CRT_TEX_OP_MULTIPLY_SQRT = 1;
inline constexpr uint32_t CRT_TEXTURE_ENTRY_VEC4S = 6;

// One texture reference of the table (crt_texture.glsl), six vec4s: what it modulates (TextureTarget) | channel << 8 |
// mapping << 12 | op << 16 | uv set << 20 | world space << 24; the library descriptor and sampler; the parameter's own value, for a target
// derived on the GPU; the uv offset and scale; the rotation's cosine and sine; the object-to-texture rows of a mapping
struct CrtTextureEntry {
	uint32_t bits;
	uint32_t texture;
	uint32_t sampler;
	float base;
	glm::vec4 offsetScale;
	glm::vec4 rotation;
	glm::vec4 mapping[3];
};
static_assert(sizeof(CrtTextureEntry) == 96);

// The parameter record of the six original types (lambertian, metal, phong, dielectric, emissive,
// pbr), four vec4s in binding 26: the 64-byte material every one of them packed before the header
// existed, field for field, so the shaders read the same floats (crt_material.glsl ClassicMaterial)
struct CrtClassicMaterial {
	// the base colour or emitted colour, by type
	glm::vec3 albedo;
	// fuzz | smoothness | ir | strength, by type; unused by pbr
	float param;
	uint32_t type;
	// the legacy texture array's layers (the pbr type's texture parameters), -1 for none. Always -1 for a material packed
	// for a shape, or under native texture storage: the texture library's references are the record's texture table
	// (docs/plans/completed/pbrt-textures.md)
	int32_t albedoLayer;
	// the traversal's alpha test threshold on the base colour texel, 0 for never cut out
	float alphaCutoff;
	float metallic;
	// pbr: emission x strength, linear
	glm::vec3 emission;
	float roughness;
	int32_t normalLayer;
	int32_t metalRoughLayer;
	int32_t emissiveLayer;
	float normalScale;
};
static_assert(sizeof(CrtClassicMaterial) == 64);

// CrtBxdfMaterial::bxdfFlags
// the conductor reads its reflectance (the albedo slot) instead of eta and k
inline constexpr uint32_t CRT_BXDF_FLAG_REFLECTANCE = 1u << 0;
// the glass's index varies with wavelength: a spectral build terminates the path's secondary wavelengths at it
inline constexpr uint32_t CRT_BXDF_FLAG_DISPERSIVE = 1u << 1;

// The parameter record of the glass, conductor, diffuse, thin glass and diffuse transmission types (crt_scatter.glsl
// resolveBxdfMaterial()), four vec4s. A diffuse type's rgbA is its reflectance, a diffuse transmission's rgbB its
// transmittance, a diffuse's alphaX and alphaY Oren-Nayar's A and B, and a thin glass's etaDesign its index. The
// fields a ClassicMaterial reader looks at (type, the texture layers, the alpha cutoff) sit where that record keeps
// them, holding "none", so code that reads any material as classic (the traversal's alpha test, the normal map) finds
// nothing to do. Exact RGB reads the RGB fields; a spectral build reads the spectra in the material's slots
struct CrtBxdfMaterial {
	// conductor: eta at 630, 532 and 465 nm
	glm::vec3 rgbA;
	// glass: the index at the design wavelength, which exact RGB uses and a constant index is everywhere
	float etaDesign;
	uint32_t type;
	int32_t albedoLayer;
	float alphaCutoff;
	// the distribution's alphas, along dp/du and dp/dv
	float alphaX;
	// conductor: k at 630, 532 and 465 nm, or the reflectance
	glm::vec3 rgbB;
	float alphaY;
	int32_t normalLayer;
	int32_t metalRoughLayer;
	int32_t emissiveLayer;
	// CRT_BXDF_FLAG_*
	uint32_t bxdfFlags;
};
static_assert(sizeof(CrtBxdfMaterial) == 64);

// The parameter record of the mix type (crt_scatter.glsl resolveMixMaterial()), four vec4s laid out as a ClassicMaterial
// reader finds them: a type, no textures and no alpha test, so the traversal and the normal map pass it by
struct CrtMixMaterial {
	// the two children, as indices into the material table
	uint32_t childA;
	uint32_t childB;
	float unusedAlbedoZ;
	// the chance of the second child
	float amount;
	uint32_t type;
	int32_t albedoLayer;
	float alphaCutoff;
	float unusedMetallic;
	float unusedEmission[3];
	float unusedRoughness;
	int32_t normalLayer;
	int32_t metalRoughLayer;
	int32_t emissiveLayer;
	float unusedNormalScale;
};
static_assert(sizeof(CrtMixMaterial) == 64);

// CrtOpticMaterial::finish
inline constexpr uint32_t CRT_OPTIC_FINISH_POLISHED = 0u;
inline constexpr uint32_t CRT_OPTIC_FINISH_GROUND = 1u;

// The parameter record of the optical interface (crt_optic_material.glsl), four vec4s laid out as a ClassicMaterial reader
// finds them: no textures and no alpha test. The coating and the two media are not here: they are a property of the
// surface, in its body's record and the instance's coating table (docs/plans/completed/physoptics-optical-materials.md)
struct CrtOpticMaterial {
	uint32_t finish;
	// the total integrated scatter of the ABg lobe (0 for none), and the lobe's B and g
	float tis;
	float abgB;
	float abgG;
	uint32_t type;
	int32_t albedoLayer;
	float alphaCutoff;
	float unusedMetallic;
	float unusedEmission[3];
	float unusedRoughness;
	int32_t normalLayer;
	int32_t metalRoughLayer;
	int32_t emissiveLayer;
	float unusedNormalScale;
};
static_assert(sizeof(CrtOpticMaterial) == 64);

// After a scattering interface's record (tis above 0): the ABg lobe's normalization A, so that A / (B + r^g) integrates to
// 1 over the disk |beta - beta0| < 2 of projected directions; the lobe's inverse cumulative distribution in r at 32
// evenly spaced probabilities, which crt_optic_material.glsl samples the lobe's radius from; and the share of the lobe
// inside the unit disk, the directions that exist, at 16 values of |beta0| from 0 to 1, which renormalizes it
inline constexpr uint32_t CRT_ABG_TABLE = 32;
inline constexpr uint32_t CRT_ABG_VISIBLE = 16;
struct CrtAbgLobe {
	float a;
	float rMax;
	float unused[2];
	float radius[CRT_ABG_TABLE];
	float visible[CRT_ABG_VISIBLE];
};
static_assert(sizeof(CrtAbgLobe) == 208);
