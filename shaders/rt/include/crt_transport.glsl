// THE TRANSPORT MODE of the bidirectional integrators' surface evaluations (docs/plans/completed/pbrt-bidirectional-path-
// tracing.md; PBR 3ed 16.1.3, PBR 4ed 9.5.2). Camera paths carry radiance and light paths importance, and one kernel can
// evaluate both: lvc's connections evaluate the camera vertex in radiance mode and the cached light vertex in importance
// mode. g_transportMode says which, for:
//
//  - pbrt-v4's BxDFs and the optical interface, which take the mode themselves: this file defines CRT_SURFACE_TRANSPORT
//    and CRT_OPTIC_RADIANCE_SCALE, which crt_scatter.glsl and crt_optic_material.glsl read, so a refraction scales
//    radiance by 1 / eta^2 and importance by nothing
//  - the six original materials, three of which are not reciprocal (phong, the fuzzy metal, the Lambertian lobe of pbr,
//    docs/plans/completed/pbrt-verification-harness.md): their adjoint is f*(wo, wi) = f(wi, wo), evaluated by swapping
//    the directions, and a sample's weight is that over the density of the sample the material drew. The user chose
//    this on 2026-11-01, so every integrator converges to the path tracer's image on them
//  - pbrt's correction for shading normals in importance mode (PBR 3ed 16.1.3), which applies to every material
//
// The path tracer's kernels never include this file, so they are the code they were. Include it before crt_scatter.glsl;
// the functions below need SurfaceMaterial (ClassicMaterial or ComplexMaterial) defined, after crt_scatter.glsl.

#ifndef CRT_TRANSPORT_GLSL
#define CRT_TRANSPORT_GLSL

// crt_bxdf.glsl's, defined identically here for the basic class, which does not include it
#define CRT_TRANSPORT_RADIANCE 0u
#define CRT_TRANSPORT_IMPORTANCE 1u

uint g_transportMode = CRT_TRANSPORT_RADIANCE;

#define CRT_SURFACE_TRANSPORT g_transportMode
#define CRT_OPTIC_RADIANCE_SCALE(eta) (g_transportMode == 0u ? (eta) * (eta) : 1.0)

#endif

#if defined(SurfaceMaterial) && !defined(CRT_TRANSPORT_FUNCTIONS)
#define CRT_TRANSPORT_FUNCTIONS

// the material's own evaluation takes the mode (a BxDF of pbrt's or an optical interface); otherwise its adjoint is
// made by swapping the directions
bool surfaceTakesMode(ClassicMaterial m)
{
	return false;
}

#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
bool surfaceTakesMode(ComplexMaterial m)
{
	return m.isBxdf || m.isOptic;
}
#endif

// f(wo, wi) |cos(n, wi)| in the transport mode: wo points back along the path being built, wi the other way. Both
// directions unit and leaving the surface
Spectrum transportEval(SurfaceMaterial m, vec3 wo, vec3 normal, vec3 wi)
{
	if (g_transportMode == CRT_TRANSPORT_RADIANCE || surfaceTakesMode(m)) {
		return evalSurface(m, -wo, normal, wi);
	}
	// f*(wo, wi) = f(wi, wo): evaluated from wi, its cosine traded for wi's
	const float cosWo = abs(dot(normal, wo));
	if (!(cosWo > 0.0)) {
		return spectrumConst(0.0);
	}
	return spectrumScale(evalSurface(m, -wi, normal, wo), abs(dot(normal, wi)) / cosWo);
}

// the density with which the material, met from wo, draws wi: the same in either mode, since the material draws the same
// way and only weighs the draw differently
float transportPdf(SurfaceMaterial m, vec3 wo, vec3 normal, vec3 wi)
{
	return pdfSurface(m, -wo, normal, wi);
}

// pbrt's correction for shading normals in importance mode, |wo.ns| |wi.ng| / (|wo.ng| |wi.ns|); 1 in radiance mode
float shadingNormalCorrection(vec3 wo, vec3 wi, vec3 shadingNormal, vec3 geometricNormal)
{
	if (g_transportMode == CRT_TRANSPORT_RADIANCE) {
		return 1.0;
	}
	const float denominator = abs(dot(wo, geometricNormal)) * abs(dot(wi, shadingNormal));
	return denominator > 0.0 ? abs(dot(wo, shadingNormal)) * abs(dot(wi, geometricNormal)) / denominator : 0.0;
}

// A direction drawn as scatterSurface() draws it, weighed f cos / pdf in the transport mode: the material's own weight in
// radiance mode, for a material that takes the mode, and for a delta lobe, which is symmetric; the adjoint's evaluation
// over the draw's density for the rest. `delta` reports a delta lobe, which nothing can connect through
bool transportSample(SurfaceMaterial m, vec3 inDir, vec3 normal, bool frontFace, uint pathIndex, inout uint rng, out Spectrum weight, out vec3 outDir, out bool delta)
{
	const bool scattered = scatterSurface(m, inDir, normal, frontFace, pathIndex, rng, weight, outDir);
	delta = isSpecularMaterial(m);
#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
	// an optical interface is part delta and part lobe: the draw says which it took
	if (CRT_OPTICS && m.isOptic && !m.isBxdf) {
		delta = delta || g_opticSampledDelta;
	}
#endif
	if (!scattered || g_transportMode == CRT_TRANSPORT_RADIANCE || surfaceTakesMode(m) || delta) {
		return scattered;
	}
	const vec3 wi = normalize(outDir);
	const float pdf = transportPdf(m, -inDir, normal, wi);
	if (!(pdf > 0.0)) {
		return false;
	}
	weight = spectrumScale(transportEval(m, -inDir, normal, wi), 1.0 / pdf);
	return true;
}

#endif
