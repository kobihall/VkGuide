// Kernel 08 Trace Shadow Rays - the body every variant shares.
//
// The including .comp has already supplied occluded() through crt_trace.glsl, for whichever
// traversal the build compiled it for (crt_traverse.glsl states the contract). This file drains CRT_QUEUE_SHADOW, which kernel 06 filled
// with the ray queue position of every path that sampled a light, and for each shadow ray asks one
// question - is anything in the way - adding the contribution 06 already worked out when nothing is.
// No closest hit, no attributes, no material: pbrt-v4's IntersectShadow() (PBR 4ed 15.3.10).
//
// A shadow ray starts at the spawn point kernel 06 gave it (ShadowRay.origin) and stops just short
// of the light (ShadowRay.tMax). The alpha test is part of the triangle test every
// strategy shares, so cut-out foliage casts cut-out shadows here too.
//
// A shadow ray walks the same acceleration structure as a camera ray, and a CWBVH walk cannot read
// binary nodes, so kernel 08 is built for every traversal like kernel 02 and the renderer runs both
// for the one traversal selected (src/rt_kernels.h TraversalVariant).
//
// Its traversal work goes into traversalStats after the extend stage's, per bounce, so the panel can
// show what shadow rays cost apart from the rest.
//
// Requires crt_common.glsl, crt_spectrum.glsl, crt_trace.glsl and crt_light_groups.glsl.

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
		// the queue holds the RAY QUEUE POSITION: the index of the shadow ray
		const uint entry = queues[queueSlot(CRT_QUEUE_SHADOW, index)];
		const ShadowRay ray = shadowRays[entry];
		if (!occluded(ray.origin, ray.direction, traceTMin(), ray.tMax)) {
			// added, not written: one path sends at most one shadow ray a bounce, and 03 and 04 have
			// already added this bounce's emission
#ifndef CRT_SPECTRAL
			radiance[ray.radianceSlot].rgb += ray.contribution;
			if (CRT_LIGHT_GROUPS) {
				lightGroupAdd(ray.radianceSlot, ray.mediumAndGroup >> 24u, ray.contribution);
			}
#else
			// a debug view's value rides in the first lane (kernel 06): green for the MIS weights, gray for ambient
			// occlusion. Radiance turns into the film's color at the wavelengths the path carried
			if (pc.debugView != CRT_DEBUG_NONE) {
				const float value = spectrumLane(ray.contribution, 0u);
				radiance[ray.radianceSlot].rgb += pc.debugView == CRT_DEBUG_MIS_WEIGHTS ? vec3(0.0, value, 0.0) : vec3(value);
			} else {
				spectralBegin(ray.lambdaU);
				const vec3 added = filmAdd(ray.radianceSlot, ray.contribution);
				if (CRT_LIGHT_GROUPS) {
					lightGroupAdd(ray.radianceSlot, ray.mediumAndGroup >> 24u, added);
				}
				if (CRT_SPECTRAL_IMAGES) {
					spectralImageAdd(ray.radianceSlot, ray.contribution);
				}
			}
#endif
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
