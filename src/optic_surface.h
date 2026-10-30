#pragma once

// OPTICAL BODIES (docs/plans/completed/physoptics-optical-surfaces.md): a lens, a mirror, a prism or a cube as a list of exact
// surfaces, each a sag function in its own frame, cut to an aperture, with a medium on either side. Static library
// rt_optics, no Vulkan, templated on the scalar type: the solver works in double, and the float instantiation is the
// line-for-line mirror of shaders/rt/include/crt_optic.glsl, which reads the same numbers from binding 55.
//
// THE FRAMES. A body is described in its own frame, the body frame: +z along the axis light is designed to travel,
// the origin at the element's datum. Each surface lives in a frame of its own, placed in the body frame by a rotation
// and its vertex's position, so faces need not share an axis (a prism's faces, a cube's diagonal, an off-axis
// parabola's parent vertex). The intersector carries the ray into the surface frame with the rows of the body ->
// surface transform (OpticSurfaceT::rows), which the GPU record stores as they are.
//
// THE SIDES. Every surface has a normal, and its "plus" side is the side the normal points to: +z for a conic and an
// asphere, outward for a cylinder. mediumPlus and mediumMinus name the media there (indices into the body's media,
// 0 the surrounding medium), so a hit says by the sign of d . n which medium the ray leaves and which it enters, and
// nothing needs a stack of nested media.
//
// THE SHAPES, in the surface frame:
//   Conic       c (x^2 + y^2 + (1 + k) z^2) - 2 z = 0, the branch through the vertex (1 + k) c z < 1. A plane at c = 0,
//               a sphere at k = 0, a paraboloid at k = -1. Written in vertex form so the quadratic stays well
//               conditioned as c -> 0 and the useful root, near the vertex, is the accurate one
//   Cylinder    x^2 + y^2 = r^2 between two bounds lower(x, y) <= z <= upper(x, y), each a plane with slopes plus a
//               conic's sag about an offset center: a lens edge between its faces' sags, a wedge's rim under its
//               tilted back, an off-axis parabola's rim under the parent paraboloid
//   EvenAsphere the conic's sag plus sum(alpha_i rho^(2i)), i = 2 to 8; the conic's root refined by Newton's method
//
// APERTURES, in the surface frame's xy about an offset center: circle, annulus, rectangle and convex polygon (up to
// six vertices), and the ellipse a cylinder cuts from a tilted plane. Each widens its bound by a relative APERTURE_SLACK, so a ray through the rim where a face meets its
// edge cylinder is caught by one of the two rather than slipping between them.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// the values are the GPU's tags (crt_optic.glsl CRT_OPTIC_SHAPE_*): append only
enum class SurfaceShape : uint8_t {
	Conic,
	Cylinder,
	EvenAsphere
};

// crt_optic.glsl CRT_OPTIC_APERTURE_*: append only
enum class ApertureKind : uint8_t {
	// unbounded: a cylinder, which its z bounds cut instead
	None,
	Circle,
	Annulus,
	Rectangle,
	Polygon,
	// semi-axes a (x) and b (y): a tilted plane cut by a cylinder, the back of a wedge
	Ellipse
};

// OpticSurfaceT::flags
// a face light is designed to cross or reflect from (polished)
inline constexpr uint16_t OPTIC_SURFACE_OPTICAL = 1u << 0;
// a lens's or a mirror's rim, ground or blackened
inline constexpr uint16_t OPTIC_SURFACE_EDGE = 1u << 1;
// between two media of the body: a cemented doublet's middle, a cube's diagonal
inline constexpr uint16_t OPTIC_SURFACE_INTERNAL = 1u << 2;

inline constexpr size_t OPTIC_POLYGON_VERTICES = 6;
inline constexpr size_t OPTIC_ASPHERE_TERMS = 7;
// the relative widening of every aperture and cylinder bound
inline constexpr double APERTURE_SLACK = 4e-6;
// the Newton iterations an even asphere's root takes at most
inline constexpr int ASPHERE_NEWTON_STEPS = 8;

template <typename Real>
using Vec2T = glm::vec<2, Real, glm::defaultp>;
template <typename Real>
using Vec3T = glm::vec<3, Real, glm::defaultp>;
template <typename Real>
using Vec4T = glm::vec<4, Real, glm::defaultp>;
template <typename Real>
using Mat3T = glm::mat<3, 3, Real, glm::defaultp>;

// surface frame -> body frame: p_body = rotation * p_surface + origin
template <typename Real>
struct OpticFrameT {
	Mat3T<Real> rotation { Real(1) };
	Vec3T<Real> origin { Real(0) };
	bool operator==(const OpticFrameT&) const = default;
};

template <typename Real>
struct ApertureT {
	ApertureKind kind { ApertureKind::Circle };
	// Circle: the radius in a. Annulus: inner radius a, outer b. Rectangle: half width a (x), half height b (y).
	// Polygon: the vertex count in a. Ellipse: the semi-axes a (x) and b (y)
	Real a { 0 };
	Real b { 0 };
	Vec2T<Real> center { Real(0) };
	// Polygon: counter-clockwise seen from +z, convex
	std::array<Vec2T<Real>, OPTIC_POLYGON_VERTICES> polygon {};
	bool operator==(const ApertureT&) const = default;
};

// one z bound of a cylinder: z0 + slope . (x, y) + sag(curvature, conic; (x, y) - center)
template <typename Real>
struct CylinderBoundT {
	Real z0 { 0 };
	Vec2T<Real> slope { Real(0) };
	Real curvature { 0 };
	Real conic { 0 };
	Vec2T<Real> center { Real(0) };
	bool operator==(const CylinderBoundT&) const = default;
};

template <typename Real>
struct OpticSurfaceT {
	SurfaceShape shape { SurfaceShape::Conic };
	OpticFrameT<Real> placement;
	// Conic and EvenAsphere: c = 1 / R (0 is a plane) and k
	Real curvature { 0 };
	Real conic { 0 };
	// EvenAsphere: alpha_2 .. alpha_8, the coefficients of rho^4 .. rho^16
	std::array<Real, OPTIC_ASPHERE_TERMS> asphere {};
	// Cylinder: the radius and the two z bounds
	Real radius { 0 };
	CylinderBoundT<Real> lower;
	CylinderBoundT<Real> upper;
	ApertureT<Real> aperture;
	// into the body's media; 0 is the surrounding medium
	uint8_t mediumMinus { 0 };
	uint8_t mediumPlus { 0 };
	// which of the element's materials this surface is shaded with
	uint16_t materialSlot { 0 };
	uint16_t flags { 0 };

	// derived by finishSurface(): the rows of the body -> surface transform, s_i = rows[i].xyz . p + rows[i].w. What
	// binding 55 stores, and what both intersectors read
	std::array<Vec4T<Real>, 3> rows {};

	bool operator==(const OpticSurfaceT&) const = default;
};

// one medium of a body: a glass by its catalog name (src/optic_glass.h), or the surrounding medium for an empty name
struct OpticMedium {
	std::string glass;
	bool operator==(const OpticMedium&) const = default;
};

// a box in the body frame
template <typename Real>
struct OpticBoundsT {
	Vec3T<Real> min { Real(1) };
	Vec3T<Real> max { Real(-1) };
	bool empty() const { return min.x > max.x; }
	void grow(const Vec3T<Real>& p)
	{
		if (empty()) {
			min = max = p;
		} else {
			min = glm::min(min, p);
			max = glm::max(max, p);
		}
	}
	bool operator==(const OpticBoundsT&) const = default;
};

template <typename Real>
struct OpticPrescriptionT {
	std::vector<OpticSurfaceT<Real>> surfaces;
	// [0] is the surrounding medium, always
	std::vector<OpticMedium> media { OpticMedium {} };
	// conservative, from finishPrescription()
	OpticBoundsT<Real> bounds;
	bool operator==(const OpticPrescriptionT&) const = default;
};

using OpticSurface = OpticSurfaceT<double>;
using OpticPrescription = OpticPrescriptionT<double>;
using OpticPrescriptionF = OpticPrescriptionT<float>;

//---------------------------------------------------------------- building

// derives the rows from the placement
template <typename Real>
void finishSurface(OpticSurfaceT<Real>& surface);
// finishes every surface and computes the bounds
void finishPrescription(OpticPrescription& prescription);
// the float copy the GPU and its mirror read: the rows rounded from the double ones, never recomputed in float
OpticPrescriptionF toFloat(const OpticPrescription& prescription);

// a conic's sag z(rho^2) on the vertex branch; nullopt where the conic has no such point (past a sphere's rim)
template <typename Real>
std::optional<Real> conicSag(Real curvature, Real conic, Real rho2);
// the sag of a conic or even asphere surface at (x, y) of its own frame
template <typename Real>
std::optional<Real> surfaceSag(const OpticSurfaceT<Real>& surface, Vec2T<Real> xy);
// a cylinder bound's z at (x, y)
template <typename Real>
Real boundZ(const CylinderBoundT<Real>& bound, Vec2T<Real> xy);

// THE GPU RECORD (binding 55, shaders/rt/include/crt_optic.glsl): six vec4 per surface from the returned base, in order,
// then each surface's payload. Per surface: [0..2] the rows; [3] (c, k, 0, 0), or (radius, 0, 0, 0) for a cylinder;
// [4] the aperture (a, b, center); [5] uint bits: shape | aperture << 8 | flags << 16, the material slot, the media
// (minus | plus << 16), and the payload's offset in vec4s from the start of the buffer (0 for none). The payload is a
// cylinder's two bounds (z0, slope, c) (k, center, 0) each, an asphere's seven terms in two vec4, or a polygon's six
// corners in three
inline constexpr uint32_t OPTIC_SURFACE_VEC4S = 6;
uint32_t packOpticBody(const OpticPrescriptionF& body, std::vector<glm::vec4>& data);

// placements
OpticFrameT<double> translationFrame(const glm::dvec3& origin);
// a frame whose +z is `normal`, with +x as close to `xHint` as it can be
OpticFrameT<double> frameFromNormal(const glm::dvec3& origin, const glm::dvec3& normal, const glm::dvec3& xHint);

//---------------------------------------------------------------- intersecting

// a point or direction of the body frame in the surface frame
template <typename Real>
Vec3T<Real> toSurfacePoint(const OpticSurfaceT<Real>& surface, const Vec3T<Real>& p);
template <typename Real>
Vec3T<Real> toSurfaceVector(const OpticSurfaceT<Real>& surface, const Vec3T<Real>& v);
template <typename Real>
Vec3T<Real> toBodyVector(const OpticSurfaceT<Real>& surface, const Vec3T<Real>& v);

// whether a point of the surface frame's xy lies inside the aperture, widened by a relative `slack`
template <typename Real>
bool insideAperture(const ApertureT<Real>& aperture, Vec2T<Real> xy, Real slack = Real(APERTURE_SLACK));

// the nearest hit of one surface in [tMin, tMax] by a ray of the surface frame. The direction need not be unit
// length: t is the parameter along it, as everywhere in the tracer
template <typename Real>
bool intersectSurfaceLocal(const OpticSurfaceT<Real>& surface, const Vec3T<Real>& o, const Vec3T<Real>& d, Real tMin, Real tMax, Real& t);

template <typename Real>
struct OpticHitT {
	Real t { 0 };
	uint32_t surface { 0 };
	// the hit in the surface frame: what the GPU's hit record keeps (x, y) of
	Vec3T<Real> local { Real(0) };
};

// the nearest hit of a body in [tMin, tMax] by a ray of the body frame
template <typename Real>
bool intersectOpticBody(const OpticPrescriptionT<Real>& body, const Vec3T<Real>& o, const Vec3T<Real>& d, Real tMin, Real tMax, OpticHitT<Real>& hit);

// a point of the surface frame brought onto the surface (crt_optic.glsl opticReproject()): a cylinder's radially, a
// conic's by one step along its implicit function's gradient (exact for a plane), an asphere's to its sag
template <typename Real>
Vec3T<Real> reprojectOntoSurface(const OpticSurfaceT<Real>& surface, Vec3T<Real> p);
template <typename Real>
Vec3T<Real> toBodyPoint(const OpticSurfaceT<Real>& surface, const Vec3T<Real>& p);

//---------------------------------------------------------------- the surface at a hit

// the unit normal in the surface frame, toward the plus side, at a point of the surface
template <typename Real>
Vec3T<Real> surfaceNormalLocal(const OpticSurfaceT<Real>& surface, const Vec3T<Real>& p);

// the principal curvatures at a point (positive where the surface bends toward its normal) and their directions, in
// the surface frame. What the beam solver's differential ray transfer needs (physoptics-gaussian-beam-solver.md)
template <typename Real>
struct PrincipalCurvaturesT {
	Real k1 { 0 };
	Real k2 { 0 };
	Vec3T<Real> d1 { Real(1), Real(0), Real(0) };
	Vec3T<Real> d2 { Real(0), Real(1), Real(0) };
};
template <typename Real>
PrincipalCurvaturesT<Real> principalCurvatures(const OpticSurfaceT<Real>& surface, const Vec3T<Real>& p);

//---------------------------------------------------------------- tracing light through a body

// a reflection or a refraction at a surface: the direction after it, by Snell's law with total internal reflection.
// `n` the unit normal (either side), `d` the unit direction, `eta` = n_incident / n_transmitted. False when a refraction
// is asked for past the critical angle
bool reflectDirection(const glm::dvec3& d, const glm::dvec3& n, glm::dvec3& out);
bool refractDirection(const glm::dvec3& d, const glm::dvec3& n, double eta, glm::dvec3& out);
