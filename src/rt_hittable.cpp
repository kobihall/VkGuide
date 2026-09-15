#include <rt_hittable.h>

bool sphere::hit(const ray& r, double t_min, double t_max, hit_record& rec) const
{
	glm::dvec3 oc = r.origin() - center;
	double a = glm::dot(r.direction(), r.direction());
	double half_b = glm::dot(oc, r.direction());
	double c = glm::dot(oc, oc) - radius * radius;

	auto discriminant = half_b * half_b - a * c;
	if (discriminant < 0) return false;
	auto sqrtd = sqrt(discriminant);

	// Find the nearest root that lies in the acceptable range.
	auto root = (-half_b - sqrtd) / a;
	if (root < t_min || t_max < root) {
		root = (-half_b + sqrtd) / a;
		if (root < t_min || t_max < root)
			return false;
	}

	rec.t = root;
	rec.p = r.at(rec.t);
	glm::dvec3 outward_normal = (rec.p - center) / radius;
	rec.set_face_normal(r, outward_normal);
	rec.mat_ptr = mat_ptr.get();

	return true;
}
