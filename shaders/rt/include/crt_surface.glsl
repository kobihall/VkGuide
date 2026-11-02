// Kernel 06 Sample Surface Scattering - the body every variant shares.
//
// The paths kernel 02 pushed onto a surface queue: rays that hit a surface that scatters. Each
// one does up to two things here, in the order pbrt-v4's PathIntegrator does them:
//
//  - NEXT-EVENT ESTIMATION. Sample a light and push a shadow ray towards it onto CRT_QUEUE_SHADOW,
//    carrying everything it would add - throughput, BSDF, cosine, emitted radiance, MIS weight, pdf
//    - so that kernel 08 only has to decide whether anything is in the way.
//  - CONTINUATION. Sample the BSDF for the path's next direction, append the path to the next
//    bounce's ray queue, and leave in its PathState the MIS RECORD that tells kernels 03 and 04
//    how much of the light that ray finds to count.
//
// THE STRATEGY is chosen by the including .comp, which defines CRT_DIRECT_LIGHTING before including
// this file. This is the one place the three strategies differ; kernels 03, 04 and 08 follow the
// record and the shadow queue whatever wrote them:
//
//   CRT_DIRECT_BSDF   0601. A light counts only when a BSDF sample happens to hit it, at full weight.
//                     Only the delta lights - point, spot, directional, which no BSDF sample can ever
//                     hit - get shadow rays, so all three strategies converge to the same image
//                     (Veach's own setup: his BSDF-sampling figure keeps the spotlight).
//   CRT_DIRECT_LIGHT  0602. Every light is reached by a light sample, at full weight. What the BSDF
//                     sample finds on a light counts only where no light sample could have gone:
//                     after a specular bounce, or on an emitter that is not in the light list.
//   CRT_DIRECT_MIS    0603 / 0604. Both, each weighted by the heuristic of exponent
//                     CRT_MIS_EXPONENT: 1 balance, 2 power (Veach 1997 ch. 9, PBR 4ed 13.4).
//
// A path either continues, or ends - absorbed, killed by Russian roulette, or out of depth - and
// leaves the zero kernel 00 pre-wrote in its radiance slot. Russian roulette kills low-throughput
// paths after a few bounces with probability 1 - max(throughput) and reweights the survivors,
// which is unbiased and makes late bounces cheap under compaction. It judges only the
// continuation: the shadow ray from this vertex is already on its way.
//
// The surface itself comes from crt_interaction.glsl, rebuilt from the thin hit record, and the
// shadow ray leaves from its point: kernel 08 reads the origin from the ShadowRay, not from hits[].
//
// The auxiliary outputs (docs/plans/completed/pbrt-aovs-and-image-output.md) are written here too: the first
// visible surface of a camera path goes into PixelState with the BSDF's directional albedo there, and a shadow ray
// carries its light's group to kernel 08. Both are behind specialization constants, and the ambient occlusion view
// casts its rays from here. Requires crt_common.glsl, crt_spectrum.glsl, crt_interaction.glsl, crt_random.glsl,
// crt_samples.glsl, equirect.glsl, crt_scatter.glsl, crt_light.glsl and crt_debug.glsl.

#include "crt_aov.glsl"

#define CRT_DIRECT_BSDF 0
#define CRT_DIRECT_LIGHT 1
#define CRT_DIRECT_MIS 2

#ifndef CRT_DIRECT_LIGHTING
#error "define CRT_DIRECT_LIGHTING before including crt_surface.glsl"
#endif

#ifndef CRT_MATERIAL_CLASS
#error "a kernel 06 variant includes include/crt_material_class.glsl, and is built once per material class"
#endif

#if CRT_DIRECT_LIGHTING == CRT_DIRECT_MIS && !defined(CRT_MIS_EXPONENT)
#error "an MIS variant of kernel 06 must define CRT_MIS_EXPONENT"
#endif

layout (local_size_x = CRT_WORKGROUP) in;

// The material the body works with: the six original types' record in the basic build, and in the complex build one
// that can also be pbrt-v4's glass or conductor (crt_scatter.glsl ComplexMaterial), whose overloads of the material
// functions the same calls below resolve to
#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
#define SurfaceMaterial ComplexMaterial
#else
#define SurfaceMaterial ClassicMaterial
#endif

#include "crt_spawn.glsl"

// The next-event estimate at this vertex: sample one light, evaluate the BSDF towards it, and write
// shadowRays[entry] if it would add anything. `si` is the vertex, `normal` the shading normal the
// material takes. Draws its random numbers after the BSDF sample's, so a strategy that samples
// nothing here leaves the path's random stream exactly as it was
bool nextEvent(SurfaceMaterial material, vec3 inDir, SurfaceInteraction si, vec3 normal, PathState path, Spectrum throughput, uint pathIndex, inout uint rng, uint entry)
{
	const vec3 position = si.p;
	vec3 direction;
	float distance;
	Spectrum emitted;
	float lightPdf;
	vec3 lightNormal;
	float weight = 1.0;

#if CRT_DIRECT_LIGHTING == CRT_DIRECT_BSDF
	// the delta lights alone, by their own power
	if (lightHeader.deltaCount == 0u) {
		return false;
	}
	const uint index = sampleDeltaLightIndex(sampleDimension(pathIndex, rng, CRT_SAMPLE_DIRECT_LIGHT));
	if (!sampleDeltaLight(lights[index], position, direction, distance, emitted, lightNormal)) {
		return false;
	}
	lightPdf = deltaLightPmf(index);
#else
	if (lightHeader.count == 0u) {
		return false;
	}
	const uint index = sampleLightIndex(sampleDimension(pathIndex, rng, CRT_SAMPLE_DIRECT_LIGHT));
	const vec2 u = sampleDimension2D(pathIndex, rng, CRT_SAMPLE_DIRECT_POINT);
	// MIS lets the environment's light samples skip what a BSDF sample finds well enough anyway
	// (pbrt-v4's compensated distribution); on its own, light sampling has to cover everything
	const uint environmentTable = CRT_DIRECT_LIGHTING == CRT_DIRECT_MIS ? 1u : 0u;
	float directionPdf;
	if (!sampleLight(index, position, u, environmentTable, direction, distance, emitted, directionPdf, lightNormal)) {
		return false;
	}
	lightPdf = lightPmf(index) * directionPdf;
#if CRT_DIRECT_LIGHTING == CRT_DIRECT_MIS
	// a delta light is out of the BSDF sample's reach, so light sampling keeps its full weight
	if (!isDeltaLight(lights[index].kind)) {
		weight = misWeight(lightPdf, pdfSurface(material, inDir, normal, direction), CRT_MIS_EXPONENT);
	}
#endif
#endif

	if (!(lightPdf > 0.0)) {
		return false;
	}
	// the MIS weights view shows the choice between techniques, and a delta light offers none: it is
	// left out, as Veach's figure 9.8(d) leaves out his spotlight
	if (pc.debugView == CRT_DEBUG_MIS_WEIGHTS && isDeltaLight(lights[index].kind)) {
		return false;
	}
	Spectrum contribution = spectrumScale(spectrumMul(spectrumMul(throughput, evalSurface(material, inDir, normal, direction)), emitted), weight / lightPdf);
	if (!(spectrumMaxValue(contribution) > 0.0)) {
		return false;
	}
	if (pc.debugView == CRT_DEBUG_MIS_WEIGHTS) {
		// what light sampling found, in green. A spectral build hands kernel 08 the green in its first lane
#ifndef CRT_SPECTRAL
		contribution = vec3(0.0, luminance(contribution), 0.0);
#else
		const float green = luminance(spectrumToFilm(contribution));
		contribution = spectrumConst(0.0);
		spectrumSetLane(contribution, 0u, green);
#endif
	}

	ShadowRay ray;
	if (!spawnShadowRay(si, direction, distance, lightNormal, ray)) {
		return false;
	}
	ray.contribution = contribution;
	ray.radianceSlot = path.radianceSlot;
#ifdef CRT_SPECTRAL
	ray.lambdaU = path.lambdaU;
	ray.laneMask = path.laneMask;
#endif
	if (CRT_LIGHT_GROUPS) {
		ray.mediumAndGroup |= (lights[index].group & 0xFFu) << 24u;
	}
	shadowRays[entry] = ray;
	return true;
}

#include "crt_surface_views.glsl"

void main()
{
	const uint nextQueue = rayQueueFor(pc.bounce + 1u);
	const uint index = gl_GlobalInvocationID.x;
	// no early return: the queueAppend() calls below have barriers every invocation must reach
	const bool isActive = index < headers[CRT_SURFACE_QUEUE].rayCount;

	bool survives = false;
	bool shadow = false;
	uint pathIndex = 0u;
	uint entry = 0u;

	if (isActive) {
		// the queue holds the RAY QUEUE POSITION, which is also the index of the path's hit record
		// and of its shadow ray
		entry = queues[queueSlot(CRT_SURFACE_QUEUE, index)];
		pathIndex = queues[queueSlot(rayQueueFor(pc.bounce), entry)];
		PathState path = paths[pathIndex];
		const HitRecord hit = hits[entry];
		const vec3 inDir = normalize(path.direction);
#ifndef CRT_SPECTRAL
		// not const: an optical body's glass absorbs along the segment just traveled (below)
		Spectrum pathThroughput = path.throughput;
#else
		spectralBegin(path.lambdaU);
		// not const: a dispersive glass terminates the secondary lanes below
		Spectrum pathThroughput = pathSpectra[pathIndex];
#endif
		// the path's origin has not moved since kernel 02 traced from it
		const SurfaceInteraction si = surfaceInteraction(hit, path.origin, inDir);
		// where the texture library's lookups are, and how wide the ray cone is there (crt_texture.glsl)
		const TextureSite site = surfaceTextureSite(si, hit, inDir, path.coneWidth, path.coneSpread);
#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
		// a mix is one of its children for this hit, here and everywhere the hit's material is read below
		const uint materialIndex = resolveMixMaterial(si.material, si.p, -inDir, site);
#else
		const uint materialIndex = si.material;
#endif
		bool frontFace;
		const vec3 normal = materialShadingNormal(classicMaterial(materialIndex), hit, si, inDir, frontFace, materialIndex, site);

		vec3 debugValue;
		if (pc.debugView == CRT_DEBUG_AMBIENT_OCCLUSION) {
			shadow = ambientOcclusionRay(si, normal, pathIndex, path, entry);
		} else if (debugTerminalValue(normal, si.frontFace ? si.n : -si.n, hit.traversalCost, inDir, true, debugValue)) {
			// the debug views replace shading: each writes its value as the path's contribution
			// and ends the path, so they accumulate (and average under jitter) like radiance
			radiance[path.radianceSlot] = vec4(debugValue, 1.0);
		} else {
#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
			ComplexMaterial material = resolveComplexMaterial(materialIndex, si, site);
			material.frontFace = frontFace;
#else
			const ClassicMaterial material = resolveMaterial(materialIndex, site);
#endif
			if (CRT_AOV) {
				aovCapture(path.radianceSlot, hit, si, material, inDir, normal, frontFace);
			}
			uint rng = path.rngState;
#if defined(CRT_SPECTRAL) && CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
			// DISPERSION. A glass whose index varies with wavelength sends each wavelength its own way, so the path keeps
			// one lane (crt_spectrum.glsl terminateSecondary(): hero keeps its first, the bands one chosen at random) and
			// the glass reads its index at that lane's wavelength, before anything at this vertex uses the throughput
			if (material.isBxdf && (material.bxdfFlags & CRT_BXDF_FLAG_DISPERSIVE) != 0u) {
				const float pick = spectralBanded() ? randomFloat(rng) : 0.0;
				const uint lane = terminateSecondary(pathThroughput, path.laneMask, pick);
				material.bxdf.eta = scalarFromRef(materialSpectrumRef(materialIndex, CRT_SLOT_AUX0), lane);
			}
#endif
#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
			// AN OPTICAL BODY'S SURFACE (crt_optic_material.glsl). The segment since the last vertex ran through the incident
			// medium, whose absorption the throughput carries first (Beer-Lambert), for the light sample and the
			// continuation alike. A refraction between dispersive media keeps one lane, as a dispersive glass does, and
			// every index of the vertex is read at it
			if (CRT_OPTICS && material.isOptic) {
				pathThroughput = spectrumMul(pathThroughput, opticAbsorption(material.optic));
#ifdef CRT_SPECTRAL
				if (opticDisperses(material.optic)) {
					const float pick = spectralBanded() ? randomFloat(rng) : 0.0;
					material.optic.lane = terminateSecondary(pathThroughput, path.laneMask, pick);
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

			Spectrum attenuation;
			vec3 outDir;
			bool alive = scatterSurface(material, inDir, normal, frontFace, pathIndex, rng, attenuation, outDir);
			Spectrum throughput = spectrumMul(pathThroughput, attenuation);
			const uint nextBounce = path.bounce + 1u;

			// the MIS weights view is the direct light at the first vertex and nothing further
			const bool pastFirstVertex = pc.debugView == CRT_DEBUG_MIS_WEIGHTS && path.bounce > 0u;

			// nowhere further to go: the next ray would be past the ray depth, so it is never traced
			if (nextBounce >= pc.rayDepth || pastFirstVertex) {
				alive = false;
			}

			if (alive && (pc.flags & CRT_FLAG_ROULETTE) != 0u && nextBounce > pc.minBouncesBeforeRoulette) {
				const float survival = clamp(spectrumMaxValue(throughput), 0.05, 1.0);
				if (sampleDimension(pathIndex, rng, CRT_SAMPLE_INDIRECT_ROULETTE) >= survival) {
					alive = false;
				} else {
					throughput = spectrumDivide(throughput, survival);
				}
			}

			// a light sample needs the shadow ray it would send to be within the path's depth, like
			// the continuation, and a BSDF it could land in
			const bool specular = isSpecularMaterial(material);
			if (nextBounce < pc.rayDepth && !specular && !pastFirstVertex) {
				shadow = nextEvent(material, inDir, si, normal, path, pathThroughput, pathIndex, rng, entry);
			}

			if (alive) {
				// the continuation leaves through the side outDir points to: from past the hit
				// point's error bound, or from the hit point itself without error-bounded offsets
				path.origin = (pc.flags & CRT_FLAG_ERROR_BOUNDS) != 0u ? offsetRayOrigin(si.p, si.pError, si.n, outDir) : si.p;
				path.direction = outDir;
				path.rngState = rng;
#ifndef CRT_SPECTRAL
				path.throughput = throughput;
#else
				pathSpectra[pathIndex] = throughput;
#endif
				path.bounce = nextBounce;
				// the ray cone the continuation carries: a delta bounce widens it by the curvature alone, a glossy or diffuse
				// one by its lobe too
				if (CRT_RAY_CONES) {
					const vec2 cone = continuedRayCone(si, path.coneWidth, path.coneSpread, specular ? 0.0 : surfaceConeRoughness(material));
					path.coneWidth = cone.x;
					path.coneSpread = cone.y;
				}
				// THE MIS RECORD for the light the new ray finds
				// a surface that is part delta and part glossy (an optical interface's ABg lobe) says which this continuation was
#if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX
				const bool continuationDelta = specular || (CRT_OPTICS && material.isOptic && !material.isBxdf && g_opticSampledDelta);
#else
				const bool continuationDelta = specular;
#endif
#if CRT_DIRECT_LIGHTING == CRT_DIRECT_BSDF
				path.misPdf = CRT_MIS_FULL;
#elif CRT_DIRECT_LIGHTING == CRT_DIRECT_LIGHT
				path.misPdf = continuationDelta ? CRT_MIS_FULL : 0.0;
#else
				path.misPdf = continuationDelta ? CRT_MIS_FULL : pdfSurface(material, inDir, normal, normalize(outDir));
				path.misExponent = CRT_MIS_EXPONENT;
#endif
				paths[pathIndex] = path;
				survives = true;
			} else if (pc.debugView == CRT_DEBUG_BOUNCE_HEAT) {
				radiance[path.radianceSlot] = vec4(bounceHeat(nextBounce), 1.0);
			}
			// otherwise the pre-written zero stands: absorbed, killed, or out of depth
		}
	}

	queueAppend(nextQueue, survives, pathIndex);
	queueAppend(CRT_QUEUE_SHADOW, shadow, entry);
}
