// Kernel 08 Trace Shadow Rays - the body every variant shares.
//
// The including .comp has already supplied occluded() through one of the traversal strategies
// (crt_traverse.glsl states the contract). This file drains CRT_QUEUE_SHADOW, which kernel 06 filled
// with the ray queue position of every path that sampled a light, and for each shadow ray asks one
// question - is anything in the way - adding the contribution 06 already worked out when nothing is.
// No closest hit, no attributes, no material: pbrt-v4's IntersectShadow() (PBR 4ed 15.3.10).
//
// A shadow ray starts at its path's hit point, which is hits[] at the same queue position, and
// stops just short of the light (ShadowRay.tMax). The alpha test is part of the triangle test every
// strategy shares, so cut-out foliage casts cut-out shadows here too.
//
// Why kernel 08 has one variant per traversal rather than one shader: a shadow ray walks the same
// acceleration structure as a camera ray, and a CWBVH kernel cannot read binary nodes. Selecting a
// kernel 02 variant therefore selects its kernel 08 partner (src/rt_kernels.h followsVariant).
//
// Its traversal work goes into traversalStats after the extend stage's, per bounce, so the panel can
// show what shadow rays cost apart from the rest.
//
// Requires crt_common.glsl and an occluded() from one of the strategy headers.

layout (local_size_x = CRT_WORKGROUP) in;

// the workgroup's share of the traversal counters, so the global ones take one atomic per group
shared uint s_nodesVisited;
shared uint s_primitivesTested;

void main()
{
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
	if (index < headers[CRT_QUEUE_SHADOW].rayCount) {
		// the queue holds the RAY QUEUE POSITION: the index of the shadow ray and of the hit it leaves
		const uint entry = queues[queueSlot(CRT_QUEUE_SHADOW, index)];
		const ShadowRay ray = shadowRays[entry];
		if (!occluded(hits[entry].position, ray.direction, CRT_T_MIN, ray.tMax)) {
			// added, not written: one path sends at most one shadow ray a bounce, and 03 and 04 have
			// already added this bounce's emission
			radiance[ray.radianceSlot].rgb += ray.contribution;
		}
		atomicAdd(s_nodesVisited, g_nodesVisited);
		atomicAdd(s_primitivesTested, g_primitivesTested);
	}
	memoryBarrierShared();
	barrier();

	if (gl_LocalInvocationID.x == 0u) {
		atomicAdd(traversalStats[2u * CRT_MAX_DEPTH + 2u * pc.bounce], s_nodesVisited);
		atomicAdd(traversalStats[2u * CRT_MAX_DEPTH + 2u * pc.bounce + 1u], s_primitivesTested);
	}
}
