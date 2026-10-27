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

// The parameter record of the six original types (lambertian, metal, phong, dielectric, emissive,
// pbr), four vec4s in binding 26: the 64-byte material every one of them packed before the header
// existed, field for field, so the shaders read the same floats (crt_material.glsl ClassicMaterial)
struct CrtClassicMaterial {
	// the base colour or emitted colour, by type
	glm::vec3 albedo;
	// fuzz | smoothness | ir | strength, by type; unused by pbr
	float param;
	uint32_t type;
	// the texture layers (the pbr type's texture parameters), -1 for none. Always -1 for a material
	// packed for a shape, which carries no textures until docs/plans/pbrt-textures.md
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
