// The body of kernel 00, shared by its variants: 0001 jitters a sample uniformly inside its pixel, and 0002
// (CRT_PIXEL_FILTER) places it by the pixel filter, in a neighbourhood that can reach past the pixel.
// Everything after the sample's place in the image plane is the same for both, so it is written once; the
// including .comp holds the version, the extension and the includes below. A change here changes both
// kernels, and 0001's compiled shader is checked against the commit before for exactly that reason.
// Requires crt_common.glsl, crt_random.glsl, crt_samples.glsl, and crt_filter.glsl for CRT_PIXEL_FILTER.

layout (local_size_x = CRT_WORKGROUP) in;

void main()
{
	const uint index = gl_GlobalInvocationID.x;
	// no early return: queueAppend() below has barriers every invocation must reach
	const bool inPool = index < pc.poolSize;

	bool spawn = false;

	if (inPool) {
		const uint pixel = index / pc.samplesPerFrame;
		const uint k = index - pixel * pc.samplesPerFrame;
		const uint x = pixel % pc.width;
		const uint y = pixel / pc.width;

		spawn = k < min(sampleBudget[pixel], pc.samplesPerFrame);
		radiance[index] = vec4(0.0, 0.0, 0.0, spawn ? 1.0 : 0.0);

		if (spawn) {
			// the stream depends only on the pixel, which sample of that pixel this is, and the render
			const uint sampleIndex = imageLoad(samplesTaken, ivec2(x, y)).r + k;
			uint rng = pcgHash(pixel ^ pcgHash(sampleIndex ^ pcgHash(pc.seed)));
			if (CRT_SAMPLE_RECORD) {
				rng = pcgHash(rng ^ CRT_RECORD_STREAM_SALT);
			}

			// pixel x covers [x/W, (x+1)/W) of the image plane, rows top-down; a jittered sample
			// lands anywhere in the pixel, an un-jittered one at its centre
			const bool jitter = (pc.flags & CRT_FLAG_JITTER) != 0u;
			const vec2 jitterSample = jitter ? sampleDimension2D(index, rng, CRT_SAMPLE_CAMERA_PIXEL) : vec2(0.5);
#ifdef CRT_PIXEL_FILTER
			// the sample's place in the pixel's neighbourhood comes from the filter, and the sign of the
			// filter there rides in the slot's w (kernel 09 weighs the sample by it). An un-jittered
			// sample is the centre of its pixel, as in the unfiltered variant
			float slotWeight = 1.0;
			vec2 offsetInPixel = jitterSample;
			if (jitter) {
				const FilterSample filtered = sampleFilter(jitterSample);
				offsetInPixel = vec2(0.5) + filtered.offset;
				slotWeight = filtered.weight;
			}
			radiance[index].w = slotWeight;
			const float jx = offsetInPixel.x;
			const float jy = offsetInPixel.y;
#else
			const float jx = jitterSample.x;
			const float jy = jitterSample.y;
#endif
			const float u = (float(x) + jx) / float(pc.width);
			const float v = (float(pc.height - 1u - y) + jy) / float(pc.height);

			vec3 origin = pc.origin.xyz;
			vec3 target = pc.lowerLeft.xyz + u * pc.horizontal.xyz + v * pc.vertical.xyz;
			const float lensRadius = pc.lensU.w;
			if (lensRadius > 0.0) {
				vec2 rd = lensRadius * diskFromSample(sampleDimension2D(index, rng, CRT_SAMPLE_CAMERA_LENS));
				origin += rd.x * pc.lensU.xyz + rd.y * pc.lensV.xyz;
			}

			PathState path;
			path.origin = origin;
			path.radianceSlot = index;
			path.direction = target - origin;
			path.rngState = rng;
			path.throughput = vec3(1.0);
			path.bounce = 0u;
			// the camera sampled no light, so whatever a camera ray finds counts in full
			path.misPdf = CRT_MIS_FULL;
			path.misExponent = 1.0;
			path.pad0 = 0u;
			path.pad1 = 0u;
			paths[index] = path;
		}
	}

	queueAppend(CRT_QUEUE_RAY_A, spawn, index);
}
