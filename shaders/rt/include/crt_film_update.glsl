// The body of kernel 09's variants that keep a film record, shared: 0902 sums and resolves in one pass, and
// 0903 (CRT_FILM_CASCADE) also splats each sample into the pixel's cascade, and resolves in a second pass
// that reads the neighbors' cascades, so that no pixel reads what another is still writing. The including
// .comp holds the version, the extension, the comments and the includes below.
// Requires crt_common.glsl, crt_film.glsl and crt_aov.glsl.

layout (local_size_x = CRT_WORKGROUP) in;

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
		imageStore(accumulation, coord, vec4(outlierResolve(pixel, coord, record.accepted, record.sum.w + record.compensation.w, pc.outlierKappa), 1.0));
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
	// THE SPLAT HOOK: light tracing and the bidirectional methods add contributions to arbitrary pixels
	// (docs/plans/pbrt-bidirectional-path-tracing.md). Their splats go in a buffer of their own, and this is
	// where the resolve adds splatSum / samplesPerPixel to the weighted mean, as pbrt's RGBFilm::GetPixelRGB() does
	imageStore(accumulation, coord, vec4(filmResolve(film), 1.0));
#endif
}
