// The auxiliary outputs' side of the kernels (docs/plans/completed/pbrt-aovs-and-image-output.md): the first
// visible surface a camera path records into PixelState, and kernel 09's fold of a frame's slots into AovImages
// and the light groups' weight sum. The arithmetic of the fold mirrors src/aov.h, line for line: change both together and run
// bin/aov_test and the GPU self-tests.
//
// Everything is behind the specialization constants CRT_AOV and CRT_LIGHT_GROUPS (crt_common.glsl), set when the
// pipeline is built, so a render that asks for neither runs the code it did before. The light groups' atomic adds
// are in crt_light_groups.glsl, which only the kernels that add to them include. Requires crt_common.glsl.

//---------------------------------------------------------------- PixelState, written by kernels 00, 03, 04, 06

// kernel 00: a slot starts the frame with no surface recorded
void aovClear(uint slot)
{
	pixelStates[slot].state = 0u;
}

// whether the camera path in `slot` records the surface it is at now. A path records at most one. With the first
// surface (pbrt's way) that is the one at bounce 0; capturing through specular, the first that is not a delta
// BSDF, so a mirror or a window shows what is behind it
bool aovWantsSurface(uint slot, bool specular)
{
	if ((pixelStates[slot].state & CRT_AOV_STATE_HIT) != 0u) {
		return false;
	}
	if ((pc.aovOutputs & CRT_AOV_THROUGH_SPECULAR) != 0u) {
		return !specular;
	}
	return pc.bounce == 0u;
}

// the distance of a world-space point along the camera's forward axis: -t, where t = n x b is the camera's backward
// axis, whose two other axes are in lensU and lensV (src/rt_gpu.cpp baseParams())
float aovDepth(vec3 p)
{
	const vec3 backward = cross(pc.lensU.xyz, pc.lensV.xyz);
	return dot(pc.origin.xyz - p, backward);
}

void aovRecord(uint slot, vec3 p, vec3 n, vec3 ns, vec2 uv, vec3 albedo, uint instance, uint material)
{
	PixelState state;
	state.px = p.x;
	state.py = p.y;
	state.pz = p.z;
	state.depth = aovDepth(p);
	state.nx = n.x;
	state.ny = n.y;
	state.nz = n.z;
	state.nsx = ns.x;
	state.nsy = ns.y;
	state.nsz = ns.z;
	state.u = uv.x;
	state.v = uv.y;
	state.ar = albedo.x;
	state.ag = albedo.y;
	state.ab = albedo.z;
	state.instance = instance;
	state.material = material;
	state.state = CRT_AOV_STATE_HIT;
	pixelStates[slot] = state;
}

//---------------------------------------------------------------- the fold, in kernel 09

// where the layer `which` is in aovImages (src/aov.h AovLayer), or AOV_LAYER_NONE
#define CRT_AOV_LAYER_NONE 15u
#define CRT_AOV_LAYER_WEIGHTS 0u
#define CRT_AOV_LAYER_POSITION_DEPTH 1u
#define CRT_AOV_LAYER_NORMAL 2u
#define CRT_AOV_LAYER_SHADING_NORMAL 3u
#define CRT_AOV_LAYER_UV 4u
#define CRT_AOV_LAYER_ALBEDO 5u
#define CRT_AOV_LAYER_VARIANCE_MEAN 6u
#define CRT_AOV_LAYER_VARIANCE_M2 7u
#define CRT_AOV_LAYERS 8u

uint aovLayerIndex(uint which)
{
	return (pc.aovLayers >> (4u * which)) & 0xFu;
}

// Folds this frame's slots of a pixel into the layers the render keeps, and the sum of the filter weights into the
// light groups' sums. Each slot is taken exactly as the film takes it: unspawned slots and non-finite samples are
// skipped, the sign of the slot's w is the filter weight, and the radiance the variance sees is clamped as the film
// clamps it. The sums are read once, added to sample by sample in registers (the arithmetic of AovPixel::add), and
// written back once
void aovFoldPixel(uint pixel, ivec2 coord)
{
	vec4 layers[CRT_AOV_LAYERS];
	for (uint i = 0u; i < CRT_AOV_LAYERS; i++) {
		layers[i] = vec4(0.0);
		if (CRT_AOV) {
			const uint layer = aovLayerIndex(i);
			if (layer != CRT_AOV_LAYER_NONE) {
				layers[i] = imageLoad(aovImages, ivec3(coord, int(layer)));
			}
		}
	}

	float weightSum = 0.0;
	bool any = false;
	for (uint k = 0u; k < pc.samplesPerFrame; k++) {
		const uint slot = pixel * pc.samplesPerFrame + k;
		const vec4 slotValue = radiance[slot];
		if (slotValue.w == 0.0) {
			continue;
		}
		const bool finite = all(equal(slotValue.rgb, slotValue.rgb)) && all(lessThan(abs(slotValue.rgb), vec3(CRT_INFINITY)));
		if (!finite) {
			continue;
		}
		const float weight = slotValue.w < 0.0 ? -1.0 : 1.0;
		weightSum += weight;
		any = true;
		if (!CRT_AOV) {
			continue;
		}

		const PixelState state = pixelStates[slot];
		const bool hit = (state.state & CRT_AOV_STATE_HIT) != 0u;
		layers[CRT_AOV_LAYER_WEIGHTS].y += weight;
		if (hit) {
			layers[CRT_AOV_LAYER_WEIGHTS].x += weight;
			if (layers[CRT_AOV_LAYER_WEIGHTS].z == 0.0) {
				layers[CRT_AOV_LAYER_WEIGHTS].z = float(state.instance) + 1.0;
				layers[CRT_AOV_LAYER_WEIGHTS].w = float(state.material) + 1.0;
			}
			layers[CRT_AOV_LAYER_POSITION_DEPTH] += weight * vec4(state.px, state.py, state.pz, state.depth);
			layers[CRT_AOV_LAYER_NORMAL].xyz += weight * vec3(state.nx, state.ny, state.nz);
			layers[CRT_AOV_LAYER_SHADING_NORMAL].xyz += weight * vec3(state.nsx, state.nsy, state.nsz);
			layers[CRT_AOV_LAYER_UV].xy += weight * vec2(state.u, state.v);
			layers[CRT_AOV_LAYER_ALBEDO].xyz += weight * vec3(state.ar, state.ag, state.ab);
		}

		// Welford's update per channel (pbrt's VarianceEstimator::Add) of the sample as the film took it
		vec3 sampleRadiance = slotValue.rgb;
		if (pc.filmClamp > 0.0) {
			const float m = max(sampleRadiance.r, max(sampleRadiance.g, sampleRadiance.b));
			if (m > pc.filmClamp) {
				sampleRadiance *= pc.filmClamp / m;
			}
		}
		layers[CRT_AOV_LAYER_VARIANCE_MEAN].w += 1.0;
		const float n = layers[CRT_AOV_LAYER_VARIANCE_MEAN].w;
		const vec3 delta = sampleRadiance - layers[CRT_AOV_LAYER_VARIANCE_MEAN].xyz;
		layers[CRT_AOV_LAYER_VARIANCE_MEAN].xyz += delta / n;
		const vec3 delta2 = sampleRadiance - layers[CRT_AOV_LAYER_VARIANCE_MEAN].xyz;
		layers[CRT_AOV_LAYER_VARIANCE_M2].xyz += delta * delta2;
	}
	if (!any) {
		return;
	}

	if (CRT_AOV) {
		for (uint i = 0u; i < CRT_AOV_LAYERS; i++) {
			const uint layer = aovLayerIndex(i);
			if (layer != CRT_AOV_LAYER_NONE) {
				imageStore(aovImages, ivec3(coord, int(layer)), layers[i]);
			}
		}
	}
	if (CRT_LIGHT_GROUPS) {
		// kernels 03, 04 and 08 are done, and this pixel is this invocation's alone: no atomic
		channelSums[pixel * (3u * pc.lightGroupCount + 1u) + 3u * pc.lightGroupCount] += weightSum;
	}
}
