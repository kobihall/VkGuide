// Equirectangular environment map lookup, shared by everything that samples the scene's
// environment map (the "environment" background effect today, the compute raytracer's miss
// branch next) so they all agree on which way the map faces.
//
// Convention: +y is up (v = 0 is the top row of the image). The centre column of the map faces
// -z, the direction the raster camera looks at yaw 0, and u increases turning right (towards +x),
// which is how a panorama reads when viewed from inside.

const float EQUIRECT_PI = 3.14159265358979323846;

// direction must be normalised
vec2 equirectUv(vec3 direction)
{
	float u = 0.5 + atan(direction.x, -direction.z) / (2.0 * EQUIRECT_PI);
	float v = 0.5 - asin(clamp(direction.y, -1.0, 1.0)) / EQUIRECT_PI;
	return vec2(u, v);
}
