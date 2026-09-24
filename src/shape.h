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
// shaders/crt_shape.glsl - a change to one side must be made to the other.

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
bool intersectShape(ShapeKind kind, const glm::vec3& origin, const glm::vec3& direction, float tMin, float tMax, float& t);

// the outward object-space normal at a point on the unit primitive, not normalised
glm::vec3 shapeNormal(ShapeKind kind, const glm::vec3& point);
