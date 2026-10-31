// Ray Tracing in One Weekend's sky, the background BackgroundMode::Sky: white at the horizon blending to light blue
// overhead, linear radiance by world direction. Shared by the path tracer's escaped rays (crt_background.glsl) and the
// raster preview's background (env_background.comp), so the two show the same sky

vec3 skyGradient(vec3 direction)
{
	float t = 0.5 * (direction.y + 1.0);
	return mix(vec3(1.0), vec3(0.5, 0.7, 1.0), t);
}
