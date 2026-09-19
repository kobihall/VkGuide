// The object-split strategies: every reference goes wholly to one side, by its centroid.

#include <bvh_split.h>

#include <algorithm>
#include <numeric>

namespace bvh_detail {

namespace {

void boundsOf(const std::vector<PrimRef>& refs, Aabb& bounds)
{
	bounds = {};
	for (const PrimRef& ref : refs) {
		bounds.grow(ref.box);
	}
}

// the bin a centroid falls in along `axis`, for `binCount` bins spanning `centroids`
struct Binner {
	int axis;
	float origin;
	float scale;
	uint32_t binCount;

	Binner(const Aabb& centroids, int axis, uint32_t binCount)
		: axis(axis)
		, origin(centroids.min[axis])
		, binCount(binCount)
	{
		//scaled a hair under binCount so the maximum centroid lands in the last bin, not past it
		const float extent = centroids.max[axis] - centroids.min[axis];
		scale = extent > 0.f ? (float)binCount * (1.f - 1e-6f) / extent : 0.f;
	}

	uint32_t operator()(const glm::vec3& c) const
	{
		const int bin = (int)((c[axis] - origin) * scale);
		return (uint32_t)std::clamp(bin, 0, (int)binCount - 1);
	}
};

}

SplitOutcome splitMidpoint(std::vector<PrimRef>& refs, const Aabb& /*bounds*/, BuildContext& context, float /*leafCostArea*/, SplitResult& out)
{
	//no cost model: the size limit alone ends the recursion (Bikker part 1 stops at two)
	if (refs.size() <= context.options.maxLeafSize) {
		return SplitOutcome::Leaf;
	}

	//the middle of the centroid bounds rather than of the node's box (Bikker, RTNW): a split plane
	//inside the node's box but outside every centroid would put everything on one side
	const Aabb centroids = centroidBounds(refs);
	const int axis = centroids.longestAxis();
	const float plane = centroids.center()[axis];

	auto middle = std::partition(refs.begin(), refs.end(), [&](const PrimRef& ref) { return centroid(ref)[axis] < plane; });
	if (middle == refs.begin() || middle == refs.end()) {
		return SplitOutcome::None;
	}

	out.left.assign(refs.begin(), middle);
	out.right.assign(middle, refs.end());
	boundsOf(out.left, out.leftBounds);
	boundsOf(out.right, out.rightBounds);
	out.costArea = std::numeric_limits<float>::quiet_NaN();
	refs.clear();
	return SplitOutcome::Split;
}

BinnedObjectSplit findBinnedObjectSplit(const std::vector<PrimRef>& refs, const Aabb& bounds, const Aabb& centroids, const BuildContext& context)
{
	const uint32_t binCount = binCountFor(context.options, refs.size());
	const float parentArea = bounds.halfArea();

	struct Bin {
		Aabb bounds;
		uint32_t count { 0 };
	};
	//per-thread scratch rather than locals: a local array of MAX_BINS boxes is constructed in full on
	//every call, and at one call per node that construction outweighed the search itself
	thread_local std::vector<Bin> bins(MAX_BINS);
	thread_local std::vector<float> rightArea(MAX_BINS);
	thread_local std::vector<uint32_t> rightCount(MAX_BINS);

	BinnedObjectSplit best;
	best.binCount = binCount;
	for (int axis = 0; axis < 3; axis++) {
		if (!(centroids.max[axis] > centroids.min[axis])) {
			continue;
		}

		std::fill(bins.begin(), bins.begin() + binCount, Bin {});
		const Binner binner(centroids, axis, binCount);
		for (const PrimRef& ref : refs) {
			Bin& bin = bins[binner(centroid(ref))];
			bin.bounds.grow(ref.box);
			bin.count++;
		}

		//sweep from the right storing the area and count to the right of each plane, then from the
		//left evaluating each of the binCount - 1 planes (Wald 2007)
		Aabb accumulated;
		uint32_t count = 0;
		for (uint32_t i = binCount - 1; i > 0; i--) {
			accumulated.grow(bins[i].bounds);
			count += bins[i].count;
			rightArea[i] = accumulated.halfArea();
			rightCount[i] = count;
		}

		accumulated = {};
		count = 0;
		for (uint32_t i = 0; i + 1 < binCount; i++) {
			accumulated.grow(bins[i].bounds);
			count += bins[i].count;
			if (count == 0 || rightCount[i + 1] == 0) {
				continue;
			}
			const float cost = splitCostArea(context, parentArea, accumulated.halfArea(), count, rightArea[i + 1], rightCount[i + 1]);
			if (cost < best.costArea) {
				best.axis = axis;
				best.bin = i + 1;
				best.costArea = cost;
				best.leftCount = count;
			}
		}
	}

	if (best.axis >= 0) {
		//the winning children's boxes, which the spatial split needs for its overlap test
		const Binner binner(centroids, best.axis, binCount);
		for (const PrimRef& ref : refs) {
			(binner(centroid(ref)) < best.bin ? best.leftBounds : best.rightBounds).grow(ref.box);
		}
	}
	return best;
}

void applyBinnedObjectSplit(std::vector<PrimRef>& refs, const Aabb& centroids, const BinnedObjectSplit& split, SplitResult& out)
{
	const Binner binner(centroids, split.axis, split.binCount);
	out.left.clear();
	out.right.clear();
	out.left.reserve(split.leftCount);
	out.right.reserve(refs.size() - split.leftCount);
	for (const PrimRef& ref : refs) {
		(binner(centroid(ref)) < split.bin ? out.left : out.right).push_back(ref);
	}
	out.leftBounds = split.leftBounds;
	out.rightBounds = split.rightBounds;
	out.costArea = split.costArea;
	refs.clear();
}

SplitOutcome splitBinnedSah(std::vector<PrimRef>& refs, const Aabb& bounds, BuildContext& context, float leafCostArea, SplitResult& out)
{
	const Aabb centroids = centroidBounds(refs);
	const BinnedObjectSplit split = findBinnedObjectSplit(refs, bounds, centroids, context);
	if (split.axis < 0) {
		return SplitOutcome::None;
	}
	if (preferLeaf(context, refs.size(), split.costArea, leafCostArea)) {
		return SplitOutcome::Leaf;
	}
	applyBinnedObjectSplit(refs, centroids, split, out);
	return SplitOutcome::Split;
}

SplitOutcome splitSweepSah(std::vector<PrimRef>& refs, const Aabb& bounds, BuildContext& context, float leafCostArea, SplitResult& out)
{
	//Bikker part 2 evaluates the SAH at every centroid by re-partitioning for each one, O(n^2) per
	//node. Sorting the references along each axis gives the same candidates - every boundary
	//between two adjacent centroids - with the box on either side from one prefix and one suffix
	//sweep, O(n log n) per node. The split is by rank in that order, so references sharing a
	//centroid can still be separated
	const size_t n = refs.size();
	const float parentArea = bounds.halfArea();

	std::vector<uint32_t> order[3];
	std::vector<float> rightArea(n);
	float bestCost = std::numeric_limits<float>::infinity();
	int bestAxis = -1;
	size_t bestRank = 0;

	for (int axis = 0; axis < 3; axis++) {
		std::vector<uint32_t>& sorted = order[axis];
		sorted.resize(n);
		std::iota(sorted.begin(), sorted.end(), 0u);
		std::sort(sorted.begin(), sorted.end(), [&](uint32_t a, uint32_t b) {
			const float ca = centroid(refs[a])[axis];
			const float cb = centroid(refs[b])[axis];
			return ca < cb || (ca == cb && refs[a].prim < refs[b].prim);
		});

		Aabb accumulated;
		for (size_t i = n - 1; i > 0; i--) {
			accumulated.grow(refs[sorted[i]].box);
			rightArea[i] = accumulated.halfArea();
		}

		accumulated = {};
		for (size_t i = 0; i + 1 < n; i++) {
			accumulated.grow(refs[sorted[i]].box);
			//a plane between two equal centroids is not a plane on this axis; skip it here, the
			//other axes (or the median fallback) will separate them
			if (centroid(refs[sorted[i]])[axis] == centroid(refs[sorted[i + 1]])[axis]) {
				continue;
			}
			const float cost = splitCostArea(context, parentArea, accumulated.halfArea(), i + 1, rightArea[i + 1], n - i - 1);
			if (cost < bestCost) {
				bestCost = cost;
				bestAxis = axis;
				bestRank = i + 1;
			}
		}
	}

	if (bestAxis < 0) {
		return SplitOutcome::None;
	}
	if (preferLeaf(context, n, bestCost, leafCostArea)) {
		return SplitOutcome::Leaf;
	}

	const std::vector<uint32_t>& sorted = order[bestAxis];
	out.left.clear();
	out.right.clear();
	out.left.reserve(bestRank);
	out.right.reserve(n - bestRank);
	for (size_t i = 0; i < n; i++) {
		(i < bestRank ? out.left : out.right).push_back(refs[sorted[i]]);
	}
	boundsOf(out.left, out.leftBounds);
	boundsOf(out.right, out.rightBounds);
	out.costArea = bestCost;
	refs.clear();
	return SplitOutcome::Split;
}

}
