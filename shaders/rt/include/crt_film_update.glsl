// The body of kernel 09's variants that keep a film record, shared: 0902 sums and resolves in one pass, and
// 0903 (CRT_FILM_CASCADE) also splats each sample into the pixel's cascade, and resolves in a second pass
// that reads the neighbors' cascades, so that no pixel reads what another is still writing. The including
// .comp holds the version, the extension, the comments and the includes below.
// Requires crt_common.glsl, crt_film.glsl and crt_aov.glsl.

layout (local_size_x = CRT_WORKGROUP) in;

// what the splats add to the pixel: their sum over its `samples` (one light path each, CrtParams::lightPathScale having
// scaled a light path that stood for more), divided by the pixel filter's integral (CrtParams::splatNormalization)
vec3 splatValue(uint pixel, uint samples)
{
	const vec3 sum = vec3(splats[3u * pixel], splats[3u * pixel + 1u], splats[3u * pixel + 2u]);
	return samples > 0u ? sum * (pc.splatNormalization / float(samples)) : vec3(0.0);
}

void main()
{
	const uint pixel = gl_GlobalInvocationID.x;
	if (pixel >= pc.width * pc.height) {
		return;
	}
	const ivec2 coord = ivec2(pixel % pc.width, pixel / pc.width);

#ifdef CRT_FILM_CASCADE
	// pass 1: the resolve, from the cascade every pixel finished in pass 0
	if (pc.bounce == 1u) {
		if (pc.debugView == CRT_DEBUG_SAMPLE_COUNT_HEAT) {
			return;
		}
		const FilmPixel record = filmPixels[pixel];
		if (record.accepted == 0u) {
			return;
		}
		vec3 value = outlierResolve(pixel, coord, record.accepted, record.sum.w + record.compensation.w, pc.outlierKappa);
		if (CRT_SPLATS) {
			value += splatValue(pixel, imageLoad(samplesTaken, coord).r);
		}
		imageStore(accumulation, coord, vec4(value, 1.0));
		return;
	}
#endif

	FilmPixel film = filmPixels[pixel];
	uint spawned = 0u;
	uint accepted = 0u;
	uint dropped = 0u;
	for (uint k = 0u; k < pc.samplesPerFrame; k++) {
		const vec4 slotValue = radiance[pixel * pc.samplesPerFrame + k];
		if (slotValue.w == 0.0) {
			continue;
		}
		spawned++;
		const bool finite = all(equal(slotValue.rgb, slotValue.rgb)) && all(lessThan(abs(slotValue.rgb), vec3(CRT_INFINITY)));
		if (!finite) {
			dropped++;
			continue;
		}
		const float weight = slotValue.w < 0.0 ? -1.0 : 1.0;
		filmAddSample(film, slotValue.rgb, weight, pc.filmClamp);
#ifdef CRT_FILM_CASCADE
		cascadeAddSample(pixel, filmClampSample(slotValue.rgb, pc.filmClamp), weight);
#endif
		accepted++;
	}
	if (dropped > 0u) {
		atomicAdd(traversalStats[CRT_STAT_DROPPED_SAMPLES], dropped);
	}
	// the auxiliary outputs and the light groups' weight sum, from the same slots. Once per frame: in the outlier
	// film's first pass
	if (CRT_AOV || CRT_LIGHT_GROUPS || CRT_SPECTRAL_IMAGES) {
		aovFoldPixel(pixel, coord);
	}
	if (spawned == 0u) {
		// a bidirectional integrator's splats land on pixels that took no sample this frame: their value moves still
		if (CRT_SPLATS && film.accepted > 0u && pc.debugView != CRT_DEBUG_SAMPLE_COUNT_HEAT) {
			imageStore(accumulation, coord, vec4(filmResolve(film) + splatValue(pixel, imageLoad(samplesTaken, coord).r), 1.0));
		}
		return;
	}

	const uint total = imageLoad(samplesTaken, coord).r + spawned;
	imageStore(samplesTaken, coord, uvec4(total, 0u, 0u, 0u));

	if (pc.debugView == CRT_DEBUG_SAMPLE_COUNT_HEAT) {
		// the count itself, as a grey ramp against the target - flat for a uniform budget
		imageStore(accumulation, coord, vec4(vec3(float(total) / float(max(pc.maxSamples, 1u))), 1.0));
		return;
	}

	if (accepted == 0u) {
		return;
	}
	filmPixels[pixel] = film;
#ifndef CRT_FILM_CASCADE
	// THE SPLATS of light tracing and the bidirectional integrators (docs/plans/completed/pbrt-bidirectional-path-
	// tracing.md) join the weighted mean here, their sum over the pixel's samples, as pbrt's RGBFilm::GetPixelRGB() adds
	// them. CRT_SPLATS off, the resolve is the code it always was
	if (CRT_SPLATS) {
		imageStore(accumulation, coord, vec4(filmResolve(film) + splatValue(pixel, total), 1.0));
	} else {
		imageStore(accumulation, coord, vec4(filmResolve(film), 1.0));
	}
#endif
}
