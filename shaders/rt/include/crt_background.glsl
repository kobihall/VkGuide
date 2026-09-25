// What a ray that leaves the scene sees. Used by kernel 03 Handle Escaped, and by nothing else -
// once a path misses it is finished, so this is the only place the background is evaluated.
//
// Three sources in priority order, chosen by the push-constant flags: a solid colour, the
// scene's equirectangular environment map, or the procedural sky gradient that the CPU backend
// and Ray Tracing in One Weekend use.
//
// Requires crt_common.glsl and equirect.glsl.

// the CPU backend's sky: white at the horizon blending to light blue overhead
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
