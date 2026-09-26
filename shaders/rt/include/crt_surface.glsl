// Kernel 06 Sample Surface Scattering - the body every variant shares.
//
// The paths kernel 02 pushed onto CRT_QUEUE_SURFACE: rays that hit a surface that scatters. Each
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
// Requires crt_common.glsl, crt_random.glsl, equirect.glsl, crt_scatter.glsl, crt_light.glsl and
// crt_debug.glsl.

#define CRT_DIRECT_BSDF 0
#define CRT_DIRECT_LIGHT 1
#define CRT_DIRECT_MIS 2

#ifndef CRT_DIRECT_LIGHTING
#error "define CRT_DIRECT_LIGHTING before including crt_surface.glsl"
#endif

#if CRT_DIRECT_LIGHTING == CRT_DIRECT_MIS && !defined(CRT_MIS_EXPONENT)
#error "an MIS variant of kernel 06 must define CRT_MIS_EXPONENT"
#endif

layout (local_size_x = CRT_WORKGROUP) in;

// The next-event estimate at this vertex: sample one light, evaluate the BSDF towards it, and write
// shadowRays[entry] if it would add anything. Draws its random numbers after the BSDF sample's, so
// a strategy that samples nothing here leaves the path's random stream exactly as it was
bool nextEvent(GpuMaterial material, vec3 inDir, HitRecord hit, PathState path, inout uint rng, uint entry)
{
	vec3 direction;
	float distance;
	vec3 emitted;
	float lightPdf;
	float weight = 1.0;

#if CRT_DIRECT_LIGHTING == CRT_DIRECT_BSDF
	// the delta lights alone, by their own power
	if (lightHeader.deltaCount == 0u) {
		return false;
	}
	const uint index = sampleDeltaLightIndex(randomFloat(rng));
	if (!sampleDeltaLight(lights[index], hit.position, direction, distance, emitted)) {
		return false;
	}
	lightPdf = deltaLightPmf(index);
#else
	if (lightHeader.count == 0u) {
		return false;
	}
	const uint index = sampleLightIndex(randomFloat(rng));
	const vec2 u = vec2(randomFloat(rng), randomFloat(rng));
	// MIS lets the environment's light samples skip what a BSDF sample finds well enough anyway
	// (pbrt-v4's compensated distribution); on its own, light sampling has to cover everything
	const uint environmentTable = CRT_DIRECT_LIGHTING == CRT_DIRECT_MIS ? 1u : 0u;
	float directionPdf;
	if (!sampleLight(index, hit.position, u, environmentTable, direction, distance, emitted, directionPdf)) {
		return false;
	}
	lightPdf = lightPmf(index) * directionPdf;
#if CRT_DIRECT_LIGHTING == CRT_DIRECT_MIS
	// a delta light is out of the BSDF sample's reach, so light sampling keeps its full weight
	if (!isDeltaLight(lights[index].kind)) {
		weight = misWeight(lightPdf, pdfSurface(material, inDir, hit.normal, direction), CRT_MIS_EXPONENT);
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
	vec3 contribution = path.throughput * evalSurface(material, inDir, hit.normal, direction) * emitted * (weight / lightPdf);
	if (!(max(contribution.r, max(contribution.g, contribution.b)) > 0.0)) {
		return false;
	}
	if (pc.debugView == CRT_DEBUG_MIS_WEIGHTS) {
		// what light sampling found, in green
		contribution = vec3(0.0, luminance(contribution), 0.0);
	}

	ShadowRay ray;
	ray.direction = direction;
	// short of the light itself, so the shadow ray cannot be stopped by the surface it is aimed at
	ray.tMax = distance >= CRT_INFINITY ? CRT_INFINITY : distance * (1.0 - 1e-4);
	ray.contribution = contribution;
	ray.radianceSlot = path.radianceSlot;
	shadowRays[entry] = ray;
	return true;
}

void main()
{
	const uint nextQueue = rayQueueFor(pc.bounce + 1u);
	const uint index = gl_GlobalInvocationID.x;
	// no early return: the queueAppend() calls below have barriers every invocation must reach
	const bool isActive = index < headers[CRT_QUEUE_SURFACE].rayCount;

	bool survives = false;
	bool shadow = false;
	uint pathIndex = 0u;
	uint entry = 0u;

	if (isActive) {
		// the queue holds the RAY QUEUE POSITION, which is also the index of the path's hit record
		// and of its shadow ray
		entry = queues[queueSlot(CRT_QUEUE_SURFACE, index)];
		pathIndex = queues[queueSlot(rayQueueFor(pc.bounce), entry)];
		PathState path = paths[pathIndex];
		const HitRecord hit = hits[entry];
		const vec3 inDir = normalize(path.direction);

		vec3 debugValue;
		if (debugTerminalValue(hit, inDir, true, debugValue)) {
			// the debug views replace shading: each writes its value as the path's contribution
			// and ends the path, so they accumulate (and average under jitter) like radiance
			radiance[path.radianceSlot] = vec4(debugValue, 1.0);
		} else {
			const GpuMaterial material = resolveMaterial(hit.materialAndFace >> 1u, hit.uv);
			const bool frontFace = (hit.materialAndFace & 1u) != 0u;
			uint rng = path.rngState;

			vec3 attenuation;
			vec3 outDir;
			bool alive = scatterSurface(material, inDir, hit.normal, frontFace, rng, attenuation, outDir);
			vec3 throughput = path.throughput * attenuation;
			const uint nextBounce = path.bounce + 1u;

			// the MIS weights view is the direct light at the first vertex and nothing further
			const bool pastFirstVertex = pc.debugView == CRT_DEBUG_MIS_WEIGHTS && path.bounce > 0u;

			// nowhere further to go: the next ray would be past the ray depth, so it is never traced
			if (nextBounce >= pc.rayDepth || pastFirstVertex) {
				alive = false;
			}

			if (alive && (pc.flags & CRT_FLAG_ROULETTE) != 0u && nextBounce > pc.minBouncesBeforeRoulette) {
				const float survival = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 1.0);
				if (randomFloat(rng) >= survival) {
					alive = false;
				} else {
					throughput /= survival;
				}
			}

			// a light sample needs the shadow ray it would send to be within the path's depth, like
			// the continuation, and a BSDF it could land in
			const bool specular = isSpecularMaterial(material);
			if (nextBounce < pc.rayDepth && !specular && !pastFirstVertex) {
				shadow = nextEvent(material, inDir, hit, path, rng, entry);
			}

			if (alive) {
				path.origin = hit.position;
				path.direction = outDir;
				path.rngState = rng;
				path.throughput = throughput;
				path.bounce = nextBounce;
				// THE MIS RECORD for the light the new ray finds
#if CRT_DIRECT_LIGHTING == CRT_DIRECT_BSDF
				path.misPdf = CRT_MIS_FULL;
#elif CRT_DIRECT_LIGHTING == CRT_DIRECT_LIGHT
				path.misPdf = specular ? CRT_MIS_FULL : 0.0;
#else
				path.misPdf = specular ? CRT_MIS_FULL : pdfSurface(material, inDir, hit.normal, normalize(outDir));
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
