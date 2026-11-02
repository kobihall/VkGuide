// A LIGHT PATH AT A SURFACE: what kernels 12 (connect to the camera) and 13 (scatter light paths) both start from, the
// surface queue position's path rebuilt as kernel 06 rebuilds a camera path's (docs/plans/completed/pbrt-bidirectional-
// path-tracing.md). Both kernels must see the same vertex, so everything here is a function of the path and the hit,
// and the lane a dispersive refraction keeps is picked from a hash of the path's stream, which neither kernel advances.
// Requires crt_common.glsl, crt_spectrum.glsl, crt_material_class.glsl, crt_interaction.glsl, crt_random.glsl,
// crt_samples.glsl, crt_transport.glsl, crt_scatter.glsl and crt_light.glsl.

#ifndef CRT_LIGHT_PATH_GLSL
#define CRT_LIGHT_PATH_GLSL

#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
#define SurfaceMaterial ComplexMaterial
#else
#define SurfaceMaterial ClassicMaterial
#endif

// a light path's connection to the camera draws its lens point from its own copy of the path's stream, without a
// sample record (crt_samples.glsl CRT_SAMPLE_CONNECT_LENS), so the BSDF sample kernel 13 draws is not the same numbers
#define CRT_CONNECT_STREAM_SALT 0x27d4eb2fu
// and the lane a dispersive refraction keeps, from another
#define CRT_DISPERSION_STREAM_SALT 0x165667b1u

struct LightVertex {
	uint entry;
	uint pathIndex;
	PathState path;
	HitRecord hit;
	// unit, the way the path travelled to the surface
	vec3 inDir;
	SurfaceInteraction si;
	uint materialIndex;
	vec3 normal;
	bool frontFace;
	// what reaches the surface: the path's throughput after the range window of the point light it left, an optical
	// body's absorption and a dispersive refraction's one lane
	Spectrum throughput;
	uint laneMask;
};

// The light path at position `index` of this material class's surface queue, and its material resolved at the hit
LightVertex lightVertexAt(uint index, out SurfaceMaterial material)
{
	LightVertex v;
	v.entry = queues[queueSlot(CRT_SURFACE_QUEUE, index)];
	v.pathIndex = queues[queueSlot(rayQueueFor(pc.bounce), v.entry)];
	v.path = paths[v.pathIndex];
	v.hit = hits[v.entry];
	v.inDir = normalize(v.path.direction);
#ifndef CRT_SPECTRAL
	v.throughput = v.path.throughput;
	v.laneMask = 0u;
#else
	spectralBegin(v.path.lambdaU);
	v.throughput = pathSpectra[v.pathIndex];
	v.laneMask = v.path.laneMask;
#endif
	v.si = surfaceInteraction(v.hit, v.path.origin, v.inDir);
	const TextureSite site = surfaceTextureSite(v.si, v.hit, v.inDir, v.path.coneWidth, v.path.coneSpread);
#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
	v.materialIndex = resolveMixMaterial(v.si.material, v.si.p, -v.inDir, site);
#else
	v.materialIndex = v.si.material;
#endif
	v.normal = materialShadingNormal(classicMaterial(v.materialIndex), v.hit, v.si, v.inDir, v.frontFace, v.materialIndex, site);
#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
	material = resolveComplexMaterial(v.materialIndex, v.si, site);
	material.frontFace = v.frontFace;
#else
	material = resolveMaterial(v.materialIndex, site);
#endif

	// the range window of the point or spot light the path left (kernel 11 keeps its range in misPdf), which only its
	// first segment falls off by: the direction is unit, so the hit's t is the distance
	if (v.path.bounce == 0u && v.path.misPdf > 0.0) {
		v.throughput = spectrumScale(v.throughput, rangeWindow(v.hit.t, v.path.misPdf));
	}
#if defined(CRT_SPECTRAL) && CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
	uint pickRng = pcgHash(v.path.rngState ^ CRT_DISPERSION_STREAM_SALT);
	if (material.isBxdf && (material.bxdfFlags & CRT_BXDF_FLAG_DISPERSIVE) != 0u) {
		const float pick = spectralBanded() ? randomFloat(pickRng) : 0.0;
		const uint lane = terminateSecondary(v.throughput, v.laneMask, pick);
		material.bxdf.eta = scalarFromRef(materialSpectrumRef(v.materialIndex, CRT_SLOT_AUX0), lane);
	}
#endif
#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
	// an optical body's glass absorbs along the segment just travelled, and a refraction between dispersive media keeps
	// one lane, as at a camera path's vertex (crt_surface.glsl)
	if (CRT_OPTICS && material.isOptic) {
		v.throughput = spectrumMul(v.throughput, opticAbsorption(material.optic));
#ifdef CRT_SPECTRAL
		if (opticDisperses(material.optic)) {
			const float pick = spectralBanded() ? randomFloat(pickRng) : 0.0;
			material.optic.lane = terminateSecondary(v.throughput, v.laneMask, pick);
			if (material.isBxdf) {
				const uint lane = material.optic.lane;
				const float nPlus = opticIndex(material.optic.fromPlus ? material.optic.incident : material.optic.other, lane);
				const float nMinus = opticIndex(material.optic.fromPlus ? material.optic.other : material.optic.incident, lane);
				material.bxdf.eta = nMinus / nPlus;
			}
		}
#endif
	}
#endif
	return v;
}

#endif
