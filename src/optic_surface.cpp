#include <optic_surface.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

// The float instantiation of everything below is the mirror of shaders/rt/include/crt_optic.glsl: the same operations
// in the same order, so that a fixed ray gives the same surface and a t within float rounding on both sides (the GPU
// self-test compares them). The double one is what the beam solver and the tests' references use.

namespace {

template <typename Real>
Real signNonZero(Real v)
{
	return v >= Real(0) ? Real(1) : Real(-1);
}

// the sag of a rotationally symmetric conic at rho^2, and its first and second derivatives with respect to rho
template <typename Real>
bool conicSagDerivatives(Real c, Real k, Real rho2, Real& sag, Real& slopeOverRho, Real& second)
{
	const Real kz = Real(1) + k;
	const Real root = Real(1) - kz * c * c * rho2;
	if (root < Real(0)) {
		return false;
	}
	const Real s = std::sqrt(root);
	sag = c * rho2 / (Real(1) + s);
	// s'(rho) = c rho / sqrt(1 - kz c^2 rho^2), kept as s'/rho so rho = 0 is not special
	slopeOverRho = s > Real(0) ? c / s : Real(0);
	second = s > Real(0) ? c / (root * s) : Real(0);
	return true;
}

// the asphere's polynomial: sum alpha_i rho^(2i), i = 2..8, and its derivatives with respect to rho (as d/rho) and
// rho twice
template <typename Real>
void asphereTerms(const std::array<Real, OPTIC_ASPHERE_TERMS>& alpha, Real rho2, Real& value, Real& slopeOverRho, Real& second)
{
	value = Real(0);
	slopeOverRho = Real(0);
	second = Real(0);
	// rho^(2i) for i = 2.. by Horner's rule from the highest term down
	for (int i = (int)OPTIC_ASPHERE_TERMS - 1; i >= 0; i--) {
		const Real power = Real(2 * (i + 2));
		value = value * rho2 + alpha[(size_t)i];
		slopeOverRho = slopeOverRho * rho2 + power * alpha[(size_t)i];
		second = second * rho2 + power * (power - Real(1)) * alpha[(size_t)i];
	}
	// value holds sum alpha_i rho^(2i - 4): multiply back the rho^4 the loop factored out
	const Real rho4 = rho2 * rho2;
	value *= rho4;
	// d/drho of alpha rho^p is p alpha rho^(p-1); over rho, p alpha rho^(p-2): the loop's sum times rho^2
	slopeOverRho *= rho2;
	// d2/drho2 of alpha rho^p is p (p - 1) alpha rho^(p-2)
	second *= rho2;
}

// the conic's two roots in the stable form: q = -(b + sign(b) sqrt(b^2 - 4ae)) / 2, t = e / q and q / a, the one that
// never subtracts nearly equal numbers. A root that would divide by zero is marked invalid (a flag, not a NaN: the GPU
// compiles with fast math, where a NaN test is not reliable)
template <typename Real>
bool stableRoots(Real a, Real b, Real e, Real& t0, Real& t1, bool& valid0, bool& valid1)
{
	const Real discriminant = b * b - Real(4) * a * e;
	if (discriminant < Real(0)) {
		return false;
	}
	const Real q = Real(-0.5) * (b + signNonZero(b) * std::sqrt(discriminant));
	valid0 = q != Real(0);
	valid1 = a != Real(0);
	t0 = valid0 ? e / q : Real(0);
	t1 = valid1 ? q / a : Real(0);
	return true;
}

// whether t is in range and lower than the best so far
template <typename Real>
bool inRange(Real t, Real tMin, Real tMax)
{
	return t >= tMin && t <= tMax;
}

template <typename Real>
bool withinCylinderBounds(const OpticSurfaceT<Real>& s, const Vec3T<Real>& p)
{
	const Vec2T<Real> xy(p.x, p.y);
	const Real slack = Real(APERTURE_SLACK) * s.radius;
	return p.z >= boundZ(s.lower, xy) - slack && p.z <= boundZ(s.upper, xy) + slack;
}

// the conic's branch through its vertex, and the aperture
template <typename Real>
bool acceptConicPoint(const OpticSurfaceT<Real>& s, const Vec3T<Real>& p)
{
	const Real kz = Real(1) + s.conic;
	return kz * s.curvature * p.z < Real(1) && insideAperture(s.aperture, Vec2T<Real>(p.x, p.y));
}

// One Newton step on the implicit function along the ray, at the root: F and its derivative formed from the hit point,
// whose coordinates are the size of the surface, rather than from the origin, which may be far away. A root of the
// quadratic carries the rounding of c |o|^2 against 2 o_z divided by a q that is small for a ray across the axis; the
// step leaves only the hit point's own rounding (Ray Tracing Gems ch. 7, the refinement after the stable root). Near a
// tangent the derivative vanishes and a step could land anywhere, so a step larger than 1e-4 of t is not taken:
// a well-conditioned root only ever moves by its rounding
template <typename Real>
Real limitedStep(Real t, Real f, Real df)
{
	if (df == Real(0)) {
		return t;
	}
	const Real step = f / df;
	return std::abs(step) <= Real(1e-4) * std::abs(t) ? t - step : t;
}

template <typename Real>
Real refineConic(Real c, Real kz, const Vec3T<Real>& o, const Vec3T<Real>& d, Real t)
{
	const Vec3T<Real> p = o + t * d;
	const Real f = c * (p.x * p.x + p.y * p.y + kz * p.z * p.z) - Real(2) * p.z;
	const Real df = Real(2) * (c * (p.x * d.x + p.y * d.y + kz * p.z * d.z) - d.z);
	return limitedStep(t, f, df);
}

template <typename Real>
Real refineCylinder(Real radius, const Vec3T<Real>& o, const Vec3T<Real>& d, Real t)
{
	const Vec3T<Real> p = o + t * d;
	const Real f = p.x * p.x + p.y * p.y - radius * radius;
	const Real df = Real(2) * (p.x * d.x + p.y * d.y);
	return limitedStep(t, f, df);
}

// whether a point lies on an even asphere within a relative residual: a Newton refinement that has not converged (near a
// tangent) gives a point off the surface, which is a miss, not a hit
template <typename Real>
bool onAsphere(const OpticSurfaceT<Real>& s, const Vec3T<Real>& p)
{
	const std::optional<Real> sag = surfaceSag(s, Vec2T<Real>(p.x, p.y));
	if (!sag) {
		return false;
	}
	const Real scale = std::max({ std::abs(p.x), std::abs(p.y), std::abs(p.z) });
	return std::abs(p.z - *sag) <= Real(1e-5) * scale;
}

// Newton's method on z - sag(rho) = 0 along the ray, from a conic root
template <typename Real>
Real refineAsphere(const OpticSurfaceT<Real>& s, const Vec3T<Real>& o, const Vec3T<Real>& d, Real t)
{
	for (int i = 0; i < ASPHERE_NEWTON_STEPS; i++) {
		const Vec3T<Real> p = o + t * d;
		const Real rho2 = p.x * p.x + p.y * p.y;
		Real sag, slopeOverRho, second;
		if (!conicSagDerivatives(s.curvature, s.conic, rho2, sag, slopeOverRho, second)) {
			return t;
		}
		Real polynomial, polynomialSlope, polynomialSecond;
		asphereTerms(s.asphere, rho2, polynomial, polynomialSlope, polynomialSecond);
		const Real g = p.z - (sag + polynomial);
		// d rho / dt = (x dx + y dy) / rho, so d sag / dt = (s' / rho) (x dx + y dy)
		const Real gPrime = d.z - (slopeOverRho + polynomialSlope) * (p.x * d.x + p.y * d.y);
		if (gPrime == Real(0)) {
			return t;
		}
		t -= g / gPrime;
	}
	return t;
}

}

template <typename Real>
std::optional<Real> conicSag(Real curvature, Real conic, Real rho2)
{
	Real sag, slope, second;
	if (!conicSagDerivatives(curvature, conic, rho2, sag, slope, second)) {
		return std::nullopt;
	}
	return sag;
}

template <typename Real>
std::optional<Real> surfaceSag(const OpticSurfaceT<Real>& surface, Vec2T<Real> xy)
{
	const Real rho2 = xy.x * xy.x + xy.y * xy.y;
	std::optional<Real> sag = conicSag(surface.curvature, surface.conic, rho2);
	if (sag && surface.shape == SurfaceShape::EvenAsphere) {
		Real polynomial, slope, second;
		asphereTerms(surface.asphere, rho2, polynomial, slope, second);
		*sag += polynomial;
	}
	return sag;
}

template <typename Real>
Real boundZ(const CylinderBoundT<Real>& bound, Vec2T<Real> xy)
{
	Real z = bound.z0 + bound.slope.x * xy.x + bound.slope.y * xy.y;
	if (bound.curvature != Real(0)) {
		const Vec2T<Real> r = xy - bound.center;
		const std::optional<Real> sag = conicSag(bound.curvature, bound.conic, r.x * r.x + r.y * r.y);
		// past a sphere's rim the bound is the sphere's equator, the furthest the surface reaches
		z += sag ? *sag : Real(1) / bound.curvature;
	}
	return z;
}

template <typename Real>
void finishSurface(OpticSurfaceT<Real>& surface)
{
	// the inverse of a rotation is its transpose: s = R^T (p - o)
	const Mat3T<Real>& r = surface.placement.rotation;
	const Vec3T<Real>& o = surface.placement.origin;
	for (int row = 0; row < 3; row++) {
		const Vec3T<Real> axis(r[row][0], r[row][1], r[row][2]);
		surface.rows[(size_t)row] = Vec4T<Real>(axis, -glm::dot(axis, o));
	}
}

template <typename Real>
Vec3T<Real> toSurfacePoint(const OpticSurfaceT<Real>& s, const Vec3T<Real>& p)
{
	return Vec3T<Real>(glm::dot(Vec3T<Real>(s.rows[0]), p) + s.rows[0].w, glm::dot(Vec3T<Real>(s.rows[1]), p) + s.rows[1].w,
		glm::dot(Vec3T<Real>(s.rows[2]), p) + s.rows[2].w);
}

template <typename Real>
Vec3T<Real> toSurfaceVector(const OpticSurfaceT<Real>& s, const Vec3T<Real>& v)
{
	return Vec3T<Real>(glm::dot(Vec3T<Real>(s.rows[0]), v), glm::dot(Vec3T<Real>(s.rows[1]), v), glm::dot(Vec3T<Real>(s.rows[2]), v));
}

template <typename Real>
Vec3T<Real> toBodyVector(const OpticSurfaceT<Real>& s, const Vec3T<Real>& v)
{
	return v.x * Vec3T<Real>(s.rows[0]) + v.y * Vec3T<Real>(s.rows[1]) + v.z * Vec3T<Real>(s.rows[2]);
}

template <typename Real>
Vec3T<Real> toBodyPoint(const OpticSurfaceT<Real>& s, const Vec3T<Real>& p)
{
	const Vec3T<Real> origin = -(s.rows[0].w * Vec3T<Real>(s.rows[0]) + s.rows[1].w * Vec3T<Real>(s.rows[1]) + s.rows[2].w * Vec3T<Real>(s.rows[2]));
	return toBodyVector(s, p) + origin;
}

template <typename Real>
Vec3T<Real> reprojectOntoSurface(const OpticSurfaceT<Real>& s, Vec3T<Real> p)
{
	if (s.shape == SurfaceShape::Cylinder) {
		const Real rho = std::sqrt(p.x * p.x + p.y * p.y);
		if (rho > Real(0)) {
			p.x *= s.radius / rho;
			p.y *= s.radius / rho;
		}
		return p;
	}
	if (s.shape == SurfaceShape::EvenAsphere) {
		if (const std::optional<Real> sag = surfaceSag(s, Vec2T<Real>(p.x, p.y))) {
			p.z = *sag;
		}
		return p;
	}
	const Real c = s.curvature;
	const Real kz = Real(1) + s.conic;
	const Real f = c * (p.x * p.x + p.y * p.y + kz * p.z * p.z) - Real(2) * p.z;
	const Vec3T<Real> gradient(Real(2) * c * p.x, Real(2) * c * p.y, Real(2) * c * kz * p.z - Real(2));
	return p - (f / glm::dot(gradient, gradient)) * gradient;
}

template <typename Real>
bool insideAperture(const ApertureT<Real>& aperture, Vec2T<Real> xy, Real relativeSlack)
{
	const Real grow = Real(1) + relativeSlack;
	const Vec2T<Real> p = xy - aperture.center;
	switch (aperture.kind) {
	case ApertureKind::None:
		return true;
	case ApertureKind::Circle: {
		const Real r = aperture.a * grow;
		return p.x * p.x + p.y * p.y <= r * r;
	}
	case ApertureKind::Annulus: {
		const Real rho2 = p.x * p.x + p.y * p.y;
		const Real inner = aperture.a / grow;
		const Real outer = aperture.b * grow;
		return rho2 >= inner * inner && rho2 <= outer * outer;
	}
	case ApertureKind::Rectangle:
		return std::abs(p.x) <= aperture.a * grow && std::abs(p.y) <= aperture.b * grow;
	case ApertureKind::Ellipse: {
		const Real u = p.x / aperture.a;
		const Real v = p.y / aperture.b;
		return u * u + v * v <= grow * grow;
	}
	case ApertureKind::Polygon: {
		// inside every edge, each widened by the slack times the polygon's size
		const int count = std::min((int)aperture.a, (int)OPTIC_POLYGON_VERTICES);
		Real size = Real(0);
		for (int i = 0; i < count; i++) {
			size = std::max(size, std::abs(aperture.polygon[(size_t)i].x) + std::abs(aperture.polygon[(size_t)i].y));
		}
		const Real slack = relativeSlack * size;
		for (int i = 0; i < count; i++) {
			const Vec2T<Real> a = aperture.polygon[(size_t)i];
			const Vec2T<Real> b = aperture.polygon[(size_t)((i + 1) % count)];
			const Vec2T<Real> edge = b - a;
			const Real length = std::sqrt(edge.x * edge.x + edge.y * edge.y);
			// counter-clockwise: the inside is to the left of each edge
			const Real side = edge.x * (p.y - a.y) - edge.y * (p.x - a.x);
			if (side < -slack * length) {
				return false;
			}
		}
		return count >= 3;
	}
	}
	return false;
}

template <typename Real>
bool intersectSurfaceLocal(const OpticSurfaceT<Real>& s, const Vec3T<Real>& oIn, const Vec3T<Real>& d, Real tMin, Real tMax, Real& t)
{
	const Real limit = tMax;
	// The ray re-centered on its closest approach to the frame's origin (the cylinder's axis, the conic's vertex), so the
	// quadratic's coefficients are formed from coordinates the size of the surface: from an origin far away, b^2 and 4ae
	// cancel in the discriminant and a near-tangent ray's two roots merge. t is then found from there and shifted back
	Real shift = Real(0);
	if (s.shape == SurfaceShape::Cylinder) {
		const Real dd = d.x * d.x + d.y * d.y;
		shift = dd > Real(0) ? -(oIn.x * d.x + oIn.y * d.y) / dd : Real(0);
	} else {
		shift = -glm::dot(oIn, d) / glm::dot(d, d);
	}
	const Vec3T<Real> o = oIn + shift * d;
	Real best = tMax - shift;
	const Real lo = tMin - shift;
	Real roots[2] = { Real(0), Real(0) };
	bool valid[2] = { false, false };
	if (s.shape == SurfaceShape::Cylinder) {
		const Real a = d.x * d.x + d.y * d.y;
		const Real b = Real(2) * (o.x * d.x + o.y * d.y);
		const Real e = o.x * o.x + o.y * o.y - s.radius * s.radius;
		if (stableRoots(a, b, e, roots[0], roots[1], valid[0], valid[1])) {
			for (int i = 0; i < 2; i++) {
				if (!valid[i]) {
					continue;
				}
				const Real candidate = refineCylinder(s.radius, o, d, roots[i]);
				if (inRange(candidate, lo, best) && withinCylinderBounds(s, o + candidate * d)) {
					best = candidate;
				}
			}
		}
	} else {
		const Real c = s.curvature;
		const Real kz = Real(1) + s.conic;
		const Real a = c * (d.x * d.x + d.y * d.y + kz * d.z * d.z);
		const Real b = Real(2) * (c * (o.x * d.x + o.y * d.y + kz * o.z * d.z) - d.z);
		const Real e = c * (o.x * o.x + o.y * o.y + kz * o.z * o.z) - Real(2) * o.z;
		if (stableRoots(a, b, e, roots[0], roots[1], valid[0], valid[1])) {
			for (int i = 0; i < 2; i++) {
				if (!valid[i]) {
					continue;
				}
				const Real candidate = s.shape == SurfaceShape::EvenAsphere ? refineAsphere(s, o, d, roots[i]) : refineConic(c, kz, o, d, roots[i]);
				if (inRange(candidate, lo, best) && acceptConicPoint(s, o + candidate * d) && (s.shape != SurfaceShape::EvenAsphere || onAsphere(s, o + candidate * d))) {
					best = candidate;
				}
			}
		}
	}
	t = best + shift;
	return best < limit - shift;
}

template <typename Real>
bool intersectOpticBody(const OpticPrescriptionT<Real>& body, const Vec3T<Real>& o, const Vec3T<Real>& d, Real tMin, Real tMax, OpticHitT<Real>& hit)
{
	bool found = false;
	for (uint32_t i = 0; i < (uint32_t)body.surfaces.size(); i++) {
		const OpticSurfaceT<Real>& s = body.surfaces[i];
		const Vec3T<Real> localOrigin = toSurfacePoint(s, o);
		const Vec3T<Real> localDirection = toSurfaceVector(s, d);
		Real t;
		if (intersectSurfaceLocal(s, localOrigin, localDirection, tMin, tMax, t)) {
			tMax = t;
			hit.t = t;
			hit.surface = i;
			hit.local = localOrigin + t * localDirection;
			found = true;
		}
	}
	return found;
}

template <typename Real>
Vec3T<Real> surfaceNormalLocal(const OpticSurfaceT<Real>& s, const Vec3T<Real>& p)
{
	if (s.shape == SurfaceShape::Cylinder) {
		return glm::normalize(Vec3T<Real>(p.x, p.y, Real(0)));
	}
	if (s.shape == SurfaceShape::EvenAsphere) {
		const Real rho2 = p.x * p.x + p.y * p.y;
		Real sag, slopeOverRho, second;
		conicSagDerivatives(s.curvature, s.conic, rho2, sag, slopeOverRho, second);
		Real polynomial, polynomialSlope, polynomialSecond;
		asphereTerms(s.asphere, rho2, polynomial, polynomialSlope, polynomialSecond);
		const Real g = slopeOverRho + polynomialSlope;
		return glm::normalize(Vec3T<Real>(-g * p.x, -g * p.y, Real(1)));
	}
	// the gradient of c (x^2 + y^2 + kz z^2) - 2 z, negated so it points to +z at the vertex
	const Real c = s.curvature;
	const Real kz = Real(1) + s.conic;
	return glm::normalize(Vec3T<Real>(-c * p.x, -c * p.y, Real(1) - c * kz * p.z));
}

template <typename Real>
PrincipalCurvaturesT<Real> principalCurvatures(const OpticSurfaceT<Real>& s, const Vec3T<Real>& p)
{
	PrincipalCurvaturesT<Real> out;
	if (s.shape == SurfaceShape::Cylinder) {
		const Real rho = std::sqrt(p.x * p.x + p.y * p.y);
		// around the axis it bends away from its outward normal; along the axis it is straight
		out.k1 = -Real(1) / rho;
		out.k2 = Real(0);
		out.d1 = Vec3T<Real>(-p.y / rho, p.x / rho, Real(0));
		out.d2 = Vec3T<Real>(Real(0), Real(0), Real(1));
		return out;
	}
	// a surface of revolution z = f(rho): meridional f'' / (1 + f'^2)^(3/2) and sagittal f' / (rho sqrt(1 + f'^2))
	const Real rho2 = p.x * p.x + p.y * p.y;
	Real sag, slopeOverRho, second;
	conicSagDerivatives(s.curvature, s.conic, rho2, sag, slopeOverRho, second);
	if (s.shape == SurfaceShape::EvenAsphere) {
		Real polynomial, polynomialSlope, polynomialSecond;
		asphereTerms(s.asphere, rho2, polynomial, polynomialSlope, polynomialSecond);
		slopeOverRho += polynomialSlope;
		second += polynomialSecond;
	}
	const Real slope2 = slopeOverRho * slopeOverRho * rho2;
	const Real w = std::sqrt(Real(1) + slope2);
	out.k1 = second / (w * w * w);
	out.k2 = slopeOverRho / w;
	const Real rho = std::sqrt(rho2);
	if (rho > Real(0)) {
		const Real slope = slopeOverRho * rho;
		out.d1 = glm::normalize(Vec3T<Real>(p.x / rho, p.y / rho, slope));
		out.d2 = Vec3T<Real>(-p.y / rho, p.x / rho, Real(0));
	}
	return out;
}

namespace {

// the box of one surface in its own frame, before the placement
OpticBoundsT<double> localBounds(const OpticSurface& s)
{
	OpticBoundsT<double> box;
	if (s.shape == SurfaceShape::Cylinder) {
		// the z range over the disk, sampled densely: the bounds are planes and conics, monotonic along any ray
		// from the center, so the extremes are on the rim or at a bound's own center
		double zMin = 1e300;
		double zMax = -1e300;
		const int rings = 8;
		const int spokes = 64;
		auto sample = [&](glm::dvec2 xy) {
			zMin = std::min({ zMin, boundZ(s.lower, xy), boundZ(s.upper, xy) });
			zMax = std::max({ zMax, boundZ(s.lower, xy), boundZ(s.upper, xy) });
		};
		sample(glm::dvec2(0.0));
		for (int ring = 1; ring <= rings; ring++) {
			for (int spoke = 0; spoke < spokes; spoke++) {
				const double angle = 2.0 * 3.14159265358979323846 * spoke / spokes;
				sample(s.radius * ring / rings * glm::dvec2(std::cos(angle), std::sin(angle)));
			}
		}
		box.grow(glm::dvec3(-s.radius, -s.radius, zMin));
		box.grow(glm::dvec3(s.radius, s.radius, zMax));
		return box;
	}

	// the aperture's xy rectangle, and the sag's range over it
	glm::dvec2 lo, hi;
	const ApertureT<double>& ap = s.aperture;
	switch (ap.kind) {
	case ApertureKind::Circle:
		lo = ap.center - glm::dvec2(ap.a);
		hi = ap.center + glm::dvec2(ap.a);
		break;
	case ApertureKind::Annulus:
		lo = ap.center - glm::dvec2(ap.b);
		hi = ap.center + glm::dvec2(ap.b);
		break;
	case ApertureKind::Rectangle:
	case ApertureKind::Ellipse:
		lo = ap.center - glm::dvec2(ap.a, ap.b);
		hi = ap.center + glm::dvec2(ap.a, ap.b);
		break;
	case ApertureKind::Polygon:
		lo = glm::dvec2(1e300);
		hi = glm::dvec2(-1e300);
		for (int i = 0; i < (int)ap.a && i < (int)OPTIC_POLYGON_VERTICES; i++) {
			lo = glm::min(lo, ap.center + ap.polygon[(size_t)i]);
			hi = glm::max(hi, ap.center + ap.polygon[(size_t)i]);
		}
		break;
	case ApertureKind::None:
		lo = glm::dvec2(-1.0);
		hi = glm::dvec2(1.0);
		break;
	}
	double zMin = 1e300;
	double zMax = -1e300;
	const int steps = 48;
	for (int i = 0; i <= steps; i++) {
		for (int j = 0; j <= steps; j++) {
			const glm::dvec2 xy = glm::mix(lo, hi, glm::dvec2((double)i / steps, (double)j / steps));
			const std::optional<double> sag = surfaceSag(s, xy);
			// past a sphere's rim the surface reaches its equator at most
			const double z = sag ? *sag : (s.curvature != 0.0 ? 1.0 / ((1.0 + s.conic) * s.curvature) : 0.0);
			zMin = std::min(zMin, z);
			zMax = std::max(zMax, z);
		}
	}
	// the sag's curvature between samples: what a sphere of the steepest curvature rises over half a step
	const double step = std::max(hi.x - lo.x, hi.y - lo.y) / steps;
	const double rise = 0.5 * std::abs(s.curvature) * step * step + 1e-9;
	box.grow(glm::dvec3(lo, zMin - rise));
	box.grow(glm::dvec3(hi, zMax + rise));
	return box;
}

}

void finishPrescription(OpticPrescription& prescription)
{
	prescription.bounds = {};
	for (OpticSurface& surface : prescription.surfaces) {
		finishSurface(surface);
		const OpticBoundsT<double> local = localBounds(surface);
		for (int corner = 0; corner < 8; corner++) {
			const glm::dvec3 p((corner & 1) ? local.max.x : local.min.x, (corner & 2) ? local.max.y : local.min.y, (corner & 4) ? local.max.z : local.min.z);
			prescription.bounds.grow(surface.placement.rotation * p + surface.placement.origin);
		}
	}
	// a relative margin, so the float box the TLAS gets cannot cut a surface the float intersector reaches
	if (!prescription.bounds.empty()) {
		const glm::dvec3 size = prescription.bounds.max - prescription.bounds.min;
		const double margin = 1e-5 * std::max({ size.x, size.y, size.z }) + 1e-7;
		prescription.bounds.min -= glm::dvec3(margin);
		prescription.bounds.max += glm::dvec3(margin);
	}
}

OpticPrescriptionF toFloat(const OpticPrescription& p)
{
	OpticPrescriptionF out;
	out.media = p.media;
	out.bounds.min = glm::vec3(p.bounds.min);
	out.bounds.max = glm::vec3(p.bounds.max);
	auto bound = [](const CylinderBoundT<double>& b) {
		CylinderBoundT<float> f;
		f.z0 = (float)b.z0;
		f.slope = glm::vec2(b.slope);
		f.curvature = (float)b.curvature;
		f.conic = (float)b.conic;
		f.center = glm::vec2(b.center);
		return f;
	};
	for (const OpticSurface& s : p.surfaces) {
		OpticSurfaceT<float> f;
		f.shape = s.shape;
		f.placement.rotation = glm::mat3(s.placement.rotation);
		f.placement.origin = glm::vec3(s.placement.origin);
		f.curvature = (float)s.curvature;
		f.conic = (float)s.conic;
		for (size_t i = 0; i < OPTIC_ASPHERE_TERMS; i++) {
			f.asphere[i] = (float)s.asphere[i];
		}
		f.radius = (float)s.radius;
		f.lower = bound(s.lower);
		f.upper = bound(s.upper);
		f.aperture.kind = s.aperture.kind;
		f.aperture.a = (float)s.aperture.a;
		f.aperture.b = (float)s.aperture.b;
		f.aperture.center = glm::vec2(s.aperture.center);
		for (size_t i = 0; i < OPTIC_POLYGON_VERTICES; i++) {
			f.aperture.polygon[i] = glm::vec2(s.aperture.polygon[i]);
		}
		f.mediumMinus = s.mediumMinus;
		f.mediumPlus = s.mediumPlus;
		f.materialSlot = s.materialSlot;
		f.flags = s.flags;
		for (size_t row = 0; row < 3; row++) {
			f.rows[row] = glm::vec4(s.rows[row]);
		}
		out.surfaces.push_back(f);
	}
	return out;
}

uint32_t packOpticBody(const OpticPrescriptionF& body, std::vector<glm::vec4>& data)
{
	const uint32_t base = (uint32_t)data.size();
	const size_t count = body.surfaces.size();
	data.resize(data.size() + count * OPTIC_SURFACE_VEC4S, glm::vec4(0.f));
	auto bits = [](uint32_t v) { return std::bit_cast<float>(v); };
	auto bound = [&](const CylinderBoundT<float>& b) {
		data.push_back(glm::vec4(b.z0, b.slope.x, b.slope.y, b.curvature));
		data.push_back(glm::vec4(b.conic, b.center.x, b.center.y, 0.f));
	};
	for (size_t i = 0; i < count; i++) {
		const OpticSurfaceT<float>& s = body.surfaces[i];
		// the payload, after every surface's six words: a cylinder's bounds, an asphere's terms, a polygon's corners
		uint32_t payload = 0;
		if (s.shape == SurfaceShape::Cylinder) {
			payload = (uint32_t)data.size();
			bound(s.lower);
			bound(s.upper);
		} else if (s.shape == SurfaceShape::EvenAsphere) {
			payload = (uint32_t)data.size();
			data.push_back(glm::vec4(s.asphere[0], s.asphere[1], s.asphere[2], s.asphere[3]));
			data.push_back(glm::vec4(s.asphere[4], s.asphere[5], s.asphere[6], 0.f));
		} else if (s.aperture.kind == ApertureKind::Polygon) {
			payload = (uint32_t)data.size();
			for (size_t v = 0; v < OPTIC_POLYGON_VERTICES; v += 2) {
				data.push_back(glm::vec4(s.aperture.polygon[v], s.aperture.polygon[v + 1]));
			}
		}
		glm::vec4* record = &data[base + i * OPTIC_SURFACE_VEC4S];
		record[0] = s.rows[0];
		record[1] = s.rows[1];
		record[2] = s.rows[2];
		record[3] = s.shape == SurfaceShape::Cylinder ? glm::vec4(s.radius, 0.f, 0.f, 0.f) : glm::vec4(s.curvature, s.conic, 0.f, 0.f);
		record[4] = glm::vec4(s.aperture.a, s.aperture.b, s.aperture.center.x, s.aperture.center.y);
		record[5] = glm::vec4(bits((uint32_t)s.shape | ((uint32_t)s.aperture.kind << 8u) | ((uint32_t)s.flags << 16u)), bits(s.materialSlot),
			bits((uint32_t)s.mediumMinus | ((uint32_t)s.mediumPlus << 16u)), bits(payload));
	}
	return base;
}

OpticFrameT<double> translationFrame(const glm::dvec3& origin)
{
	OpticFrameT<double> frame;
	frame.origin = origin;
	return frame;
}

OpticFrameT<double> frameFromNormal(const glm::dvec3& origin, const glm::dvec3& normal, const glm::dvec3& xHint)
{
	const glm::dvec3 z = glm::normalize(normal);
	glm::dvec3 x = xHint - z * glm::dot(z, xHint);
	if (glm::dot(x, x) < 1e-24) {
		x = std::abs(z.x) < 0.9 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 1.0, 0.0);
		x -= z * glm::dot(z, x);
	}
	x = glm::normalize(x);
	const glm::dvec3 y = glm::cross(z, x);
	OpticFrameT<double> frame;
	frame.rotation = glm::dmat3(x, y, z);
	frame.origin = origin;
	return frame;
}

bool reflectDirection(const glm::dvec3& d, const glm::dvec3& n, glm::dvec3& out)
{
	out = d - 2.0 * glm::dot(d, n) * n;
	return true;
}

bool refractDirection(const glm::dvec3& d, const glm::dvec3& nIn, double eta, glm::dvec3& out)
{
	// the normal on the side the ray comes from
	const glm::dvec3 n = glm::dot(d, nIn) < 0.0 ? nIn : -nIn;
	const double cosI = -glm::dot(d, n);
	const double sin2T = eta * eta * std::max(0.0, 1.0 - cosI * cosI);
	if (sin2T > 1.0) {
		return false;
	}
	const double cosT = std::sqrt(1.0 - sin2T);
	out = eta * d + (eta * cosI - cosT) * n;
	return true;
}

#define OPTIC_INSTANTIATE(Real)                                                                                                            \
	template std::optional<Real> conicSag<Real>(Real, Real, Real);                                                                         \
	template std::optional<Real> surfaceSag<Real>(const OpticSurfaceT<Real>&, Vec2T<Real>);                                                \
	template Real boundZ<Real>(const CylinderBoundT<Real>&, Vec2T<Real>);                                                                  \
	template void finishSurface<Real>(OpticSurfaceT<Real>&);                                                                               \
	template Vec3T<Real> toSurfacePoint<Real>(const OpticSurfaceT<Real>&, const Vec3T<Real>&);                                             \
	template Vec3T<Real> toSurfaceVector<Real>(const OpticSurfaceT<Real>&, const Vec3T<Real>&);                                            \
	template Vec3T<Real> toBodyVector<Real>(const OpticSurfaceT<Real>&, const Vec3T<Real>&);                                               \
	template Vec3T<Real> toBodyPoint<Real>(const OpticSurfaceT<Real>&, const Vec3T<Real>&);                                                \
	template Vec3T<Real> reprojectOntoSurface<Real>(const OpticSurfaceT<Real>&, Vec3T<Real>);                                              \
	template bool insideAperture<Real>(const ApertureT<Real>&, Vec2T<Real>, Real);                                                               \
	template bool intersectSurfaceLocal<Real>(const OpticSurfaceT<Real>&, const Vec3T<Real>&, const Vec3T<Real>&, Real, Real, Real&);      \
	template bool intersectOpticBody<Real>(const OpticPrescriptionT<Real>&, const Vec3T<Real>&, const Vec3T<Real>&, Real, Real, OpticHitT<Real>&); \
	template Vec3T<Real> surfaceNormalLocal<Real>(const OpticSurfaceT<Real>&, const Vec3T<Real>&);                                         \
	template PrincipalCurvaturesT<Real> principalCurvatures<Real>(const OpticSurfaceT<Real>&, const Vec3T<Real>&);

OPTIC_INSTANTIATE(float)
OPTIC_INSTANTIATE(double)
OPTIC_INSTANTIATE(long double)
