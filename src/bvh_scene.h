#pragma once

// The two-level acceleration structure, independent of Vulkan and of the engine: one bottom-level
// BVH (BLAS) per unique mesh, in the mesh's object space and built once for as long as the mesh is
// loaded, and a top-level BVH (TLAS) over the placed instances of those meshes - plus the
// analytic shapes (shape.h) - rebuilt whenever an object moves. A ray is intersected with an instance by
// transforming it into the instance's object space, so one BLAS serves every placement of its
// mesh however it is translated, rotated or scaled (the "instances" of RTNW §8, the TLAS/BLAS of
// Bikker's articles 5-6, and the structure of VK_KHR_acceleration_structure that GPSnoopy's
// renderer drives in hardware). A shape is placed the same way, its unit primitive standing in for
// the BLAS. An unbounded shape (the infinite plane) has no box to put in the TLAS, so it stays out
// of it and every ray tests it directly.
//
// traceScene() and occludedScene() are the CPU mirrors of the GPU's two-level traversal and its
// shadow-ray query (shaders/rt/include/crt_bvh.glsl traceScene() and occluded()).

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <bvh.h>
#include <bvh_layout.h>
#include <shape.h>

// One mesh's bottom-level structure
struct Blas {
	PackedBvh bvh;
	// the mesh's triangles in the BVH's slot order: re-ordered, and duplicated by spatial splits.
	// Each carries its original index in v0.w
	std::vector<BvhTriangle> triangles;
	// the mesh's own triangle count, before duplication
	uint32_t triangleCount { 0 };
	Aabb bounds;
	// boxes cutting across the tree's upper levels: an instance's world box is the union of these
	// transformed, which for a rotated mesh is far tighter than the transformed root box
	std::vector<Aabb> frontier;
	BvhStats stats;
	double buildMs { 0.0 };
	double packMs { 0.0 };
	// empty for a usable BLAS; otherwise what validation or packing found, and it must not be traced
	std::string error;

	bool usable() const { return error.empty() && bvh.nodeCount > 0; }
};

// Builds, validates and packs. `triangleVertices`: three per triangle, in object space. A triangle's index is its position there
Blas buildBlas(std::span<const glm::vec3> triangleVertices, const BvhBuildOptions& options, BvhLayout layout);

enum class SceneInstanceKind : uint32_t {
	Mesh,
	Shape
};

struct SceneInstance {
	SceneInstanceKind kind { SceneInstanceKind::Mesh };
	// Mesh: which BLAS
	uint32_t blas { 0 };
	// Shape: which unit primitive
	ShapeKind shape { ShapeKind::Sphere };
	// where the BLAS or the unit primitive is placed
	glm::mat4 objectToWorld { 1.f };
};

struct Tlas {
	// binary layout over the instances; its primOrder is the instance in each slot
	PackedBvh bvh;
	// per instance, by instance index: the inverse transform the ray is carried into object space
	// with, and whether the instance is traced at all (a singular transform or a missing BLAS is
	// left out)
	std::vector<glm::mat4> worldToObject;
	std::vector<uint8_t> traced;
	// the traced instances outside the tree, by instance index: the unbounded shapes, which every
	// ray tests before the TLAS walk
	std::vector<uint32_t> unbounded;
	Aabb bounds;
	BvhStats stats;
	// the expected cost of one ray through the whole scene - TLAS nodes, plus each instance's BLAS
	// cost weighted by how likely a ray is to reach it - in the SAH's units (roughly node visits +
	// triangle tests). The number to compare scenes, builders and layouts by, and what the render
	// budget estimates frame cost from
	float sceneSahCost { 0.f };
	double buildMs { 0.0 };
};

Tlas buildTlas(std::span<const Blas* const> blases, std::span<const SceneInstance> instances);

struct SceneHit {
	bool hit { false };
	float t { 1e30f };
	uint32_t instance { 0 };
	// Mesh: the hit triangle's slot in its BLAS (Blas::triangles), and its barycentrics. Nothing more
	// for a shape: its normal follows from the hit point
	uint32_t slot { 0 };
	glm::vec2 barycentrics { 0.f };
	uint32_t nodesVisited { 0 };
	uint32_t trianglesTested { 0 };
	bool stackOverflow { false };
};

// the closest hit of a world-space ray, as the GPU finds it - or, for a ray with anyHit set, the
// first hit found
SceneHit traceScene(const Tlas& tlas, std::span<const Blas* const> blases, std::span<const SceneInstance> instances, const BvhRay& ray);

// whether anything lies on the ray within [tMin, tMax]: a shadow ray, answered by the first hit found
// (shaders/rt/include/crt_bvh.glsl occluded())
bool occludedScene(const Tlas& tlas, std::span<const Blas* const> blases, std::span<const SceneInstance> instances, const BvhRay& ray);

// The point a hit landed on, as the GPU's surfaceInteraction() computes it under
// CRT_FLAG_ERROR_BOUNDS (shaders/rt/include/crt_interaction.glsl): in world space, with the absolute
// error of each coordinate (PBR 4ed 6.8.5) and the geometric normal (unit, in no particular
// orientation). A triangle's point is rebuilt from its barycentrics, a shape's from the object-space
// ray and put exactly on the primitive. For tests: nothing in the renderer calls it
struct SceneHitPoint {
	glm::vec3 p { 0.f };
	glm::vec3 pError { 0.f };
	glm::vec3 n { 0.f, 1.f, 0.f };
	// a shape: the point in the unit primitive's space and the part of it hit (shapePart())
	glm::vec3 objectPoint { 0.f };
	uint32_t part { 0 };
};

// `ray` is the world-space ray that found `hit` (direction normalised)
SceneHitPoint sceneHitPoint(const Tlas& tlas, std::span<const Blas* const> blases, std::span<const SceneInstance> instances, const SceneHit& hit, const BvhRay& ray);

// a point of an object's space in world space under `objectToWorld`, with the error of its
// coordinates: `pError` carried through the transform plus the transform's own rounding (PBR 4ed
// 6.8.5), and the round trip through `worldToObject` and back, which measures how far the two
// matrices are from inverting each other. As toWorldPoint() in crt_common.glsl
glm::vec3 transformPointWithError(const glm::mat4& objectToWorld, const glm::mat4& worldToObject, const glm::vec3& p, const glm::vec3& pError, glm::vec3& worldError);
