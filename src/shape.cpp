#include <shape.h>

#include <algorithm>
#include <cmath>

#include <bvh_layout.h>

namespace {

// lowers tMax to `candidate` when it is a nearer hit in range; every shape offers its candidate
// roots through this in any order and is left with the nearest
bool accept(float candidate, float tMin, float& tMax)
{
	if (candidate >= tMin && candidate <= tMax) {
		tMax = candidate;
		return true;
	}
	return false;
}

// the two roots of |o + t d|^2 = 1 in the numerically stable form (Ray Tracing Gems ch. 7): the
// discriminant from the closest-approach vector rather than |o|^2 - 1, which cancels
// catastrophically for a large sphere seen from close by. Generalised to a direction that is not
// unit length, a = |d|^2. Shared by the sphere (in 3D) and the cylinder's side (in xz). False when
// the line misses, or only grazes it through the origin
template <typename Vec>
bool unitQuadricRoots(const Vec& o, const Vec& d, float& t0, float& t1)
{
	const float a = glm::dot(d, d);
	if (a < 1e-30f) {
		return false;
	}
	const float bPrime = -glm::dot(o, d);
	const Vec closest = o + (bPrime / a) * d;
	const float delta = 1.f - glm::dot(closest, closest);
	if (delta < 0.f) {
		return false;
	}
	const float q = bPrime + (bPrime >= 0.f ? 1.f : -1.f) * std::sqrt(a * delta);
	if (std::abs(q) < 1e-30f) {
		return false;
	}
	t0 = (glm::dot(o, o) - 1.f) / q;
	t1 = q / a;
	return true;
}

}

bool shapeBounded(ShapeKind kind)
{
	return kind != ShapeKind::Plane;
}

Aabb shapeObjectBounds(ShapeKind kind)
{
	Aabb box;
	switch (kind) {
	case ShapeKind::Sphere:
		box.grow(glm::vec3(-1.f));
		box.grow(glm::vec3(1.f));
		break;
	case ShapeKind::Plane:
		break;
	case ShapeKind::Quad:
		box.grow(glm::vec3(-0.5f, 0.f, -0.5f));
		box.grow(glm::vec3(0.5f, 0.f, 0.5f));
		break;
	case ShapeKind::Box:
		box.grow(glm::vec3(-0.5f));
		box.grow(glm::vec3(0.5f));
		break;
	case ShapeKind::Cylinder:
		box.grow(glm::vec3(-1.f, -0.5f, -1.f));
		box.grow(glm::vec3(1.f, 0.5f, 1.f));
		break;
	}
	return box;
}

bool intersectShape(ShapeKind kind, const glm::vec3& origin, const glm::vec3& direction, float tMin, float tMax, float& t)
{
	const float tLimit = tMax;

	switch (kind) {
	case ShapeKind::Sphere: {
		float t0, t1;
		if (unitQuadricRoots(origin, direction, t0, t1)) {
			accept(t0, tMin, tMax);
			accept(t1, tMin, tMax);
		}
		break;
	}

	case ShapeKind::Plane:
	case ShapeKind::Quad: {
		if (std::abs(direction.y) < 1e-30f) {
			return false;
		}
		const float candidate = -origin.y / direction.y;
		const glm::vec3 p = origin + candidate * direction;
		if (kind == ShapeKind::Plane || (std::abs(p.x) <= 0.5f && std::abs(p.z) <= 0.5f)) {
			accept(candidate, tMin, tMax);
		}
		break;
	}

	case ShapeKind::Box: {
		// the slab test, taking the far side when the ray starts inside - the way out of a glass box
		const glm::vec3 reciprocal = bvhSafeReciprocal(direction);
		const glm::vec3 t0 = (glm::vec3(-0.5f) - origin) * reciprocal;
		const glm::vec3 t1 = (glm::vec3(0.5f) - origin) * reciprocal;
		const glm::vec3 near = glm::min(t0, t1);
		const glm::vec3 far = glm::max(t0, t1);
		const float tNear = std::max(std::max(near.x, near.y), near.z);
		const float tFar = std::min(std::min(far.x, far.y), far.z);
		if (tNear <= tFar && !accept(tNear, tMin, tMax)) {
			accept(tFar, tMin, tMax);
		}
		break;
	}

	case ShapeKind::Cylinder: {
		// the side, where it lies between the caps: the nearer root may be off the end while the
		// farther one is not
		float t0, t1;
		if (unitQuadricRoots(glm::vec2(origin.x, origin.z), glm::vec2(direction.x, direction.z), t0, t1)) {
			for (const float root : { t0, t1 }) {
				if (std::abs(origin.y + root * direction.y) <= 0.5f) {
					accept(root, tMin, tMax);
				}
			}
		}
		// the caps, where they lie inside the rim
		if (std::abs(direction.y) >= 1e-30f) {
			for (const float capY : { -0.5f, 0.5f }) {
				const float candidate = (capY - origin.y) / direction.y;
				const glm::vec3 p = origin + candidate * direction;
				if (p.x * p.x + p.z * p.z <= 1.f) {
					accept(candidate, tMin, tMax);
				}
			}
		}
		break;
	}
	}

	if (tMax < tLimit) {
		t = tMax;
		return true;
	}
	return false;
}

glm::vec3 shapeNormal(ShapeKind kind, const glm::vec3& point)
{
	switch (kind) {
	case ShapeKind::Sphere:
		return point;
	case ShapeKind::Plane:
	case ShapeKind::Quad:
		return glm::vec3(0.f, 1.f, 0.f);
	case ShapeKind::Box: {
		// the face is the axis the point lies furthest out along
		const glm::vec3 a = glm::abs(point);
		if (a.x >= a.y && a.x >= a.z) {
			return glm::vec3(point.x >= 0.f ? 1.f : -1.f, 0.f, 0.f);
		}
		if (a.y >= a.z) {
			return glm::vec3(0.f, point.y >= 0.f ? 1.f : -1.f, 0.f);
		}
		return glm::vec3(0.f, 0.f, point.z >= 0.f ? 1.f : -1.f);
	}
	case ShapeKind::Cylinder: {
		// whichever surface the point is nearer to: the rim at radius 1, or a cap at |y| = 0.5
		const float radial = std::sqrt(point.x * point.x + point.z * point.z);
		if (std::abs(0.5f - std::abs(point.y)) < std::abs(1.f - radial)) {
			return glm::vec3(0.f, point.y >= 0.f ? 1.f : -1.f, 0.f);
		}
		return glm::vec3(point.x, 0.f, point.z);
	}
	}
	return glm::vec3(0.f, 1.f, 0.f);
}
