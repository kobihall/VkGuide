#pragma once

// Floating-point error bounds and the ray origins built on them (PBR 4ed 6.8). Part of rt_bvh, so
// the CPU traversals, the tests and the GPU's mirror agree on every constant.
//
// Mirrored line for line by the robust section of shaders/rt/include/crt_common.glsl: a change to
// one side must be made to the other, then run bin/bvh_bench.
//
// The idea (PBR 4ed 6.8.5, 6.8.6): every point an intersection computes in float lies within a
// known absolute error `pError` of the true surface point. A ray spawned there is moved along the
// geometric normal by just more than that error, to the side it leaves through, so it can never
// re-intersect the surface it left, and no fixed epsilon is needed.

#include <bit>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

// 2^-24: half the spacing of floats just above one, the largest relative error of one rounded
// operation
inline constexpr float MACHINE_EPSILON = 5.9604644775390625e-8f;

// The bound on the relative error of n chained floating-point operations (PBR 4ed eq. 6.4)
constexpr float gammaBound(int n)
{
	return ((float)n * MACHINE_EPSILON) / (1.f - (float)n * MACHINE_EPSILON);
}

// The smallest float above v (pbrt's NextFloatUp). Finite v only: a position is never infinite, and
// the GPU's fast-math compilers may not honour an infinity test. Works on the bits throughout: the
// same compilers are free to treat -0 and +0 alike
inline float nextFloatUp(float v)
{
	uint32_t bits = std::bit_cast<uint32_t>(v);
	if (bits == 0x80000000u) {
		bits = 0u;
	}
	bits = (bits >> 31) == 0u ? bits + 1u : bits - 1u;
	return std::bit_cast<float>(bits);
}

// the largest float below v
inline float nextFloatDown(float v)
{
	uint32_t bits = std::bit_cast<uint32_t>(v);
	if (bits == 0u) {
		bits = 0x80000000u;
	}
	bits = (bits >> 31) == 0u ? bits - 1u : bits + 1u;
	return std::bit_cast<float>(bits);
}

// a*b - c*d with a rounding error of about one ulp of the result, not of the products (Kahan's
// algorithm on fma). Exactly zero when the true value is zero
inline float differenceOfProducts(float a, float b, float c, float d)
{
	const float cd = c * d;
	const float difference = std::fma(a, b, -cd);
	const float error = std::fma(-c, d, cd);
	return difference + error;
}

// The point `p`, whose coordinates are each within `pError` of the truth, moved along `n` by more
// than that error towards the side `w` points to (PBR 4ed 6.8.6, pbrt's OffsetRayOrigin), then each
// coordinate rounded one float further away from the surface. `n` need not be unit length
inline glm::vec3 offsetRayOrigin(const glm::vec3& p, const glm::vec3& pError, const glm::vec3& n, const glm::vec3& w)
{
	const float d = glm::dot(glm::abs(n), pError);
	glm::vec3 offset = d * n;
	if (glm::dot(w, n) < 0.f) {
		offset = -offset;
	}
	glm::vec3 po = p + offset;
	for (int i = 0; i < 3; i++) {
		if (offset[i] > 0.f) {
			po[i] = nextFloatUp(po[i]);
		} else if (offset[i] < 0.f) {
			po[i] = nextFloatDown(po[i]);
		}
	}
	return po;
}

// The nearest t a trace accepts. Without error-bounded offsets a ray spawned on a surface would hit
// it again at t of a few ulps, so every trace skips the first CRT_T_MIN; with them, the spawn point
// is already clear of the surface and the trace takes any t above zero. Strictly above: a ray
// spawned exactly on a plane (the infinite plane at y = 0 under an identity transform has no error
// along its normal) computes t = -0 or +0 for it, and must not hit it
inline constexpr float TRACE_T_MIN_EPSILON = 0.001f;
inline constexpr float TRACE_T_MIN_BOUNDED = 1e-30f;

// How far a box's slab test widens its far distance so a ray grazing an edge is never culled by
// rounding (Ize 2013, PBR 4ed 6.8.2): 1 + 2 gamma(3)
inline constexpr float BOX_FAR_SCALE = 1.f + 2.f * gammaBound(3);
