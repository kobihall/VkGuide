#pragma once

// Ported from the sibling project's material.h / material.cpp - lambertian, metal and
// dielectric from the book, plus its own non-book phong.
//
// There is deliberately no glTF-material-to-raytracer-material mapping here: nothing shades
// glTF geometry in this feature, so the shape such a conversion should take is not yet known.

#include <optional>

#include <rt_types.h>
#include <rt_random.h>

// Carried so a material can be identified without a dynamic_cast. The CPU raytracer itself
// never branches on this - it dispatches virtually through scatter() - but a material has to
// be writable to a flat tagged struct to be uploaded to the GPU or saved to a file, and both
// of those are planned follow-on features.
enum class MaterialType : uint8_t {
	Lambertian,
	Metal,
	Phong,
	Dielectric
};

inline constexpr MaterialType MATERIAL_TYPES[] = {
	MaterialType::Lambertian,
	MaterialType::Metal,
	MaterialType::Phong,
	MaterialType::Dielectric,
};

class material {
public:
	virtual ~material() = default;

	virtual MaterialType type() const = 0;
	virtual bool scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const = 0;

	// an independent copy. The render snapshot clones every material, so the editor can keep
	// changing its own copies while a render on the worker thread reads the snapshot's
	virtual std::shared_ptr<material> clone() const = 0;

	// draws this material's own imgui controls, the same per-type dispatch hittable::params()
	// uses. Returns true if any value changed
	virtual bool params() = 0;
};

class lambertian : public material {
public:
	lambertian(const glm::dvec3& a) : albedo(a) {}

	virtual MaterialType type() const override { return MaterialType::Lambertian; }
	virtual bool scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const override;
	virtual std::shared_ptr<material> clone() const override { return std::make_shared<lambertian>(*this); }
	virtual bool params() override;

	glm::dvec3 albedo;
};

class metal : public material {
public:
	metal(const glm::dvec3& a, double f) : albedo(a), fuzz(f < 1 ? f : 1) {}

	virtual MaterialType type() const override { return MaterialType::Metal; }
	virtual bool scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const override;
	virtual std::shared_ptr<material> clone() const override { return std::make_shared<metal>(*this); }
	virtual bool params() override;

	glm::dvec3 albedo;
	double fuzz;
};

class phong : public material {
public:
	phong(const glm::dvec3& a, double s) : albedo(a), smoothness(s) {}

	virtual MaterialType type() const override { return MaterialType::Phong; }
	virtual bool scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const override;
	virtual std::shared_ptr<material> clone() const override { return std::make_shared<phong>(*this); }
	virtual bool params() override;

	glm::dvec3 albedo;
	double smoothness;
};

glm::dvec3 refract(const glm::dvec3& uv, const glm::dvec3& n, double etai_over_etat);

class dielectric : public material {
public:
	dielectric(double index_of_refraction) : ir(index_of_refraction) {}

	virtual MaterialType type() const override { return MaterialType::Dielectric; }
	virtual bool scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const override;
	virtual std::shared_ptr<material> clone() const override { return std::make_shared<dielectric>(*this); }
	virtual bool params() override;

	double ir; //Index of Refraction

private:
	static double reflectance(double cosine, double ref_idx);
};

const char* materialTypeName(MaterialType type);

// a material of the given type with default parameters. albedo is ignored by types without one
std::shared_ptr<material> makeMaterial(MaterialType type, const glm::dvec3& albedo);

// the material's own albedo, or nothing for a type that has none (dielectric)
std::optional<glm::dvec3> materialAlbedo(const material& mat);

// the single flat colour a material is best shown as outside the raytracer - used by the
// raster preview spheres, which have no path tracer to resolve a real appearance with
glm::dvec3 materialPreviewColor(const material& mat);
