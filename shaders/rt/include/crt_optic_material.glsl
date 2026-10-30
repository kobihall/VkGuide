// THE OPTICAL INTERFACE (docs/plans/completed/physoptics-optical-materials.md): the material of an optical body's surface, in
// kernel 06's complex build. It has no index of its own: the two indices are the surface's media, which its record
// names (crt_optic.glsl), and the side the ray arrived from says which is which. Its parameter record
// (src/rt_material_record.h CrtOpticMaterial) holds the finish and the surface scatter; the coating is the surface's,
// in the instance's coating table.
//
// POLISHED. A delta reflection with probability R / (R + T) and a delta refraction (Snell's law) otherwise, weighted
// R / p and T / (1 - p): R and T are exact Fresnel between the two media when uncoated, the coating's table (or its
// transfer matrix under Exact coatings) when coated, per channel in exact RGB and per lane in a spectral build. What a
// coating absorbs, 1 - R - T, is lost there. A refraction in radiance transport is divided by (n_t / n_i)^2, as pbrt's
// dielectric is (PBR 4ed 9.5.2). The choice weighs R and T as a lobe choice does (spectrumSelectionWeight()).
//
// SURFACE SCATTER. A polished interface with a total integrated scatter TIS sends that share of what it reflects or
// transmits into an ABg lobe about the specular direction (Stover, Optical scattering, ch. 4; FRED's and OpticStudio's
// model): BSDF = A / (B + |beta - beta0|^g) in projected directions beta, with A normalizing it over |beta - beta0| < 2.
// Only directions with |beta| < 1 exist, so the lobe is renormalized by the share of it inside the unit disk for its
// beta0 (tabulated on the CPU), and sampled by its inverse CDF in r with rejection outside the disk, up to
// CRT_ABG_TRIES draws. Evaluated for a light sample too, so a scattering interface is not specular for next-event
// estimation; which kind of sample a continuation was is g_opticSampledDelta, which kernel 06 reads for the MIS record.
//
// GROUND. pbrt's rough dielectric at roughness 1 between the same media (crt_bxdf.glsl), which ComplexMaterial evaluates
// as any BxDF; the coating does not apply.
//
// ABSORPTION. A ray reaching a surface of a body from its minus side traveled the segment since its last vertex inside
// the minus medium (from the plus side, in the plus medium): kernel 06 multiplies the path's throughput by
// exp(-alpha t) for it before anything else at the vertex (Beer-Lambert, exact for the homogeneous glass of an optic).
// Exact RGB reads alpha at 630, 532 and 465 nm, a spectral build at each lane.
//
// THE MEDIA TABLE of a body in binding 55, two vec4 per medium: (n at the design wavelength, alpha at 630, 532 and
// 465 nm per meter), then (the index's SceneSpectra entry, the absorption's, flags, 0).
//
// Requires crt_common.glsl, crt_spectrum.glsl, crt_samples.glsl, crt_interaction.glsl, crt_fresnel.glsl and
// crt_coating.glsl.

#ifndef CRT_OPTIC_MATERIAL_GLSL
#define CRT_OPTIC_MATERIAL_GLSL

// src/rt_material_record.h CRT_OPTIC_FINISH_*
#define CRT_OPTIC_FINISH_POLISHED 0u
#define CRT_OPTIC_FINISH_GROUND 1u
// a medium's flags: its index varies with wavelength
#define CRT_MEDIUM_DISPERSIVE 1u
// src/rt_gpu_types.h
#define CRT_FLAG_EXACT_COATINGS 64u

struct OpticMedium {
	float nDesign;
	vec3 alphaRgb;
	uint indexRef;
	uint absorptionRef;
	uint flags;
};

struct OpticInterface {
	// the ray arrived from the surface's plus side
	bool fromPlus;
	OpticMedium incident;
	OpticMedium other;
	// the coating table, CRT_NO_OPTIC for an uncoated surface; and its header's flags
	uint coating;
	uint coatingFlags;
	uint finish;
	float tis;
	float abgB;
	float abgG;
	// the length of the segment the ray just traveled in the incident medium
	float distance;
	// a spectral path that kept one lane at a dispersive refraction reads every index there; CRT_NO_OPTIC otherwise
	uint lane;
	// the material's record in binding 26, where a scattering interface's ABg lobe follows its first four vec4
	uint record;
};

// whether the last continuation an optical interface sampled was a delta one: what the MIS record needs from a surface
// that is part delta and part glossy
bool g_opticSampledDelta = false;

OpticMedium opticMedium(uint mediaBase, uint medium)
{
	const vec4 a = opticData[mediaBase + 2u * medium];
	const uvec4 b = floatBitsToUint(opticData[mediaBase + 2u * medium + 1u]);
	OpticMedium m;
	m.nDesign = a.x;
	m.alphaRgb = a.yzw;
	m.indexRef = b.x;
	m.absorptionRef = b.y;
	m.flags = b.z;
	return m;
}

OpticInterface resolveOpticInterface(uint materialIndex, SurfaceInteraction si)
{
	OpticInterface o;
	o.record = materials[materialIndex].paramOffset;
	const vec4 record = materialParams[o.record];
	o.finish = floatBitsToUint(record.x);
	o.tis = record.y;
	o.abgB = record.z;
	o.abgG = record.w;
	o.fromPlus = si.frontFace;
	o.distance = si.t;
	o.lane = CRT_NO_OPTIC;
	if (si.opticSurface == CRT_NO_OPTIC) {
		// on anything but an optic's surface: air on both sides, so light goes straight through
		o.incident.nDesign = 1.0;
		o.incident.alphaRgb = vec3(0.0);
		o.incident.indexRef = 0u;
		o.incident.absorptionRef = 0u;
		o.incident.flags = 0u;
		o.other = o.incident;
		o.coating = CRT_NO_OPTIC;
		o.coatingFlags = 0u;
		return o;
	}
	const OpticSurfaceRecord surface = opticSurfaceRecord(si.opticSurface);
	const uint plus = opticMediumPlus(surface);
	const uint minus = opticMediumMinus(surface);
	o.incident = opticMedium(si.opticMedia, o.fromPlus ? plus : minus);
	o.other = opticMedium(si.opticMedia, o.fromPlus ? minus : plus);
	o.coating = si.opticCoating;
	o.coatingFlags = o.coating != CRT_NO_OPTIC ? coatingHeader(o.coating).y : 0u;
	return o;
}

// whether a spectral path must keep one lane here: something can refract, between media whose index varies
bool opticDisperses(OpticInterface o)
{
	return ((o.incident.flags | o.other.flags) & CRT_MEDIUM_DISPERSIVE) != 0u && (o.coatingFlags & CRT_COATING_OPAQUE) == 0u;
}

// a medium's index for a lane (a spectral build's lane, or the one a dispersive path kept); exact RGB's at the design
// wavelength
float opticIndex(OpticMedium m, uint lane)
{
#ifndef CRT_SPECTRAL
	return m.nDesign;
#else
	return scalarFromRef(sceneSpectrumRef(m.indexRef), lane);
#endif
}

// what the segment just traveled through the incident medium leaves of the throughput
Spectrum opticAbsorption(OpticInterface o)
{
#ifndef CRT_SPECTRAL
	return exp(-o.incident.alphaRgb * o.distance);
#else
	Spectrum s = spectrumConst(1.0);
	for (uint i = 0u; i < CRT_LANE_END; i++) {
		const float alpha = scalarFromRef(sceneSpectrumRef(o.incident.absorptionRef), i);
		spectrumSetLane(s, i, exp(-max(alpha, 0.0) * o.distance));
	}
	return s;
#endif
}

// the lane a channel or lane reads its indices at: the kept lane after a dispersive termination
uint opticLaneFor(OpticInterface o, uint i)
{
	return o.lane != CRT_NO_OPTIC ? o.lane : i;
}

// R and T for the incident cosine, per channel (exact RGB) or lane
void opticPowers(OpticInterface o, float cosI, out Spectrum R, out Spectrum T)
{
	R = spectrumConst(0.0);
	T = spectrumConst(0.0);
	const bool exact = (pc.flags & CRT_FLAG_EXACT_COATINGS) != 0u && (o.coatingFlags & CRT_COATING_LAYERS) != 0u;
	for (uint i = 0u; i < CRT_LANE_END; i++) {
		const uint lane = opticLaneFor(o, i);
		const float nI = opticIndex(o.incident, lane);
		const float nO = opticIndex(o.other, lane);
		float r;
		float t;
		if (o.coating == CRT_NO_OPTIC) {
			r = frDielectric(cosI, nO / nI);
			t = 1.0 - r;
		} else {
			const uint direction = o.fromPlus ? 0u : 1u;
#ifndef CRT_SPECTRAL
			const float lambda = i == 0u ? 630.0 : (i == 1u ? 532.0 : 465.0);
			const float column = float(i);
#else
			const float lambda = g_lambda[lane];
			const float column = coatingColumn(o.coating, lambda);
#endif
			vec4 powers;
			if (exact) {
				powers = coatingExact(o.coating, direction, i, lane, lambda, cosI, vec2(nI, 0.0), vec2(nO, 0.0));
			} else {
				powers = coatingLookup(o.coating, direction, column, cosI, nI / nO);
			}
			r = 0.5 * (powers.x + powers.y);
			t = 0.5 * (powers.z + powers.w);
		}
		spectrumSetLane(R, i, r);
		spectrumSetLane(T, i, t);
	}
}

// whether a polished interface scatters a share into its ABg lobe
bool opticScatters(OpticInterface o)
{
	return o.finish == CRT_OPTIC_FINISH_POLISHED && o.tis > 0.0;
}

#define CRT_ABG_TRIES 16

// the share of the lobe about beta0 inside the unit disk, from the table after the radii
float opticAbgVisible(OpticInterface o, vec2 center)
{
	const float x = clamp(length(center), 0.0, 1.0) * 15.0;
	const uint i = min(uint(x), 14u);
	const float f = x - float(i);
	const uint base = o.record + 13u;
	const float a = materialParams[base + i / 4u][i % 4u];
	const float b = materialParams[base + (i + 1u) / 4u][(i + 1u) % 4u];
	return max(mix(a, b, f), 1e-6);
}

// the lobe's density in projected directions at beta, renormalized over the directions that exist
float opticAbgDensity(OpticInterface o, vec2 beta, vec2 center)
{
	return materialParams[o.record + 4u].x / (o.abgB + pow(length(beta - center), o.abgG)) / opticAbgVisible(o, center);
}

// a radius from the lobe's inverse CDF table
float opticAbgRadius(OpticInterface o, float u)
{
	const float x = clamp(u, 0.0, 1.0) * 31.0;
	const uint i = min(uint(x), 30u);
	const float f = x - float(i);
	const uint base = o.record + 5u;
	const float a = materialParams[base + i / 4u][i % 4u];
	const float b = materialParams[base + (i + 1u) / 4u][(i + 1u) % 4u];
	return mix(a, b, f);
}

// f cos and the density of a direction in the lobe, for a light sample: on the reflected side about the mirror
// direction, on the transmitted side about the refracted one, each times its share of R or T and of TIS
void opticLobe(OpticInterface o, vec3 inDir, vec3 n, vec3 wi, out Spectrum value, out float pdf)
{
	value = spectrumConst(0.0);
	pdf = 0.0;
	if (!opticScatters(o)) {
		return;
	}
	const vec3 facing = o.fromPlus ? n : -n;
	vec3 t1, t2;
	interactionFrame(facing, t1, t2);
	const float cosI = clamp(-dot(inDir, facing), 0.0, 1.0);
	Spectrum R;
	Spectrum T;
	opticPowers(o, cosI, R, T);
	const float wr = spectrumSelectionWeight(R);
	const float wt = spectrumSelectionWeight(T);
	if (!(wr + wt > 0.0)) {
		return;
	}
	const float pr = wr / (wr + wt);
	const float cosOut = dot(wi, facing);
	const vec2 beta = vec2(dot(wi, t1), dot(wi, t2));
	const vec2 tangential = vec2(dot(inDir, t1), dot(inDir, t2));
	const uint lane = opticLaneFor(o, 0u);
	const float eta = opticIndex(o.incident, lane) / opticIndex(o.other, lane);
	if (dot(beta, beta) >= 1.0) {
		return;
	}
	if (cosOut > 0.0) {
		const float density = opticAbgDensity(o, beta, tangential) * o.tis;
		value = spectrumScale(R, density * cosOut);
		pdf = pr * density * cosOut;
	} else {
		const float density = opticAbgDensity(o, beta, eta * tangential) * o.tis;
		value = spectrumScale(T, eta * eta * density * -cosOut);
		pdf = (1.0 - pr) * density * -cosOut;
	}
}

// a direction in the lobe about `center` (a projected direction), on the facing side or the far one: the first draw from
// the sample, rejections past grazing redrawn from the path's stream. False when every draw fell outside
bool opticSampleLobe(OpticInterface o, vec2 center, vec3 facing, bool far, vec2 u, inout uint rng, out vec3 outDir)
{
	vec3 t1, t2;
	interactionFrame(facing, t1, t2);
	outDir = facing;
	for (int attempt = 0; attempt < CRT_ABG_TRIES; attempt++) {
		if (attempt > 0) {
			u = vec2(randomFloat(rng), randomFloat(rng));
		}
		const float r = opticAbgRadius(o, u.x);
		const float phi = 2.0 * CRT_PI * u.y;
		const vec2 beta = center + r * vec2(cos(phi), sin(phi));
		const float b2 = dot(beta, beta);
		if (b2 < 1.0) {
			const float z = sqrt(1.0 - b2);
			outDir = beta.x * t1 + beta.y * t2 + (far ? -z : z) * facing;
			return true;
		}
	}
	return false;
}

// the polished interface's sample. `n` is the surface's outward normal (toward its plus side), whichever side the ray
// is on; the ray's throughput already carries the segment's absorption
bool scatterOpticInterface(OpticInterface o, vec3 inDir, vec3 n, uint pathIndex, inout uint rng, out Spectrum attenuation, out vec3 outDir)
{
	attenuation = spectrumConst(0.0);
	outDir = n;
	// the normal on the incident side
	const vec3 facing = o.fromPlus ? n : -n;
	const float cosI = clamp(-dot(inDir, facing), 0.0, 1.0);
	Spectrum R;
	Spectrum T;
	opticPowers(o, cosI, R, T);
	const float wr = spectrumSelectionWeight(R);
	const float wt = spectrumSelectionWeight(T);
	const float u = sampleDimension(pathIndex, rng, CRT_SAMPLE_INDIRECT_LOBE);
	g_opticSampledDelta = true;
	if (!(wr + wt > 0.0)) {
		return false;
	}
	const float pr = wr / (wr + wt);
	// the share TIS of either event goes into the ABg lobe: the event's own number, rescaled, decides it, and the lobe's
	// direction takes the 2D sample. The weight is the event's either way, since the lobe is sampled as it is distributed
	const bool scatters = opticScatters(o);
	vec2 lobeSample = vec2(0.0);
	if (scatters) {
		lobeSample = sampleDimension2D(pathIndex, rng, CRT_SAMPLE_INDIRECT_DIRECTION);
	}
	vec3 t1, t2;
	interactionFrame(facing, t1, t2);
	const vec2 tangential = vec2(dot(inDir, t1), dot(inDir, t2));
	if (u < pr) {
		attenuation = spectrumScale(R, 1.0 / pr);
		if (scatters && u / pr < o.tis) {
			g_opticSampledDelta = false;
			return opticSampleLobe(o, tangential, facing, false, lobeSample, rng, outDir);
		}
		outDir = inDir + 2.0 * cosI * facing;
		return true;
	}
	// Snell's law at the lane the indices are read at (the kept one, or every lane's own, which agree when nothing disperses)
	const uint lane = opticLaneFor(o, 0u);
	const float eta = opticIndex(o.incident, lane) / opticIndex(o.other, lane);
	const float sin2T = eta * eta * max(0.0, 1.0 - cosI * cosI);
	if (sin2T >= 1.0) {
		// past the critical angle at this lane: the coating's table reports reflection there too, so this is its rounding
		return false;
	}
	const float cosT = sqrt(1.0 - sin2T);
	// radiance is divided by (n_t / n_i)^2 on the way in, as pbrt's dielectric divides it
	attenuation = spectrumScale(T, eta * eta / (1.0 - pr));
	if (scatters && (u - pr) / (1.0 - pr) < o.tis) {
		g_opticSampledDelta = false;
		return opticSampleLobe(o, eta * tangential, facing, true, lobeSample, rng, outDir);
	}
	outDir = eta * inDir + (eta * cosI - cosT) * facing;
	return true;
}

#endif
