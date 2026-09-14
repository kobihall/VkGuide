#pragma once

// Ported from the sibling project's hittable.h / sphere.h. sphere is the only concrete
// hittable, matching the source: this feature traces spheres and nothing else.
//
// There is deliberately no bounding_box()/aabb here - nothing builds an acceleration structure
// over these, and adding one now would be guessing at what a future mesh-tracing feature wants.

#include <rt_types.h>

class hittable {
public:
	virtual ~hittable() = default;

	// draws this object's own imgui controls. The scene browser dispatches to it virtually,
	// so each hittable subtype owns the UI for its own parameters. Returns true if any value changed
	virtual bool params() = 0;
	virtual bool hit(const ray& r, double t_min, double t_max, hit_record& rec) const = 0;
};

class sphere : public hittable {
public:
	sphere() {}
	sphere(glm::dvec3 cen, double r, std::shared_ptr<material> m) : center(cen), radius(r), mat_ptr(m) {}

	virtual bool params() override;
	virtual bool hit(const ray& r, double t_min, double t_max, hit_record& rec) const override;

	glm::dvec3 center { 0.0 };
	double radius { 1.0 };
	std::shared_ptr<material> mat_ptr;
};
