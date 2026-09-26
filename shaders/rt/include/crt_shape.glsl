// The analytic shapes as unit primitives in their own object space: sphere, infinite plane, quad,
// box and capped cylinder. A placed shape is one of these under its instance's transform, so the
// ray arrives here in object space with a direction that is not unit length, and t is the
// parameter along it - the same t as the world-space ray.
//
// Mirrored line for line by src/shape.h/.cpp, where the primitives are defined; a change to one
// side must be made to the other. Every surface is two-sided.
//
// Included by crt_bvh.glsl, after the safeReciprocal it uses.

// src/shape.h ShapeKind
#define CRT_SHAPE_SPHERE 0u
#define CRT_SHAPE_PLANE 1u
#define CRT_SHAPE_QUAD 2u
#define CRT_SHAPE_BOX 3u
#define CRT_SHAPE_CYLINDER 4u

// lowers tMax to `candidate` when it is a nearer hit in range
bool acceptRoot(float candidate, float tMin, inout float tMax)
{
	if (candidate >= tMin && candidate <= tMax) {
		tMax = candidate;
		return true;
	}
	return false;
}

// The two roots of |o + t d|^2 = 1 in the numerically stable form (Ray Tracing Gems ch. 7): the
// discriminant from the closest-approach vector rather than |o|^2 - 1, which cancels
// catastrophically for a large sphere seen from close by. Generalised to a direction that is not
// unit length, a = |d|^2. The sphere's in 3D; the cylinder's side is the same in xz
bool unitSphereRoots(vec3 o, vec3 d, out float t0, out float t1)
{
	t0 = 0.0;
	t1 = 0.0;
	const float a = dot(d, d);
	if (a < 1e-30) {
		return false;
	}
	const float bPrime = -dot(o, d);
	const vec3 closest = o + (bPrime / a) * d;
	const float delta = 1.0 - dot(closest, closest);
	if (delta < 0.0) {
		return false;
	}
	const float q = bPrime + (bPrime >= 0.0 ? 1.0 : -1.0) * sqrt(a * delta);
	if (abs(q) < 1e-30) {
		return false;
	}
	t0 = (dot(o, o) - 1.0) / q;
	t1 = q / a;
	return true;
}

// the nearest hit in [tMin, tMax], or false
bool hitShape(uint kind, vec3 origin, vec3 direction, float tMin, float tMax, out float tHit)
{
	const float tLimit = tMax;

	if (kind == CRT_SHAPE_SPHERE) {
		float t0, t1;
		if (unitSphereRoots(origin, direction, t0, t1)) {
			acceptRoot(t0, tMin, tMax);
			acceptRoot(t1, tMin, tMax);
		}
	} else if (kind == CRT_SHAPE_PLANE || kind == CRT_SHAPE_QUAD) {
		if (abs(direction.y) >= 1e-30) {
			const float candidate = -origin.y / direction.y;
			const vec3 p = origin + candidate * direction;
			if (kind == CRT_SHAPE_PLANE || (abs(p.x) <= 0.5 && abs(p.z) <= 0.5)) {
				acceptRoot(candidate, tMin, tMax);
			}
		}
	} else if (kind == CRT_SHAPE_BOX) {
		// the slab test, taking the far side when the ray starts inside - the way out of a glass box
		const vec3 reciprocal = safeReciprocal(direction);
		const vec3 t0 = (vec3(-0.5) - origin) * reciprocal;
		const vec3 t1 = (vec3(0.5) - origin) * reciprocal;
		const vec3 near = min(t0, t1);
		const vec3 far = max(t0, t1);
		const float tNear = max(max(near.x, near.y), near.z);
		const float tFar = min(min(far.x, far.y), far.z);
		if (tNear <= tFar && !acceptRoot(tNear, tMin, tMax)) {
			acceptRoot(tFar, tMin, tMax);
		}
	} else if (kind == CRT_SHAPE_CYLINDER) {
		// the side, where it lies between the caps: the nearer root may be off the end while the
		// farther one is not
		float t0, t1;
		if (unitSphereRoots(vec3(origin.x, 0.0, origin.z), vec3(direction.x, 0.0, direction.z), t0, t1)) {
			if (abs(origin.y + t0 * direction.y) <= 0.5) {
				acceptRoot(t0, tMin, tMax);
			}
			if (abs(origin.y + t1 * direction.y) <= 0.5) {
				acceptRoot(t1, tMin, tMax);
			}
		}
		// the caps, where they lie inside the rim
		if (abs(direction.y) >= 1e-30) {
			for (int cap = 0; cap < 2; cap++) {
				const float capY = cap == 0 ? -0.5 : 0.5;
				const float candidate = (capY - origin.y) / direction.y;
				const vec3 p = origin + candidate * direction;
				if (p.x * p.x + p.z * p.z <= 1.0) {
					acceptRoot(candidate, tMin, tMax);
				}
			}
		}
	}

	tHit = tMax;
	return tMax < tLimit;
}

// which face of the unit box a point on it lies on: -x, +x, -y, +y, -z, +z as 0-5, the order the
// light list gives an emitting box's faces. src/light_sampling.cpp boxFaceOf
uint boxFace(vec3 p)
{
	const vec3 a = abs(p);
	if (a.x >= a.y && a.x >= a.z) {
		return p.x >= 0.0 ? 1u : 0u;
	}
	if (a.y >= a.z) {
		return p.y >= 0.0 ? 3u : 2u;
	}
	return p.z >= 0.0 ? 5u : 4u;
}

// the outward object-space normal at a point on the unit primitive, not normalised
vec3 shapeNormal(uint kind, vec3 p)
{
	if (kind == CRT_SHAPE_SPHERE) {
		return p;
	}
	if (kind == CRT_SHAPE_BOX) {
		// the face is the axis the point lies furthest out along
		const vec3 a = abs(p);
		if (a.x >= a.y && a.x >= a.z) {
			return vec3(p.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
		}
		if (a.y >= a.z) {
			return vec3(0.0, p.y >= 0.0 ? 1.0 : -1.0, 0.0);
		}
		return vec3(0.0, 0.0, p.z >= 0.0 ? 1.0 : -1.0);
	}
	if (kind == CRT_SHAPE_CYLINDER) {
		// whichever surface the point is nearer to: the rim at radius 1, or a cap at |y| = 0.5
		const float radial = sqrt(p.x * p.x + p.z * p.z);
		if (abs(0.5 - abs(p.y)) < abs(1.0 - radial)) {
			return vec3(0.0, p.y >= 0.0 ? 1.0 : -1.0, 0.0);
		}
		return vec3(p.x, 0.0, p.z);
	}
	// plane and quad
	return vec3(0.0, 1.0, 0.0);
}
