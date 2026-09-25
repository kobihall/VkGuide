#pragma once

// The node layouts a GPU traverses, converted from the generic binary tree of bvh.h. Any builder
// feeds any layout; the traversal for each lives in shaders/crt_bvh.glsl and, mirrored line for
// line, in traceBvh() below, so a packed tree can be checked against brute force on the CPU
// before a GPU ever walks it.
//
// References:
//  - Binary: Aila & Laine, "Understanding the efficiency of ray traversal on GPUs" (2009) - both
//    children's boxes stored in the parent, so one node fetch tests two boxes and orders them;
//    leaves are folded into their parent's child slot. The GPU layout of Bikker's OpenCL
//    articles and tinybvh's BVH_GPU.
//  - Cwbvh8: Ylitie, Karras & Laine, "Efficient incoherent ray traversal on GPUs through
//    compressed wide BVHs" (2017) - eight children per node with their boxes quantised to 8 bits
//    against the node's own box (80 bytes for eight boxes), children ordered per ray octant, and
//    a traversal driven by hit bitmasks instead of per-node distance sorting. The fastest
//    software GPU layout in the literature, and in tinybvh's measurements.

#include <algorithm>
#include <bit>
#include <cmath>
#include <span>
#include <string>
#include <vector>

#include <bvh.h>

enum class BvhLayout : uint32_t {
	Binary,
	Cwbvh8,
	Count
};

const char* bvhLayoutName(BvhLayout layout);

// the traversal stack depth each layout's shader allocates; packBvh() refuses a tree deeper than
// this, and the traversals stop pushing rather than overflow if it is somehow exceeded
inline constexpr uint32_t BVH_BINARY_STACK_SIZE = 64;
inline constexpr uint32_t BVH_CWBVH_STACK_SIZE = 32;

// the most primitives one CWBVH leaf child can hold: its three-bit unary count in the node's meta byte
inline constexpr uint32_t BVH_CWBVH_MAX_LEAF_SIZE = 3;

// One triangle as the traversal intersects it: the first vertex and the two edges from it, which
// is exactly what Moller-Trumbore consumes. 48 bytes; the shading attributes are fetched separately
// and only for the closest hit. v0.w carries the triangle's own index (as float bits) so the hit
// can find those attributes even though leaves re-order and spatial splits duplicate triangles.
struct BvhTriangle {
	glm::vec4 v0;
	glm::vec4 e1;
	glm::vec4 e2;
};
static_assert(sizeof(BvhTriangle) == 48);

// v0.w's bits: the triangle's index in the low 31, and in the top one a flag the engine sets for a
// triangle whose material can cut holes in it (glTF alphaMode MASK). The GPU traversal alpha-tests
// a flagged candidate against its texture before accepting it (shaders/rt/include/crt_traverse.glsl);
// the CPU traversals here have no textures and treat every triangle as opaque
inline constexpr uint32_t BVH_TRIANGLE_INDEX_MASK = 0x7FFFFFFFu;
inline constexpr uint32_t BVH_TRIANGLE_CUTOUT = 0x80000000u;

BvhTriangle makeBvhTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, uint32_t index);

// A tree in one layout, as the words the GPU reads.
//
// Binary: 4 uvec4 (64 bytes) per node, root at 0. Word k of node i is nodes[4i + k]:
//   0: left child box min xyz (float bits), left index      2: right box min xyz, left count
//   1: left child box max xyz,               right index    3: right box max xyz, right count
//   A child with count > 0 is a leaf: its index is its first primitive slot. Otherwise it is
//   the node index of an interior node.
// Cwbvh8: 5 uvec4 (80 bytes) per node, root at 0:
//   0: origin xyz (float bits), w = exponent x | y << 8 | z << 16 | interior mask << 24
//   1: x = first child node, y = first primitive slot, zw = meta bytes of children 0-7
//   2: lo x of children 0-7, lo y            3: lo z, hi x            4: hi y, hi z
//
// `primOrder` is the primitive in each slot: leaves index slots, so the caller lays its
// primitives out in this order (duplicates included).
struct PackedBvh {
	BvhLayout layout { BvhLayout::Binary };
	std::vector<glm::uvec4> nodes;
	std::vector<uint32_t> primOrder;
	uint32_t nodeCount { 0 };
	// levels of interior nodes, root = 1; what the traversal stack has to hold
	uint32_t depth { 0 };
	// empty when the tree packed; otherwise why not, and the tree must not be traced
	std::string error;

	uint32_t wordsPerNode() const { return layout == BvhLayout::Cwbvh8 ? 5u : 4u; }
};

// Takes the tree by value: the wide layout first splits leaves above BVH_CWBVH_MAX_LEAF_SIZE,
// which needs the primitives' boxes. An empty tree packs to zero nodes.
PackedBvh packBvh(Bvh bvh, const BvhBuildInput& input, BvhLayout layout);

// ---- CPU traversal, a mirror of shaders/crt_bvh.glsl

struct BvhRay {
	glm::vec3 origin { 0.f };
	glm::vec3 direction { 0.f, 0.f, -1.f };
	float tMin { 0.f };
	float tMax { 1e30f };
};

struct BvhHit {
	bool hit { false };
	float t { 1e30f };
	// the slot of the hit primitive (an index into primOrder, and into the triangles)
	uint32_t slot { 0 };
	glm::vec2 barycentrics { 0.f };
	// the cost counters the GPU's traversal-heat debug view shows
	uint32_t nodesVisited { 0 };
	uint32_t trianglesTested { 0 };
	// the stack would have overflowed; the result may have missed geometry
	bool stackOverflow { false };
};

// the shader's Moller-Trumbore; two-sided, t in [tMin, tMax]
bool intersectBvhTriangle(const BvhTriangle& triangle, const glm::vec3& origin, const glm::vec3& direction, float tMin, float tMax, float& t, glm::vec2& barycentrics);

// the closest hit along the ray, `triangles` being in primOrder's slot order
BvhHit traceBvh(const PackedBvh& bvh, std::span<const BvhTriangle> triangles, const BvhRay& ray);

// ---- the traversal building blocks, shared with the TLAS mirror in bvh_scene.cpp

// the shader's guard against a malformed tree: a real traversal never comes close
inline constexpr uint32_t BVH_MAX_TRAVERSAL_STEPS = 1u << 16;

// 1 / d with each zero component replaced by a tiny signed one, so no box test multiplies 0 by
// infinity (NaN compares false both ways, and a NaN slab would admit or reject arbitrarily)
inline glm::vec3 bvhSafeReciprocal(const glm::vec3& d)
{
	glm::vec3 out;
	for (int i = 0; i < 3; i++) {
		const float component = std::abs(d[i]) < 1e-12f ? (d[i] >= 0.f ? 1e-12f : -1e-12f) : d[i];
		out[i] = 1.f / component;
	}
	return out;
}

// near distance of a hit box, or 1e30 for a miss (Bikker part 2's IntersectAABB)
inline float bvhSlab(const glm::vec3& bmin, const glm::vec3& bmax, const glm::vec3& origin, const glm::vec3& reciprocal, float tMin, float tMax)
{
	const glm::vec3 t0 = (bmin - origin) * reciprocal;
	const glm::vec3 t1 = (bmax - origin) * reciprocal;
	const glm::vec3 near = glm::min(t0, t1);
	const glm::vec3 far = glm::max(t0, t1);
	const float tNear = std::max(std::max(near.x, near.y), std::max(near.z, tMin));
	const float tFar = std::min(std::min(far.x, far.y), std::min(far.z, tMax));
	return tNear <= tFar ? tNear : 1e30f;
}

// The binary layout's traversal (shaders/crt_bvh.glsl traverseBinary), with the leaf test left to
// the caller: `testLeaf(first, count, tMax)` intersects slots [first, first + count) and lowers
// tMax on a hit. Near child first, far child on the stack.
template<typename LeafTest>
void traverseBinaryBvh(const PackedBvh& bvh, const BvhRay& ray, float& tMax, LeafTest&& testLeaf, uint32_t& nodesVisited, bool& stackOverflow)
{
	if (bvh.nodeCount == 0) {
		return;
	}
	const glm::vec3 reciprocal = bvhSafeReciprocal(ray.direction);
	uint32_t stack[BVH_BINARY_STACK_SIZE];
	uint32_t stackSize = 0;
	uint32_t node = 0;

	for (uint32_t step = 0; step < BVH_MAX_TRAVERSAL_STEPS; step++) {
		nodesVisited++;
		const glm::uvec4* words = &bvh.nodes[(size_t)node * 4];
		auto vec = [](const glm::uvec4& w) { return glm::vec3(std::bit_cast<float>(w.x), std::bit_cast<float>(w.y), std::bit_cast<float>(w.z)); };
		const uint32_t leftIndex = words[0].w;
		const uint32_t rightIndex = words[1].w;
		const uint32_t leftCount = words[2].w;
		const uint32_t rightCount = words[3].w;

		float tLeft = bvhSlab(vec(words[0]), vec(words[1]), ray.origin, reciprocal, ray.tMin, tMax);
		float tRight = bvhSlab(vec(words[2]), vec(words[3]), ray.origin, reciprocal, ray.tMin, tMax);

		//leaves are folded into the parent: intersect their contents now, and only interior
		//children remain to be ordered
		if (leftCount > 0 && tLeft < 1e30f) {
			testLeaf(leftIndex, leftCount, tMax);
			tLeft = 1e30f;
		}
		if (rightCount > 0 && tRight < 1e30f) {
			testLeaf(rightIndex, rightCount, tMax);
			tRight = 1e30f;
		}

		if (tLeft < 1e30f && tRight < 1e30f) {
			//both: continue into the nearer, keep the farther for later (Bikker part 2's ordering)
			const bool leftFirst = tLeft <= tRight;
			if (stackSize < BVH_BINARY_STACK_SIZE) {
				stack[stackSize++] = leftFirst ? rightIndex : leftIndex;
			} else {
				stackOverflow = true;
			}
			node = leftFirst ? leftIndex : rightIndex;
		} else if (tLeft < 1e30f) {
			node = leftIndex;
		} else if (tRight < 1e30f) {
			node = rightIndex;
		} else {
			if (stackSize == 0) {
				return;
			}
			node = stack[--stackSize];
		}
	}
}
