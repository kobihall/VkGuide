#pragma once

// The two-level acceleration structure, independent of Vulkan and of the engine: one bottom-level
// BVH (BLAS) per unique mesh, in the mesh's object space and built once for as long as the mesh is
// loaded, and a top-level BVH (TLAS) over the placed instances of those meshes - plus the
// analytic spheres - rebuilt whenever an object moves. A ray is intersected with an instance by
// transforming it into the instance's object space, so one BLAS serves every placement of its
// mesh however it is translated, rotated or scaled (the "instances" of RTNW §8, the TLAS/BLAS of
// Bikker's articles 5-6, and the structure of VK_KHR_acceleration_structure that GPSnoopy's
// renderer drives in hardware).
//
// traceScene() is the CPU mirror of the GPU's two-level traversal (shaders/crt_bvh.glsl).

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <bvh.h>
#include <bvh_layout.h>

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
	Sphere
};

struct SceneInstance {
	SceneInstanceKind kind { SceneInstanceKind::Mesh };
	// Mesh: which BLAS, and where it is placed
	uint32_t blas { 0 };
	glm::mat4 objectToWorld { 1.f };
	// Sphere: centre xyz and radius w, in world space
	glm::vec4 sphere { 0.f };
};

struct Tlas {
	// binary layout over the instances; its primOrder is the instance in each slot
	PackedBvh bvh;
	// per instance, by instance index: the inverse transform the ray is carried into object space
	// with, and whether the instance is traced at all (a singular transform, a missing BLAS or a
	// non-positive radius is left out)
	std::vector<glm::mat4> worldToObject;
	std::vector<uint8_t> traced;
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
	// Mesh: the hit triangle's slot in its BLAS (Blas::triangles), and its barycentrics
	uint32_t slot { 0 };
	glm::vec2 barycentrics { 0.f };
	uint32_t nodesVisited { 0 };
	uint32_t trianglesTested { 0 };
	bool stackOverflow { false };
};

// the closest hit of a world-space ray, as the GPU finds it
SceneHit traceScene(const Tlas& tlas, std::span<const Blas* const> blases, std::span<const SceneInstance> instances, const BvhRay& ray);

// the shader's numerically stable ray-sphere (crt_common.glsl hitSphere); `direction` normalised
bool intersectSphere(const glm::vec4& sphere, const glm::vec3& origin, const glm::vec3& direction, float tMin, float tMax, float& t);
