// GPU path tracer, stage 2: intersect every queued path with the scene and write one HitRecord
// per queue position. Shade reads that record and never learns which kind of geometry produced
// it, which is the whole point of the split - spheres and triangles differ only here.
//
// The scene is a two-level BVH (crt_bvh.glsl): a TLAS over the placed meshes and the spheres, and
// a BLAS per mesh in its object space. This file is the stage's body; crt_extend.comp and
// crt_extend_cwbvh.comp include it with the binary and the CWBVH BLAS traversal respectively, and
// src/rt_gpu.cpp picks the pipeline matching the layout the scene was built with.

// requires GL_GOOGLE_include_directive, enabled by the including .comp
#include "crt_common.glsl"
#include "crt_bvh.glsl"

layout (local_size_x = CRT_WORKGROUP) in;

// the workgroup's share of the traversal counters, so the global ones take one atomic per group
shared uint s_nodesVisited;
shared uint s_primitivesTested;

// one queued path: trace it and write its HitRecord
void extendPath(uint queue, uint index)
{
	const uint pathIndex = queues[queueSlot(queue, index)];
	const PathState path = paths[pathIndex];

	const vec3 origin = path.origin;
	const vec3 direction = normalize(path.direction);

	TraceHit trace;
	const bool found = traceScene(origin, direction, CRT_T_MIN, CRT_INFINITY, trace);

	HitRecord record;
	record.uv = vec2(0.0);
	// the traversal-cost debug view's input: this ray's node visits and primitive tests
	record.pad = vec2(float(g_nodesVisited), float(g_primitivesTested));

	if (!found) {
		record.position = vec3(0.0);
		record.t = -1.0;
		record.normal = vec3(0.0);
		record.materialAndFace = 0u;
		hits[index] = record;
		return;
	}

	const GpuInstance instance = instances[trace.instance];
	record.position = origin + trace.t * direction;
	record.t = trace.t;

	vec3 normal;
	uint material;
	if (instance.nodeBase == CRT_INSTANCE_SPHERE) {
		normal = (record.position - instance.row0.xyz) / instance.row0.w;
		material = instance.materialBase;
	} else {
		// only the closest hit fetches its shading data: the traversal touched positions alone
		const BvhTriangle tri = blasTriangles[trace.triangle];
		const GpuTriangleAttributes attributes = triangleAttributes[instance.attributeBase + floatBitsToUint(tri.v0.w)];
		const float u = trace.bary.x;
		const float v = trace.bary.y;
		const float w = 1.0 - u - v;

		record.uv = w * vec2(attributes.n0.w, attributes.vAndSurface.x) + u * vec2(attributes.n1.w, attributes.vAndSurface.y) + v * vec2(attributes.n2.w, attributes.vAndSurface.z);

		// the interpolated shading normal, which is what makes a low-poly mesh shade smoothly. A
		// mesh with no NORMAL attribute loaded as (1,0,0) everywhere, so a degenerate result falls
		// back to the geometric normal rather than producing NaN
		vec3 objectNormal = w * attributes.n0.xyz + u * attributes.n1.xyz + v * attributes.n2.xyz;
		if (dot(objectNormal, objectNormal) < 1e-12) {
			objectNormal = cross(tri.e1.xyz, tri.e2.xyz);
		}
		// to world space by the inverse transpose of object-to-world, which is the transpose of the
		// world-to-object rows the instance stores: it keeps the normal perpendicular to the surface
		// under a non-uniform scale
		normal = objectNormal.x * instance.row0.xyz + objectNormal.y * instance.row1.xyz + objectNormal.z * instance.row2.xyz;

		material = instanceMaterials[instance.materialBase + floatBitsToUint(attributes.vAndSurface.w)];
	}
	normal = normalize(normal);

	// the stored normal always faces the incoming ray, and the bit records which side was hit, so a
	// two-sided triangle - and the inside of a glass sphere - shades correctly
	const bool frontFace = dot(direction, normal) < 0.0;
	record.normal = frontFace ? normal : -normal;
	record.materialAndFace = (material << 1u) | (frontFace ? 1u : 0u);
	hits[index] = record;
}

void main()
{
	const uint queue = pc.bounce & 1u;
	const uint index = gl_GlobalInvocationID.x;

	if (gl_LocalInvocationID.x == 0u) {
		s_nodesVisited = 0u;
		s_primitivesTested = 0u;
	}
	memoryBarrierShared();
	barrier();

	// the overhang of the last workgroup does no work, but stays for the barriers below
	g_nodesVisited = 0u;
	g_primitivesTested = 0u;
	if (index < headers[queue].rayCount) {
		extendPath(queue, index);
		atomicAdd(s_nodesVisited, g_nodesVisited);
		atomicAdd(s_primitivesTested, g_primitivesTested);
	}
	memoryBarrierShared();
	barrier();

	if (gl_LocalInvocationID.x == 0u) {
		atomicAdd(traversalStats[2u * pc.bounce], s_nodesVisited);
		atomicAdd(traversalStats[2u * pc.bounce + 1u], s_primitivesTested);
	}
}
