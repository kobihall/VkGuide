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
// finds; crt_shadow.glsl, the body of kernel 08, is its one caller. Both sit on the one search, so
// swapping the strategy swaps them together and shadow rays keep working whichever is selected.
//
// A third strategy is therefore one new file that includes this one and defines both functions,
// plus two .comp files: one that includes it and crt_intersect.glsl (kernel 02) and one that
// includes it and crt_shadow.glsl (kernel 08). See shaders/rt/include/crt_bvh.glsl (two-level
// BVH) and crt_linear.glsl (brute force) for the two that exist.
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

// 1 / d with each zero component replaced by a tiny signed one, so no box test multiplies 0 by
// infinity: NaN compares false both ways and would admit or reject a box arbitrarily
vec3 safeReciprocal(vec3 d)
{
	const vec3 signs = vec3(greaterThanEqual(d, vec3(0.0))) * 2.0 - 1.0;
	return 1.0 / mix(d, signs * 1e-12, lessThan(abs(d), vec3(1e-12)));
}

// the analytic shapes, which need safeReciprocal
#include "crt_shape.glsl"

// a point's uv from its triangle's attribute record and barycentrics
vec2 triangleUv(GpuTriangleAttributes attributes, vec2 bary)
{
	const float w = 1.0 - bary.x - bary.y;
	return w * vec2(attributes.n0.w, attributes.vAndSurface.x) + bary.x * vec2(attributes.n1.w, attributes.vAndSurface.y) + bary.y * vec2(attributes.n2.w, attributes.vAndSurface.z);
}

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
	localOrigin = vec3(dot(instance.row0, o), dot(instance.row1, o), dot(instance.row2, o));
	localDirection = vec3(dot(instance.row0.xyz, direction), dot(instance.row1.xyz, direction), dot(instance.row2.xyz, direction));
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
