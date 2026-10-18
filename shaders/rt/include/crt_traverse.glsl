// THE TRAVERSAL CONTRACT.
//
// Every traversal strategy supplies exactly two functions:
//
//     bool traceScene(vec3 origin, vec3 direction, float tMin, float tMax, out TraceHit hit);
//     bool occluded(vec3 origin, vec3 direction, float tMin, float tMax);
//
// Both take a world-space ray with a NORMALISED direction. traceScene() returns the closest hit
// in [tMin, tMax]; crt_intersect.glsl turns it into a HitRecord and the classification queues, and
// no kernel downstream of 02 ever learns which strategy produced the hit. occluded() answers a
// shadow ray's question - is anything at all in [tMin, tMax] - and may stop at the first hit it
// finds; crt_shadow.glsl, the body of kernel 08, calls it. Both sit on the one search, so every
// kernel that traces uses the same traversal and shadow rays keep working whichever is selected.
//
// No kernel includes a strategy directly. A kernel that traces includes crt_trace.glsl, and the
// build compiles it once per traversal, so a strategy is written once and every tracing kernel of
// every integrator runs it. A new strategy is one file that includes this one and defines both
// functions, one branch in crt_trace.glsl and one TraversalVariant row in src/rt_kernels.cpp. See
// shaders/rt/include/crt_bvh.glsl (two-level BVH) and crt_linear.glsl (brute force) for the two
// that exist.
//
// This file holds what every strategy needs regardless of how it searches: the hit record, the
// work counters that make strategies comparable, the primitive tests (alpha test included), and
// the object-space transform. Requires crt_common.glsl.
//
// THE ALPHA TEST is part of the primitive test, so every strategy gets it by calling
// testTriangles(): a candidate hit on a CRT_TRIANGLE_CUTOUT triangle whose base colour texel is
// below the material's alphaCutoff is not a hit, and the search carries on past it. The CPU
// mirror (src/bvh_scene.cpp) has no textures and treats every triangle as opaque, so it agrees
// with this only on scenes without cutouts - which is all bvh_bench checks.

struct TraceHit {
	float t;
	// the instances[] slot, and for a mesh the global blasTriangles[] index of the triangle
	uint instance;
	uint triangle;
	vec2 bary;
};

// What the traversal-cost debug view shows and what the readback counts: node visits and
// primitive tests of this ray. Every strategy must maintain them - they are the
// hardware-independent measure that makes a brute-force scan and a BVH comparable on a GPU
// whose clocks move. A strategy with no nodes to visit simply leaves g_nodesVisited at zero.
uint g_nodesVisited;
uint g_primitivesTested;

// the analytic shapes
#include "crt_shape.glsl"

// Whether a candidate hit on a CRT_TRIANGLE_CUTOUT triangle lands in one of its material's holes.
// Only flagged triangles pay for this - an instance, an attribute record, a material and a texel -
// and the flag is set only where the glTF material is alphaMode MASK. The material still decides:
// an object that overrides its glTF material has alphaCutoff 0 and is never cut
bool cutAway(uint instanceSlot, uint triangleBits, vec2 bary)
{
	const GpuInstance instance = instances[instanceSlot];
	const GpuTriangleAttributes attributes = triangleAttributes[instance.attributeBase + (triangleBits & CRT_TRIANGLE_INDEX_MASK)];
	const GpuMaterial material = materials[instanceMaterials[instance.materialBase + floatBitsToUint(attributes.vAndSurface.w)]];
	if (material.alphaCutoff <= 0.0) {
		return false;
	}
	// textureLod: compute has no derivatives, and the array has no mips
	const float alpha = material.albedoLayer >= 0 ? textureLod(materialTextures, vec3(triangleUv(attributes, bary), float(material.albedoLayer)), 0.0).a : 1.0;
	return alpha < material.alphaCutoff;
}

// triangles [first, first + count) of blasTriangles, lowering tMax on a hit. With `anyHit` the first
// accepted hit ends the loop: a shadow ray needs to know that something is there, not what is nearest
void testTriangles(uint first, uint count, uint instance, vec3 origin, vec3 direction, float tMin, inout float tMax, inout TraceHit hit, bool anyHit)
{
	for (uint i = first; i < first + count; i++) {
		g_primitivesTested++;
		float t;
		vec2 bary;
		const BvhTriangle triangle = blasTriangles[i];
		if (hitTriangle(triangle, origin, direction, tMin, tMax, t, bary)) {
			const uint bits = floatBitsToUint(triangle.v0.w);
			if ((bits & CRT_TRIANGLE_CUTOUT) != 0u && cutAway(instance, bits, bary)) {
				continue;
			}
			tMax = t;
			hit.t = t;
			hit.instance = instance;
			hit.triangle = i;
			hit.bary = bary;
			if (anyHit) {
				return;
			}
		}
	}
}

// The ray in an instance's object space. The direction is deliberately NOT renormalised there:
// origin + t * direction is then the same point in both spaces, so t, tMin and the running
// closest hit carry across the transform unchanged, with no rescaling anywhere.
void instanceRay(GpuInstance instance, vec3 origin, vec3 direction, out vec3 localOrigin, out vec3 localDirection)
{
	const vec4 o = vec4(origin, 1.0);
	localOrigin = vec3(dot(instance.toObject[0], o), dot(instance.toObject[1], o), dot(instance.toObject[2], o));
	localDirection = vec3(dot(instance.toObject[0].xyz, direction), dot(instance.toObject[1].xyz, direction), dot(instance.toObject[2].xyz, direction));
}

// one placed shape, in its own object space
void testShapeInstance(GpuInstance instance, uint slot, vec3 localOrigin, vec3 localDirection, float tMin, inout float tMax, inout TraceHit hit)
{
	g_primitivesTested++;
	float t;
	if (hitShape(instance.triangleBase, localOrigin, localDirection, tMin, tMax, t)) {
		tMax = t;
		hit.t = t;
		hit.instance = slot;
	}
}

// whether a query has its answer: an any-hit query stops at its first hit
bool traceDone(bool anyHit, TraceHit hit)
{
	return anyHit && hit.t < CRT_INFINITY;
}

// the state every traceScene() and occluded() starts from
void resetTrace(out TraceHit hit)
{
	g_nodesVisited = 0u;
	g_primitivesTested = 0u;
	hit.t = CRT_INFINITY;
	hit.instance = 0u;
	hit.triangle = 0u;
	hit.bary = vec2(0.0);
}
