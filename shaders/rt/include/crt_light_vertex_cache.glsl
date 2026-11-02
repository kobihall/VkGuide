// THE LIGHT VERTEX CACHE of the `lvc` integrator (docs/plans/completed/pbrt-bidirectional-path-tracing.md; Davidovic et al.
// 2014, after van Antwerpen 2011), and the recursive MIS weights of Georgiev's "Implementing vertex connection and
// merging" (2012) with vertex connection alone, as SmallVCM computes them. Binding 52 holds:
//
//   a header       the vertices stored this frame (an atomic counter), the capacity, the record's size in uvec4s, and
//                  where the records start
//   per pool slot  one uvec4, the path's MIS state: a light path's emission (its pdfPos pdfDir, the cosine at the light,
//                  the light, CRT_LVC_LIGHT_* flags) until its first hit, then the partial sums dVCM and dVC each
//                  scatter leaves for the next vertex; a camera path's dVCM and dVC from its first scatter on
//   the vertices   each non-specular vertex of the frame's light paths that can still connect to a camera vertex: its
//                  thin hit record and the ray that found it (so a connection rebuilds its surface), its path length, its
//                  partial sums, its live lanes and its throughput
//
// THE MIS (balance heuristic). A light path of the frame's N_L serves the frame's N_C camera paths: light tracing has
// r = N_L / N_C samples for each of the camera strategies' one (CrtParams::lightPathScale is 1 / r). Every vertex
// carries dVCM and dVC: dVCM holds the ratio of the densities that close the path at this vertex from the other side,
// dVC the sum over the strategies further back. One departure from SmallVCM: this renderer's light samples pick an
// area light's point by solid angle from the receiving point, not by area, so a light path's first dVCM, which holds
// the light sample's density of the light's point, is made at its first hit, where that receiving point is known,
// rather than at its emission. The pdfs leave Russian roulette out, as pbrt's BDPT has none: the weights are computed
// the same way from every strategy, so they still sum to one.
//
// Requires crt_common.glsl, crt_spectrum.glsl, equirect.glsl and crt_light.glsl; the including kernel declares bindings 52
// and 18 (the lights), and 65 for crt_light_emission.glsl.

#ifndef CRT_LIGHT_VERTEX_CACHE_GLSL
#define CRT_LIGHT_VERTEX_CACHE_GLSL

#include "crt_light_emission.glsl"

// CRT_LIGHT_VERTEX_CACHE: the light path kernels (11, 12, 13) serve the light vertex cache: kernel 11 keeps each light
// path's emission record, 12 weighs its connection to the camera, 13 stores its vertices and carries its partial sums.
// Off, under light tracing, they are the code they would be without it (src/rt_kernels.h servesLightVertexCache)
layout (constant_id = 11) const bool CRT_LIGHT_VERTEX_CACHE = false;

layout (std430, set = 0, binding = 52) buffer LightVertexCacheBuffer {
	uint lvcCount;
	uint lvcCapacity;
	uint lvcStride;
	uint lvcVertexBase;
	uvec4 lvcWords[];
};

// a light path's emission record's flags
#define CRT_LVC_LIGHT_DELTA_POSITION 1u
#define CRT_LVC_LIGHT_DELTA_DIRECTION 2u
#define CRT_LVC_LIGHT_INFINITE 4u

// the heuristic: the balance heuristic's identity
float lvcMis(float x)
{
	return x;
}

// the uvec4s a throughput takes: one for exact RGB and up to four lanes, a lane family's quarter otherwise
#if !defined(CRT_SPECTRAL) || CRT_LANES <= 4
#define CRT_LVC_SPECTRUM_WORDS 1u
#else
#define CRT_LVC_SPECTRUM_WORDS (CRT_LANES / 4u)
#endif
// src/rt_lvc_schedule.cpp lvcVertexWords() mirrors this
#define CRT_LVC_VERTEX_WORDS (4u + CRT_LVC_SPECTRUM_WORDS)

struct CachedVertex {
	HitRecord hit;
	// the ray that found the vertex, from the light path's previous vertex
	vec3 rayOrigin;
	vec3 rayDirection;
	// the light path's edges up to here
	uint pathLength;
	float dVCM;
	float dVC;
	uint laneMask;
	Spectrum throughput;
};

uvec4 spectrumWord(Spectrum s, uint word)
{
#ifndef CRT_SPECTRAL
	return uvec4(floatBitsToUint(s), 0u);
#elif CRT_LANES == 1
	return uvec4(floatBitsToUint(s), 0u, 0u, 0u);
#elif CRT_LANES == 4
	return floatBitsToUint(s);
#else
	return floatBitsToUint(s.c[word]);
#endif
}

void setSpectrumWord(inout Spectrum s, uint word, uvec4 bits)
{
#ifndef CRT_SPECTRAL
	s = uintBitsToFloat(bits.xyz);
#elif CRT_LANES == 1
	s = uintBitsToFloat(bits.x);
#elif CRT_LANES == 4
	s = uintBitsToFloat(bits);
#else
	s.c[word] = uintBitsToFloat(bits);
#endif
}

void lvcStoreVertex(uint index, CachedVertex v)
{
	const uint base = lvcVertexBase + index * lvcStride;
	lvcWords[base] = uvec4(floatBitsToUint(v.hit.t), v.hit.instance, floatBitsToUint(v.hit.barycentrics));
	lvcWords[base + 1u] = uvec4(v.hit.primitive, floatBitsToUint(v.rayOrigin));
	lvcWords[base + 2u] = uvec4(floatBitsToUint(v.rayDirection), v.pathLength);
	lvcWords[base + 3u] = uvec4(floatBitsToUint(v.dVCM), floatBitsToUint(v.dVC), v.laneMask, 0u);
	for (uint word = 0u; word < CRT_LVC_SPECTRUM_WORDS; word++) {
		lvcWords[base + 4u + word] = spectrumWord(v.throughput, word);
	}
}

CachedVertex lvcLoadVertex(uint index)
{
	const uint base = lvcVertexBase + index * lvcStride;
	const uvec4 w0 = lvcWords[base];
	const uvec4 w1 = lvcWords[base + 1u];
	const uvec4 w2 = lvcWords[base + 2u];
	const uvec4 w3 = lvcWords[base + 3u];
	CachedVertex v;
	v.hit.t = uintBitsToFloat(w0.x);
	v.hit.instance = w0.y;
	v.hit.barycentrics = uintBitsToFloat(w0.zw);
	v.hit.primitive = w1.x;
	v.hit.traversalCost = 0.0;
	v.rayOrigin = uintBitsToFloat(w1.yzw);
	v.rayDirection = uintBitsToFloat(w2.xyz);
	v.pathLength = w2.w;
	v.dVCM = uintBitsToFloat(w3.x);
	v.dVC = uintBitsToFloat(w3.y);
	v.laneMask = w3.z;
	v.throughput = spectrumConst(0.0);
	for (uint word = 0u; word < CRT_LVC_SPECTRUM_WORDS; word++) {
		setSpectrumWord(v.throughput, word, lvcWords[base + 4u + word]);
	}
	return v;
}

// A path's partial sums after the update its hit makes (SmallVCM's "update the MIS quantities before storing them"):
// dVCM times the squared distance travelled (unless the segment left a light at infinity), dVCM and dVC over the cosine
// at the hit. `cosine` is |cos| at the hit between the shading normal and the way back
void lvcUpdateAtHit(inout float dVCM, inout float dVC, float distanceSquared, bool fromInfinity, float cosine)
{
	if (!fromInfinity) {
		dVCM *= lvcMis(distanceSquared);
	}
	const float c = lvcMis(max(cosine, 1e-8));
	dVCM /= c;
	dVC /= c;
}

// A light path's partial sums at its vertex: from its emission record at its first hit, where the light sample's
// density of the light's point from this vertex `p` is known (`origin` is the point the path left), or from the sums
// its last scatter left. `distance` is the segment's length
void lvcLightPartials(uint pathIndex, uint bounce, vec3 origin, vec3 p, float distance, float cosine, out float dVCM, out float dVC)
{
	const uvec4 state = lvcWords[pathIndex];
	bool fromInfinity = false;
	if (bounce == 0u) {
		const float emissionPdf = uintBitsToFloat(state.x);
		const float cosLight = uintBitsToFloat(state.y);
		const uint lightIndex = state.z;
		const uint flags = state.w;
		// dVCM is the light sample's density of the point over the emission's, in which the light's pick cancels; dVC is
		// over the emission's density alone, the pick included (SmallVCM's GenerateLightSample)
		const float pickedEmissionPdf = lightPmf(lightIndex) * emissionPdf;
		if ((flags & CRT_LVC_LIGHT_INFINITE) != 0u) {
			// the light sample's density of a direction over its emission's: one over the disk's density, either kind
			fromInfinity = true;
			dVCM = lvcMis(1.0 / infiniteLightPdfPos());
			dVC = (flags & CRT_LVC_LIGHT_DELTA_DIRECTION) != 0u ? 0.0 : lvcMis(1.0 / pickedEmissionPdf);
		} else if ((flags & CRT_LVC_LIGHT_DELTA_POSITION) != 0u) {
			// a light sample picks the point for certain
			dVCM = lvcMis(1.0 / emissionPdf);
			dVC = 0.0;
		} else {
			// the light sample's density of the point, by area: its solid-angle density from p, times cos / d^2, which the
			// update's d^2 cancels, so it is folded in here and the update skips it
			const float directPdfW = areaLightPdf(lightIndex, p, origin);
			dVCM = lvcMis(directPdfW * cosLight / emissionPdf);
			dVC = lvcMis(cosLight / pickedEmissionPdf);
			fromInfinity = true;
		}
	} else {
		dVCM = uintBitsToFloat(state.x);
		dVC = uintBitsToFloat(state.y);
	}
	lvcUpdateAtHit(dVCM, dVC, distance * distance, fromInfinity, cosine);
}

// The sums a scatter leaves for the next vertex: a delta lobe's reverse density is its forward one, so only the
// cosine is carried; any other's `forwardPdf` and `reversePdf` are the densities of the drawn direction and of the way
// back drawn from it (SmallVCM's SampleScattering)
void lvcScatter(inout float dVCM, inout float dVC, bool delta, float cosOut, float forwardPdf, float reversePdf)
{
	if (delta) {
		dVCM = 0.0;
		dVC *= lvcMis(cosOut);
		return;
	}
	dVC = lvcMis(cosOut / forwardPdf) * (dVC * lvcMis(reversePdf) + dVCM);
	dVCM = lvcMis(1.0 / forwardPdf);
}

#ifdef CRT_CAMERA_GLSL
// A camera path's partial sums arriving at its vertex, before the hit's update: at its first vertex from the camera ray,
// light tracing's r samples over the camera's density of the ray (SmallVCM's dVCM = N_L / cameraPdfW; the lens's density
// is the same for both strategies, so it cancels and is left out), and after that the sums its last scatter left
void lvcCameraPartials(uint pathIndex, PathState path, out float dVCM, out float dVC)
{
	dVCM = 0.0;
	dVC = 0.0;
	if (path.bounce == 0u) {
		float pdfPos;
		float pdfDir;
		cameraPdfWe(path.origin, normalize(path.direction), pdfPos, pdfDir);
		if (pdfDir > 0.0) {
			dVCM = lvcMis(1.0 / (pc.lightPathScale * pdfDir));
		}
		return;
	}
	const uvec4 state = lvcWords[pathIndex];
	dVCM = uintBitsToFloat(state.x);
	dVC = uintBitsToFloat(state.y);
}
#endif

#endif
