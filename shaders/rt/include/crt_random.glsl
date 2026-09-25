// The GPU path tracer's random numbers and sampling, a function-for-function port of
// src/rt_random.cpp. PCG hash to seed, PCG-RXS-M-XS steps to draw, pure integer - no sin()
// hash, whose precision is driver-dependent. A path carries its state (PathState.rngState), so
// the stream depends only on (pixel, sample index, render seed) and a fixed seed reproduces
// an image exactly regardless of queue order or scheduling.

#define CRT_PI 3.14159265358979323846

uint pcgHash(uint v)
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

// one draw in [0, 1)
float randomFloat(inout uint state)
{
	state = state * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	word = (word >> 22u) ^ word;
	return float(word) * (1.0 / 4294967296.0);
}

float randomFloat(inout uint state, float lo, float hi)
{
	return lo + (hi - lo) * randomFloat(state);
}

// uniformly distributed on the surface of the unit sphere
vec3 randomUnitVector(inout uint state)
{
	float cosTheta = randomFloat(state, -1.0, 1.0);
	float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	float phi = 2.0 * CRT_PI * randomFloat(state);
	return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// uniformly distributed inside the unit ball: a unit direction scaled by the cube root of a
// uniform draw, in place of the CPU's rejection loop
vec3 randomInBall(inout uint state)
{
	return randomUnitVector(state) * pow(randomFloat(state), 1.0 / 3.0);
}

// uniformly distributed inside the unit disk, analytic
vec2 randomInDisk(inout uint state)
{
	float r = sqrt(randomFloat(state));
	float phi = 2.0 * CRT_PI * randomFloat(state);
	return r * vec2(cos(phi), sin(phi));
}

mat3 getTangentSpace(vec3 normal)
{
	vec3 helper = vec3(1.0, 0.0, 0.0);
	if (abs(normal.x) > 0.99) {
		helper = vec3(0.0, 0.0, 1.0);
	}
	vec3 tangent = normalize(cross(normal, helper));
	vec3 binormal = normalize(cross(normal, tangent));
	return mat3(tangent, binormal, normal);
}

// a direction around `normal` with a cos^alpha lobe (alpha = 0 is uniform over the hemisphere)
vec3 randomInHemisphere(vec3 normal, float alpha, inout uint state)
{
	float cosTheta = pow(randomFloat(state), 1.0 / (alpha + 1.0));
	float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	float phi = 2.0 * CRT_PI * randomFloat(state);
	vec3 tangentSpaceDir = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
	return getTangentSpace(normal) * tangentSpaceDir;
}
