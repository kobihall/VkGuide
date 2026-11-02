// WHAT A SURFACE EMITS towards a ray that found it: kernel 04's, and the light vertex cache's stage 15
// (docs/plans/completed/pbrt-bidirectional-path-tracing.md). Moved out of 0401_handle_emissive.comp unchanged.
// Requires crt_common.glsl, crt_spectrum.glsl, crt_interaction.glsl and crt_material.glsl.

#ifndef CRT_EMITTER_GLSL
#define CRT_EMITTER_GLSL

// what the surface emits towards the ray: an emissive material's colour times its strength, a pbr
// material's emission times its sRGB emissive texel, any other type's emission block (an area light on a material that
// scatters, docs/plans/completed/pbrt-light-types.md), and the texture library's emission textures over all three
vec3 emittedRadiance(ClassicMaterial light, uint materialIndex, SurfaceInteraction si, TextureSite site)
{
	vec3 emitted = light.albedo * light.param;
	if (light.type == CRT_MATERIAL_PBR) {
		emitted = light.emission;
		if (light.emissiveLayer >= 0) {
			emitted *= srgbToLinear(textureLod(materialTextures, vec3(si.uv, float(light.emissiveLayer)), 0.0).rgb);
		}
	} else if (light.type != CRT_MATERIAL_EMISSIVE) {
		emitted = materialEmissionBlock(materialIndex).xyz;
	}
	if (CRT_TEXTURES) {
		emitted *= materialEmissionTexture(materialIndex, site);
	}
	return emitted;
}

// the same as the path's spectrum: the material's emission reference (slot 1 of its spectral record, the emissive
// color or the pbr emission, in the Illuminant role) times the strength, with the emissive texel as a tint
Spectrum emittedSpectrum(ClassicMaterial light, uint materialIndex, SurfaceInteraction si, TextureSite site)
{
#ifndef CRT_SPECTRAL
	return emittedRadiance(light, materialIndex, si, site);
#else
	vec3 tint = vec3(1.0);
	if (light.type == CRT_MATERIAL_PBR && light.emissiveLayer >= 0) {
		tint = srgbToWorking(srgbToLinear(textureLod(materialTextures, vec3(si.uv, float(light.emissiveLayer)), 0.0).rgb));
	}
	if (CRT_TEXTURES) {
		tint *= srgbToWorking(materialEmissionTexture(materialIndex, site));
	}
	return spectrumScale(spectrumFromRef(materialSpectrumRef(materialIndex, CRT_SLOT_EMISSION), tint), materialEmissionScale(materialIndex));
#endif
}

// a one-sided emitter (CRT_MATERIAL_FLAG_ONE_SIDED) is seen only from the side it emits on: the front, or with
// Reverse orientation the back
bool emitsTowardsRay(uint materialIndex, SurfaceInteraction si)
{
	const uint flags = materials[materialIndex].flags;
	if ((flags & CRT_MATERIAL_FLAG_ONE_SIDED) == 0u) {
		return true;
	}
	return si.frontFace != ((flags & CRT_MATERIAL_FLAG_REVERSED) != 0u);
}

float luminanceOf(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

#endif
