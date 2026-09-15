#pragma once

// The CPU raytracer's ray and hit record. Ported from the sibling project at
// /Users/kobihall/Documents/Code/RayTracingInAWeekend, which works in double precision
// throughout - kept as-is here rather than narrowed to float, so the ported intersection and
// scattering math behaves identically. The scene model itself is float and lives in
// rt_scene_types.h; only this backend widens it.

#include <limits>

#include <glm/glm.hpp>

#include <rt_scene_types.h>

class material;

inline constexpr double RT_INFINITY = std::numeric_limits<double>::infinity();
inline constexpr double RT_PI = 3.1415926535897932385;

inline double degrees_to_radians(double degrees)
{
	return degrees * RT_PI / 180.0;
}

class ray {
public:
	ray() {}
	ray(const glm::dvec3& origin, const glm::dvec3& direction) : orig(origin), dir(direction) {}

	glm::dvec3 origin() const { return orig; }
	glm::dvec3 direction() const { return dir; }

	glm::dvec3 at(double t) const { return orig + t * dir; }

	glm::dvec3 orig { 0.0 };
	glm::dvec3 dir { 0.0 };
};

struct hit_record {
	glm::dvec3 p;
	glm::dvec3 normal;
	// non-owning: the sphere that was hit owns its material, and the render's RaytraceScene owns
	// the sphere for the whole render. A shared_ptr here paid an atomic refcount bump on every
	// hit_record copy in the innermost intersection loop
	const material* mat_ptr { nullptr };
	double t;
	bool front_face;

	// glTF and this raytracer both allow rays to start inside geometry, so the stored normal
	// always faces the incoming ray and front_face records which side was hit
	void set_face_normal(const ray& r, const glm::dvec3& outward_normal)
	{
		front_face = glm::dot(r.direction(), outward_normal) < 0;
		normal = front_face ? outward_normal : -outward_normal;
	}
};
