#pragma once

// The raytracer's analytic shapes, each defined once as a fixed unit primitive in its own object
// space. A placed shape is that primitive under an object-to-world transform (position, rotation,
// and its size as a scale), exactly as a placed mesh is its BLAS under one: the tracer carries the
// ray into object space, intersects the unit primitive there, and carries the normal back by the
// inverse transpose. So rotation and non-uniform size cost no shape any code of its own, and every
// shape and every mesh share one instance record and one path through the TLAS.
//
// The unit primitives, all centred on the origin:
//  - Sphere:   radius 1
//  - Plane:    y = 0, infinite, normal +y. The one unbounded shape: it has no box for the TLAS and
//              is tested by every ray outside it
//  - Quad:     y = 0, x and z in [-0.5, 0.5], normal +y
//  - Box:      [-0.5, 0.5] on every axis
//  - Cylinder: axis y, radius 1, y in [-0.5, 0.5], closed by both caps
//
// Every surface is two-sided, like the tracer's triangles. Mirrored line for line by
// shaders/rt/include/crt_shape.glsl - a change to one side must be made to the other.
//
// Each primitive has PBR's parametric (u, v) and their derivatives, in glTF's orientation: seen
// from outside, u runs right and v runs down, so cross(dp/du, dp/dv) points into the surface. The
// table is at the top of crt_shape.glsl, and bvh_bench checks the derivatives against the uvs.

#include <cstdint>

#include <glm/glm.hpp>

#include <bvh.h>

// the values are the GPU's tags (crt_shape.glsl CRT_SHAPE_*): append only
enum class ShapeKind : uint32_t {
	Sphere,
	Plane,
	Quad,
	Box,
	Cylinder
};

inline constexpr ShapeKind SHAPE_KINDS[] = {
	ShapeKind::Sphere,
	ShapeKind::Plane,
	ShapeKind::Quad,
	ShapeKind::Box,
	ShapeKind::Cylinder,
};

inline constexpr size_t SHAPE_KIND_COUNT = sizeof(SHAPE_KINDS) / sizeof(SHAPE_KINDS[0]);

// false only for the infinite plane
bool shapeBounded(ShapeKind kind);

// the unit primitive's box in object space; empty for an unbounded shape
Aabb shapeObjectBounds(ShapeKind kind);

// the nearest hit in [tMin, tMax] of an object-space ray. `direction` need not be unit length -
// under a scaled instance it is not - and t is the parameter along it, so it carries unchanged to
// the world-space ray it came from
// `boxFarScale` is BOX_FAR_SCALE for a watertight ray, which widens the box's slab test so rounding
// cannot lose a grazing hit, and 1 otherwise
bool intersectShape(ShapeKind kind, const glm::vec3& origin, const glm::vec3& direction, float tMin, float tMax, float& t, float boxFarScale = 1.f);

// the parts of a cylinder (shapePart())
inline constexpr uint32_t SHAPE_CYLINDER_SIDE = 0;
inline constexpr uint32_t SHAPE_CYLINDER_BOTTOM = 1;
inline constexpr uint32_t SHAPE_CYLINDER_TOP = 2;

// which part of the unit primitive a point on it lies on, what the GPU's hit record holds for a
// shape: a box's face (-x, +x, -y, +y, -z, +z as 0-5), a cylinder's side or cap by whichever surface
// the point is nearer to, and 0 for a shape of one part
uint32_t shapePart(ShapeKind kind, const glm::vec3& point);

// the outward object-space normal at a point on the unit primitive, not normalised
glm::vec3 shapeNormal(ShapeKind kind, const glm::vec3& point);

// A hit point `p` on the unit primitive as the ray computed it in float, brought exactly onto the
// primitive, with the absolute error its coordinates then have (PBR 4ed 6.8.5; shapeReproject() in
// crt_shape.glsl). A coordinate the primitive fixes (a quad's y, a box face's axis, a cap's y) is
// set to its value and has no error
glm::vec3 shapeHitPoint(ShapeKind kind, glm::vec3 p, glm::vec3& pError);

// the (u, v) of a point on `part` of the unit primitive
glm::vec2 shapeUv(ShapeKind kind, uint32_t part, const glm::vec3& point);

// the object-space derivatives of the point and of the unnormalised normal with respect to u and v
struct ShapeDerivatives {
	glm::vec3 dpdu { 1.f, 0.f, 0.f };
	glm::vec3 dpdv { 0.f, 0.f, 1.f };
	glm::vec3 dndu { 0.f };
	glm::vec3 dndv { 0.f };
};
ShapeDerivatives shapeDerivatives(ShapeKind kind, uint32_t part, const glm::vec3& point);
