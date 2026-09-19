// Spatial splits (Stich, Friedrich & Dietrich 2009, "SBVH"). An object split can only move whole
// triangles, so a mesh of long or large triangles - the walls and floors of architecture - leaves
// sibling boxes that overlap badly, and a ray in the overlap visits both. A spatial split cuts
// space at a plane instead and gives each side only its part of every straddling triangle,
// duplicating the reference. It is tried only where the best object split overlaps
// significantly, and total duplication is capped by a budget.

#include <bvh_split.h>

#include <algorithm>

namespace bvh_detail {

namespace {

// The parts of a reference's triangle on either side of the plane `axis = position`, each bounded
// by the reference's current box (Stich §4.2: clip the triangle, not its box). Either side can come
// back empty when the triangle only touches the plane.
void splitReference(const BuildContext& context, const PrimRef& ref, int axis, float position, Aabb& left, Aabb& right)
{
	left = {};
	right = {};
	const glm::vec3* vertices = &context.input.triangleVertices[(size_t)ref.prim * 3];
	for (int i = 0; i < 3; i++) {
		const glm::vec3& a = vertices[i];
		const glm::vec3& b = vertices[(i + 1) % 3];
		const float pa = a[axis];
		const float pb = b[axis];
		if (pa <= position) {
			left.grow(a);
		}
		if (pa >= position) {
			right.grow(a);
		}
		//the edge crosses the plane: the crossing point bounds both sides
		if ((pa < position && pb > position) || (pa > position && pb < position)) {
			glm::vec3 crossing = glm::mix(a, b, (position - pa) / (pb - pa));
			crossing[axis] = position;
			left.grow(crossing);
			right.grow(crossing);
		}
	}
	left = Aabb::intersection(left, ref.box);
	right = Aabb::intersection(right, ref.box);
	//rounding in the crossing point must not push either part across the plane
	left.max[axis] = std::min(left.max[axis], position);
	right.min[axis] = std::max(right.min[axis], position);
}

struct SpatialSplit {
	int axis { -1 };
	uint32_t binCount { 0 };
	// references whose last bin is below this lie wholly left, those whose first bin is at or
	// above it wholly right; the rest straddle
	uint32_t bin { 0 };
	float position { 0.f };
	float costArea { std::numeric_limits<float>::infinity() };
	Aabb leftBounds;
	Aabb rightBounds;
	uint32_t leftCount { 0 };
	uint32_t rightCount { 0 };
};

struct SpatialBinner {
	float origin;
	float width;
	uint32_t binCount;

	uint32_t operator()(float x) const
	{
		const int bin = (int)((x - origin) / width);
		return (uint32_t)std::clamp(bin, 0, (int)binCount - 1);
	}
	float plane(uint32_t bin) const { return origin + width * (float)bin; }
};

SpatialSplit findSpatialSplit(const std::vector<PrimRef>& refs, const Aabb& bounds, const BuildContext& context)
{
	const uint32_t binCount = binCountFor(context.options, refs.size());
	const float parentArea = bounds.halfArea();

	struct Bin {
		Aabb bounds;
		uint32_t entries { 0 };
		uint32_t exits { 0 };
	};
	//per-thread scratch rather than locals: a local array of MAX_BINS boxes is constructed in full on
	//every call, and at one call per node that construction outweighed the search itself
	thread_local std::vector<Bin> bins(MAX_BINS);
	thread_local std::vector<float> rightArea(MAX_BINS);
	thread_local std::vector<uint32_t> rightCount(MAX_BINS);

	SpatialSplit best;
	best.binCount = binCount;
	for (int axis = 0; axis < 3; axis++) {
		const float extent = bounds.max[axis] - bounds.min[axis];
		if (!(extent > 0.f)) {
			continue;
		}
		const SpatialBinner binner { bounds.min[axis], extent / (float)binCount, binCount };

		//each reference is chopped at every bin boundary it crosses, and each piece grows only the
		//bin it lies in; it enters the bin its box starts in and exits the one it ends in (Stich §4.3)
		std::fill(bins.begin(), bins.begin() + binCount, Bin {});
		for (const PrimRef& ref : refs) {
			const uint32_t first = binner(ref.box.min[axis]);
			const uint32_t last = binner(ref.box.max[axis]);
			bins[first].entries++;
			bins[last].exits++;

			PrimRef remainder = ref;
			for (uint32_t bin = first; bin < last; bin++) {
				Aabb left;
				Aabb right;
				splitReference(context, remainder, axis, binner.plane(bin + 1), left, right);
				bins[bin].bounds.grow(left);
				remainder.box = right;
			}
			bins[last].bounds.grow(remainder.box);
		}

		Aabb accumulated;
		uint32_t count = 0;
		for (uint32_t i = binCount - 1; i > 0; i--) {
			accumulated.grow(bins[i].bounds);
			count += bins[i].exits;
			rightArea[i] = accumulated.halfArea();
			rightCount[i] = count;
		}

		accumulated = {};
		count = 0;
		for (uint32_t i = 0; i + 1 < binCount; i++) {
			accumulated.grow(bins[i].bounds);
			count += bins[i].entries;
			if (count == 0 || rightCount[i + 1] == 0) {
				continue;
			}
			const float cost = splitCostArea(context, parentArea, accumulated.halfArea(), count, rightArea[i + 1], rightCount[i + 1]);
			if (cost < best.costArea) {
				best.axis = axis;
				best.bin = i + 1;
				best.position = binner.plane(i + 1);
				best.costArea = cost;
				best.leftCount = count;
				best.rightCount = rightCount[i + 1];
			}
		}
	}

	if (best.axis >= 0) {
		//the chosen plane's two sides, as the search saw them: the unsplitting test weighs against these
		const SpatialBinner binner { bounds.min[best.axis], (bounds.max[best.axis] - bounds.min[best.axis]) / (float)best.binCount, best.binCount };
		for (const PrimRef& ref : refs) {
			const uint32_t first = binner(ref.box.min[best.axis]);
			const uint32_t last = binner(ref.box.max[best.axis]);
			if (last < best.bin) {
				best.leftBounds.grow(ref.box);
			} else if (first >= best.bin) {
				best.rightBounds.grow(ref.box);
			} else {
				Aabb left;
				Aabb right;
				splitReference(context, ref, best.axis, best.position, left, right);
				best.leftBounds.grow(left);
				best.rightBounds.grow(right);
			}
		}
	}
	return best;
}

// Partitions by the spatial split, with Stich's reference unsplitting: a straddling reference is
// kept whole on one side when that costs less than duplicating it. Returns false (and leaves
// `refs` untouched) if the result would leave a side empty.
bool applySpatialSplit(std::vector<PrimRef>& refs, const Aabb& bounds, const SpatialSplit& split, const BuildContext& context, SplitResult& out)
{
	const SpatialBinner binner { bounds.min[split.axis], (bounds.max[split.axis] - bounds.min[split.axis]) / (float)split.binCount, split.binCount };

	Aabb leftBounds = split.leftBounds;
	Aabb rightBounds = split.rightBounds;
	float leftCount = (float)split.leftCount;
	float rightCount = (float)split.rightCount;

	out.left.clear();
	out.right.clear();
	for (const PrimRef& ref : refs) {
		const uint32_t first = binner(ref.box.min[split.axis]);
		const uint32_t last = binner(ref.box.max[split.axis]);
		if (last < split.bin) {
			out.left.push_back(ref);
			continue;
		}
		if (first >= split.bin) {
			out.right.push_back(ref);
			continue;
		}

		Aabb leftWithRef = leftBounds;
		leftWithRef.grow(ref.box);
		Aabb rightWithRef = rightBounds;
		rightWithRef.grow(ref.box);
		const float duplicate = leftBounds.halfArea() * leftCount + rightBounds.halfArea() * rightCount;
		const float allLeft = leftWithRef.halfArea() * leftCount + rightBounds.halfArea() * (rightCount - 1.f);
		const float allRight = leftBounds.halfArea() * (leftCount - 1.f) + rightWithRef.halfArea() * rightCount;

		if (allLeft < duplicate && allLeft <= allRight) {
			out.left.push_back(ref);
			leftBounds = leftWithRef;
			rightCount -= 1.f;
		} else if (allRight < duplicate) {
			out.right.push_back(ref);
			rightBounds = rightWithRef;
			leftCount -= 1.f;
		} else {
			Aabb left;
			Aabb right;
			splitReference(context, ref, split.axis, split.position, left, right);
			//a reference that merely touches the plane has nothing on one side
			if (!left.empty()) {
				out.left.push_back({ left, ref.prim });
			}
			if (!right.empty()) {
				out.right.push_back({ right, ref.prim });
			}
			if (left.empty() && right.empty()) {
				(first < split.bin ? out.left : out.right).push_back(ref);
			}
		}
	}

	if (out.left.empty() || out.right.empty()) {
		out.left.clear();
		out.right.clear();
		return false;
	}

	out.leftBounds = {};
	out.rightBounds = {};
	for (const PrimRef& ref : out.left) {
		out.leftBounds.grow(ref.box);
	}
	for (const PrimRef& ref : out.right) {
		out.rightBounds.grow(ref.box);
	}
	out.costArea = split.costArea;
	return true;
}

}

SplitOutcome splitSpatialSah(std::vector<PrimRef>& refs, const Aabb& bounds, BuildContext& context, float leafCostArea, SplitResult& out)
{
	const Aabb centroids = centroidBounds(refs);
	const BinnedObjectSplit objectSplit = findBinnedObjectSplit(refs, bounds, centroids, context);

	//spatial splits only where they can pay: the object split's children overlap by a meaningful
	//fraction of the whole scene (Stich's alpha), there are triangles to clip, and budget remains
	bool trySpatial = !context.input.triangleVertices.empty() && context.spareReferences.load() > 0;
	if (trySpatial && objectSplit.axis >= 0) {
		const float overlap = Aabb::intersection(objectSplit.leftBounds, objectSplit.rightBounds).halfArea();
		trySpatial = overlap > context.options.spatialAlpha * context.rootHalfArea;
	}

	if (trySpatial) {
		const SpatialSplit spatialSplit = findSpatialSplit(refs, bounds, context);
		if (spatialSplit.axis >= 0 && spatialSplit.costArea < objectSplit.costArea) {
			if (preferLeaf(context, refs.size(), spatialSplit.costArea, leafCostArea)) {
				return SplitOutcome::Leaf;
			}
			if (applySpatialSplit(refs, bounds, spatialSplit, context, out)) {
				const int64_t added = (int64_t)(out.left.size() + out.right.size()) - (int64_t)refs.size();
				//the budget is checked before and charged after, so a few concurrent splits can
				//overshoot it by one split's worth each - it is a cap on memory, not an exact count
				context.spareReferences -= added;
				refs.clear();
				return SplitOutcome::Split;
			}
		}
	}

	if (objectSplit.axis < 0) {
		return SplitOutcome::None;
	}
	if (preferLeaf(context, refs.size(), objectSplit.costArea, leafCostArea)) {
		return SplitOutcome::Leaf;
	}
	applyBinnedObjectSplit(refs, centroids, objectSplit, out);
	return SplitOutcome::Split;
}

}
