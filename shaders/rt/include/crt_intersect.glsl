// Kernel 02 Intersect Closest - the body every variant shares.
//
// The including .comp has already supplied traceScene() (crt_traverse.glsl). This file does
// everything around it: pull the ray from the current ray queue, trace it, turn whatever came
// back into a HitRecord, and push the path onto exactly one of the three classification queues
// that kernel 03, 04 and 06 drain.
//
//     miss                         -> CRT_QUEUE_ESCAPED   -> 03 Handle Escaped
//     hit, material is emissive    -> CRT_QUEUE_EMISSIVE  -> 04 Handle Emissive Geometry
//     hit, pbr with emission       -> CRT_QUEUE_EMISSIVE and CRT_QUEUE_SURFACE
//     hit, anything else           -> CRT_QUEUE_SURFACE   -> 06 Sample Surface Scattering
//
// A glowing pbr surface is the one case on two queues, as in pbrt-v4's wavefront integrator: 04
// adds what it emits and 06 carries the path on. src/rt_gpu.cpp puts a barrier between the two,
// because 04 reads the throughput that 06 then overwrites.
//
// This is also where a normal map is applied: the tangent frame lives in the triangle's attribute
// record, which only this kernel reads, so HitRecord.normal leaves here already perturbed.
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
// changes this file and crt_shape.glsl; it changes no kernel after 02. What 02 does record for an
// emitter is which light list entry it is (HitRecord.lightIndex), since that is what kernel 04
// needs to weigh a BSDF sample that found it against light sampling (src/rt_lights.h).
//
// Requires crt_common.glsl and a traceScene() from one of the strategy headers.

layout (local_size_x = CRT_WORKGROUP) in;

// the workgroup's share of the traversal counters, so the global ones take one atomic per group
shared uint s_nodesVisited;
shared uint s_primitivesTested;

// crt_common.glsl GpuTriangleAttributes.tangents: one octahedral-encoded unit vector.
// src/rt_accel.cpp packUnitVector
vec3 unpackTangent(uint word)
{
	const vec2 e = unpackSnorm2x16(word);
	vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	if (v.z < 0.0) {
		v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
	}
	return normalize(v);
}

// The object-space normal a material's normal map gives at this point, or false where there is
// none to apply: no map, or a vertex without a tangent (a mesh with no uvs, or degenerate ones)
bool normalMapped(GpuMaterial material, GpuTriangleAttributes attributes, vec2 bary, vec2 uv, vec3 objectNormal, out vec3 mapped)
{
	mapped = objectNormal;
	if (material.normalLayer < 0 || (attributes.tangents.w & CRT_TANGENT_MISSING) != 0u) {
		return false;
	}
	const float w = 1.0 - bary.x - bary.y;
	const vec3 n = normalize(objectNormal);
	vec3 t = w * unpackTangent(attributes.tangents.x) + bary.x * unpackTangent(attributes.tangents.y) + bary.y * unpackTangent(attributes.tangents.z);
	t -= n * dot(n, t);
	if (dot(t, t) < 1e-12) {
		return false;
	}
	t = normalize(t);
	// glTF: bitangent = cross(normal, tangent.xyz) * tangent.w. The first vertex's sign stands for
	// the triangle's, since a mirrored uv seam never runs through a triangle's interior
	const vec3 b = cross(n, t) * ((attributes.tangents.w & 1u) != 0u ? -1.0 : 1.0);
	vec3 texel = textureLod(materialTextures, vec3(uv, float(material.normalLayer)), 0.0).xyz * 2.0 - 1.0;
	texel.xy *= material.normalScale;
	// built in object space and carried to world space like any normal below. Under a non-uniform
	// scale that skews the tangent components slightly; every model in use is scaled uniformly
	mapped = t * texel.x + b * texel.y + n * texel.z;
	return dot(mapped, mapped) > 1e-12;
}

// a normal from an instance's object space to world space, by the inverse transpose of
// object-to-world - which is the transpose of the world-to-object rows the instance stores. It
// keeps the normal perpendicular to the surface under a non-uniform scale, a stretched sphere's as
// much as a mesh's
vec3 worldNormal(GpuInstance instance, vec3 objectNormal)
{
	return normalize(objectNormal.x * instance.row0.xyz + objectNormal.y * instance.row1.xyz + objectNormal.z * instance.row2.xyz);
}

// one queued path: trace it, write its HitRecord, and report which queues it belongs on, as a
// mask of (1 << CRT_QUEUE_*) bits
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
	record.traversalCost = float(g_nodesVisited) + float(g_primitivesTested);
	record.lightIndex = CRT_NO_LIGHT;

	if (!found) {
		record.position = vec3(0.0);
		record.t = -1.0;
		record.normal = vec3(0.0);
		record.materialAndFace = 0u;
		hits[index] = record;
		return 1u << CRT_QUEUE_ESCAPED;
	}

	const GpuInstance instance = instances[trace.instance];
	record.position = origin + trace.t * direction;
	record.t = trace.t;

	vec3 objectNormal;
	uint material;
	bool hasMapped = false;
	vec3 mappedObjectNormal;
	if (instance.nodeBase == CRT_INSTANCE_SHAPE) {
		// the hit point back in the unit primitive's space, where its normal is defined
		const vec4 p = vec4(record.position, 1.0);
		const vec3 objectPoint = vec3(dot(instance.row0, p), dot(instance.row1, p), dot(instance.row2, p));
		objectNormal = shapeNormal(instance.triangleBase, objectPoint);
		material = instance.materialBase;
		// an emitting shape's light record; a box has one per face, in boxFace() order
		if (instance.lightBase != CRT_NO_LIGHT) {
			record.lightIndex = instance.lightBase + (instance.triangleBase == CRT_SHAPE_BOX ? boxFace(objectPoint) : 0u);
		}
	} else {
		// only the closest hit fetches its shading data: the traversal touched positions alone
		const BvhTriangle tri = blasTriangles[trace.triangle];
		const GpuTriangleAttributes attributes = triangleAttributes[instance.attributeBase + (floatBitsToUint(tri.v0.w) & CRT_TRIANGLE_INDEX_MASK)];
		const float u = trace.bary.x;
		const float v = trace.bary.y;
		const float w = 1.0 - u - v;

		record.uv = triangleUv(attributes, trace.bary);

		// the interpolated shading normal, which is what makes a low-poly mesh shade smoothly. A
		// mesh with no NORMAL attribute loaded as (1,0,0) everywhere, so a degenerate result falls
		// back to the geometric normal rather than producing NaN
		objectNormal = w * attributes.n0.xyz + u * attributes.n1.xyz + v * attributes.n2.xyz;
		if (dot(objectNormal, objectNormal) < 1e-12) {
			objectNormal = cross(tri.e1.xyz, tri.e2.xyz);
		}

		material = instanceMaterials[instance.materialBase + floatBitsToUint(attributes.vAndSurface.w)];
		hasMapped = normalMapped(materials[material], attributes, trace.bary, record.uv, objectNormal, mappedObjectNormal);
		// an emitting triangle's light record, found through its mesh-local index
		if (instance.lightBase != CRT_NO_LIGHT) {
			record.lightIndex = triangleLights[instance.lightBase + (floatBitsToUint(tri.v0.w) & CRT_TRIANGLE_INDEX_MASK)];
		}
	}
	const vec3 normal = worldNormal(instance, objectNormal);

	// the stored normal always faces the incoming ray, and the bit records which side was hit, so a
	// two-sided triangle - and the inside of a glass shape - shades correctly. The side is decided
	// by the unmapped normal: a normal map changes how a surface shades, not which side it is
	const bool frontFace = dot(direction, normal) < 0.0;
	record.normal = frontFace ? normal : -normal;
	if (hasMapped) {
		// flipped with the side, and kept only while it still faces the ray: a mapped normal tilted
		// past the viewer would send the BSDF's continuation into the surface
		vec3 mapped = worldNormal(instance, mappedObjectNormal);
		mapped = frontFace ? mapped : -mapped;
		if (dot(mapped, direction) < 0.0) {
			record.normal = mapped;
		}
	}
	record.materialAndFace = (material << 1u) | (frontFace ? 1u : 0u);
	hits[index] = record;

	// a light ends the path wherever it is hit, so it goes to its own kernel only. A glowing pbr
	// surface goes to both: 04 collects the glow, 06 scatters
	const GpuMaterial hitMaterial = materials[material];
	const bool emits = hitMaterial.type == CRT_MATERIAL_EMISSIVE || (hitMaterial.type == CRT_MATERIAL_PBR && any(greaterThan(hitMaterial.emission, vec3(0.0))));
	const bool scatters = hitMaterial.type != CRT_MATERIAL_EMISSIVE;
	return (emits ? 1u << CRT_QUEUE_EMISSIVE : 0u) | (scatters ? 1u << CRT_QUEUE_SURFACE : 0u);
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

	// three appends rather than one, because queueAppend() takes a single queue and must be
	// reached by every invocation of the workgroup in uniform control flow. A lane pushes to one
	// of them, or to emissive and surface both, and passes survives = false to the rest
	queueAppend(CRT_QUEUE_ESCAPED, (targets & (1u << CRT_QUEUE_ESCAPED)) != 0u, index);
	queueAppend(CRT_QUEUE_EMISSIVE, (targets & (1u << CRT_QUEUE_EMISSIVE)) != 0u, index);
	queueAppend(CRT_QUEUE_SURFACE, (targets & (1u << CRT_QUEUE_SURFACE)) != 0u, index);
}
