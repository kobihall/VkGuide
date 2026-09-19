#include <bvh_layout.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include <fmt/format.h>

const char* bvhLayoutName(BvhLayout layout)
{
	switch (layout) {
	case BvhLayout::Binary:
		return "binary (Aila-Laine)";
	case BvhLayout::Cwbvh8:
		return "8-wide compressed (CWBVH)";
	case BvhLayout::Count:
		break;
	}
	return "unknown";
}

BvhTriangle makeBvhTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, uint32_t index)
{
	BvhTriangle triangle;
	triangle.v0 = glm::vec4(a, std::bit_cast<float>(index));
	triangle.e1 = glm::vec4(b - a, 0.f);
	triangle.e2 = glm::vec4(c - a, 0.f);
	return triangle;
}

namespace {

uint32_t floatBits(float f)
{
	return std::bit_cast<uint32_t>(f);
}

float bitsFloat(uint32_t u)
{
	return std::bit_cast<float>(u);
}

//---------------------------------------------------------------- binary

class BinaryPacker {
public:
	BinaryPacker(const Bvh& bvh, PackedBvh& out)
		: m_bvh(bvh)
		, m_out(out)
	{
	}

	void pack()
	{
		const BvhNode& root = m_bvh.nodes[0];
		if (root.isLeaf()) {
			//a leaf root has no parent to hold its box. The node is given the leaf in both child
			//slots rather than an "absent" marker the traversal would have to test for on every
			//node: it only happens for a mesh small enough to be one leaf, where testing its few
			//triangles twice costs nothing
			const uint32_t first = appendLeaf(root);
			writeNode(0, root.bounds, first, root.count, root.bounds, first, root.count);
			m_out.nodeCount = 1;
			m_out.depth = 1;
			return;
		}
		m_out.nodeCount = 1;
		emit(0, 0, 1);
	}

private:
	// depth-first, left subtree before right: a node's nearer child is usually adjacent in memory
	void emit(uint32_t bvhIndex, uint32_t packedIndex, uint32_t depth)
	{
		m_out.depth = std::max(m_out.depth, depth);
		const BvhNode& node = m_bvh.nodes[bvhIndex];
		const BvhNode& left = m_bvh.nodes[node.left];
		const BvhNode& right = m_bvh.nodes[node.right];

		//children's slots are resolved in order, interior ones reserving their node index first so
		//the recursion below can fill them in
		uint32_t leftIndex = 0;
		uint32_t rightIndex = 0;
		if (left.isLeaf()) {
			leftIndex = appendLeaf(left);
		} else {
			leftIndex = m_out.nodeCount++;
		}
		if (!left.isLeaf()) {
			emit(node.left, leftIndex, depth + 1);
		}
		if (right.isLeaf()) {
			rightIndex = appendLeaf(right);
		} else {
			rightIndex = m_out.nodeCount++;
			emit(node.right, rightIndex, depth + 1);
		}

		writeNode(packedIndex, left.bounds, leftIndex, left.isLeaf() ? left.count : 0, right.bounds, rightIndex, right.isLeaf() ? right.count : 0);
	}

	uint32_t appendLeaf(const BvhNode& leaf)
	{
		const uint32_t first = (uint32_t)m_out.primOrder.size();
		for (uint32_t i = 0; i < leaf.count; i++) {
			m_out.primOrder.push_back(m_bvh.primRefs[leaf.first + i]);
		}
		return first;
	}

	void writeNode(uint32_t index, const Aabb& leftBox, uint32_t leftIndex, uint32_t leftCount, const Aabb& rightBox, uint32_t rightIndex, uint32_t rightCount)
	{
		if (m_out.nodes.size() < (size_t)(index + 1) * 4) {
			m_out.nodes.resize((size_t)(index + 1) * 4);
		}
		glm::uvec4* words = &m_out.nodes[(size_t)index * 4];
		words[0] = glm::uvec4(floatBits(leftBox.min.x), floatBits(leftBox.min.y), floatBits(leftBox.min.z), leftIndex);
		words[1] = glm::uvec4(floatBits(leftBox.max.x), floatBits(leftBox.max.y), floatBits(leftBox.max.z), rightIndex);
		words[2] = glm::uvec4(floatBits(rightBox.min.x), floatBits(rightBox.min.y), floatBits(rightBox.min.z), leftCount);
		words[3] = glm::uvec4(floatBits(rightBox.max.x), floatBits(rightBox.max.y), floatBits(rightBox.max.z), rightCount);
	}

	const Bvh& m_bvh;
	PackedBvh& m_out;
};

//---------------------------------------------------------------- CWBVH

struct CwbvhNode {
	glm::vec3 origin { 0.f };
	uint8_t exponent[3] { 0, 0, 0 };
	uint8_t interiorMask { 0 };
	uint32_t childBase { 0 };
	uint32_t primBase { 0 };
	uint8_t meta[8] {};
	uint8_t lo[3][8] {};
	uint8_t hi[3][8] {};
};

uint32_t packBytes(const uint8_t* bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

class CwbvhPacker {
public:
	CwbvhPacker(const Bvh& bvh, PackedBvh& out)
		: m_bvh(bvh)
		, m_out(out)
	{
	}

	void pack()
	{
		m_nodes.emplace_back();
		convert(0, 0, 1);

		m_out.nodeCount = (uint32_t)m_nodes.size();
		m_out.nodes.reserve(m_nodes.size() * 5);
		for (const CwbvhNode& node : m_nodes) {
			const uint32_t exponents = (uint32_t)node.exponent[0] | (uint32_t)node.exponent[1] << 8 | (uint32_t)node.exponent[2] << 16 | (uint32_t)node.interiorMask << 24;
			m_out.nodes.push_back(glm::uvec4(floatBits(node.origin.x), floatBits(node.origin.y), floatBits(node.origin.z), exponents));
			m_out.nodes.push_back(glm::uvec4(node.childBase, node.primBase, packBytes(&node.meta[0]), packBytes(&node.meta[4])));
			m_out.nodes.push_back(glm::uvec4(packBytes(&node.lo[0][0]), packBytes(&node.lo[0][4]), packBytes(&node.lo[1][0]), packBytes(&node.lo[1][4])));
			m_out.nodes.push_back(glm::uvec4(packBytes(&node.lo[2][0]), packBytes(&node.lo[2][4]), packBytes(&node.hi[0][0]), packBytes(&node.hi[0][4])));
			m_out.nodes.push_back(glm::uvec4(packBytes(&node.hi[1][0]), packBytes(&node.hi[1][4]), packBytes(&node.hi[2][0]), packBytes(&node.hi[2][4])));
		}
	}

private:
	void convert(uint32_t bvhIndex, uint32_t wideIndex, uint32_t depth)
	{
		m_out.depth = std::max(m_out.depth, depth);
		const BvhNode& source = m_bvh.nodes[bvhIndex];

		//collapse: start from the binary node's two children and repeatedly open the interior child
		//with the largest surface area - the one most likely to be hit - until eight slots are used
		//or only leaves remain. The greedy collapse of tinybvh; Ylitie et al. describe an SAH-optimal
		//dynamic programme that could replace this function without touching anything else
		std::vector<uint32_t> children;
		if (source.isLeaf()) {
			children.push_back(bvhIndex);
		} else {
			children.push_back(source.left);
			children.push_back(source.right);
		}
		while (children.size() < 8) {
			int widest = -1;
			float widestArea = -1.f;
			for (size_t i = 0; i < children.size(); i++) {
				const BvhNode& child = m_bvh.nodes[children[i]];
				if (!child.isLeaf() && child.bounds.halfArea() > widestArea) {
					widest = (int)i;
					widestArea = child.bounds.halfArea();
				}
			}
			if (widest < 0) {
				break;
			}
			const BvhNode opened = m_bvh.nodes[children[widest]];
			children[widest] = opened.left;
			children.push_back(opened.right);
		}

		const std::array<int, 8> slotOf = assignSlots(source.bounds, children);
		int childInSlot[8];
		std::fill(std::begin(childInSlot), std::end(childInSlot), -1);
		for (size_t i = 0; i < children.size(); i++) {
			childInSlot[slotOf[i]] = (int)i;
		}

		CwbvhNode node;
		quantize(node, source.bounds, children, childInSlot);

		//interior children are stored contiguously in slot order from childBase, so the traversal
		//finds one by counting the interior slots below it; leaf primitives likewise from primBase
		node.childBase = (uint32_t)m_nodes.size();
		node.primBase = (uint32_t)m_out.primOrder.size();
		std::vector<std::pair<uint32_t, uint32_t>> interiorChildren;
		for (int slot = 0; slot < 8; slot++) {
			if (childInSlot[slot] < 0) {
				node.meta[slot] = 0;
				continue;
			}
			const uint32_t childIndex = children[childInSlot[slot]];
			const BvhNode& child = m_bvh.nodes[childIndex];
			if (child.isLeaf()) {
				//upper three bits: the leaf's primitive count in unary, so the traversal can shift
				//them straight into a hit mask; lower five: its first primitive's offset from primBase
				const uint32_t offset = (uint32_t)m_out.primOrder.size() - node.primBase;
				node.meta[slot] = (uint8_t)((((1u << child.count) - 1u) << 5) | offset);
				for (uint32_t i = 0; i < child.count; i++) {
					m_out.primOrder.push_back(m_bvh.primRefs[child.first + i]);
				}
			} else {
				//001 then 24 + slot: bits 3 and 4 both set marks it interior
				node.meta[slot] = (uint8_t)((1u << 5) | (24u + (uint32_t)slot));
				node.interiorMask |= (uint8_t)(1u << slot);
				const uint32_t wide = (uint32_t)m_nodes.size();
				m_nodes.emplace_back();
				interiorChildren.push_back({ childIndex, wide });
			}
		}

		m_nodes[wideIndex] = node;
		for (const auto& [childIndex, wide] : interiorChildren) {
			convert(childIndex, wide, depth + 1);
		}
	}

	// Children into slots so that, for any ray, visiting slots in the order its octant implies is
	// roughly front to back (Ylitie §3.2): slot s's diagonal direction has sign bits s, and the
	// child whose centroid lies furthest against that direction takes it. Greedy on the full
	// cost table, as tinybvh does. Any assignment traverses correctly; this only affects order.
	std::array<int, 8> assignSlots(const Aabb& bounds, const std::vector<uint32_t>& children) const
	{
		const glm::vec3 center = bounds.center();
		float cost[8][8];
		for (int s = 0; s < 8; s++) {
			const glm::vec3 direction((s & 4) ? -1.f : 1.f, (s & 2) ? -1.f : 1.f, (s & 1) ? -1.f : 1.f);
			for (size_t i = 0; i < children.size(); i++) {
				cost[s][i] = glm::dot(m_bvh.nodes[children[i]].bounds.center() - center, direction);
			}
		}

		std::array<int, 8> slotOf;
		slotOf.fill(-1);
		bool slotTaken[8] = {};
		for (size_t assigned = 0; assigned < children.size(); assigned++) {
			float best = INFINITY;
			int bestSlot = -1;
			int bestChild = -1;
			for (int s = 0; s < 8; s++) {
				for (size_t i = 0; i < children.size(); i++) {
					if (!slotTaken[s] && slotOf[i] < 0 && cost[s][i] < best) {
						best = cost[s][i];
						bestSlot = s;
						bestChild = (int)i;
					}
				}
			}
			//a NaN centroid compares false with everything; any free slot will do
			if (bestSlot < 0) {
				for (size_t i = 0; i < children.size() && bestSlot < 0; i++) {
					if (slotOf[i] < 0) {
						bestChild = (int)i;
						for (int s = 0; s < 8; s++) {
							if (!slotTaken[s]) {
								bestSlot = s;
								break;
							}
						}
					}
				}
			}
			slotTaken[bestSlot] = true;
			slotOf[bestChild] = bestSlot;
		}
		return slotOf;
	}

	// Every child box as 8-bit offsets on a power-of-two grid anchored at the node's minimum
	// corner: rounded outwards, so the decoded box always contains the real one. A grid step is
	// the smallest power of two for which 255 steps span the node; if rounding in float would
	// still leave a child uncovered, the step doubles
	void quantize(CwbvhNode& node, const Aabb& bounds, const std::vector<uint32_t>& children, const int* childInSlot) const
	{
		node.origin = bounds.min;
		for (int axis = 0; axis < 3; axis++) {
			const double extent = (double)bounds.max[axis] - (double)bounds.min[axis];
			int exponent = extent > 0.0 ? (int)std::ceil(std::log2(extent / 255.0)) : -126;
			exponent = std::clamp(exponent, -126, 127);

			for (;; exponent++) {
				const float step = std::ldexp(1.f, exponent);
				bool covered = true;
				for (int slot = 0; slot < 8; slot++) {
					if (childInSlot[slot] < 0) {
						node.lo[axis][slot] = 0;
						node.hi[axis][slot] = 0;
						continue;
					}
					const Aabb& box = m_bvh.nodes[children[childInSlot[slot]]].bounds;
					const double lo = std::floor(((double)box.min[axis] - (double)node.origin[axis]) / step);
					const double hi = std::ceil(((double)box.max[axis] - (double)node.origin[axis]) / step);
					const uint8_t qlo = (uint8_t)std::clamp(lo, 0.0, 255.0);
					const uint8_t qhi = (uint8_t)std::clamp(hi, 0.0, 255.0);
					node.lo[axis][slot] = qlo;
					node.hi[axis][slot] = qhi;
					//checked as the GPU will decode it, in float
					if (node.origin[axis] + (float)qlo * step > box.min[axis] || node.origin[axis] + (float)qhi * step < box.max[axis]) {
						covered = false;
					}
				}
				if (covered || exponent >= 127) {
					break;
				}
			}
			//stored biased like an IEEE exponent, so the shader rebuilds the step as a float directly
			node.exponent[axis] = (uint8_t)(exponent + 127);
		}
	}

	const Bvh& m_bvh;
	PackedBvh& m_out;
	std::vector<CwbvhNode> m_nodes;
};

}

PackedBvh packBvh(Bvh bvh, const BvhBuildInput& input, BvhLayout layout)
{
	PackedBvh out;
	out.layout = layout;
	if (bvh.empty()) {
		return out;
	}

	if (layout == BvhLayout::Cwbvh8) {
		limitLeafSize(bvh, input, BVH_CWBVH_MAX_LEAF_SIZE);
		CwbvhPacker(bvh, out).pack();
		if (out.depth > BVH_CWBVH_STACK_SIZE) {
			out.error = fmt::format("CWBVH depth {} exceeds the traversal stack of {}", out.depth, BVH_CWBVH_STACK_SIZE);
		}
	} else {
		BinaryPacker(bvh, out).pack();
		if (out.depth > BVH_BINARY_STACK_SIZE) {
			out.error = fmt::format("binary BVH depth {} exceeds the traversal stack of {}", out.depth, BVH_BINARY_STACK_SIZE);
		}
	}
	return out;
}

//---------------------------------------------------------------- CPU traversal

namespace {

void testTriangles(std::span<const BvhTriangle> triangles, uint32_t first, uint32_t count, const BvhRay& ray, float& tMax, BvhHit& hit)
{
	for (uint32_t i = first; i < first + count; i++) {
		hit.trianglesTested++;
		float t;
		glm::vec2 barycentrics;
		if (intersectBvhTriangle(triangles[i], ray.origin, ray.direction, ray.tMin, tMax, t, barycentrics)) {
			tMax = t;
			hit.hit = true;
			hit.t = t;
			hit.slot = i;
			hit.barycentrics = barycentrics;
		}
	}
}

BvhHit traceBinary(const PackedBvh& bvh, std::span<const BvhTriangle> triangles, const BvhRay& ray)
{
	BvhHit hit;
	float tMax = ray.tMax;
	traverseBinaryBvh(bvh, ray, tMax, [&](uint32_t first, uint32_t count, float& t) { testTriangles(triangles, first, count, ray, t, hit); }, hit.nodesVisited, hit.stackOverflow);
	return hit;
}

uint32_t byteOf(uint32_t word, uint32_t index)
{
	return (word >> (8u * (index & 3u))) & 0xFFu;
}

uint32_t findMsb(uint32_t v)
{
	return 31u - (uint32_t)std::countl_zero(v);
}

BvhHit traceCwbvh(const PackedBvh& bvh, std::span<const BvhTriangle> triangles, const BvhRay& ray)
{
	BvhHit hit;
	const glm::vec3 reciprocal = bvhSafeReciprocal(ray.direction);
	float tMax = ray.tMax;

	//the octant's inverse: XORed into an interior child's slot so that popping the highest hit bit
	//first visits slots in this ray's front-to-back order
	const uint32_t octantInverse = 7u - ((ray.direction.x < 0.f ? 4u : 0u) | (ray.direction.y < 0.f ? 2u : 0u) | (ray.direction.z < 0.f ? 1u : 0u));

	glm::uvec2 stack[BVH_CWBVH_STACK_SIZE];
	uint32_t stackSize = 0;
	//x = first child node of the group, y = hit interior children in bits 24-31 plus the node's
	//interior mask in bits 0-7. The root is a group with one child (bit 31) at node 0
	glm::uvec2 nodeGroup(0u, 0x80000000u);
	//x = first primitive slot, y = hit primitives in bits 0-23
	glm::uvec2 triangleGroup(0u, 0u);

	for (uint32_t step = 0; step < BVH_MAX_TRAVERSAL_STEPS; step++) {
		if (nodeGroup.y > 0x00FFFFFFu) {
			const uint32_t hits = nodeGroup.y;
			const uint32_t childBit = findMsb(hits);
			nodeGroup.y &= ~(1u << childBit);
			if (nodeGroup.y > 0x00FFFFFFu) {
				if (stackSize < BVH_CWBVH_STACK_SIZE) {
					stack[stackSize++] = nodeGroup;
				} else {
					hit.stackOverflow = true;
				}
			}

			const uint32_t slot = (childBit - 24u) ^ octantInverse;
			const uint32_t relative = (uint32_t)std::popcount(hits & ~(0xFFFFFFFFu << slot));
			const uint32_t childIndex = nodeGroup.x + relative;

			hit.nodesVisited++;
			const glm::uvec4* words = &bvh.nodes[(size_t)childIndex * 5];
			const glm::uvec4 n0 = words[0];
			const glm::uvec4 n1 = words[1];
			const glm::uvec4 n2 = words[2];
			const glm::uvec4 n3 = words[3];
			const glm::uvec4 n4 = words[4];

			const glm::vec3 nodeOrigin(bitsFloat(n0.x), bitsFloat(n0.y), bitsFloat(n0.z));
			//2^e rebuilt from the biased exponent byte, then folded into the reciprocal
			const glm::vec3 step3(bitsFloat((n0.w & 0xFFu) << 23), bitsFloat(((n0.w >> 8) & 0xFFu) << 23), bitsFloat(((n0.w >> 16) & 0xFFu) << 23));
			const glm::vec3 scaled = step3 * reciprocal;
			const glm::vec3 offset = (nodeOrigin - ray.origin) * reciprocal;
			const uint32_t interiorMask = n0.w >> 24;

			nodeGroup.x = n1.x;
			triangleGroup = glm::uvec2(n1.y, 0u);
			uint32_t hitMask = 0;

			for (uint32_t i = 0; i < 8; i++) {
				const uint32_t meta = byteOf(i < 4 ? n1.z : n1.w, i);
				const uint32_t half = i < 4 ? 0u : 1u;
				const glm::vec3 lo((float)byteOf(half ? n2.y : n2.x, i), (float)byteOf(half ? n2.w : n2.z, i), (float)byteOf(half ? n3.y : n3.x, i));
				const glm::vec3 hi((float)byteOf(half ? n3.w : n3.z, i), (float)byteOf(half ? n4.y : n4.x, i), (float)byteOf(half ? n4.w : n4.z, i));
				const glm::vec3 t0 = offset + lo * scaled;
				const glm::vec3 t1 = offset + hi * scaled;
				const glm::vec3 near = glm::min(t0, t1);
				const glm::vec3 far = glm::max(t0, t1);
				const float tNear = std::max(std::max(near.x, near.y), std::max(near.z, ray.tMin));
				const float tFar = std::min(std::min(far.x, far.y), std::min(far.z, tMax));
				if (tNear <= tFar) {
					//an interior child's bit index is 24 + slot, re-ordered for this octant; a leaf's is
					//its first primitive's offset, and its bits are its primitives
					const bool interior = (meta & 0x18u) == 0x18u;
					const uint32_t bitIndex = (interior ? (meta ^ octantInverse) : meta) & 0x1Fu;
					hitMask |= (meta >> 5) << bitIndex;
				}
			}

			nodeGroup.y = (hitMask & 0xFF000000u) | interiorMask;
			triangleGroup.y = hitMask & 0x00FFFFFFu;
		} else {
			triangleGroup = nodeGroup;
			nodeGroup = glm::uvec2(0u);
		}

		while (triangleGroup.y != 0u) {
			const uint32_t index = findMsb(triangleGroup.y);
			triangleGroup.y &= ~(1u << index);
			testTriangles(triangles, triangleGroup.x + index, 1, ray, tMax, hit);
		}

		if (nodeGroup.y <= 0x00FFFFFFu) {
			if (stackSize == 0) {
				break;
			}
			nodeGroup = stack[--stackSize];
		}
	}
	return hit;
}

}

bool intersectBvhTriangle(const BvhTriangle& triangle, const glm::vec3& origin, const glm::vec3& direction, float tMin, float tMax, float& t, glm::vec2& barycentrics)
{
	const glm::vec3 edge1(triangle.e1);
	const glm::vec3 edge2(triangle.e2);
	const glm::vec3 pvec = glm::cross(direction, edge2);
	const float det = glm::dot(edge1, pvec);
	//only an exactly parallel ray or a zero-area triangle; direction is not normalised in object
	//space, so a fixed epsilon here would reject small triangles under a scaled instance
	if (std::abs(det) < 1e-30f) {
		return false;
	}
	const float invDet = 1.f / det;
	const glm::vec3 tvec = origin - glm::vec3(triangle.v0);
	const float u = glm::dot(tvec, pvec) * invDet;
	if (u < 0.f || u > 1.f) {
		return false;
	}
	const glm::vec3 qvec = glm::cross(tvec, edge1);
	const float v = glm::dot(direction, qvec) * invDet;
	if (v < 0.f || u + v > 1.f) {
		return false;
	}
	const float hitT = glm::dot(edge2, qvec) * invDet;
	if (hitT < tMin || hitT > tMax) {
		return false;
	}
	t = hitT;
	barycentrics = glm::vec2(u, v);
	return true;
}

BvhHit traceBvh(const PackedBvh& bvh, std::span<const BvhTriangle> triangles, const BvhRay& ray)
{
	if (bvh.nodeCount == 0 || !bvh.error.empty()) {
		return {};
	}
	return bvh.layout == BvhLayout::Cwbvh8 ? traceCwbvh(bvh, triangles, ray) : traceBinary(bvh, triangles, ray);
}
