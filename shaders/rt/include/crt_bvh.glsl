// Kernel 02 Intersect Closest, strategy: TWO-LEVEL BOUNDING VOLUME HIERARCHY.
//
// A binary TLAS over the instances, and each instance's BLAS in whichever layout the scene was
// built with - binary (Aila & Laine 2009) by default, or the compressed 8-wide CWBVH
// (Ylitie et al. 2017) when the including shader defines CRT_BVH_CWBVH. The two builds are
// shaders/rt/0201_intersect_closest_bvh.comp and 0202_intersect_closest_cwbvh.comp.
//
// Supplies traceScene() per the contract in crt_traverse.glsl.
//
// Mirrored line for line on the CPU by src/bvh_layout.h (traverseBinaryBvh, traceCwbvh) and
// src/bvh_scene.cpp (traceScene), which tests/bvh_bench.cpp checks against brute force - a
// change to one side must be made to the other. The node word layouts are documented at
// src/bvh_layout.h PackedBvh.
//
// Requires crt_common.glsl.

#include "crt_traverse.glsl"

// src/bvh_layout.h BVH_BINARY_STACK_SIZE / BVH_CWBVH_STACK_SIZE: packBvh() refuses deeper trees,
// and a traversal that would overflow stops pushing rather than write past the array
#define CRT_BINARY_STACK_SIZE 64
#define CRT_CWBVH_STACK_SIZE 32

// node visits one ray may make across both levels. A valid tree comes nowhere near it (tens to
// hundreds); it exists so that a malformed one ends a dispatch instead of hanging the GPU, which
// on this machine takes the window server - and the whole desktop - down with it
#define CRT_MAX_TRAVERSAL_STEPS 65536u

// near distance of a hit box, or CRT_INFINITY for a miss (Bikker, "How to build a BVH" part 2)
float slabNear(vec3 bmin, vec3 bmax, vec3 origin, vec3 reciprocal, float tMin, float tMax)
{
	const vec3 t0 = (bmin - origin) * reciprocal;
	const vec3 t1 = (bmax - origin) * reciprocal;
	const vec3 near = min(t0, t1);
	const vec3 far = max(t0, t1);
	const float tNear = max(max(near.x, near.y), max(near.z, tMin));
	const float tFar = min(min(far.x, far.y), min(far.z, tMax));
	return tNear <= tFar ? tNear : CRT_INFINITY;
}

#ifndef CRT_BVH_CWBVH

//---------------------------------------------------------------- binary BLAS

// Aila-Laine: both children's boxes live in the parent, so one fetch of 64 bytes tests two boxes.
// Leaf children are intersected on the spot; of the interior ones the nearer is taken and the
// farther pushed (Bikker part 2's ordered traversal)
void traverseBlas(GpuInstance instance, uint instanceIndex, vec3 origin, vec3 direction, float tMin, inout float tMax, inout TraceHit hit)
{
	const vec3 reciprocal = safeReciprocal(direction);
	uint stack[CRT_BINARY_STACK_SIZE];
	uint stackSize = 0u;
	uint node = 0u;

	while (g_nodesVisited < CRT_MAX_TRAVERSAL_STEPS) {
		g_nodesVisited++;
		const uint base = (instance.nodeBase + node) * 4u;
		const uvec4 w0 = blasNodes[base];
		const uvec4 w1 = blasNodes[base + 1u];
		const uvec4 w2 = blasNodes[base + 2u];
		const uvec4 w3 = blasNodes[base + 3u];

		float tLeft = slabNear(uintBitsToFloat(w0.xyz), uintBitsToFloat(w1.xyz), origin, reciprocal, tMin, tMax);
		float tRight = slabNear(uintBitsToFloat(w2.xyz), uintBitsToFloat(w3.xyz), origin, reciprocal, tMin, tMax);

		if (w2.w > 0u && tLeft < CRT_INFINITY) {
			testTriangles(instance.triangleBase + w0.w, w2.w, instanceIndex, origin, direction, tMin, tMax, hit);
			tLeft = CRT_INFINITY;
		}
		if (w3.w > 0u && tRight < CRT_INFINITY) {
			testTriangles(instance.triangleBase + w1.w, w3.w, instanceIndex, origin, direction, tMin, tMax, hit);
			tRight = CRT_INFINITY;
		}

		if (tLeft < CRT_INFINITY && tRight < CRT_INFINITY) {
			const bool leftFirst = tLeft <= tRight;
			if (stackSize < CRT_BINARY_STACK_SIZE) {
				stack[stackSize++] = leftFirst ? w1.w : w0.w;
			}
			node = leftFirst ? w0.w : w1.w;
		} else if (tLeft < CRT_INFINITY) {
			node = w0.w;
		} else if (tRight < CRT_INFINITY) {
			node = w1.w;
		} else {
			if (stackSize == 0u) {
				return;
			}
			node = stack[--stackSize];
		}
	}
}

#else

//---------------------------------------------------------------- CWBVH BLAS

uint byteOf(uint word, uint index)
{
	return (word >> (8u * (index & 3u))) & 0xFFu;
}

// Ylitie, Karras & Laine 2017, Algorithm 1: a node group is (first child node, hit interior
// children in bits 24-31 | interior mask in bits 0-7), a triangle group (first triangle, hit
// triangles in bits 0-23). Interior children are visited highest bit first, and their bits were
// placed so that order is front to back for this ray's octant
void traverseBlas(GpuInstance instance, uint instanceIndex, vec3 origin, vec3 direction, float tMin, inout float tMax, inout TraceHit hit)
{
	const vec3 reciprocal = safeReciprocal(direction);
	const uint octantInverse = 7u - ((direction.x < 0.0 ? 4u : 0u) | (direction.y < 0.0 ? 2u : 0u) | (direction.z < 0.0 ? 1u : 0u));

	uvec2 stack[CRT_CWBVH_STACK_SIZE];
	uint stackSize = 0u;
	// the root: a group with one interior child, at node 0
	uvec2 nodeGroup = uvec2(0u, 0x80000000u);
	uvec2 triangleGroup = uvec2(0u);

	while (g_nodesVisited < CRT_MAX_TRAVERSAL_STEPS) {
		if (nodeGroup.y > 0x00FFFFFFu) {
			const uint hits = nodeGroup.y;
			const uint childBit = uint(findMSB(hits));
			nodeGroup.y &= ~(1u << childBit);
			if (nodeGroup.y > 0x00FFFFFFu && stackSize < CRT_CWBVH_STACK_SIZE) {
				stack[stackSize++] = nodeGroup;
			}

			const uint slot = (childBit - 24u) ^ octantInverse;
			const uint relative = uint(bitCount(hits & ~(0xFFFFFFFFu << slot)));
			const uint childIndex = nodeGroup.x + relative;

			g_nodesVisited++;
			const uint base = (instance.nodeBase + childIndex) * 5u;
			const uvec4 n0 = blasNodes[base];
			const uvec4 n1 = blasNodes[base + 1u];
			const uvec4 n2 = blasNodes[base + 2u];
			const uvec4 n3 = blasNodes[base + 3u];
			const uvec4 n4 = blasNodes[base + 4u];

			// 2^e from the biased exponent bytes, folded into the reciprocal so a child's slab is
			// offset + q * scaled
			const vec3 step3 = uintBitsToFloat(uvec3(n0.w & 0xFFu, (n0.w >> 8u) & 0xFFu, (n0.w >> 16u) & 0xFFu) << 23u);
			const vec3 scaled = step3 * reciprocal;
			const vec3 offset = (uintBitsToFloat(n0.xyz) - origin) * reciprocal;
			const uint interiorMask = n0.w >> 24u;

			nodeGroup.x = n1.x;
			triangleGroup = uvec2(n1.y, 0u);
			uint hitMask = 0u;

			for (uint half_ = 0u; half_ < 2u; half_++) {
				const uint metaWord = half_ == 0u ? n1.z : n1.w;
				const uint loX = half_ == 0u ? n2.x : n2.y;
				const uint loY = half_ == 0u ? n2.z : n2.w;
				const uint loZ = half_ == 0u ? n3.x : n3.y;
				const uint hiX = half_ == 0u ? n3.z : n3.w;
				const uint hiY = half_ == 0u ? n4.x : n4.y;
				const uint hiZ = half_ == 0u ? n4.z : n4.w;
				for (uint j = 0u; j < 4u; j++) {
					const uint meta = byteOf(metaWord, j);
					const vec3 lo = vec3(float(byteOf(loX, j)), float(byteOf(loY, j)), float(byteOf(loZ, j)));
					const vec3 hi = vec3(float(byteOf(hiX, j)), float(byteOf(hiY, j)), float(byteOf(hiZ, j)));
					const vec3 t0 = offset + lo * scaled;
					const vec3 t1 = offset + hi * scaled;
					const vec3 near = min(t0, t1);
					const vec3 far = max(t0, t1);
					const float tNear = max(max(near.x, near.y), max(near.z, tMin));
					const float tFar = min(min(far.x, far.y), min(far.z, tMax));
					if (tNear <= tFar) {
						// interior: bit 24 + slot, re-ordered for the octant; leaf: its triangles'
						// offsets, one bit each (the meta byte's upper bits are the count in unary)
						const bool interior = (meta & 0x18u) == 0x18u;
						const uint bitIndex = (interior ? (meta ^ octantInverse) : meta) & 0x1Fu;
						hitMask |= (meta >> 5u) << bitIndex;
					}
				}
			}

			nodeGroup.y = (hitMask & 0xFF000000u) | interiorMask;
			triangleGroup.y = hitMask & 0x00FFFFFFu;
		} else {
			triangleGroup = nodeGroup;
			nodeGroup = uvec2(0u);
		}

		while (triangleGroup.y != 0u) {
			const uint index = uint(findMSB(triangleGroup.y));
			triangleGroup.y &= ~(1u << index);
			testTriangles(instance.triangleBase + triangleGroup.x + index, 1u, instanceIndex, origin, direction, tMin, tMax, hit);
		}

		if (nodeGroup.y <= 0x00FFFFFFu) {
			if (stackSize == 0u) {
				return;
			}
			nodeGroup = stack[--stackSize];
		}
	}
}

#endif

//---------------------------------------------------------------- TLAS

// one instance, mesh or shape, by carrying the ray into its object space
void testInstance(uint slot, vec3 origin, vec3 direction, float tMin, inout float tMax, inout TraceHit hit)
{
	const GpuInstance instance = instances[slot];
	vec3 localOrigin;
	vec3 localDirection;
	instanceRay(instance, origin, direction, localOrigin, localDirection);

	if (instance.nodeBase == CRT_INSTANCE_SHAPE) {
		testShapeInstance(instance, slot, localOrigin, localDirection, tMin, tMax, hit);
		return;
	}

	traverseBlas(instance, slot, localOrigin, localDirection, tMin, tMax, hit);
}

// The closest hit of a world-space ray (direction normalised) in [tMin, tMax]. False for a miss.
// The TLAS is always the binary layout: it holds a few hundred instances, each far more expensive
// than a box test, so there is nothing for a wide node to save
bool traceScene(vec3 origin, vec3 direction, float tMin, float tMax, out TraceHit hit)
{
	resetTrace(hit);
	// the unbounded shapes first: a near plane hit then prunes the TLAS walk
	for (uint i = 0u; i < pc.unboundedCount; i++) {
		testInstance(i, origin, direction, tMin, tMax, hit);
	}
	if (pc.tlasInstanceCount == 0u) {
		return hit.t < CRT_INFINITY;
	}

	const vec3 reciprocal = safeReciprocal(direction);
	uint stack[CRT_BINARY_STACK_SIZE];
	uint stackSize = 0u;
	uint node = 0u;

	while (g_nodesVisited < CRT_MAX_TRAVERSAL_STEPS) {
		g_nodesVisited++;
		const uvec4 w0 = tlasNodes[node * 4u];
		const uvec4 w1 = tlasNodes[node * 4u + 1u];
		const uvec4 w2 = tlasNodes[node * 4u + 2u];
		const uvec4 w3 = tlasNodes[node * 4u + 3u];

		float tLeft = slabNear(uintBitsToFloat(w0.xyz), uintBitsToFloat(w1.xyz), origin, reciprocal, tMin, tMax);
		float tRight = slabNear(uintBitsToFloat(w2.xyz), uintBitsToFloat(w3.xyz), origin, reciprocal, tMin, tMax);

		if (w2.w > 0u && tLeft < CRT_INFINITY) {
			for (uint i = w0.w; i < w0.w + w2.w; i++) {
				testInstance(pc.unboundedCount + i, origin, direction, tMin, tMax, hit);
			}
			tLeft = CRT_INFINITY;
		}
		if (w3.w > 0u && tRight < CRT_INFINITY) {
			for (uint i = w1.w; i < w1.w + w3.w; i++) {
				testInstance(pc.unboundedCount + i, origin, direction, tMin, tMax, hit);
			}
			tRight = CRT_INFINITY;
		}

		if (tLeft < CRT_INFINITY && tRight < CRT_INFINITY) {
			const bool leftFirst = tLeft <= tRight;
			if (stackSize < CRT_BINARY_STACK_SIZE) {
				stack[stackSize++] = leftFirst ? w1.w : w0.w;
			}
			node = leftFirst ? w0.w : w1.w;
		} else if (tLeft < CRT_INFINITY) {
			node = w0.w;
		} else if (tRight < CRT_INFINITY) {
			node = w1.w;
		} else {
			if (stackSize == 0u) {
				break;
			}
			node = stack[--stackSize];
		}
	}

	return hit.t < CRT_INFINITY;
}
