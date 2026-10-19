// The analytic shapes as unit primitives in their own object space: sphere, infinite plane, quad,
// box and capped cylinder. A placed shape is one of these under its instance's transform, so the
// ray arrives here in object space with a direction that is not unit length, and t is the
// parameter along it - the same t as the world-space ray.
//
// Mirrored line for line by src/shape.h/.cpp, where the primitives are defined; a change to one
// side must be made to the other. Every surface is two-sided.
//
// THE PARAMETERIZATION. Each kind has PBR's parametric (u, v) and their derivatives, to texture a
// shape and to give it a tangent frame. One convention throughout, glTF's: seen from outside, u
// runs to the right and v runs down, so an image the material samples at (u, v) appears upright
// and unmirrored, exactly as on a glTF mesh. It makes cross(dp/du, dp/dv) point INTO the surface,
// the opposite of pbrt, whose image textures flip v instead.
//
//     sphere     u = phi / 2pi, v = theta / pi, with phi = atan(-z, x) and theta from +y
//     plane      object x and z, unscaled: a texture tiles once per unit and turns with the plane
//     quad       object x and z mapped to [0, 1]
//     box        each face its two axes mapped to [0, 1]; the four sides wrap like a cylinder, and
//                the top joins the +z face
//     cylinder   the side as the sphere, with v = 0.5 - y; each cap polar, v the radius
//
// Included by crt_traverse.glsl and crt_interaction.glsl. Requires crt_common.glsl.

#ifndef CRT_SHAPE_GLSL
#define CRT_SHAPE_GLSL

// src/shape.h ShapeKind
#define CRT_SHAPE_SPHERE 0u
#define CRT_SHAPE_PLANE 1u
#define CRT_SHAPE_QUAD 2u
#define CRT_SHAPE_BOX 3u
#define CRT_SHAPE_CYLINDER 4u

#define CRT_SHAPE_PI 3.14159265358979323846

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
		const vec3 far = max(t0, t1) * boxFarScale();
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

// the parts of a cylinder (shapePart())
#define CRT_CYLINDER_SIDE 0u
#define CRT_CYLINDER_BOTTOM 1u
#define CRT_CYLINDER_TOP 2u

// which part of the unit primitive a point on it lies on, what HitRecord.primitive holds for a
// shape: a box's face in boxFace() order, a cylinder's side or cap by whichever surface the point
// is nearer to (the rim at radius 1, or a cap at |y| = 0.5), and 0 for a shape of one part
uint shapePart(uint kind, vec3 p)
{
	if (kind == CRT_SHAPE_BOX) {
		return boxFace(p);
	}
	if (kind == CRT_SHAPE_CYLINDER) {
		const float radial = sqrt(p.x * p.x + p.z * p.z);
		if (abs(0.5 - abs(p.y)) < abs(1.0 - radial)) {
			return p.y >= 0.0 ? CRT_CYLINDER_TOP : CRT_CYLINDER_BOTTOM;
		}
		return CRT_CYLINDER_SIDE;
	}
	return 0u;
}

// the outward object-space normal at a point on the unit primitive, not normalised
vec3 shapeNormal(uint kind, vec3 p)
{
	if (kind == CRT_SHAPE_SPHERE) {
		return p;
	}
	if (kind == CRT_SHAPE_BOX) {
		// the face is the axis the point lies furthest out along
		const uint face = boxFace(p);
		vec3 n = vec3(0.0);
		n[face >> 1u] = (face & 1u) != 0u ? 1.0 : -1.0;
		return n;
	}
	if (kind == CRT_SHAPE_CYLINDER) {
		const uint part = shapePart(kind, p);
		if (part != CRT_CYLINDER_SIDE) {
			return vec3(0.0, part == CRT_CYLINDER_TOP ? 1.0 : -1.0, 0.0);
		}
		return vec3(p.x, 0.0, p.z);
	}
	// plane and quad
	return vec3(0.0, 1.0, 0.0);
}

// A hit point `p` on the unit primitive, as the ray computed it in float, brought exactly onto the
// primitive, with the absolute error its coordinates then have (PBR 4ed 6.8.5). A sphere's point is
// scaled back to the radius, a cylinder's side to its rim, and a coordinate the primitive fixes (a
// quad's y, a box face's axis, a cap's y) is set to it and has no error: the bound along the
// normal, which is the one the spawn offset reads, is then zero for the flat parts. The rest are
// bounded by gamma(5), the error of o + t d with a t that is itself only known that well.
// src/shape.cpp shapeHitPoint
vec3 shapeReproject(uint kind, vec3 p, out vec3 pError)
{
	if (kind == CRT_SHAPE_SPHERE) {
		p *= 1.0 / length(p);
		pError = CRT_GAMMA(5) * abs(p);
	} else if (kind == CRT_SHAPE_PLANE || kind == CRT_SHAPE_QUAD) {
		p.y = 0.0;
		pError = CRT_GAMMA(5) * abs(p);
	} else if (kind == CRT_SHAPE_BOX) {
		const uint face = boxFace(p);
		p[face >> 1u] = (face & 1u) != 0u ? 0.5 : -0.5;
		pError = CRT_GAMMA(5) * abs(p);
		pError[face >> 1u] = 0.0;
	} else if (kind == CRT_SHAPE_CYLINDER) {
		const uint part = shapePart(kind, p);
		if (part == CRT_CYLINDER_SIDE) {
			p.xz *= 1.0 / length(p.xz);
			pError = CRT_GAMMA(5) * abs(p);
		} else {
			p.y = part == CRT_CYLINDER_TOP ? 0.5 : -0.5;
			pError = CRT_GAMMA(5) * abs(p);
			pError.y = 0.0;
		}
	} else {
		// a kind this function does not know: the point as it is, with the error of o + t d
		pError = CRT_GAMMA(5) * abs(p);
	}
	return p;
}

// The point a shape hit landed on, in the unit primitive's object space, and the error of its
// coordinates. Kernel 02 (for the part and the uv) and crt_interaction.glsl both call this with the
// ray that found the hit, so they agree on the point, and on the part and uv derived from it.
// Without error-bounded offsets it is the ray's point carried into object space, as the tracer has
// always taken it, with no error to report. With them it is the object-space ray's point, put on
// the primitive by shapeReproject()
vec3 shapeObjectHit(GpuInstance instance, vec3 origin, vec3 direction, float t, out vec3 pError)
{
	if ((pc.flags & CRT_FLAG_ERROR_BOUNDS) == 0u) {
		pError = vec3(0.0);
		return toObjectPoint(instance, fma(vec3(t), direction, origin));
	}
	vec3 localOrigin;
	vec3 localDirection;
	toObjectRay(instance, origin, direction, localOrigin, localDirection);
	return shapeReproject(instance.triangleBase, fma(vec3(t), localDirection, localOrigin), pError);
}

// an angle about +y in [0, 1), measured from +x towards `towardsZ` (-1 or 1) times +z
float shapeTurn(vec3 p, float towardsZ)
{
	const float phi = atan(towardsZ * p.z, p.x);
	return (phi < 0.0 ? phi + 2.0 * CRT_SHAPE_PI : phi) / (2.0 * CRT_SHAPE_PI);
}

// the (u, v) of a point on `part` of the unit primitive (the table at the top of this file). Kernel
// 02 stores it in HitRecord.barycentrics, so the angles are solved once per hit
vec2 shapeUv(uint kind, uint part, vec3 p)
{
	if (kind == CRT_SHAPE_SPHERE) {
		return vec2(shapeTurn(p, -1.0), acos(clamp(p.y, -1.0, 1.0)) / CRT_SHAPE_PI);
	}
	if (kind == CRT_SHAPE_PLANE) {
		return p.xz;
	}
	if (kind == CRT_SHAPE_QUAD) {
		return p.xz + 0.5;
	}
	if (kind == CRT_SHAPE_BOX) {
		switch (part) {
		case 0u:
			return vec2(p.z + 0.5, 0.5 - p.y);
		case 1u:
			return vec2(0.5 - p.z, 0.5 - p.y);
		case 2u:
			return vec2(0.5 - p.x, p.z + 0.5);
		case 3u:
			return vec2(p.x + 0.5, p.z + 0.5);
		case 4u:
			return vec2(0.5 - p.x, 0.5 - p.y);
		}
		return vec2(p.x + 0.5, 0.5 - p.y);
	}
	if (kind == CRT_SHAPE_CYLINDER) {
		if (part == CRT_CYLINDER_SIDE) {
			return vec2(shapeTurn(p, -1.0), 0.5 - p.y);
		}
		const float radial = sqrt(p.x * p.x + p.z * p.z);
		return vec2(shapeTurn(p, part == CRT_CYLINDER_TOP ? -1.0 : 1.0), radial);
	}
	return vec2(0.0);
}

// The object-space derivatives of the point and of the normal (shapeNormal(), unnormalised) with
// respect to shapeUv()'s u and v, from the point alone. The sphere's u derivative vanishes at its
// poles and a cap's v derivative is taken along +x at its centre: wherever dp/du is zero, a
// tangent frame has to come from the normal instead
void shapeDerivatives(uint kind, uint part, vec3 p, out vec3 dpdu, out vec3 dpdv, out vec3 dndu, out vec3 dndv)
{
	dpdu = vec3(1.0, 0.0, 0.0);
	dpdv = vec3(0.0, 0.0, 1.0);
	dndu = vec3(0.0);
	dndv = vec3(0.0);
	const float turn = 2.0 * CRT_SHAPE_PI;
	if (kind == CRT_SHAPE_SPHERE) {
		// p = (sin(theta) cos(phi), cos(theta), -sin(theta) sin(phi)), and n = p
		const float sinTheta = sqrt(p.x * p.x + p.z * p.z);
		const vec2 phi = sinTheta > 0.0 ? vec2(p.x, -p.z) / sinTheta : vec2(1.0, 0.0);
		dpdu = turn * vec3(p.z, 0.0, -p.x);
		dpdv = CRT_SHAPE_PI * vec3(p.y * phi.x, -sinTheta, -p.y * phi.y);
		dndu = dpdu;
		dndv = dpdv;
	} else if (kind == CRT_SHAPE_BOX) {
		switch (part) {
		case 0u:
			dpdu = vec3(0.0, 0.0, 1.0);
			dpdv = vec3(0.0, -1.0, 0.0);
			break;
		case 1u:
			dpdu = vec3(0.0, 0.0, -1.0);
			dpdv = vec3(0.0, -1.0, 0.0);
			break;
		case 2u:
			dpdu = vec3(-1.0, 0.0, 0.0);
			dpdv = vec3(0.0, 0.0, 1.0);
			break;
		case 3u:
			break;
		case 4u:
			dpdu = vec3(-1.0, 0.0, 0.0);
			dpdv = vec3(0.0, -1.0, 0.0);
			break;
		default:
			dpdv = vec3(0.0, -1.0, 0.0);
			break;
		}
	} else if (kind == CRT_SHAPE_CYLINDER) {
		if (part == CRT_CYLINDER_SIDE) {
			// p = (cos(phi), y, -sin(phi)), and n = (x, 0, z)
			dpdu = turn * vec3(p.z, 0.0, -p.x);
			dpdv = vec3(0.0, -1.0, 0.0);
			dndu = dpdu;
		} else {
			// the top cap turns as the side does and the bottom cap the other way, each seen from outside
			const float towardsZ = part == CRT_CYLINDER_TOP ? -1.0 : 1.0;
			dpdu = turn * vec3(-towardsZ * p.z, 0.0, towardsZ * p.x);
			const float radial = sqrt(p.x * p.x + p.z * p.z);
			dpdv = radial > 0.0 ? vec3(p.x, 0.0, p.z) / radial : vec3(1.0, 0.0, 0.0);
		}
	}
	// plane and quad: the defaults
}

#endif
