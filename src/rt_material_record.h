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
