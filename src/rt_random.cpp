#include <rt_random.h>

#include <algorithm>
#include <cmath>

#include <rt_types.h>

namespace {

//seeded once per thread on first use, so the worker thread and the UI thread never share state
std::mt19937& engine()
{
	static thread_local std::mt19937 s_engine { std::random_device {}() };
	return s_engine;
}

thread_local std::uniform_int_distribution<std::mt19937::result_type> s_iDistribution;
thread_local std::uniform_real_distribution<double> s_dDistribution;

}

void Random::seed(uint32_t value)
{
	engine().seed(value);
	s_iDistribution.reset();
	s_dDistribution.reset();
}

uint32_t Random::random_uint()
{
	return s_iDistribution(engine());
}

uint32_t Random::random_uint(uint32_t min, uint32_t max)
{
	return min + (s_iDistribution(engine()) % (max - min + 1));
}

double Random::random_double()
{
	return s_dDistribution(engine());
}

double Random::random_double(double min, double max)
{
	return min + (max - min) * random_double();
}

glm::dvec2 Random::random_vec2()
{
	return glm::dvec2(random_double(), random_double());
}

glm::dvec2 Random::random_vec2(double min, double max)
{
	return glm::dvec2(random_double(min, max), random_double(min, max));
}

glm::dvec2 Random::random_in_disk()
{
	while (true) {
		auto p = random_vec2(-1, 1);
		if (glm::dot(p, p) >= 1) continue;
		return p;
	}
}

glm::dvec3 Random::random_vec3()
{
	return glm::dvec3(random_double(), random_double(), random_double());
}

glm::dvec3 Random::random_vec3(double min, double max)
{
	return glm::dvec3(random_double(min, max), random_double(min, max), random_double(min, max));
}

glm::dvec3 Random::random_in_ball()
{
	while (true) {
		auto p = random_vec3(-1, 1);
		if (glm::dot(p, p) >= 1) continue;
		return p;
	}
}

glm::dvec3 Random::random_unit_vector()
{
	double cosTheta = random_double(-1, 1);
	double sinTheta = sqrt(std::max(0.0, 1 - cosTheta * cosTheta));
	double phi = 2 * RT_PI * random_double();
	return glm::dvec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

glm::dvec3 Random::random_in_hemisphere(glm::dvec3 normal)
{
	return Random::random_in_hemisphere(normal, 0);
}

glm::dvec3 Random::random_in_hemisphere(glm::dvec3 normal, double alpha)
{
	double cosTheta = pow(random_double(), 1.0 / (alpha + 1.0));
	double sinTheta = sqrt(std::max(0.0, 1 - cosTheta * cosTheta));
	double phi = 2 * RT_PI * random_double();
	glm::dvec3 tangentSpaceDir = glm::dvec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

	return getTangentSpace(normal) * tangentSpaceDir;
}

glm::dmat3 getTangentSpace(glm::dvec3 normal)
{
	glm::dvec3 helper = glm::dvec3(1, 0, 0);
	if (std::abs(normal.x) > 0.99)
		helper = glm::dvec3(0, 0, 1);
	glm::dvec3 tangent = glm::normalize(glm::cross(normal, helper));
	glm::dvec3 binormal = glm::normalize(glm::cross(normal, tangent));
	return glm::dmat3(tangent, binormal, normal);
}
