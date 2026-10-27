// THE MATERIAL RECORDS: each type's parameters, read back from binding 26 in the order its pack
// function in src/rt_material.cpp wrote them. The header in binding 7 (crt_common.glsl GpuMaterial)
// names the type and where its record starts.
//
// The six original types (lambertian, metal, phong, dielectric, emissive, pbr) share one record,
// ClassicMaterial: the 64 bytes every material was before the header existed, field for field, so
// crt_scatter.glsl and the kernels compute exactly what they did. A new type adds its own record
// struct and reader here, beside its pack function (docs/plans/completed/pbrt-bsdf-library.md).
//
// Included by crt_traverse.glsl, crt_light.glsl and crt_scatter.glsl, and by any kernel that reads
// a material. Requires crt_common.glsl, and crt_spectrum.glsl in a spectral build.

#ifndef CRT_MATERIAL_GLSL
#define CRT_MATERIAL_GLSL

// 64 bytes, four vec4s. src/rt_gpu_types.h CrtClassicMaterial
struct ClassicMaterial {
	// the base colour or the emitted colour, by type
	vec3 albedo;
	// fuzz | smoothness | ir | strength, by type; unused by pbr
	float param;
	uint type;
	// layers of materialTextures, or -1 for none. Always -1 on a shape. albedo: rgb base colour
	// (sRGB), a coverage
	int albedoLayer;
	// the base colour texel's alpha below which the traversal cuts the surface away; 0 for never
	float alphaCutoff;
	float metallic;
	// pbr: emitted radiance (colour x strength), linear. Zero for a surface that does not glow
	vec3 emission;
	float roughness;
	// tangent-space normal, linear
	int normalLayer;
	// g roughness, b metallic, linear
	int metalRoughLayer;
	// emitted colour, sRGB
	int emissiveLayer;
	float normalScale;
#ifdef CRT_SPECTRAL
	// the albedo at the path's lanes, as resolveMaterial() evaluates it (crt_scatter.glsl): the material's albedo
	// reference times its base color texel. The RGB fields above stay what the record holds
	Spectrum color;
#endif
};

// one of the six original types' records
ClassicMaterial classicMaterial(uint index)
{
	const uint base = materials[index].paramOffset;
	const vec4 a = materialParams[base];
	const vec4 b = materialParams[base + 1u];
	const vec4 c = materialParams[base + 2u];
	const vec4 d = materialParams[base + 3u];
	ClassicMaterial m;
	m.albedo = a.xyz;
	m.param = a.w;
	m.type = floatBitsToUint(b.x);
	m.albedoLayer = floatBitsToInt(b.y);
	m.alphaCutoff = b.z;
	m.metallic = b.w;
	m.emission = c.xyz;
	m.roughness = c.w;
	m.normalLayer = floatBitsToInt(d.x);
	m.metalRoughLayer = floatBitsToInt(d.y);
	m.emissiveLayer = floatBitsToInt(d.z);
	m.normalScale = d.w;
#ifdef CRT_SPECTRAL
	m.color = spectrumConst(0.0);
#endif
	return m;
}

#endif
