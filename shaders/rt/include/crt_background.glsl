// What a ray that leaves the scene sees. Used by kernel 03 Handle Escaped - once a path misses it is
// finished. Kernel 06's light samples read the environment map too, through crt_light.glsl, but only
// when the map is the background: a solid colour and the sky gradient are not in the light list.
//
// Three sources in priority order, chosen by the push-constant flags: a solid colour, the
// scene's equirectangular environment map, or the procedural sky gradient of Ray Tracing in One
// Weekend.
//
// Requires crt_common.glsl and equirect.glsl.

// Ray Tracing in One Weekend's sky: white at the horizon blending to light blue overhead
vec3 skyGradient(vec3 direction)
{
	float t = 0.5 * (direction.y + 1.0);
	return mix(vec3(1.0), vec3(0.5, 0.7, 1.0), t);
}

vec3 backgroundRadiance(vec3 direction)
{
	if ((pc.flags & CRT_FLAG_SOLID_BACKGROUND) != 0u) {
		return pc.background.rgb;
	}
	if ((pc.flags & CRT_FLAG_ENVIRONMENT_MAP) != 0u) {
		// textureLod: compute has no derivatives to pick a level with, and the map has no mips
		return textureLod(environmentMap, equirectUv(direction), 0.0).rgb * pc.environmentIntensity;
	}
	return skyGradient(direction);
}
