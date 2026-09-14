#pragma once

// Ported from the sibling project's Random.h/.cpp. One change: the engine is thread_local
// rather than a single global static. The raytrace runs on a worker thread while the UI thread
// is live, and a shared std::mt19937 mutated from two threads is a data race - thread_local
// costs nothing here and removes the hazard entirely.

#include <random>

#include <glm/glm.hpp>

class Random {
public:
	// reseeds the calling thread's generator only
	static void seed(uint32_t value);

	static uint32_t random_uint();
	static uint32_t random_uint(uint32_t min, uint32_t max);

	static double random_double();
	static double random_double(double min, double max);

	static glm::dvec2 random_vec2();
	static glm::dvec2 random_vec2(double min, double max);

	static glm::dvec2 random_in_disk();

	static glm::dvec3 random_vec3();
	static glm::dvec3 random_vec3(double min, double max);

	static glm::dvec3 random_in_ball();
	// uniformly distributed on the surface of the unit sphere. Named random_in_unit_sphere in the
	// sibling project, which it never was - renamed once it gained a caller
	static glm::dvec3 random_unit_vector();
	static glm::dvec3 random_in_hemisphere(glm::dvec3 normal);
	static glm::dvec3 random_in_hemisphere(glm::dvec3 normal, double alpha);
};

glm::dmat3 getTangentSpace(glm::dvec3 normal);
