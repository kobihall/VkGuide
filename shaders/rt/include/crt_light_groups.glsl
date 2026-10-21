// The light groups' sums (docs/plans/completed/pbrt-aovs-and-image-output.md), added to by kernels 03, 04 and 08:
// every contribution to a pixel comes from one light, one emissive material or the background, and each belongs to
// a group, 0 to 7. Several slots land on one pixel in a frame, so the adds are atomic, which needs
// GL_EXT_shader_atomic_float: the including .comp enables it before its includes. Image float atomics are missing on
// this GPU, so the sums are a buffer (binding 45) and not a layer of the AOV images. Kernel 09 adds the weight sum
// they divide by (crt_aov.glsl). Behind CRT_LIGHT_GROUPS, set when the pipeline is built. Requires crt_common.glsl.

// Adds a contribution to its group's sums at the pixel of `slot`, weighted by the sample's filter sign as the film
// weighs it. The debug views write their own values and have no groups. A contribution that is not finite is left
// out, since one NaN would poison the sum for good (the film drops the whole sample; the groups only that term)
void lightGroupAdd(uint slot, uint group, vec3 contribution)
{
	if (pc.debugView != CRT_DEBUG_NONE) {
		return;
	}
	if (!(all(equal(contribution, contribution)) && all(lessThan(abs(contribution), vec3(CRT_INFINITY))))) {
		return;
	}
	const float weight = radiance[slot].w < 0.0 ? -1.0 : 1.0;
	const uint pixel = slot / pc.samplesPerFrame;
	const uint stride = 3u * pc.lightGroupCount + 1u;
	const uint base = pixel * stride + 3u * min(group, pc.lightGroupCount - 1u);
	atomicAdd(channelSums[base + 0u], weight * contribution.r);
	atomicAdd(channelSums[base + 1u], weight * contribution.g);
	atomicAdd(channelSums[base + 2u], weight * contribution.b);
}

