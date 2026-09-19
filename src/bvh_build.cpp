#include <bvh.h>
#include <bvh_split.h>

#include <algorithm>
#include <cmath>
#include <future>
#include <thread>

#include <fmt/format.h>

using namespace bvh_detail;

const char* bvhBuilderName(BvhBuilder builder)
{
	switch (builder) {
	case BvhBuilder::Midpoint:
		return "midpoint";
	case BvhBuilder::BinnedSah:
		return "binned SAH";
	case BvhBuilder::SweepSah:
		return "sweep SAH";
	case BvhBuilder::SpatialSah:
		return "spatial splits (SBVH)";
	case BvhBuilder::Count:
		break;
	}
	return "unknown";
}

namespace bvh_detail {

Aabb centroidBounds(const std::vector<PrimRef>& refs)
{
	Aabb bounds;
	for (const PrimRef& ref : refs) {
		bounds.grow(centroid(ref));
	}
	return bounds;
}

void splitMedian(std::vector<PrimRef>& refs, SplitResult& out)
{
	const int axis = centroidBounds(refs).longestAxis();
	const size_t half = refs.size() / 2;
	//nth_element alone would leave ties in an unspecified order; the prim index breaks them, so the
	//result is the same on every run and every platform
	std::nth_element(refs.begin(), refs.begin() + half, refs.end(), [axis](const PrimRef& a, const PrimRef& b) {
		const float ca = centroid(a)[axis];
		const float cb = centroid(b)[axis];
		return ca < cb || (ca == cb && a.prim < b.prim);
	});

	out.left.assign(refs.begin(), refs.begin() + half);
	out.right.assign(refs.begin() + half, refs.end());
	out.leftBounds = {};
	out.rightBounds = {};
	for (const PrimRef& ref : out.left) {
		out.leftBounds.grow(ref.box);
	}
	for (const PrimRef& ref : out.right) {
		out.rightBounds.grow(ref.box);
	}
	out.costArea = std::numeric_limits<float>::quiet_NaN();
}

}

namespace {

// below this many references on either side a subtree is built on the calling thread: a task
// costs more than it saves on a small subtree
constexpr size_t PARALLEL_THRESHOLD = 8 * 1024;

// the part of the tree one thread builds: nodes indexed within it, leaves indexing its own refs
struct Subtree {
	std::vector<BvhNode> nodes;
	std::vector<uint32_t> primRefs;
};

SplitStrategy strategyFor(BvhBuilder builder)
{
	switch (builder) {
	case BvhBuilder::Midpoint:
		return splitMidpoint;
	case BvhBuilder::BinnedSah:
		return splitBinnedSah;
	case BvhBuilder::SweepSah:
		return splitSweepSah;
	case BvhBuilder::SpatialSah:
		return splitSpatialSah;
	case BvhBuilder::Count:
		break;
	}
	return splitBinnedSah;
}

// The recursion every builder shares: ask the strategy to split a node, make a leaf when it says
// so (or cannot, and the node is small enough), fall back to a median split when it cannot and
// the node is too big to be a leaf, and recurse. Large subtrees go to worker threads; the tree is
// the same either way, only the order its nodes are stored in differs, and every layout re-orders
// them anyway.
class TopDownBuilder {
public:
	TopDownBuilder(BuildContext& context, SplitStrategy strategy)
		: m_context(context)
		, m_strategy(strategy)
	{
		const unsigned hardware = std::max(std::thread::hardware_concurrency(), 1u);
		m_freeWorkers = context.options.parallel ? (int)hardware - 1 : 0;
	}

	uint32_t build(std::vector<PrimRef>&& refs, const Aabb& bounds, uint32_t depth, Subtree& out)
	{
		const BvhBuildOptions& options = m_context.options;
		const size_t count = refs.size();

		const uint32_t index = (uint32_t)out.nodes.size();
		out.nodes.emplace_back();
		out.nodes[index].bounds = bounds;

		if (count <= 1 || depth >= options.maxDepth) {
			makeLeaf(refs, index, out);
			return index;
		}

		const float leafCostArea = options.intersectionCost * (float)count * bounds.halfArea();
		SplitResult split;
		const SplitOutcome outcome = m_strategy(refs, bounds, m_context, leafCostArea, split);

		if (outcome == SplitOutcome::Leaf || (outcome == SplitOutcome::None && count <= options.maxLeafSize)) {
			makeLeaf(refs, index, out);
			return index;
		}
		if (outcome == SplitOutcome::None) {
			splitMedian(refs, split);
		}

		//the parent's references are no longer needed; releasing them before recursing keeps the
		//peak at about two copies of the references rather than one per level
		std::vector<PrimRef>().swap(refs);

		uint32_t left = 0;
		uint32_t right = 0;
		if (split.left.size() >= PARALLEL_THRESHOLD && split.right.size() >= PARALLEL_THRESHOLD && tryAcquireWorker()) {
			auto task = std::async(std::launch::async, [this, leftRefs = std::move(split.left), leftBounds = split.leftBounds, depth]() mutable {
				Subtree subtree;
				const uint32_t root = build(std::move(leftRefs), leftBounds, depth + 1, subtree);
				releaseWorker();
				return std::make_pair(std::move(subtree), root);
			});
			right = build(std::move(split.right), split.rightBounds, depth + 1, out);
			auto [subtree, subtreeRoot] = task.get();
			left = splice(subtree, subtreeRoot, out);
		} else {
			left = build(std::move(split.left), split.leftBounds, depth + 1, out);
			right = build(std::move(split.right), split.rightBounds, depth + 1, out);
		}

		out.nodes[index].left = left;
		out.nodes[index].right = right;
		return index;
	}

private:
	void makeLeaf(const std::vector<PrimRef>& refs, uint32_t index, Subtree& out)
	{
		BvhNode& node = out.nodes[index];
		node.first = (uint32_t)out.primRefs.size();
		node.count = (uint32_t)refs.size();
		for (const PrimRef& ref : refs) {
			out.primRefs.push_back(ref.prim);
		}
	}

	// appends a subtree built elsewhere, rebasing its indices; returns its root's new index
	static uint32_t splice(Subtree& from, uint32_t root, Subtree& into)
	{
		const uint32_t nodeOffset = (uint32_t)into.nodes.size();
		const uint32_t primOffset = (uint32_t)into.primRefs.size();
		for (BvhNode node : from.nodes) {
			if (node.isLeaf()) {
				node.first += primOffset;
			} else {
				node.left += nodeOffset;
				node.right += nodeOffset;
			}
			into.nodes.push_back(node);
		}
		into.primRefs.insert(into.primRefs.end(), from.primRefs.begin(), from.primRefs.end());
		return root + nodeOffset;
	}

	bool tryAcquireWorker()
	{
		int free = m_freeWorkers.load();
		while (free > 0) {
			if (m_freeWorkers.compare_exchange_weak(free, free - 1)) {
				return true;
			}
		}
		return false;
	}
	void releaseWorker() { m_freeWorkers++; }

	BuildContext& m_context;
	SplitStrategy m_strategy;
	std::atomic<int> m_freeWorkers { 0 };
};

bool finite(const Aabb& box)
{
	for (int axis = 0; axis < 3; axis++) {
		if (!std::isfinite(box.min[axis]) || !std::isfinite(box.max[axis])) {
			return false;
		}
	}
	return !box.empty();
}

}

Bvh buildBvh(const BvhBuildInput& input, const BvhBuildOptions& options)
{
	Bvh bvh;
	bvh.primitiveCount = (uint32_t)input.boxes.size();

	//a primitive with a NaN or infinite vertex has no meaningful box: it is left out entirely
	//rather than allowed to poison every box above it
	std::vector<PrimRef> refs;
	refs.reserve(input.boxes.size());
	Aabb bounds;
	for (uint32_t i = 0; i < (uint32_t)input.boxes.size(); i++) {
		if (finite(input.boxes[i])) {
			refs.push_back({ input.boxes[i], i });
			bounds.grow(input.boxes[i]);
		}
	}
	if (refs.empty()) {
		return bvh;
	}

	BuildContext context { input, options };
	context.rootHalfArea = bounds.halfArea();
	context.spareReferences = (int64_t)(options.spatialBudget * (float)refs.size());

	TopDownBuilder builder(context, strategyFor(options.builder));
	Subtree tree;
	tree.nodes.reserve(refs.size() * 2 / std::max(options.maxLeafSize, 1u) + 1);
	builder.build(std::move(refs), bounds, 0, tree);

	bvh.nodes = std::move(tree.nodes);
	bvh.primRefs = std::move(tree.primRefs);
	return bvh;
}

BvhStats computeBvhStats(const Bvh& bvh, float traversalCost, float intersectionCost)
{
	BvhStats stats;
	if (bvh.empty()) {
		return stats;
	}

	double interiorArea = 0.0;
	double leafArea = 0.0;
	double leafDepthSum = 0.0;

	std::vector<std::pair<uint32_t, uint32_t>> stack { { 0u, 0u } };
	while (!stack.empty()) {
		const auto [index, depth] = stack.back();
		stack.pop_back();
		const BvhNode& node = bvh.nodes[index];
		stats.nodes++;
		stats.maxDepth = std::max(stats.maxDepth, depth);
		if (node.isLeaf()) {
			stats.leaves++;
			stats.primRefs += node.count;
			stats.maxLeafSize = std::max(stats.maxLeafSize, node.count);
			leafArea += (double)node.bounds.halfArea() * node.count;
			leafDepthSum += depth;
		} else {
			interiorArea += node.bounds.halfArea();
			stack.push_back({ node.left, depth + 1 });
			stack.push_back({ node.right, depth + 1 });
		}
	}

	stats.averageLeafSize = (float)stats.primRefs / (float)std::max(stats.leaves, 1u);
	stats.averageLeafDepth = (float)(leafDepthSum / std::max(stats.leaves, 1u));
	const double rootArea = bvh.nodes[0].bounds.halfArea();
	//a zero-area root (everything at one point) has no meaningful ray probability; count every test
	stats.sahCost = rootArea > 0.0
		? (float)((traversalCost * interiorArea + intersectionCost * leafArea) / rootArea)
		: (float)(traversalCost * (stats.nodes - stats.leaves) + intersectionCost * stats.primRefs);
	return stats;
}

std::string validateBvh(const Bvh& bvh, const BvhBuildInput& input)
{
	const size_t primitiveCount = input.boxes.size();
	if (bvh.primitiveCount != primitiveCount) {
		return fmt::format("built over {} primitives, validated against {}", bvh.primitiveCount, primitiveCount);
	}
	if (bvh.empty()) {
		for (const Aabb& box : input.boxes) {
			if (finite(box)) {
				return "empty tree over a non-empty primitive set";
			}
		}
		return {};
	}

	std::vector<uint8_t> visited(bvh.nodes.size(), 0);
	std::vector<uint8_t> slotUsed(bvh.primRefs.size(), 0);
	std::vector<uint8_t> referenced(primitiveCount, 0);

	//generous: boxes are unions of floats and never recomputed, so containment should be exact, but
	//a clipped spatial-split box can differ from its parent's by rounding
	auto tolerance = [](const Aabb& box) {
		return 1e-5f * glm::max(glm::max(box.extent().x, box.extent().y), box.extent().z) + 1e-6f;
	};

	std::vector<uint32_t> stack { 0u };
	visited[0] = 1;
	while (!stack.empty()) {
		const uint32_t index = stack.back();
		stack.pop_back();
		const BvhNode& node = bvh.nodes[index];

		if (node.bounds.empty()) {
			return fmt::format("node {} has an empty box", index);
		}

		if (node.isLeaf()) {
			if ((size_t)node.first + node.count > bvh.primRefs.size()) {
				return fmt::format("leaf {} range [{}, +{}) is outside the {} references", index, node.first, node.count, bvh.primRefs.size());
			}
			for (uint32_t i = node.first; i < node.first + node.count; i++) {
				if (slotUsed[i]) {
					return fmt::format("reference slot {} belongs to two leaves", i);
				}
				slotUsed[i] = 1;
				const uint32_t prim = bvh.primRefs[i];
				if (prim >= primitiveCount) {
					return fmt::format("leaf {} references primitive {} of {}", index, prim, primitiveCount);
				}
				referenced[prim] = 1;
				const Aabb overlap = Aabb::intersection(node.bounds, input.boxes[prim]);
				const float epsilon = tolerance(node.bounds);
				if (glm::any(glm::greaterThan(overlap.min - epsilon, overlap.max + epsilon))) {
					return fmt::format("leaf {} does not overlap its primitive {}", index, prim);
				}
			}
			continue;
		}

		for (const uint32_t child : { node.left, node.right }) {
			if (child >= bvh.nodes.size()) {
				return fmt::format("node {} has child {} of {}", index, child, bvh.nodes.size());
			}
			if (visited[child]) {
				return fmt::format("node {} is reached twice (from {})", child, index);
			}
			if (!node.bounds.contains(bvh.nodes[child].bounds, tolerance(node.bounds))) {
				return fmt::format("child {} is not inside its parent {}", child, index);
			}
			visited[child] = 1;
			stack.push_back(child);
		}
	}

	for (size_t i = 0; i < bvh.nodes.size(); i++) {
		if (!visited[i]) {
			return fmt::format("node {} is unreachable", i);
		}
	}
	for (size_t i = 0; i < primitiveCount; i++) {
		if (!referenced[i] && finite(input.boxes[i])) {
			return fmt::format("primitive {} is in no leaf", i);
		}
	}
	return {};
}

namespace {

// median-splits the references in primRefs[first, first + count) into leaves of at most maxPrims,
// writing the subtree's root into the existing node `index` and appending the rest. `clip` bounds
// every box, so a subtree made from spatially split references stays inside the leaf it replaces
void buildLimitedSubtree(Bvh& bvh, const BvhBuildInput& input, uint32_t index, uint32_t first, uint32_t count, uint32_t maxPrims, const Aabb& clip)
{
	auto clipped = [&](uint32_t prim) { return Aabb::intersection(input.boxes[prim], clip); };

	Aabb bounds;
	Aabb centroids;
	for (uint32_t i = first; i < first + count; i++) {
		const Aabb box = clipped(bvh.primRefs[i]);
		bounds.grow(box);
		centroids.grow(box.center());
	}
	//a reference barely touching the clip box can intersect it in a sliver that rounds to inverted
	if (bounds.empty() || !clip.contains(bounds)) {
		bounds = clip;
	}

	BvhNode node;
	node.bounds = bounds;
	if (count <= maxPrims) {
		node.first = first;
		node.count = count;
		bvh.nodes[index] = node;
		return;
	}

	const int axis = centroids.longestAxis();
	const uint32_t half = count / 2;
	auto begin = bvh.primRefs.begin() + first;
	std::nth_element(begin, begin + half, begin + count, [&](uint32_t a, uint32_t b) {
		const float ca = clipped(a).center()[axis];
		const float cb = clipped(b).center()[axis];
		return ca < cb || (ca == cb && a < b);
	});

	node.left = (uint32_t)bvh.nodes.size();
	node.right = node.left + 1;
	bvh.nodes[index] = node;
	bvh.nodes.emplace_back();
	bvh.nodes.emplace_back();
	buildLimitedSubtree(bvh, input, node.left, first, half, maxPrims, bounds);
	buildLimitedSubtree(bvh, input, node.right, first + half, count - half, maxPrims, bounds);
}

}

void limitLeafSize(Bvh& bvh, const BvhBuildInput& input, uint32_t maxPrims)
{
	maxPrims = std::max(maxPrims, 1u);
	const size_t originalCount = bvh.nodes.size();
	for (size_t i = 0; i < originalCount; i++) {
		const BvhNode leaf = bvh.nodes[i];
		if (leaf.count > maxPrims) {
			//the leaf becomes the root of a subtree over the same reference range, re-ordered in place
			buildLimitedSubtree(bvh, input, (uint32_t)i, leaf.first, leaf.count, maxPrims, leaf.bounds);
			bvh.nodes[i].bounds = leaf.bounds;
		}
	}
}
