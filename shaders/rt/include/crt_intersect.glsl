// Kernel 02 Intersect Closest - the body every variant shares.
//
// The including .comp has already supplied traceScene() through crt_trace.glsl, for whichever
// traversal the build compiled it for (crt_traverse.glsl states the contract). This file does
// everything around it: pull the ray from the current ray queue, trace it, turn whatever came
// back into a HitRecord, and push the path onto exactly one of the three classification queues
// that kernel 03, 04 and 06 drain.
//
//     miss                         -> CRT_QUEUE_ESCAPED   -> 03 Handle Escaped
//     hit, material is emissive    -> CRT_QUEUE_EMISSIVE  -> 04 Handle Emissive Geometry
//     hit, pbr with emission       -> CRT_QUEUE_EMISSIVE and CRT_QUEUE_SURFACE
//     hit, anything else           -> CRT_QUEUE_SURFACE   -> 06 Sample Surface Scattering, basic build
//     hit, a complex material      -> CRT_QUEUE_SURFACE_COMPLEX -> 06, complex build
//
// Basic or complex is the material's CRT_MATERIAL_FLAG_COMPLEX: a large or branchy evaluation gets
// a build of 06 of its own, so the common materials' build allocates registers for them alone
// (PBR 4ed 15.3.9). None of the six original types is complex, so today the complex queue is empty.
//
// A glowing pbr surface is the one case on two queues, as in pbrt-v4's wavefront integrator: 04
// adds what it emits and 06 carries the path on. src/rt_path_schedule.cpp puts a barrier between the two,
// because 04 reads the throughput that 06 then overwrites.
//
// This is the split that makes each of those kernels start out CONVERGED: 06 runs only over
// paths that really do need a BSDF sampled, not over a queue where two lanes in three are
// missing or hitting a light. It is the whole reason the wavefront is worth its bandwidth
// (PBR 4ed 15.1.2).
//
// The queues hold the RAY QUEUE POSITION, not the path index. That one number is also the index
// of the path's HitRecord, so a consumer reaches both the hit and the path from it, and the
// hits[] array stays written once and read once with no indirection of its own.
//
// THE RECORD IS THIN: the instance, the primitive, where on it, t and the traversal cost, nothing
// derived. Kernels 04 and 06 rebuild the point, the normals, the uv, the material and the light
// record from it with crt_interaction.glsl, and the material's normal map is applied there, in
// material evaluation. Classifying needs only the material, which a triangle names through its
// surface index in BvhTriangle.v1.w, so the closest hit's attribute record is not fetched here at
// all. A shape's part and parameters (crt_shape.glsl shapePart(), shapeUv()) are solved here once,
// since the point is at hand. Adding a primitive kind changes this file, crt_shape.glsl and
// crt_interaction.glsl; it changes no kernel after 02.
//
// Requires crt_common.glsl and a traceScene() from one of the strategy headers.

layout (local_size_x = CRT_WORKGROUP) in;

// the workgroup's share of the traversal counters, so the global ones take one atomic per group
shared uint s_nodesVisited;
shared uint s_primitivesTested;

// one queued path: trace it, write its HitRecord, and report which queues it belongs on, as a
// mask of (1 << CRT_QUEUE_*) bits
uint intersectPath(uint queue, uint index)
{
	const uint pathIndex = queues[queueSlot(queue, index)];
	const PathState path = paths[pathIndex];

	const vec3 origin = path.origin;
	const vec3 direction = normalize(path.direction);

	TraceHit trace;
	const bool found = traceScene(origin, direction, traceTMin(), CRT_INFINITY, trace);

	HitRecord record;
	record.instance = 0u;
	record.primitive = 0u;
	record.barycentrics = vec2(0.0);
	// the traversal-cost debug view's input: this ray's node visits and primitive tests
	record.traversalCost = float(g_nodesVisited) + float(g_primitivesTested);

	if (!found) {
		record.t = -1.0;
		hits[index] = record;
		return 1u << CRT_QUEUE_ESCAPED;
	}

	const GpuInstance instance = instances[trace.instance];
	record.t = trace.t;
	record.instance = trace.instance;

	uint material;
	if (instance.nodeBase == CRT_INSTANCE_SHAPE) {
		// the hit point back in the unit primitive's space, where its parts and parameters are
		// defined. The same point crt_interaction.glsl rebuilds from the same ray, fused the same way
		vec3 objectError;
		const vec3 objectPoint = shapeObjectHit(instance, origin, direction, trace.t, objectError);
		record.primitive = shapePart(instance.triangleBase, objectPoint);
		record.barycentrics = shapeUv(instance.triangleBase, record.primitive, objectPoint);
		material = instance.materialBase;
	} else {
		record.primitive = trace.triangle;
		record.barycentrics = trace.bary;
		material = instanceMaterials[instance.materialBase + floatBitsToUint(blasTriangles[trace.triangle].v1.w)];
	}
	hits[index] = record;

	// a light ends the path wherever it is hit, so it goes to its own kernel only. A glowing pbr
	// surface goes to both: 04 collects the glow, 06 scatters. The header's flags say which, so the
	// material's parameters are not read here at all
	const uint flags = materials[material].flags;
	const bool emits = (flags & CRT_MATERIAL_FLAG_EMITS) != 0u;
	const bool scatters = (flags & CRT_MATERIAL_FLAG_SCATTERS) != 0u;
	const uint surfaceQueue = (flags & CRT_MATERIAL_FLAG_COMPLEX) != 0u ? CRT_QUEUE_SURFACE_COMPLEX : CRT_QUEUE_SURFACE;
	return (emits ? 1u << CRT_QUEUE_EMISSIVE : 0u) | (scatters ? 1u << surfaceQueue : 0u);
}

void main()
{
	const uint queue = rayQueueFor(pc.bounce);
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
	uint targets = 0u;
	if (index < headers[queue].rayCount) {
		targets = intersectPath(queue, index);
		atomicAdd(s_nodesVisited, g_nodesVisited);
		atomicAdd(s_primitivesTested, g_primitivesTested);
	}
	memoryBarrierShared();
	barrier();

	if (gl_LocalInvocationID.x == 0u) {
		atomicAdd(traversalStats[2u * pc.bounce], s_nodesVisited);
		atomicAdd(traversalStats[2u * pc.bounce + 1u], s_primitivesTested);
	}

	// four appends rather than one, because queueAppend() takes a single queue and must be reached
	// by every invocation of the workgroup in uniform control flow. A lane pushes to one of them, or
	// to emissive and a surface queue both, and passes survives = false to the rest
	queueAppend(CRT_QUEUE_ESCAPED, (targets & (1u << CRT_QUEUE_ESCAPED)) != 0u, index);
	queueAppend(CRT_QUEUE_EMISSIVE, (targets & (1u << CRT_QUEUE_EMISSIVE)) != 0u, index);
	queueAppend(CRT_QUEUE_SURFACE, (targets & (1u << CRT_QUEUE_SURFACE)) != 0u, index);
	queueAppend(CRT_QUEUE_SURFACE_COMPLEX, (targets & (1u << CRT_QUEUE_SURFACE_COMPLEX)) != 0u, index);
}
