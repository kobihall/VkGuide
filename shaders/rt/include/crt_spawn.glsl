// THE SHADOW RAY towards a point: kernel 06's towards a light sample, and the light paths' towards the camera's lens
// (docs/plans/completed/pbrt-bidirectional-path-tracing.md). Moved out of crt_surface.glsl unchanged.
// Requires crt_common.glsl and crt_interaction.glsl (SurfaceInteraction).

#ifndef CRT_SPAWN_GLSL
#define CRT_SPAWN_GLSL

// How far a sampled point on an area light may be from the truth, per coordinate, in units of its
// magnitude and the shadow ray origin's: the sample's own arithmetic, the light list's world-space
// copy of the surface (built on the CPU from the instance's transform) against the object-space copy
// the traversal tests, and the transform that carries the ray there. Generous: a ray that ends this
// much short of a light costs nothing measurable, and one that ends past it would shadow itself
#define CRT_LIGHT_END_GAMMA 16

// The shadow ray from the vertex `si` towards a light sample, written to `ray`: false when the ray
// would have no length. Without error-bounded offsets it starts at the vertex and stops a fraction
// short of the light, as it always did. With them it starts past the vertex's error bound on the
// side it leaves through, and ends past the light point's on the side it arrives from (pbrt's
// SpawnRayTo, PBR 4ed 6.8.6), so nothing fixed to an epsilon is left in it. A point or spot light is
// a point: only its start is offset. A directional light or the environment has no end at all
bool spawnShadowRay(SurfaceInteraction si, vec3 direction, float distance, vec3 lightNormal, out ShadowRay ray)
{
	ray.mediumAndGroup = 0u;
#ifdef CRT_SPECTRAL
	ray.lambdaU = 0.0;
	ray.laneMask = 0u;
	ray.spectralPad = 0u;
#endif
	if ((pc.flags & CRT_FLAG_ERROR_BOUNDS) == 0u) {
		ray.origin = si.p;
		ray.direction = direction;
		// short of the light itself, so the shadow ray cannot be stopped by the surface it is aimed at
		ray.tMax = distance >= CRT_INFINITY ? CRT_INFINITY : distance * (1.0 - 1e-4);
		return true;
	}
	ray.origin = offsetRayOrigin(si.p, si.pError, si.n, direction);
	if (distance >= CRT_INFINITY) {
		ray.direction = direction;
		ray.tMax = CRT_INFINITY;
		return true;
	}
	// the light point as the sample's own direction and distance give it: the sampler hands over no
	// point, so that exposing one cannot change how the compiler fuses the sampling arithmetic (and
	// with it the bits of a render made without error-bounded offsets)
	vec3 end = fma(vec3(distance), direction, si.p);
	if (dot(lightNormal, lightNormal) > 0.0) {
		const vec3 lightPoint = end;
		const vec3 endError = CRT_GAMMA(CRT_LIGHT_END_GAMMA) * (abs(lightPoint) + abs(ray.origin));
		end = offsetRayOrigin(lightPoint, endError, lightNormal, ray.origin - lightPoint);
	}
	const vec3 toEnd = end - ray.origin;
	const float length = sqrt(dot(toEnd, toEnd));
	if (!(length > 0.0)) {
		return false;
	}
	ray.direction = toEnd / length;
	ray.tMax = length;
	return true;
}

#endif
