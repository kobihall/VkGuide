// OPTICAL BODIES (docs/plans/completed/physoptics-optical-surfaces.md): a lens, a mirror, a prism or a cube as a list of exact
// surfaces, each a sag function in its own frame cut to an aperture, read from binding 55. The line-for-line mirror of
// the float instantiation of src/optic_surface.cpp: change both together, and run bin/optics_test and the GPU
// self-test.
//
// A body is a shape instance whose kind is CRT_SHAPE_OPTIC. Its GpuInstance names its block: attributeBase the first
// surface record, triangleCount the surface count, materialBase its table in instanceMaterials[] (one material per slot,
// then one coating table per surface), opticMedia its media table and opticSlots its slot count. The ray arrives in the
// body frame (the instance's object space); each surface carries it on into its own frame with the rows of its record.
//
// A RECORD is six vec4 (src/optic_surface.h packOpticBody()): the rows of the body -> surface transform; (c, k, 0, 0),
// or (radius, 0, 0, 0) for a cylinder; the aperture (a, b, center); and the bits: shape | aperture << 8 | flags << 16,
// the material slot, the media (minus | plus << 16) and the payload's offset (a cylinder's bounds, an asphere's terms, a
// polygon's corners).
//
// Every surface has a normal, and its plus side is the side the normal points to: +z for a conic, outward for a
// cylinder. A hit arriving against the normal comes from the plus medium.
//
// Included by crt_traverse.glsl and crt_interaction.glsl. Requires crt_common.glsl.

#ifndef CRT_OPTIC_GLSL
#define CRT_OPTIC_GLSL

// outside ShapeKind, so the Geometry menu never offers it (src/shape.h GPU_SHAPE_OPTIC)
#define CRT_SHAPE_OPTIC 255u

// src/optic_surface.h SurfaceShape and ApertureKind
#define CRT_OPTIC_SHAPE_CONIC 0u
#define CRT_OPTIC_SHAPE_CYLINDER 1u
#define CRT_OPTIC_SHAPE_ASPHERE 2u
#define CRT_OPTIC_APERTURE_NONE 0u
#define CRT_OPTIC_APERTURE_CIRCLE 1u
#define CRT_OPTIC_APERTURE_ANNULUS 2u
#define CRT_OPTIC_APERTURE_RECTANGLE 3u
#define CRT_OPTIC_APERTURE_POLYGON 4u
#define CRT_OPTIC_APERTURE_ELLIPSE 5u

#define CRT_OPTIC_SURFACE_VEC4S 6u
// src/optic_surface.h APERTURE_SLACK and ASPHERE_NEWTON_STEPS
#define CRT_APERTURE_SLACK 4e-6
#define CRT_ASPHERE_NEWTON_STEPS 8
#define CRT_OPTIC_POLYGON_VERTICES 6u

struct OpticSurfaceRecord {
	vec4 rows[3];
	vec4 shape;
	vec4 aperture;
	uvec4 bits;
};

OpticSurfaceRecord opticSurfaceRecord(uint offset)
{
	OpticSurfaceRecord r;
	r.rows[0] = opticData[offset];
	r.rows[1] = opticData[offset + 1u];
	r.rows[2] = opticData[offset + 2u];
	r.shape = opticData[offset + 3u];
	r.aperture = opticData[offset + 4u];
	r.bits = floatBitsToUint(opticData[offset + 5u]);
	return r;
}

uint opticShape(OpticSurfaceRecord r)
{
	return r.bits.x & 0xFFu;
}

uint opticApertureKind(OpticSurfaceRecord r)
{
	return (r.bits.x >> 8u) & 0xFFu;
}

uint opticSlot(OpticSurfaceRecord r)
{
	return r.bits.y;
}

uint opticMediumMinus(OpticSurfaceRecord r)
{
	return r.bits.z & 0xFFFFu;
}

uint opticMediumPlus(OpticSurfaceRecord r)
{
	return r.bits.z >> 16u;
}

vec3 opticToSurfacePoint(OpticSurfaceRecord r, vec3 p)
{
	return vec3(dot(r.rows[0].xyz, p) + r.rows[0].w, dot(r.rows[1].xyz, p) + r.rows[1].w, dot(r.rows[2].xyz, p) + r.rows[2].w);
}

vec3 opticToSurfaceVector(OpticSurfaceRecord r, vec3 v)
{
	return vec3(dot(r.rows[0].xyz, v), dot(r.rows[1].xyz, v), dot(r.rows[2].xyz, v));
}

vec3 opticToBodyVector(OpticSurfaceRecord r, vec3 v)
{
	return v.x * r.rows[0].xyz + v.y * r.rows[1].xyz + v.z * r.rows[2].xyz;
}

// the surface frame's origin in the body frame, -R^T t
vec3 opticSurfaceOrigin(OpticSurfaceRecord r)
{
	return -(r.rows[0].w * r.rows[0].xyz + r.rows[1].w * r.rows[1].xyz + r.rows[2].w * r.rows[2].xyz);
}

float opticSignNonZero(float v)
{
	return v >= 0.0 ? 1.0 : -1.0;
}

bool opticConicSagDerivatives(float c, float k, float rho2, out float sag, out float slopeOverRho, out float second)
{
	sag = 0.0;
	slopeOverRho = 0.0;
	second = 0.0;
	const float kz = 1.0 + k;
	const float root = 1.0 - kz * c * c * rho2;
	if (root < 0.0) {
		return false;
	}
	const float s = sqrt(root);
	sag = c * rho2 / (1.0 + s);
	slopeOverRho = s > 0.0 ? c / s : 0.0;
	second = s > 0.0 ? c / (root * s) : 0.0;
	return true;
}

// an asphere's polynomial from its payload: alpha_2..alpha_8 in the first two vec4
void opticAsphereTerms(uint payload, float rho2, out float value, out float slopeOverRho, out float second)
{
	const vec4 a = opticData[payload];
	const vec4 b = opticData[payload + 1u];
	const float alpha[7] = float[7](a.x, a.y, a.z, a.w, b.x, b.y, b.z);
	value = 0.0;
	slopeOverRho = 0.0;
	second = 0.0;
	for (int i = 6; i >= 0; i--) {
		const float power = float(2 * (i + 2));
		value = value * rho2 + alpha[i];
		slopeOverRho = slopeOverRho * rho2 + power * alpha[i];
		second = second * rho2 + power * (power - 1.0) * alpha[i];
	}
	const float rho4 = rho2 * rho2;
	value *= rho4;
	slopeOverRho *= rho2;
	second *= rho2;
}

// the sag of a conic or asphere surface at (x, y) of its frame; false where the conic has no point there
bool opticSurfaceSag(OpticSurfaceRecord r, vec2 xy, out float sag)
{
	const float rho2 = xy.x * xy.x + xy.y * xy.y;
	float slope, second;
	if (!opticConicSagDerivatives(r.shape.x, r.shape.y, rho2, sag, slope, second)) {
		return false;
	}
	if (opticShape(r) == CRT_OPTIC_SHAPE_ASPHERE) {
		float polynomial, polynomialSlope, polynomialSecond;
		opticAsphereTerms(r.bits.w, rho2, polynomial, polynomialSlope, polynomialSecond);
		sag += polynomial;
	}
	return true;
}

// a cylinder bound's z at (x, y): bound 0 is the lower, 1 the upper
float opticBoundZ(uint payload, uint bound, vec2 xy)
{
	const vec4 a = opticData[payload + 2u * bound];
	const vec4 b = opticData[payload + 2u * bound + 1u];
	float z = a.x + a.y * xy.x + a.z * xy.y;
	if (a.w != 0.0) {
		const vec2 rr = xy - b.yz;
		float sag, slope, second;
		z += opticConicSagDerivatives(a.w, b.x, rr.x * rr.x + rr.y * rr.y, sag, slope, second) ? sag : 1.0 / a.w;
	}
	return z;
}

bool opticInsideAperture(OpticSurfaceRecord r, vec2 xy, float relativeSlack)
{
	const float grow = 1.0 + relativeSlack;
	const vec4 ap = r.aperture;
	const vec2 p = xy - ap.zw;
	const uint kind = opticApertureKind(r);
	if (kind == CRT_OPTIC_APERTURE_NONE) {
		return true;
	}
	if (kind == CRT_OPTIC_APERTURE_CIRCLE) {
		const float radius = ap.x * grow;
		return p.x * p.x + p.y * p.y <= radius * radius;
	}
	if (kind == CRT_OPTIC_APERTURE_ANNULUS) {
		const float rho2 = p.x * p.x + p.y * p.y;
		const float inner = ap.x / grow;
		const float outer = ap.y * grow;
		return rho2 >= inner * inner && rho2 <= outer * outer;
	}
	if (kind == CRT_OPTIC_APERTURE_RECTANGLE) {
		return abs(p.x) <= ap.x * grow && abs(p.y) <= ap.y * grow;
	}
	if (kind == CRT_OPTIC_APERTURE_ELLIPSE) {
		const float u = p.x / ap.x;
		const float v = p.y / ap.y;
		return u * u + v * v <= grow * grow;
	}
	if (kind == CRT_OPTIC_APERTURE_POLYGON) {
		const uint count = min(uint(ap.x), CRT_OPTIC_POLYGON_VERTICES);
		const uint payload = r.bits.w;
		vec2 corners[6];
		for (uint i = 0u; i < 3u; i++) {
			const vec4 pair = opticData[payload + i];
			corners[2u * i] = pair.xy;
			corners[2u * i + 1u] = pair.zw;
		}
		float size = 0.0;
		for (uint i = 0u; i < count; i++) {
			size = max(size, abs(corners[i].x) + abs(corners[i].y));
		}
		const float slack = relativeSlack * size;
		for (uint i = 0u; i < count; i++) {
			const vec2 a = corners[i];
			const vec2 b = corners[(i + 1u) % count];
			const vec2 edge = b - a;
			const float edgeLength = sqrt(edge.x * edge.x + edge.y * edge.y);
			const float side = edge.x * (p.y - a.y) - edge.y * (p.x - a.x);
			if (side < -slack * edgeLength) {
				return false;
			}
		}
		return count >= 3u;
	}
	return false;
}

bool opticStableRoots(float a, float b, float e, out float t0, out float t1, out bool valid0, out bool valid1)
{
	t0 = 0.0;
	t1 = 0.0;
	valid0 = false;
	valid1 = false;
	const float discriminant = b * b - 4.0 * a * e;
	if (discriminant < 0.0) {
		return false;
	}
	const float q = -0.5 * (b + opticSignNonZero(b) * sqrt(discriminant));
	valid0 = q != 0.0;
	valid1 = a != 0.0;
	t0 = valid0 ? e / q : 0.0;
	t1 = valid1 ? q / a : 0.0;
	return true;
}

// one Newton step, taken only when it moves t by at most 1e-4 of t (src/optic_surface.cpp limitedStep())
float opticLimitedStep(float t, float f, float df)
{
	if (df == 0.0) {
		return t;
	}
	const float step = f / df;
	return abs(step) <= 1e-4 * abs(t) ? t - step : t;
}

float opticRefineConic(float c, float kz, vec3 o, vec3 d, float t)
{
	const vec3 p = o + t * d;
	const float f = c * (p.x * p.x + p.y * p.y + kz * p.z * p.z) - 2.0 * p.z;
	const float df = 2.0 * (c * (p.x * d.x + p.y * d.y + kz * p.z * d.z) - d.z);
	return opticLimitedStep(t, f, df);
}

float opticRefineCylinder(float radius, vec3 o, vec3 d, float t)
{
	const vec3 p = o + t * d;
	const float f = p.x * p.x + p.y * p.y - radius * radius;
	const float df = 2.0 * (p.x * d.x + p.y * d.y);
	return opticLimitedStep(t, f, df);
}

bool opticOnAsphere(OpticSurfaceRecord r, vec3 p)
{
	float sag;
	if (!opticSurfaceSag(r, p.xy, sag)) {
		return false;
	}
	const float scale = max(max(abs(p.x), abs(p.y)), abs(p.z));
	return abs(p.z - sag) <= 1e-5 * scale;
}

float opticRefineAsphere(OpticSurfaceRecord r, vec3 o, vec3 d, float t)
{
	for (int i = 0; i < CRT_ASPHERE_NEWTON_STEPS; i++) {
		const vec3 p = o + t * d;
		const float rho2 = p.x * p.x + p.y * p.y;
		float sag, slopeOverRho, second;
		if (!opticConicSagDerivatives(r.shape.x, r.shape.y, rho2, sag, slopeOverRho, second)) {
			return t;
		}
		float polynomial, polynomialSlope, polynomialSecond;
		opticAsphereTerms(r.bits.w, rho2, polynomial, polynomialSlope, polynomialSecond);
		const float g = p.z - (sag + polynomial);
		const float gPrime = d.z - (slopeOverRho + polynomialSlope) * (p.x * d.x + p.y * d.y);
		if (gPrime == 0.0) {
			return t;
		}
		t -= g / gPrime;
	}
	return t;
}

bool opticWithinCylinderBounds(OpticSurfaceRecord r, vec3 p)
{
	const float slack = CRT_APERTURE_SLACK * r.shape.x;
	return p.z >= opticBoundZ(r.bits.w, 0u, p.xy) - slack && p.z <= opticBoundZ(r.bits.w, 1u, p.xy) + slack;
}

bool opticAcceptConicPoint(OpticSurfaceRecord r, vec3 p)
{
	const float kz = 1.0 + r.shape.y;
	return kz * r.shape.x * p.z < 1.0 && opticInsideAperture(r, p.xy, CRT_APERTURE_SLACK);
}

// the nearest hit of one surface in [tMin, tMax] by a ray of its frame
bool opticIntersectSurface(OpticSurfaceRecord r, vec3 oIn, vec3 d, float tMin, float tMax, out float t)
{
	const float tLimit = tMax;
	const uint shape = opticShape(r);
	// re-centered on the closest approach to the frame's origin, so the quadratic is formed from coordinates the size
	// of the surface
	float shift = 0.0;
	if (shape == CRT_OPTIC_SHAPE_CYLINDER) {
		const float dd = d.x * d.x + d.y * d.y;
		shift = dd > 0.0 ? -(oIn.x * d.x + oIn.y * d.y) / dd : 0.0;
	} else {
		shift = -dot(oIn, d) / dot(d, d);
	}
	const vec3 o = oIn + shift * d;
	float best = tMax - shift;
	const float lo = tMin - shift;
	float roots[2];
	bool valid[2];
	if (shape == CRT_OPTIC_SHAPE_CYLINDER) {
		const float radius = r.shape.x;
		const float a = d.x * d.x + d.y * d.y;
		const float b = 2.0 * (o.x * d.x + o.y * d.y);
		const float e = o.x * o.x + o.y * o.y - radius * radius;
		if (opticStableRoots(a, b, e, roots[0], roots[1], valid[0], valid[1])) {
			for (int i = 0; i < 2; i++) {
				if (!valid[i]) {
					continue;
				}
				const float candidate = opticRefineCylinder(radius, o, d, roots[i]);
				if (candidate >= lo && candidate <= best && opticWithinCylinderBounds(r, o + candidate * d)) {
					best = candidate;
				}
			}
		}
	} else {
		const float c = r.shape.x;
		const float kz = 1.0 + r.shape.y;
		const float a = c * (d.x * d.x + d.y * d.y + kz * d.z * d.z);
		const float b = 2.0 * (c * (o.x * d.x + o.y * d.y + kz * o.z * d.z) - d.z);
		const float e = c * (o.x * o.x + o.y * o.y + kz * o.z * o.z) - 2.0 * o.z;
		if (opticStableRoots(a, b, e, roots[0], roots[1], valid[0], valid[1])) {
			for (int i = 0; i < 2; i++) {
				if (!valid[i]) {
					continue;
				}
				const float candidate = shape == CRT_OPTIC_SHAPE_ASPHERE ? opticRefineAsphere(r, o, d, roots[i]) : opticRefineConic(c, kz, o, d, roots[i]);
				if (candidate >= lo && candidate <= best && opticAcceptConicPoint(r, o + candidate * d) && (shape != CRT_OPTIC_SHAPE_ASPHERE || opticOnAsphere(r, o + candidate * d))) {
					best = candidate;
				}
			}
		}
	}
	t = best + shift;
	return best < tLimit - shift;
}

// the unit normal in the surface frame, toward the plus side
vec3 opticSurfaceNormal(OpticSurfaceRecord r, vec3 p)
{
	const uint shape = opticShape(r);
	if (shape == CRT_OPTIC_SHAPE_CYLINDER) {
		return normalize(vec3(p.x, p.y, 0.0));
	}
	if (shape == CRT_OPTIC_SHAPE_ASPHERE) {
		const float rho2 = p.x * p.x + p.y * p.y;
		float sag, slopeOverRho, second;
		opticConicSagDerivatives(r.shape.x, r.shape.y, rho2, sag, slopeOverRho, second);
		float polynomial, polynomialSlope, polynomialSecond;
		opticAsphereTerms(r.bits.w, rho2, polynomial, polynomialSlope, polynomialSecond);
		const float g = slopeOverRho + polynomialSlope;
		return normalize(vec3(-g * p.x, -g * p.y, 1.0));
	}
	const float c = r.shape.x;
	const float kz = 1.0 + r.shape.y;
	return normalize(vec3(-c * p.x, -c * p.y, 1.0 - c * kz * p.z));
}

// the record of surface `index` of an optic instance
OpticSurfaceRecord opticInstanceSurface(GpuInstance instance, uint index)
{
	return opticSurfaceRecord(instance.attributeBase + CRT_OPTIC_SURFACE_VEC4S * index);
}

// a point of the surface frame in the body frame
vec3 opticToBodyPoint(OpticSurfaceRecord r, vec3 p)
{
	return opticToBodyVector(r, p) + opticSurfaceOrigin(r);
}

// A hit point of the surface frame brought onto the surface, as shapeReproject() brings a shape's onto its primitive
// (PBR 4ed 6.8.5): a cylinder's radially, a conic's by one step along its implicit function's gradient (exact for a
// plane, whose z it zeroes), an asphere's to its sag. The ray's point carries the rounding of o + t d, which for a ray
// from far away is far larger than the point's own size can bound; on the surface, only the coordinates' rounding is left
vec3 opticReproject(OpticSurfaceRecord r, vec3 p)
{
	const uint shape = opticShape(r);
	if (shape == CRT_OPTIC_SHAPE_CYLINDER) {
		const float rho = length(p.xy);
		if (rho > 0.0) {
			p.xy *= r.shape.x / rho;
		}
		return p;
	}
	if (shape == CRT_OPTIC_SHAPE_ASPHERE) {
		float sag;
		if (opticSurfaceSag(r, p.xy, sag)) {
			p.z = sag;
		}
		return p;
	}
	const float c = r.shape.x;
	const float kz = 1.0 + r.shape.y;
	const float f = c * (p.x * p.x + p.y * p.y + kz * p.z * p.z) - 2.0 * p.z;
	const vec3 gradient = vec3(2.0 * c * p.x, 2.0 * c * p.y, 2.0 * c * kz * p.z - 2.0);
	return p - (f / dot(gradient, gradient)) * gradient;
}

// The point an optic hit landed on in the body frame, and its error: without error-bounded offsets the world ray's
// point carried into the body, as a shape's is; with them the body-frame ray's point brought onto the surface
// (opticReproject()). Its bound is the same on every axis, gamma(12) of the point's largest coordinate and the surface
// origin's: the intersector re-centers a ray on its closest approach to the surface's origin, which rounds the t of a
// ray spawned on the surface by that much, so a bound that vanished along a plane's normal (at its center, z = 0
// exactly) would let the ray find the plane it left (src/bvh_scene.cpp sceneHitPoint()). Kernel 02 and
// crt_interaction.glsl both call it with the ray that found the hit
vec3 opticObjectHit(GpuInstance instance, OpticSurfaceRecord r, vec3 origin, vec3 direction, float t, out vec3 pError)
{
	if ((pc.flags & CRT_FLAG_ERROR_BOUNDS) == 0u) {
		pError = vec3(0.0);
		return toObjectPoint(instance, fma(vec3(t), direction, origin));
	}
	vec3 localOrigin;
	vec3 localDirection;
	toObjectRay(instance, origin, direction, localOrigin, localDirection);
	const vec3 p = opticToBodyPoint(r, opticReproject(r, opticToSurfacePoint(r, fma(vec3(t), localDirection, localOrigin))));
	const vec3 a = abs(p);
	const vec3 o = abs(opticSurfaceOrigin(r));
	pError = vec3(CRT_GAMMA(12) * (max(max(a.x, a.y), a.z) + max(max(o.x, o.y), o.z)));
	return p;
}

#endif
