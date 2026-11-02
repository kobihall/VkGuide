// Equirectangular environment map lookup, shared by everything that samples the scene's
// environment map (the "environment" background effect today, the compute raytracer's miss
// branch next) so they all agree on which way the map faces.
//
// Convention: +y is up (v = 0 is the top row of the image). The centre column of the map faces
// -z, the direction the raster camera looks at yaw 0, and u increases turning right (towards +x),
// which is how a panorama reads when viewed from inside.

#ifndef EQUIRECT_GLSL
#define EQUIRECT_GLSL

const float EQUIRECT_PI = 3.14159265358979323846;

// direction must be normalised
vec2 equirectUv(vec3 direction)
{
	float u = 0.5 + atan(direction.x, -direction.z) / (2.0 * EQUIRECT_PI);
	float v = 0.5 - asin(clamp(direction.y, -1.0, 1.0)) / EQUIRECT_PI;
	return vec2(u, v);
}

// the direction a point of the map faces, equirectUv()'s inverse (src/light_sampling.cpp equirectDirectionOf()): what the
// environment's sampling and an equirectangular spherical camera turn a point of the map into
vec3 equirectDirectionOf(vec2 uv)
{
	const float phi = (uv.x - 0.5) * 2.0 * EQUIRECT_PI;
	const float latitude = (0.5 - uv.y) * EQUIRECT_PI;
	const float cosLatitude = cos(latitude);
	return vec3(cosLatitude * sin(phi), sin(latitude), -cosLatitude * cos(phi));
}

// The map turned about +y by the scene's rotation (VulkanEngine::m_environmentRotation), given as its cosine and sine:
// a world direction in the map's own frame, which equirectUv() reads, and a direction of the map's frame in the world.
// A positive angle turns the map counterclockwise seen from above. src/light_sampling.cpp mirrors both
vec3 environmentToMap(vec3 direction, vec2 rotation)
{
	return vec3(rotation.x * direction.x - rotation.y * direction.z, direction.y, rotation.y * direction.x + rotation.x * direction.z);
}

vec3 environmentFromMap(vec3 direction, vec2 rotation)
{
	return vec3(rotation.x * direction.x + rotation.y * direction.z, direction.y, -rotation.y * direction.x + rotation.x * direction.z);
}

#endif
