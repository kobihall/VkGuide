// PBRT 3'S BIDIRECTIONAL PATH TRACER'S VERTICES (docs/plans/completed/pbrt-bidirectional-path-tracing.md; PBR 3ed 16.3,
// pbrt-v3's bdpt.h Vertex). Binding 53 holds, for each pixel sample of the frame's tile (CrtParams::bdptTileStart on):
//
//   a header        the tile's first pixel, the vertices a pixel sample keeps, the record's size in uvec4s, where the
//                   records start
//   per slot        the camera subpath's and the light subpath's vertex counts
//   the vertices    the camera subpath's (the camera, then a vertex per bounce, rayDepth + 1 at most), then the light
//                   subpath's (the light, then a vertex per bounce, rayDepth at most), each pbrt's Vertex: its kind, its
//                   point and geometric normal, beta, pdfFwd and pdfRev by area (by solid angle at a light at infinity),
//                   and for a surface its thin hit record and the ray that found it, from which stage 18 rebuilds its BSDF
//
// pbrt's functions that walk a subpath read these: ConvertDensity(), Vertex::Pdf(), PdfLight(), PdfLightOrigin(), each in
// crt_bdpt_connect.glsl beside MISWeight(). Requires crt_common.glsl and crt_spectrum.glsl; the including kernel declares
// binding 53.

#ifndef CRT_BDPT_GLSL
#define CRT_BDPT_GLSL

// CRT_BDPT: kernel 11 stores each light subpath's first vertex for pbrt 3's BDPT, and connects nothing to the camera
// (src/rt_kernels.h storesBdptVertices)
layout (constant_id = 12) const bool CRT_BDPT = false;

layout (std430, set = 0, binding = 53) buffer BdptVertexBuffer {
	uint bdptTileStart;
	uint bdptVerticesPerSlot;
	uint bdptStride;
	uint bdptVertexBase;
	uvec4 bdptWords[];
};

// a vertex's kind (bits 0 and 1) and its flags; its light, plus one, in bits 8 to 31 (0 for none)
#define CRT_BDPT_CAMERA 0u
#define CRT_BDPT_LIGHT 1u
#define CRT_BDPT_SURFACE 2u
// a camera path that left the scene: pbrt's endpoint vertex on the lights at infinity
#define CRT_BDPT_ESCAPED 3u
#define CRT_BDPT_KIND_MASK 3u
// the surface's BSDF was sampled by a delta lobe here (pbrt's Vertex::delta)
#define CRT_BDPT_DELTA 4u
// a light of no area, or of a single direction (pbrt's IsDeltaLight())
#define CRT_BDPT_DELTA_LIGHT 8u
// the vertex lies on a surface whose normal converts densities (pbrt's IsOnSurface())
#define CRT_BDPT_ON_SURFACE 16u
// a light at infinity, or a camera path that reached it (pbrt's IsInfiniteLight())
#define CRT_BDPT_INFINITE 32u
// a strategy can connect to it (pbrt's IsConnectible())
#define CRT_BDPT_CONNECTIBLE 64u
// a surface that emits, whether or not the light list holds it (pbrt's IsLight() for a surface)
#define CRT_BDPT_EMITS 128u

// a light subpath's PathState.misPdf while its first segment left a light at infinity (kernel 11; otherwise a point
// light's range, 0 or more)
#define CRT_BDPT_FROM_INFINITY -2.0

#if !defined(CRT_SPECTRAL) || CRT_LANES <= 4
#define CRT_BDPT_SPECTRUM_WORDS 1u
#else
#define CRT_BDPT_SPECTRUM_WORDS (CRT_LANES / 4u)
#endif
// src/rt_bdpt_schedule.h bdptVertexWords() mirrors this
#define CRT_BDPT_VERTEX_WORDS (5u + CRT_BDPT_SPECTRUM_WORDS)

struct BdptVertex {
	uint flags;
	vec3 p;
	// the geometric normal (on a surface or an area light), or the direction an escaped camera path or a light at infinity
	// leaves along (from the scene towards the light)
	vec3 n;
	float pdfFwd;
	float pdfRev;
	// a surface's hit record and the ray that found it
	HitRecord hit;
	vec3 rayOrigin;
	vec3 rayDirection;
	Spectrum beta;
};

uint bdptKind(uint flags)
{
	return flags & CRT_BDPT_KIND_MASK;
}

// the vertex's light, or CRT_NO_LIGHT
uint bdptLightIndex(uint flags)
{
	const uint stored = flags >> 8u;
	return stored == 0u ? CRT_NO_LIGHT : stored - 1u;
}

uint bdptLightBits(uint lightIndex)
{
	return lightIndex == CRT_NO_LIGHT ? 0u : (lightIndex + 1u) << 8u;
}

// the slot of the pixel a path index belongs to, in the tile; the camera subpath's vertices first
uint bdptSlotOf(uint pathIndex)
{
	return pathIndex - bdptTileStart;
}

uint bdptVertexWord(uint slot, bool light, uint vertex)
{
	const uint index = slot * bdptVerticesPerSlot + (light ? pc.rayDepth + 1u : 0u) + vertex;
	return bdptVertexBase + index * bdptStride;
}

uvec4 bdptSpectrumWord(Spectrum s, uint word)
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

void bdptSetSpectrumWord(inout Spectrum s, uint word, uvec4 bits)
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

void bdptStoreVertex(uint slot, bool light, uint vertex, BdptVertex v)
{
	const uint base = bdptVertexWord(slot, light, vertex);
	bdptWords[base] = uvec4(floatBitsToUint(v.p), v.flags);
	bdptWords[base + 1u] = uvec4(floatBitsToUint(v.n), floatBitsToUint(v.pdfFwd));
	bdptWords[base + 2u] = uvec4(floatBitsToUint(v.hit.t), v.hit.instance, floatBitsToUint(v.hit.barycentrics));
	bdptWords[base + 3u] = uvec4(v.hit.primitive, floatBitsToUint(v.rayOrigin));
	bdptWords[base + 4u] = uvec4(floatBitsToUint(v.rayDirection), floatBitsToUint(v.pdfRev));
	for (uint word = 0u; word < CRT_BDPT_SPECTRUM_WORDS; word++) {
		bdptWords[base + 5u + word] = bdptSpectrumWord(v.beta, word);
	}
}

BdptVertex bdptLoadVertex(uint slot, bool light, uint vertex)
{
	const uint base = bdptVertexWord(slot, light, vertex);
	const uvec4 w0 = bdptWords[base];
	const uvec4 w1 = bdptWords[base + 1u];
	const uvec4 w2 = bdptWords[base + 2u];
	const uvec4 w3 = bdptWords[base + 3u];
	const uvec4 w4 = bdptWords[base + 4u];
	BdptVertex v;
	v.p = uintBitsToFloat(w0.xyz);
	v.flags = w0.w;
	v.n = uintBitsToFloat(w1.xyz);
	v.pdfFwd = uintBitsToFloat(w1.w);
	v.hit.t = uintBitsToFloat(w2.x);
	v.hit.instance = w2.y;
	v.hit.barycentrics = uintBitsToFloat(w2.zw);
	v.hit.primitive = w3.x;
	v.hit.traversalCost = 0.0;
	v.rayOrigin = uintBitsToFloat(w3.yzw);
	v.rayDirection = uintBitsToFloat(w4.xyz);
	v.pdfRev = uintBitsToFloat(w4.w);
	v.beta = spectrumConst(0.0);
	for (uint word = 0u; word < CRT_BDPT_SPECTRUM_WORDS; word++) {
		bdptSetSpectrumWord(v.beta, word, bdptWords[base + 5u + word]);
	}
	return v;
}

// what MISWeight() reads of a vertex: its flags, pdfFwd and pdfRev
uvec3 bdptLoadMis(uint slot, bool light, uint vertex)
{
	const uint base = bdptVertexWord(slot, light, vertex);
	return uvec3(bdptWords[base].w, bdptWords[base + 1u].w, bdptWords[base + 4u].w);
}

// a vertex's pdfRev alone, which the next vertex's scatter sets (pbrt's prev.pdfRev = vertex.ConvertDensity(...)), and its
// flags, which a delta scatter changes
void bdptSetPdfRev(uint slot, bool light, uint vertex, float pdfRev)
{
	bdptWords[bdptVertexWord(slot, light, vertex) + 4u].w = floatBitsToUint(pdfRev);
}

void bdptSetFlags(uint slot, bool light, uint vertex, uint flags)
{
	bdptWords[bdptVertexWord(slot, light, vertex)].w = flags;
}

// the subpaths' vertex counts, after the header's word for each slot
uvec2 bdptCounts(uint slot)
{
	return bdptWords[slot].xy;
}

void bdptSetCount(uint slot, bool light, uint count)
{
	if (light) {
		bdptWords[slot].y = count;
	} else {
		bdptWords[slot].x = count;
	}
}

// pbrt's ConvertDensity(): a solid-angle density at `from` towards `to` as a density by area at `to`; unchanged towards
// a light at infinity
float bdptConvertDensity(float pdf, vec3 from, BdptVertex to)
{
	if ((to.flags & CRT_BDPT_INFINITE) != 0u) {
		return pdf;
	}
	const vec3 w = to.p - from;
	const float distanceSquared = dot(w, w);
	if (!(distanceSquared > 0.0)) {
		return 0.0;
	}
	if ((to.flags & CRT_BDPT_ON_SURFACE) != 0u) {
		pdf *= abs(dot(to.n, w)) / sqrt(distanceSquared);
	}
	return pdf / distanceSquared;
}

// the direction from `from` to vertex `to`, unit: towards a light at infinity, its own
vec3 bdptDirection(vec3 from, BdptVertex to)
{
	if ((to.flags & CRT_BDPT_INFINITE) != 0u) {
		return to.n;
	}
	return normalize(to.p - from);
}

// the one hero wavelength sample of a pixel sample, which its camera and light subpaths share (the user's choice on
// 2026-11-01): from the pixel, its sample index and the seed, so the light pass and the camera pass find the same
float bdptLambdaU(uint pixel, uint sampleIndex)
{
	uint h = pcgHash(pixel ^ pcgHash(sampleIndex ^ pcgHash(pc.seed ^ 0x6a09e667u)));
	return float(h >> 8u) * (1.0 / 16777216.0);
}

#endif
