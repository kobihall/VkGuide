// TEXTURES (docs/plans/completed/pbrt-textures.md): the texture library's images, each at its own size with its own mip chain and
// sampler, and the texture references material parameters read them through.
//
// Set 1 is the library (src/texture_library.h), bound to every kernel that declares CrtBinding::Textures: binding 0 the
// distinct samplers, twice each (its own filters, then nearest), binding 27 the images, written once when the library
// changes. A descriptor array indexed per invocation needs nonuniformEXT; MoltenVK runs it through a Metal argument
// buffer.
//
// A material's references are a table in its parameter record (src/rt_material.cpp packTextureTable()): one entry of
// CRT_TEXTURE_ENTRY_VEC4S vec4s per textured parameter, naming what it modulates (CRT_TEX_TARGET_*), the image and
// sampler, the channel a scalar reads, the mapping and the uv transform. The record's last vec4 is its extension
// header when the material's flags hold CRT_MATERIAL_FLAG_EXTENDED: x the emission block's offset, y the table's, z
// the entry count, each from paramOffset. Everything here runs only under CRT_TEXTURES, so a scene that textures
// nothing through the library runs the code it always did.
//
// Requires crt_common.glsl.

#ifndef CRT_TEXTURE_GLSL
#define CRT_TEXTURE_GLSL

#extension GL_EXT_nonuniform_qualifier : require

// src/texture_library.h MAX_SAMPLERS x 2
#define CRT_MAX_TEXTURE_SAMPLERS 128u
layout (set = 1, binding = 0) uniform sampler textureSamplers[CRT_MAX_TEXTURE_SAMPLERS];
layout (set = 1, binding = 27) uniform texture2D textures[];

// the scene textures something through the library (GpuRenderer::pipelineFeatures()). Off, every function below folds
// to its no-texture answer
layout (constant_id = 8) const bool CRT_TEXTURES = false;

// src/rt_material.h TextureTarget: what an entry modulates. The classic record's fields, a BxDF record's, the mix's,
// the optical interface's, the emission block's, and the two any material has
#define CRT_TEX_TARGET_ALBEDO 1u
#define CRT_TEX_TARGET_PARAM 2u
#define CRT_TEX_TARGET_METALLIC 3u
#define CRT_TEX_TARGET_ROUGHNESS 4u
#define CRT_TEX_TARGET_EMISSION 5u
#define CRT_TEX_TARGET_NORMAL_SCALE 6u
#define CRT_TEX_TARGET_RGB_A 7u
#define CRT_TEX_TARGET_RGB_B 8u
#define CRT_TEX_TARGET_ETA 9u
#define CRT_TEX_TARGET_ALPHA_X 10u
#define CRT_TEX_TARGET_ALPHA_Y 11u
#define CRT_TEX_TARGET_SIGMA 12u
#define CRT_TEX_TARGET_SCALE 13u
#define CRT_TEX_TARGET_AMOUNT 14u
#define CRT_TEX_TARGET_TIS 15u
#define CRT_TEX_TARGET_EMIT_COLOR 16u
#define CRT_TEX_TARGET_EMIT_STRENGTH 17u
#define CRT_TEX_TARGET_NORMAL_MAP 18u
#define CRT_TEX_TARGET_ALPHA 19u
#define CRT_TEX_TARGET_REFLECTANCE 20u

// src/rt_material.h TextureChannel
#define CRT_TEX_CHANNEL_RGB 0u
#define CRT_TEX_CHANNEL_RED 1u
#define CRT_TEX_CHANNEL_GREEN 2u
#define CRT_TEX_CHANNEL_BLUE 3u
#define CRT_TEX_CHANNEL_ALPHA 4u
#define CRT_TEX_CHANNEL_AVERAGE 5u

// src/rt_material.h TextureMapping
#define CRT_TEX_MAP_UV 0u
#define CRT_TEX_MAP_SPHERICAL 1u
#define CRT_TEX_MAP_CYLINDRICAL 2u
#define CRT_TEX_MAP_PLANAR 3u

// how a value is applied: multiplied in, multiplied by its square root (a roughness that is remapped to alpha by its
// root), or (for the alpha test) compared
#define CRT_TEX_OP_MULTIPLY 0u
#define CRT_TEX_OP_MULTIPLY_SQRT 1u

// src/rt_gpu_types.h CRT_TEXTURE_FILTER_*: CrtParams.flags bits 7 and 8
#define CRT_TEXTURE_FILTER_SHIFT 7u
#define CRT_TEXTURE_FILTER_POINT 0u
#define CRT_TEXTURE_FILTER_BILINEAR 1u
#define CRT_TEXTURE_FILTER_TRILINEAR 2u
#define CRT_TEXTURE_FILTER_ANISOTROPIC 3u

#define CRT_TEXTURE_ENTRY_VEC4S 6u
#define CRT_TEXTURE_PI 3.14159265358979323846

// where a surface is, for a texture lookup: its uv sets, its object-space point (the mappings other than uv), its
// parametric derivatives, and the ray cone's footprint there as two world-space vectors across it (zero for none)
struct TextureSite {
	vec2 uv;
	vec2 uv1;
	vec3 objectPoint;
	vec3 worldPoint;
	vec3 dpdu;
	vec3 dpdv;
	vec3 footprintA;
	vec3 footprintB;
};

TextureSite textureSiteAt(vec2 uv)
{
	TextureSite site;
	site.uv = uv;
	site.uv1 = uv;
	site.objectPoint = vec3(0.0);
	site.worldPoint = vec3(0.0);
	site.dpdu = vec3(0.0);
	site.dpdv = vec3(0.0);
	site.footprintA = vec3(0.0);
	site.footprintB = vec3(0.0);
	return site;
}

uint textureFilterMode()
{
	return (pc.flags >> CRT_TEXTURE_FILTER_SHIFT) & 3u;
}

// the extension header of material `index`, or zeros
uvec4 materialExtension(uint index)
{
	if ((materials[index].flags & CRT_MATERIAL_FLAG_EXTENDED) == 0u) {
		return uvec4(0u);
	}
	return floatBitsToUint(materialParams[materials[index].paramOffset + materials[index].paramCount - 1u]);
}

// pbrt's mappings (PBR 4ed 10.2.1): object space through the entry's transform into texture space, then onto (u, v)
vec2 mappedCoordinates(uint mapping, vec3 p)
{
	if (mapping == CRT_TEX_MAP_SPHERICAL) {
		const vec3 d = normalize(p);
		const float theta = acos(clamp(d.z, -1.0, 1.0));
		float phi = atan(d.y, d.x);
		if (phi < 0.0) {
			phi += 2.0 * CRT_TEXTURE_PI;
		}
		return vec2(theta / CRT_TEXTURE_PI, phi / (2.0 * CRT_TEXTURE_PI));
	}
	if (mapping == CRT_TEX_MAP_CYLINDRICAL) {
		float phi = atan(p.y, p.x);
		if (phi < 0.0) {
			phi += 2.0 * CRT_TEXTURE_PI;
		}
		return vec2(phi / (2.0 * CRT_TEXTURE_PI), p.z);
	}
	return p.xy;
}

// an entry's (u, v) at a point, before its offset, scale and rotation
vec2 entryCoordinates(uint base, uint mapping, uint uvSet, TextureSite site, vec3 objectOffset)
{
	if (mapping == CRT_TEX_MAP_UV) {
		return uvSet == 1u ? site.uv1 : site.uv;
	}
	const vec4 r0 = materialParams[base + 3u];
	const vec4 r1 = materialParams[base + 4u];
	const vec4 r2 = materialParams[base + 5u];
	// bit 24: the transform takes the world point (a pbrt texture's mapping) rather than the object-space one
	const bool world = ((floatBitsToUint(materialParams[base].x) >> 24u) & 1u) != 0u;
	const vec4 p = vec4((world ? site.worldPoint : site.objectPoint) + objectOffset, 1.0);
	return mappedCoordinates(mapping, vec3(dot(r0, p), dot(r1, p), dot(r2, p)));
}

// KHR_texture_transform: scale, then rotate, then offset (glTF's order, T * R * S)
vec2 transformUv(vec2 uv, vec4 offsetScale, vec2 rotation)
{
	const vec2 scaled = uv * offsetScale.zw;
	return vec2(rotation.x * scaled.x + rotation.y * scaled.y, -rotation.y * scaled.x + rotation.x * scaled.y) + offsetScale.xy;
}

// The texel of entry `base` (its first vec4 in materialParams) at `site`, filtered as Texture filtering says: point and
// bilinear at the finest level, trilinear at the level the ray cone's footprint gives, anisotropic along the
// footprint's two axes
vec4 sampleTextureEntry(uint base, TextureSite site)
{
	const uvec4 e0 = floatBitsToUint(materialParams[base]);
	const vec4 offsetScale = materialParams[base + 1u];
	const vec2 rotation = materialParams[base + 2u].xy;
	const uint mapping = (e0.x >> 12u) & 15u;
	const uint uvSet = (e0.x >> 20u) & 15u;
	const uint texture = e0.y;
	const uint filterMode = textureFilterMode();
	const uint samplerIndex = 2u * e0.z + (filterMode == CRT_TEXTURE_FILTER_POINT ? 1u : 0u);
	const vec2 uv = transformUv(entryCoordinates(base, mapping, uvSet, site, vec3(0.0)), offsetScale, rotation);
	if (!CRT_RAY_CONES || filterMode < CRT_TEXTURE_FILTER_TRILINEAR || dot(site.footprintA, site.footprintA) == 0.0) {
		return textureLod(sampler2D(textures[nonuniformEXT(texture)], textureSamplers[nonuniformEXT(samplerIndex)]), uv, 0.0);
	}
	// the footprint's axes in (u, v): through the inverse of the uv-to-world Jacobian for the uv mapping (least squares
	// on [dp/du dp/dv]), by finite differences of the mapping for the others
	vec2 du;
	vec2 dv;
	if (mapping == CRT_TEX_MAP_UV) {
		const float a = dot(site.dpdu, site.dpdu);
		const float b = dot(site.dpdu, site.dpdv);
		const float c = dot(site.dpdv, site.dpdv);
		const float det = a * c - b * b;
		if (!(abs(det) > 1e-30)) {
			return textureLod(sampler2D(textures[nonuniformEXT(texture)], textureSamplers[nonuniformEXT(samplerIndex)]), uv, 0.0);
		}
		const float invDet = 1.0 / det;
		const vec2 pa = vec2(dot(site.dpdu, site.footprintA), dot(site.dpdv, site.footprintA));
		const vec2 pb = vec2(dot(site.dpdu, site.footprintB), dot(site.dpdv, site.footprintB));
		du = invDet * vec2(c * pa.x - b * pa.y, -b * pa.x + a * pa.y);
		dv = invDet * vec2(c * pb.x - b * pb.y, -b * pb.x + a * pb.y);
		const mat2 scaleRotate = mat2(rotation.x * offsetScale.z, -rotation.y * offsetScale.z, rotation.y * offsetScale.w, rotation.x * offsetScale.w);
		du = scaleRotate * du;
		dv = scaleRotate * dv;
	} else {
		du = transformUv(entryCoordinates(base, mapping, uvSet, site, site.footprintA), offsetScale, rotation) - uv;
		dv = transformUv(entryCoordinates(base, mapping, uvSet, site, site.footprintB), offsetScale, rotation) - uv;
	}
	if (filterMode == CRT_TEXTURE_FILTER_ANISOTROPIC) {
		return textureGrad(sampler2D(textures[nonuniformEXT(texture)], textureSamplers[nonuniformEXT(samplerIndex)]), uv, du, dv);
	}
	// Akenine-Moller et al. 2021: the level whose texels match the footprint's larger axis
	const vec2 size = vec2(textureSize(sampler2D(textures[nonuniformEXT(texture)], textureSamplers[nonuniformEXT(samplerIndex)]), 0));
	const float extent = max(length(du * size), length(dv * size));
	return textureLod(sampler2D(textures[nonuniformEXT(texture)], textureSamplers[nonuniformEXT(samplerIndex)]), uv, log2(max(extent, 1e-8)));
}

// one channel of a texel, for a scalar parameter
float textureChannelValue(vec4 texel, uint channel)
{
	switch (channel) {
	case CRT_TEX_CHANNEL_RED:
		return texel.r;
	case CRT_TEX_CHANNEL_GREEN:
		return texel.g;
	case CRT_TEX_CHANNEL_BLUE:
		return texel.b;
	case CRT_TEX_CHANNEL_ALPHA:
		return texel.a;
	}
	return (texel.r + texel.g + texel.b) / 3.0;
}

// The product of every entry of material `index` that targets `target`, as a color (a scalar target's channel in all
// three) and the scalar the entry's op makes of it. 1 where none does; `found` says whether one did
vec3 materialTextureColor(uint index, uint target, TextureSite site, out bool found)
{
	found = false;
	vec3 value = vec3(1.0);
	if (!CRT_TEXTURES) {
		return value;
	}
	const uvec4 extension = materialExtension(index);
	const uint base = materials[index].paramOffset + extension.y;
	for (uint i = 0u; i < extension.z; i++) {
		const uint entry = base + i * CRT_TEXTURE_ENTRY_VEC4S;
		const uint bits = floatBitsToUint(materialParams[entry].x);
		if ((bits & 255u) != target) {
			continue;
		}
		found = true;
		const vec4 texel = sampleTextureEntry(entry, site);
		const uint channel = (bits >> 8u) & 15u;
		const uint op = (bits >> 16u) & 15u;
		vec3 v = channel == CRT_TEX_CHANNEL_RGB ? texel.rgb : vec3(textureChannelValue(texel, channel));
		if (op == CRT_TEX_OP_MULTIPLY_SQRT) {
			v = sqrt(max(v, vec3(0.0)));
		}
		value *= v;
	}
	return value;
}

float materialTextureScalar(uint index, uint target, TextureSite site, out bool found)
{
	return materialTextureColor(index, target, site, found).x;
}

// the base value an entry carries for a target whose value is derived on the GPU (Oren-Nayar's sigma)
float materialTextureBase(uint index, uint target)
{
	if (!CRT_TEXTURES) {
		return 0.0;
	}
	const uvec4 extension = materialExtension(index);
	const uint base = materials[index].paramOffset + extension.y;
	for (uint i = 0u; i < extension.z; i++) {
		const uint entry = base + i * CRT_TEXTURE_ENTRY_VEC4S;
		if ((floatBitsToUint(materialParams[entry].x) & 255u) == target) {
			return materialParams[entry].w;
		}
	}
	return 0.0;
}

// What material `index` emits at `site` through its library textures, as a multiple of its untextured emission: an
// emissive's albedo and strength, a pbr's emission and strength, any other type's emission block's color and strength
vec3 materialEmissionTexture(uint index, TextureSite site)
{
	if (!CRT_TEXTURES) {
		return vec3(1.0);
	}
	bool found;
	const uint type = materials[index].type;
	if (type == CRT_MATERIAL_EMISSIVE) {
		return materialTextureColor(index, CRT_TEX_TARGET_ALBEDO, site, found) * materialTextureScalar(index, CRT_TEX_TARGET_PARAM, site, found);
	}
	if (type == CRT_MATERIAL_PBR) {
		return materialTextureColor(index, CRT_TEX_TARGET_EMISSION, site, found);
	}
	return materialTextureColor(index, CRT_TEX_TARGET_EMIT_COLOR, site, found) * materialTextureScalar(index, CRT_TEX_TARGET_EMIT_STRENGTH, site, found);
}

// the emission block of a type without emission of its own (src/rt_material_record.h CrtEmissionBlock): its radiance,
// color x strength, in xyz and the strength in w; zero for a material without one
vec4 materialEmissionBlock(uint index)
{
	const uvec4 extension = materialExtension(index);
	return extension.x != 0u ? materialParams[materials[index].paramOffset + extension.x] : vec4(0.0);
}

#endif
