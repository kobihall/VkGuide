#pragma once

// Bounding volume hierarchies, independent of Vulkan and of the scene: the generic binary tree
// every builder produces, the builders themselves (behind one options struct, so they can be
// swapped and compared), and the quality measures to compare them by.
//
// A builder only ever sees primitives as boxes (plus, for triangles, their vertices - the spatial
// split builder clips triangles against planes). It produces a Bvh: a plain binary tree with
// explicit child indices and leaf ranges into a primitive-reference list. That tree is never
// traced directly; bvh_layout.h converts it into one of the GPU node layouts. Keeping the two
// apart is what lets any builder feed any layout.
//
// References:
//  - Midpoint split and the basic node: Bikker, "How to build a BVH - part 1: basics" (2022), and
//    the longest-axis split of Shirley, "Ray Tracing: The Next Week" §3.
//  - The surface area heuristic, and SAH-driven leaf termination: Bikker part 2; MacDonald &
//    Booth, "Heuristics for ray tracing using space subdivision" (1990).
//  - Binned SAH: Wald, "On fast construction of SAH-based bounding volume hierarchies" (2007).
//  - Spatial splits: Stich, Friedrich & Dietrich, "Spatial splits in bounding volume
//    hierarchies" (2009).

#include <cfloat>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

struct Aabb {
	glm::vec3 min { FLT_MAX };
	glm::vec3 max { -FLT_MAX };

	void grow(const glm::vec3& p)
	{
		min = glm::min(min, p);
		max = glm::max(max, p);
	}
	void grow(const Aabb& b)
	{
		min = glm::min(min, b.min);
		max = glm::max(max, b.max);
	}
	// an empty box is inverted on every axis, so growing it by anything yields that thing
	bool empty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }
	glm::vec3 extent() const { return max - min; }
	glm::vec3 center() const { return (min + max) * 0.5f; }
	// half the surface area. The SAH only ever compares areas, so the factor of two is dropped
	// everywhere, consistently
	float halfArea() const
	{
		if (empty()) {
			return 0.f;
		}
		const glm::vec3 e = max - min;
		return e.x * e.y + e.y * e.z + e.z * e.x;
	}
	int longestAxis() const
	{
		const glm::vec3 e = extent();
		return e.x >= e.y && e.x >= e.z ? 0 : (e.y >= e.z ? 1 : 2);
	}
	bool contains(const Aabb& b, float epsilon = 0.f) const
	{
		return glm::all(glm::lessThanEqual(min - epsilon, b.min)) && glm::all(glm::greaterThanEqual(max + epsilon, b.max));
	}

	static Aabb intersection(const Aabb& a, const Aabb& b)
	{
		Aabb out;
		out.min = glm::max(a.min, b.min);
		out.max = glm::min(a.max, b.max);
		return out;
	}
};

// One node of the generic binary tree. Interior nodes name both children explicitly (rather than
// Bikker's implicit `left + 1`), because the tree is an intermediate: the layouts re-order the
// nodes anyway, and explicit indices let a builder emit them in whatever order it builds them.
struct BvhNode {
	Aabb bounds;
	// interior: the two children
	uint32_t left { 0 };
	uint32_t right { 0 };
	// leaf: Bvh::primRefs[first, first + count). A node is a leaf exactly when count > 0
	uint32_t first { 0 };
	uint32_t count { 0 };

	bool isLeaf() const { return count > 0; }
};

// A binary BVH over `primitiveCount` primitives. Root at nodes[0]; empty when there are no
// primitives. primRefs holds primitive indices in leaf order. With spatial splits one primitive
// may be referenced from several leaves, so primRefs can be longer than primitiveCount.
struct Bvh {
	std::vector<BvhNode> nodes;
	std::vector<uint32_t> primRefs;
	uint32_t primitiveCount { 0 };

	bool empty() const { return nodes.empty(); }
};

// What a builder is given: one box per primitive, and for triangle primitives their vertices
// (three per primitive). Only the spatial-split builder reads the vertices; with none given it
// degrades to its object splits.
struct BvhBuildInput {
	std::span<const Aabb> boxes;
	std::span<const glm::vec3> triangleVertices;
};

// The construction methods, cheapest and worst first. Every one produces the same Bvh shape, so
// they can be swapped freely and compared on the same scene.
enum class BvhBuilder : uint32_t {
	// longest axis of the centroid bounds, split in the middle (Bikker part 1, RTNW). No cost
	// model: splits until a node has at most maxLeafSize primitives. The baseline
	Midpoint,
	// SAH evaluated at binCount evenly spaced planes per axis (Wald 2007). The usual trade-off:
	// near-sweep quality at a fraction of the cost
	BinnedSah,
	// SAH evaluated between every pair of adjacent centroids on every axis (Bikker part 2, the
	// exhaustive search, done in O(n log n) per node by sorting). The best object-split tree
	SweepSah,
	// binned SAH plus spatial splits, which may cut a triangle in two and reference it from both
	// sides when that removes enough overlap (Stich et al. 2009, "SBVH"). The best tree here for
	// meshes with long or large triangles (architecture); the slowest to build
	SpatialSah,
	Count
};

const char* bvhBuilderName(BvhBuilder builder);

struct BvhBuildOptions {
	BvhBuilder builder { BvhBuilder::SpatialSah };
	// SAH costs, relative: visiting a node, and intersecting one primitive. Only their ratio matters
	float traversalCost { 1.f };
	float intersectionCost { 1.f };
	// a leaf never holds more than this. Below it, the SAH alone decides when to stop splitting
	uint32_t maxLeafSize { 4 };
	// planes per axis for the binned searches (object and spatial)
	uint32_t binCount { 32 };
	// spatial splits: only tried when the best object split's children overlap by more than this
	// fraction of the root's area (Stich's alpha; 1e-5 in the paper, 1 disables spatial splits)
	float spatialAlpha { 1e-5f };
	// spatial splits: at most this many extra references, as a fraction of the primitive count
	float spatialBudget { 0.3f };
	// the builder makes a leaf at this depth whatever its size, so no traversal stack can overflow
	uint32_t maxDepth { 48 };
	// build large subtrees on worker threads. The result is identical either way
	bool parallel { true };

	bool operator==(const BvhBuildOptions&) const = default;
};

Bvh buildBvh(const BvhBuildInput& input, const BvhBuildOptions& options);

// Measures of a tree's quality, for comparing builders. `sahCost` is the expected cost of one
// random ray in units of primitive intersections (Ct x node visits + Ci x primitive tests) - the
// number the SAH builders minimise, and the one to compare. It ignores traversal order and early
// termination, so it predicts relative rather than absolute performance.
struct BvhStats {
	uint32_t nodes { 0 };
	uint32_t leaves { 0 };
	uint32_t primRefs { 0 };
	uint32_t maxDepth { 0 };
	uint32_t maxLeafSize { 0 };
	float averageLeafSize { 0.f };
	float averageLeafDepth { 0.f };
	float sahCost { 0.f };
};

BvhStats computeBvhStats(const Bvh& bvh, float traversalCost = 1.f, float intersectionCost = 1.f);

// Structural checks: every node reachable exactly once, indices in range, children inside their
// parent, every primitive referenced and every reference's primitive overlapping its leaf.
// Returns an empty string for a valid tree, otherwise the first problem found. Cheap enough to
// run on every build - a malformed tree is not a slow render on the GPU but a hung one.
std::string validateBvh(const Bvh& bvh, const BvhBuildInput& input);

// Splits every leaf with more than maxPrims references into a subtree of leaves that have at most
// that many, by median split along the longest axis of their centroids. The wide layout needs
// this (a CWBVH leaf holds at most three triangles), whatever the builder decided.
void limitLeafSize(Bvh& bvh, const BvhBuildInput& input, uint32_t maxPrims);
