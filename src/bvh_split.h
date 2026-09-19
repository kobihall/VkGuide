#pragma once

// Internal to the BVH builders (bvh_build.cpp and bvh_split_*.cpp): the top-down framework hands
// each node's primitive references to one split strategy, and the strategy divides them in two.
// Everything else - leaf decisions, recursion, threading, the output tree - is the framework's,
// so a strategy is only the part that actually differs between builders.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>

#include <bvh.h>

namespace bvh_detail {

// One reference to a primitive: its box, which a spatial split may have clipped to a part of the
// primitive's own box
struct PrimRef {
	Aabb box;
	uint32_t prim { 0 };
};

inline glm::vec3 centroid(const PrimRef& ref)
{
	return ref.box.center();
}

struct BuildContext {
	const BvhBuildInput& input;
	const BvhBuildOptions& options;
	float rootHalfArea { 0.f };
	// spatial splits: how many more references the budget allows, shared by every thread
	std::atomic<int64_t> spareReferences { 0 };
};

// The outcome of a split. `costArea` is the split's SAH cost scaled by the parent's area,
// Ct * A + Ci * (A_left * n_left + A_right * n_right) - kept unnormalised so a zero-area node needs
// no special case. NaN from a strategy with no cost model (midpoint).
struct SplitResult {
	std::vector<PrimRef> left;
	std::vector<PrimRef> right;
	Aabb leftBounds;
	Aabb rightBounds;
	float costArea { std::numeric_limits<float>::quiet_NaN() };
};

enum class SplitOutcome {
	// `out` holds both halves and `refs` has been consumed
	Split,
	// the best split found costs no less than keeping the node as a leaf (the SAH termination of
	// Bikker part 2); `refs` is untouched
	Leaf,
	// no split puts references on both sides; `refs` is untouched
	None
};

// Divides `refs` (whose union is `bounds`) in two. `leafCostArea` is what keeping them as one leaf
// would cost, Ci * n * A, in the same units as SplitResult::costArea.
using SplitStrategy = SplitOutcome (*)(std::vector<PrimRef>& refs, const Aabb& bounds, BuildContext& context, float leafCostArea, SplitResult& out);

SplitOutcome splitMidpoint(std::vector<PrimRef>& refs, const Aabb& bounds, BuildContext& context, float leafCostArea, SplitResult& out);
SplitOutcome splitBinnedSah(std::vector<PrimRef>& refs, const Aabb& bounds, BuildContext& context, float leafCostArea, SplitResult& out);
SplitOutcome splitSweepSah(std::vector<PrimRef>& refs, const Aabb& bounds, BuildContext& context, float leafCostArea, SplitResult& out);
SplitOutcome splitSpatialSah(std::vector<PrimRef>& refs, const Aabb& bounds, BuildContext& context, float leafCostArea, SplitResult& out);

// A node may stay a leaf when it is small enough and no split beats it. Above maxLeafSize a node
// is split with the strategy's best candidate however poor, so leaf size stays bounded
inline bool preferLeaf(const BuildContext& context, size_t count, float splitCostArea, float leafCostArea)
{
	return count <= context.options.maxLeafSize && !(splitCostArea < leafCostArea);
}

// The split every strategy falls back on: the references sorted by centroid along the longest
// centroid axis and cut at the median rank. Always succeeds for two or more references, even when
// every centroid coincides, which is what guarantees the recursion ends.
void splitMedian(std::vector<PrimRef>& refs, SplitResult& out);

Aabb centroidBounds(const std::vector<PrimRef>& refs);

// ---- the binned object split, shared by BinnedSah and SpatialSah

// the most bins either binned search uses; their working arrays live on the stack at this size
inline constexpr uint32_t MAX_BINS = 256;

struct BinnedObjectSplit {
	int axis { -1 };
	uint32_t binCount { 0 };
	// references whose bin is below this go left
	uint32_t bin { 0 };
	float costArea { std::numeric_limits<float>::infinity() };
	Aabb leftBounds;
	Aabb rightBounds;
	uint32_t leftCount { 0 };
};

BinnedObjectSplit findBinnedObjectSplit(const std::vector<PrimRef>& refs, const Aabb& bounds, const Aabb& centroids, const BuildContext& context);
void applyBinnedObjectSplit(std::vector<PrimRef>& refs, const Aabb& centroids, const BinnedObjectSplit& split, SplitResult& out);

// the binned searches' plane count for a node of `count` references: the configured count, but
// never more bins than references (and at least two). A node of three triangles gains nothing from
// 32 candidate planes, and nodes that small are most of the tree - with the SAH splitting almost
// to single triangles, sweeping empty bins was most of the build time
inline uint32_t binCountFor(const BvhBuildOptions& options, size_t count)
{
	return std::clamp<uint32_t>((uint32_t)std::min<size_t>(count, options.binCount), 2u, MAX_BINS);
}

inline float splitCostArea(const BuildContext& context, float parentArea, float leftArea, size_t leftCount, float rightArea, size_t rightCount)
{
	return context.options.traversalCost * parentArea
		+ context.options.intersectionCost * (leftArea * (float)leftCount + rightArea * (float)rightCount);
}

}
