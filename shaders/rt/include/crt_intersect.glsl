// Kernel 02 Intersect Closest - the body every variant shares.
//
// The including .comp has already supplied traceScene() (crt_traverse.glsl). This file does
// everything around it: pull the ray from the current ray queue, trace it, turn whatever came
// back into a HitRecord, and push the path onto exactly one of the three classification queues
// that kernel 03, 04 and 06 drain.
//
//     miss                         -> CRT_QUEUE_ESCAPED   -> 03 Handle Escaped
//     hit, material is emissive    -> CRT_QUEUE_EMISSIVE  -> 04 Handle Emissive Geometry
//     hit, anything else           -> CRT_QUEUE_SURFACE   -> 06 Sample Surface Scattering
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
// The HitRecord is deliberately geometry-agnostic: a shape and a triangle write the same
// record, and nothing downstream of here can tell which it came from. Adding a primitive kind
// changes this file and crt_shape.glsl; it changes no kernel after 02.
//
// Requires crt_common.glsl and a traceScene() from one of the strategy headers.

layout (local_size_x = CRT_WORKGROUP) in;

// the workgroup's share of the traversal counters, so the global ones take one atomic per group
shared uint s_nodesVisited;
shared uint s_primitivesTested;

// one queued path: trace it, write its HitRecord, and report which queue it belongs on
uint intersectPath(uint queue, uint index)
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
		return CRT_QUEUE_ESCAPED;
	}

	const GpuInstance instance = instances[trace.instance];
	record.position = origin + trace.t * direction;
	record.t = trace.t;

	vec3 objectNormal;
	uint material;
	if (instance.nodeBase == CRT_INSTANCE_SHAPE) {
		// the hit point back in the unit primitive's space, where its normal is defined
		const vec4 p = vec4(record.position, 1.0);
		objectNormal = shapeNormal(instance.triangleBase, vec3(dot(instance.row0, p), dot(instance.row1, p), dot(instance.row2, p)));
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
		objectNormal = w * attributes.n0.xyz + u * attributes.n1.xyz + v * attributes.n2.xyz;
		if (dot(objectNormal, objectNormal) < 1e-12) {
			objectNormal = cross(tri.e1.xyz, tri.e2.xyz);
		}

		material = instanceMaterials[instance.materialBase + floatBitsToUint(attributes.vAndSurface.w)];
	}
	// to world space by the inverse transpose of object-to-world, which is the transpose of the
	// world-to-object rows the instance stores: it keeps the normal perpendicular to the surface
	// under a non-uniform scale - a stretched sphere's as much as a mesh's
	vec3 normal = objectNormal.x * instance.row0.xyz + objectNormal.y * instance.row1.xyz + objectNormal.z * instance.row2.xyz;
	normal = normalize(normal);

	// the stored normal always faces the incoming ray, and the bit records which side was hit, so a
	// two-sided triangle - and the inside of a glass shape - shades correctly
	const bool frontFace = dot(direction, normal) < 0.0;
	record.normal = frontFace ? normal : -normal;
	record.materialAndFace = (material << 1u) | (frontFace ? 1u : 0u);
	hits[index] = record;

	// a light ends the path wherever it is hit, so it goes to its own kernel rather than through
	// the scattering one's front door
	return materials[material].type == CRT_MATERIAL_EMISSIVE ? CRT_QUEUE_EMISSIVE : CRT_QUEUE_SURFACE;
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
	uint target = CRT_QUEUE_COUNT;
	if (index < headers[queue].rayCount) {
		target = intersectPath(queue, index);
		atomicAdd(s_nodesVisited, g_nodesVisited);
		atomicAdd(s_primitivesTested, g_primitivesTested);
	}
	memoryBarrierShared();
	barrier();

	if (gl_LocalInvocationID.x == 0u) {
		atomicAdd(traversalStats[2u * pc.bounce], s_nodesVisited);
		atomicAdd(traversalStats[2u * pc.bounce + 1u], s_primitivesTested);
	}

	// three appends rather than one, because queueAppend() takes a single queue and must be
	// reached by every invocation of the workgroup in uniform control flow. A lane pushes to
	// exactly one of them and passes survives = false to the other two
	queueAppend(CRT_QUEUE_ESCAPED, target == CRT_QUEUE_ESCAPED, index);
	queueAppend(CRT_QUEUE_EMISSIVE, target == CRT_QUEUE_EMISSIVE, index);
	queueAppend(CRT_QUEUE_SURFACE, target == CRT_QUEUE_SURFACE, index);
}
