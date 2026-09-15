#include <rt_material.h>

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

bool metal::scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const
{
	glm::dvec3 reflected = glm::reflect(glm::normalize(r_in.direction()), rec.normal);
	scattered = ray(rec.p, reflected + fuzz * Random::random_in_ball());
	attenuation = albedo;
	return (glm::dot(scattered.direction(), rec.normal) > 0);
}

bool phong::scatter(const ray& r_in, const hit_record& rec, glm::dvec3& attenuation, ray& scattered) const
{
	glm::dvec3 reflected = glm::reflect(glm::normalize(r_in.direction()), rec.normal);
	double alpha = pow(1000.0, smoothness * smoothness);
	scattered = ray(rec.p, Random::random_in_hemisphere(reflected, alpha));
	attenuation = albedo;
	return (glm::dot(scattered.direction(), rec.normal) > 0);
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

double dielectric::reflectance(double cosine, double ref_idx)
{
	auto r0 = (1 - ref_idx) / (1 + ref_idx);
	r0 = r0 * r0;
	return r0 + (1 - r0) * pow(1 - cosine, 5);
}
