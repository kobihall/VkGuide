// Kernel 02 Intersect Closest, strategy: BRUTE FORCE.
//
// No acceleration structure at all. Every ray is tested against every instance, and inside a
// mesh instance against every one of its triangles. This is what the tracer did before the BVH
// (commit 85823b5, when the scene was flat sphere and triangle lists); it is kept as a variant
// because it is the ground truth every other strategy is checked against, and because the
// contrast in the traversal counters is the clearest demonstration of what a BVH buys.
//
// Supplies traceScene() per the contract in crt_traverse.glsl.
//
// It reads NO acceleration-structure bindings: no TLAS nodes, no BLAS nodes. It needs only the
// instances, their triangles and - unlike the BVH - GpuInstance::triangleCount, which is what
// tells it where a mesh's triangles end. That is the whole reason that field exists.
//
// COST: O(rays x triangles). The render guard in src/rt_renderer.cpp sizes a frame's work from
// the strategy's cost per ray, and for this one that cost is the scene's entire primitive
// count - which is exactly the 1M-triangle case that once hung the GPU and took the window
// server down with it. The guard is not optional here; it is the only thing standing between a
// large model and a dead machine.
//
// Requires crt_common.glsl.

#include "crt_traverse.glsl"

// one instance, mesh or shape, by carrying the ray into its object space
void scanInstance(uint slot, vec3 origin, vec3 direction, float tMin, inout float tMax, inout TraceHit hit)
{
	const GpuInstance instance = instances[slot];
	vec3 localOrigin;
	vec3 localDirection;
	instanceRay(instance, origin, direction, localOrigin, localDirection);

	if (instance.nodeBase == CRT_INSTANCE_SHAPE) {
		testShapeInstance(instance, slot, localOrigin, localDirection, tMin, tMax, hit);
		return;
	}

	// every triangle of the BLAS, in the order it was packed. testTriangles() lowers tMax as it
	// goes, so a later triangle behind an earlier hit still costs its test but cannot win
	testTriangles(instance.triangleBase, instance.triangleCount, slot, localOrigin, localDirection, tMin, tMax, hit);
}

bool traceScene(vec3 origin, vec3 direction, float tMin, float tMax, out TraceHit hit)
{
	resetTrace(hit);

	// the whole array in one scan. Deliberately pc.instanceCount and not
	// unboundedCount + tlasInstanceCount: the latter goes to zero when the TLAS fails to pack,
	// which must not make bounded geometry vanish from a strategy that never walks a TLAS
	for (uint i = 0u; i < pc.instanceCount; i++) {
		scanInstance(i, origin, direction, tMin, tMax, hit);
	}

	return hit.t < CRT_INFINITY;
}
