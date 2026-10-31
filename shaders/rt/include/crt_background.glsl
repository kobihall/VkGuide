// What a ray that leaves the scene sees. Used by kernel 03 Handle Escaped - once a path misses it is
// finished. Kernel 06's light samples read the environment map too, through crt_light.glsl, but only
// when the map is the background: a solid colour and the sky gradient are not in the light list.
//
// Three sources in priority order, chosen by the push-constant flags: a solid colour, the
// scene's equirectangular environment map, or the procedural sky gradient of Ray Tracing in One
// Weekend.
//
// Requires crt_common.glsl, crt_spectrum.glsl, equirect.glsl and sky_gradient.glsl.

vec3 backgroundRadiance(vec3 direction)
{
	if ((pc.flags & CRT_FLAG_SOLID_BACKGROUND) != 0u) {
		return pc.background.rgb;
	}
	if ((pc.flags & CRT_FLAG_ENVIRONMENT_MAP) != 0u) {
		// textureLod: compute has no derivatives to pick a level with, and the map has no mips
		return textureLod(environmentMap, equirectUv(environmentToMap(direction, environmentRotation())), 0.0).rgb * pc.environmentIntensity;
	}
	return skyGradient(direction);
}

// the background as the path's spectrum: in a spectral build the solid color is the scene's packed reference, and a
// texel of the map or the sky's color, linear sRGB, is uplifted as an emitter in the working space (pbrt's
// RGBIlluminantSpectrum), so a white background lights a white surface white
Spectrum backgroundSpectrum(vec3 direction)
{
#ifndef CRT_SPECTRAL
	return backgroundRadiance(direction);
#else
	if ((pc.flags & CRT_FLAG_SOLID_BACKGROUND) != 0u) {
		return spectrumFromRef(spectralWords[CRT_SH_BACKGROUND], vec3(1.0));
	}
	if ((pc.flags & CRT_FLAG_ENVIRONMENT_MAP) != 0u) {
		const vec3 texel = textureLod(environmentMap, equirectUv(environmentToMap(direction, environmentRotation())), 0.0).rgb;
		return spectrumScale(spectrumFromRgb(srgbToWorking(texel), CRT_ROLE_ILLUMINANT), pc.environmentIntensity);
	}
	return spectrumFromRgb(srgbToWorking(skyGradient(direction)), CRT_ROLE_ILLUMINANT);
#endif
}
