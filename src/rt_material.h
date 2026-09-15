#pragma once

// Ported from the sibling project's material.h / material.cpp - lambertian, metal and
// dielectric from the book, plus its own non-book phong. The CPU backend's scattering only;
// the material *data* (type, albedo, per-type parameters) is SphereMaterial in
// rt_scene_types.h, and makeCpuMaterial() in rt_scene.cpp is the one place these are built.

#include <rt_types.h>
#include <rt_random.h>

class material {
public:
	virtual ~material() = default;

	virtual bool scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const = 0;
};

class lambertian : public material {
public:
	lambertian(const glm::dvec3& a) : albedo(a) {}

	virtual bool scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const override;

	glm::dvec3 albedo;
};

class metal : public material {
public:
	metal(const glm::dvec3& a, double f) : albedo(a), fuzz(f < 1 ? f : 1) {}

	virtual bool scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const override;

	glm::dvec3 albedo;
	double fuzz;
};

class phong : public material {
public:
	phong(const glm::dvec3& a, double s) : albedo(a), smoothness(s) {}

	virtual bool scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const override;

	glm::dvec3 albedo;
	double smoothness;
};

glm::dvec3 refract(const glm::dvec3& uv, const glm::dvec3& n, double etai_over_etat);

class dielectric : public material {
public:
	dielectric(double index_of_refraction) : ir(index_of_refraction) {}

	virtual bool scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const override;

	double ir; //Index of Refraction

private:
	static double reflectance(double cosine, double ref_idx);
};
