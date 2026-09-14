#include <rt_material.h>

#include <imgui.h>

namespace {

//imgui's colour widgets work in float, so the double albedo round-trips through a float copy.
//It is only written back when the widget reports a change, so an untouched albedo keeps its
//full double precision
bool editAlbedo(glm::dvec3& albedo)
{
	float color[3] = { (float)albedo.x, (float)albedo.y, (float)albedo.z };
	if (!ImGui::ColorEdit3("albedo", color)) {
		return false;
	}

	albedo = glm::dvec3(color[0], color[1], color[2]);
	return true;
}

bool sliderDouble(const char* label, double& value, double min, double max)
{
	return ImGui::SliderScalar(label, ImGuiDataType_Double, &value, &min, &max, "%.3f");
}

}

bool lambertian::scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const
{
	//normal + a point on the unit sphere is a true cosine-weighted Lambertian distribution. The
	//sibling used a point inside the unit ball, which only approximates one
	auto scatter_direction = rec.normal + Random::random_unit_vector();

	//the two can cancel almost exactly, and a zero direction turns into NaN once it is normalized
	//further down the path
	if (glm::dot(scatter_direction, scatter_direction) < 1e-16)
		scatter_direction = rec.normal;

	scattered = ray(rec.p, scatter_direction);
	attenuation = albedo;
	return true;
}

bool lambertian::params()
{
	return editAlbedo(albedo);
}

bool metal::scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const
{
	glm::dvec3 reflected = glm::reflect(glm::normalize(r_in.direction()), rec.normal);
	scattered = ray(rec.p, reflected + fuzz * Random::random_in_ball());
	attenuation = albedo;
	return (glm::dot(scattered.direction(), rec.normal) > 0);
}

bool metal::params()
{
	bool changed = editAlbedo(albedo);
	//the constructor clamps fuzz to at most 1, so the slider stops there too
	changed |= sliderDouble("fuzz", fuzz, 0.0, 1.0);
	return changed;
}

bool phong::scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const
{
	glm::dvec3 reflected = glm::reflect(glm::normalize(r_in.direction()), rec.normal);
	double alpha = pow(1000.0, smoothness * smoothness);
	scattered = ray(rec.p, Random::random_in_hemisphere(reflected, alpha));
	attenuation = albedo;
	return (glm::dot(scattered.direction(), rec.normal) > 0);
}

bool phong::params()
{
	bool changed = editAlbedo(albedo);
	//smoothness 0-1 maps to a lobe exponent of 1-1000 in scatter()
	changed |= sliderDouble("smoothness", smoothness, 0.0, 1.0);
	return changed;
}

glm::dvec3 refract(const glm::dvec3& uv, const glm::dvec3& n, double etai_over_etat)
{
	auto cos_theta = glm::min(glm::dot(-uv, n), 1.0);
	glm::dvec3 r_out_perp = etai_over_etat * (uv + cos_theta * n);
	glm::dvec3 r_out_parallel = -sqrt(fabs(1.0 - glm::dot(r_out_perp, r_out_perp))) * n;
	return r_out_perp + r_out_parallel;
}

bool dielectric::scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const
{
	attenuation = glm::dvec3(1.0, 1.0, 1.0);
	double refraction_ratio = rec.front_face ? (1.0 / ir) : ir;

	glm::dvec3 unit_direction = glm::normalize(r_in.direction());
	double cos_theta = glm::min(glm::dot(-unit_direction, rec.normal), 1.0);
	double sin_theta = sqrt(1.0 - cos_theta * cos_theta);

	bool cannot_refract = refraction_ratio * sin_theta > 1.0;
	glm::dvec3 direction;

	if (cannot_refract || reflectance(cos_theta, refraction_ratio) > Random::random_double())
		direction = glm::reflect(unit_direction, rec.normal);
	else
		direction = refract(unit_direction, rec.normal, refraction_ratio);

	scattered = ray(rec.p, direction);
	return true;
}

bool dielectric::params()
{
	//1.0 is vacuum; 1.33 water, 1.5 glass, 2.42 diamond
	return sliderDouble("index of refraction", ir, 1.0, 3.0);
}

double dielectric::reflectance(double cosine, double ref_idx)
{
	auto r0 = (1 - ref_idx) / (1 + ref_idx);
	r0 = r0 * r0;
	return r0 + (1 - r0) * pow(1 - cosine, 5);
}

const char* materialTypeName(MaterialType type)
{
	switch (type) {
	case MaterialType::Lambertian:
		return "lambertian";
	case MaterialType::Metal:
		return "metal";
	case MaterialType::Phong:
		return "phong";
	case MaterialType::Dielectric:
		return "dielectric";
	}

	return "unknown";
}

std::shared_ptr<material> makeMaterial(MaterialType type, const glm::dvec3& albedo)
{
	switch (type) {
	case MaterialType::Lambertian:
		return std::make_shared<lambertian>(albedo);
	case MaterialType::Metal:
		return std::make_shared<metal>(albedo, 0.3);
	case MaterialType::Phong:
		return std::make_shared<phong>(albedo, 0.5);
	case MaterialType::Dielectric:
		return std::make_shared<dielectric>(1.5);
	}

	return std::make_shared<lambertian>(albedo);
}

std::optional<glm::dvec3> materialAlbedo(const material& mat)
{
	switch (mat.type()) {
	case MaterialType::Lambertian:
		return static_cast<const lambertian&>(mat).albedo;
	case MaterialType::Metal:
		return static_cast<const metal&>(mat).albedo;
	case MaterialType::Phong:
		return static_cast<const phong&>(mat).albedo;
	case MaterialType::Dielectric:
		return std::nullopt;
	}

	return std::nullopt;
}

glm::dvec3 materialPreviewColor(const material& mat)
{
	//a dielectric has no albedo of its own - it attenuates by white - so a pale tint reads as glass
	//in the preview rather than as a plain white diffuse sphere
	return materialAlbedo(mat).value_or(glm::dvec3(0.75, 0.85, 1.0));
}
