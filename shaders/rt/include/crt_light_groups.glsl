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
	const uint base = pixel * channelStride() + 3u * min(group, pc.lightGroupCount - 1u);
	atomicAdd(channelSums[base + 0u], weight * contribution.r);
	atomicAdd(channelSums[base + 1u], weight * contribution.g);
	atomicAdd(channelSums[base + 2u], weight * contribution.b);
}


#ifdef CRT_SPECTRAL
// THE SPECTRAL IMAGES (pbrt's SpectralFilm): a contribution's lanes added to the wavelength buckets of its pixel, after
// the light groups and the weight sum in ChannelSums. A bucket holds the mean spectral radiance over its width, so it
// reads in the film's units per nanometer once divided by the weight sum: a hero lane adds L / (pdf N) / width to the
// bucket its wavelength falls in, a band its own value to its own bucket. Weighted by the sample's filter sign, and a
// value that is not finite is left out, as lightGroupAdd() leaves it. Behind CRT_SPECTRAL_IMAGES. Requires
// crt_spectrum.glsl
void spectralImageAdd(uint slot, Spectrum contribution)
{
	if (pc.debugView != CRT_DEBUG_NONE) {
		return;
	}
	const float weight = radiance[slot].w < 0.0 ? -1.0 : 1.0;
	const uint pixel = slot / pc.samplesPerFrame;
	const uint base = pixel * channelStride() + 3u * pc.lightGroupCount + 1u;
	const uint buckets = pc.spectralBuckets;
	if (spectralRepresentation() == CRT_REPRESENTATION_HERO) {
		const float lo = spectralFloat(CRT_SH_LAMBDA_MIN);
		const float hi = spectralFloat(CRT_SH_LAMBDA_MAX);
		const float width = (hi - lo) / float(buckets);
		const float lanes = float(spectrumActiveLanes());
		for (uint i = 0u; i < CRT_LANE_END; i++) {
			const float value = spectrumLane(contribution, i);
			if (!(g_lambdaPdf[i] > 0.0) || value == 0.0 || !(abs(value) < CRT_INFINITY)) {
				continue;
			}
			const uint bucket = uint(clamp((g_lambda[i] - lo) / width, 0.0, float(buckets) - 0.5));
			atomicAdd(channelSums[base + bucket], weight * value / (g_lambdaPdf[i] * lanes * width));
		}
		return;
	}
	for (uint b = 0u; b < min(buckets, spectrumActiveLanes()); b++) {
		const float value = spectrumLane(contribution, b);
		if (value != 0.0 && abs(value) < CRT_INFINITY) {
			atomicAdd(channelSums[base + b], weight * value);
		}
	}
}
#endif
