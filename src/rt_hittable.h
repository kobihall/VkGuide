#pragma once

// Ported from the sibling project's hittable.h / sphere.h. sphere is the only concrete
// hittable, matching the source: this backend traces spheres and nothing else.
//
// Constructed only by buildRaytraceScene() from the plain SceneSphere data; the editor, the
// scene file and the preview spheres never see these classes.

#include <rt_types.h>

class hittable {
public:
	virtual ~hittable() = default;

	virtual bool hit(const ray& r, double t_min, double t_max, hit_record& rec) const = 0;
};

class sphere : public hittable {
public:
	sphere() {}
	sphere(glm::dvec3 cen, double r, std::shared_ptr<material> m) : center(cen), radius(r), mat_ptr(m) {}

	virtual bool hit(const ray& r, double t_min, double t_max, hit_record& rec) const override;

	glm::dvec3 center { 0.0 };
	double radius { 1.0 };
	std::shared_ptr<material> mat_ptr;
};
