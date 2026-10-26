// PBRT-V4'S BXDFS, in its local shading frame (PBR 4ed 9.1.1): z is the OUTWARD shading normal, directions point away
// from the surface, and wo.z < 0 means the ray arrived from inside. The line-for-line mirror of src/bxdf.cpp.
// docs/plans/pbrt-bsdf-library.md; the `glass` and `conductor` material types read their parameters into a
// BxdfMaterial (crt_scatter.glsl resolveBxdfMaterial()), and kernel 06's complex build alone evaluates them.
//
//   glass      smooth or rough dielectric with exact Fresnel (Walter et al. 2007), dividing radiance by eta^2 on
//              transmission. One index for every lane: a dispersive glass first terminates the path's secondary
//              wavelengths (crt_spectrum.glsl terminateSecondary()) and reads its index at the one that is left
//   conductor  smooth or rough metal with complex Fresnel per lane
//
// Requires crt_common.glsl, crt_spectrum.glsl, crt_fresnel.glsl and crt_microfacet.glsl.

#ifndef CRT_BXDF_GLSL
#define CRT_BXDF_GLSL

#define CRT_BXDF_REFLECTION 1u
#define CRT_BXDF_TRANSMISSION 2u
#define CRT_BXDF_GLOSSY 8u
#define CRT_BXDF_SPECULAR 16u

#define CRT_TRANSPORT_RADIANCE 0u
#define CRT_TRANSPORT_IMPORTANCE 1u

struct BxdfMaterial {
	// CRT_MATERIAL_GLASS or CRT_MATERIAL_CONDUCTOR
	uint type;
	// the distribution's alphas, trowbridgeReitz() applied
	vec2 alpha;
	// glass: the relative index, outside over inside
	float eta;
	// conductor: the complex index per lane
	Spectrum conductorEta;
	Spectrum conductorK;
};

struct BxdfSample {
	// the BxDF's value, not yet times |cos|
	Spectrum f;
	vec3 wi;
	// the density; for a delta lobe the probability it was chosen with
	float pdf;
	uint flags;
	// the relative index a transmission crossed, else 1
	float eta;
};

Spectrum frComplexSpectrum(float cosThetaI, Spectrum eta, Spectrum k)
{
	Spectrum r = spectrumConst(0.0);
	for (uint i = 0u; i < CRT_LANE_END; i++) {
		spectrumSetLane(r, i, frComplex(cosThetaI, spectrumLane(eta, i), spectrumLane(k, i)));
	}
	return r;
}

bool bxdfSameHemisphere(vec3 a, vec3 b)
{
	return a.z * b.z > 0.0;
}

vec3 bxdfReflect(vec3 wo, vec3 n)
{
	return -wo + 2.0 * dot(wo, n) * n;
}

vec3 bxdfFaceForward(vec3 n, vec3 v)
{
	return dot(n, v) < 0.0 ? -n : n;
}

uint bxdfFlags(BxdfMaterial b)
{
	const uint roughness = effectivelySmooth(b.alpha) ? CRT_BXDF_SPECULAR : CRT_BXDF_GLOSSY;
	if (b.type == CRT_MATERIAL_CONDUCTOR) {
		return CRT_BXDF_REFLECTION | roughness;
	}
	return (b.eta == 1.0 ? CRT_BXDF_TRANSMISSION : (CRT_BXDF_REFLECTION | CRT_BXDF_TRANSMISSION)) | roughness;
}

//---------------------------------------------------------------- the dielectric

bool dielectricSample(BxdfMaterial b, vec3 wo, float uc, vec2 u, uint mode, out BxdfSample s)
{
	s.f = spectrumConst(0.0);
	s.wi = vec3(0.0, 0.0, 1.0);
	s.pdf = 0.0;
	s.flags = 0u;
	s.eta = 1.0;
	const float eta = b.eta;
	if (eta == 1.0 || effectivelySmooth(b.alpha)) {
		const float R = frDielectric(wo.z, eta);
		const float T = 1.0 - R;
		const float pr = R;
		const float pt = T;
		if (pr == 0.0 && pt == 0.0) {
			return false;
		}
		if (uc < pr / (pr + pt)) {
			s.wi = vec3(-wo.x, -wo.y, wo.z);
			s.f = spectrumConst(R / abs(s.wi.z));
			s.pdf = pr / (pr + pt);
			s.flags = CRT_BXDF_SPECULAR | CRT_BXDF_REFLECTION;
			return true;
		}
		vec3 wi;
		float etap;
		if (!refractDirection(wo, vec3(0.0, 0.0, 1.0), eta, etap, wi)) {
			return false;
		}
		float ft = T / abs(wi.z);
		if (mode == CRT_TRANSPORT_RADIANCE) {
			ft /= etap * etap;
		}
		s.wi = wi;
		s.f = spectrumConst(ft);
		s.pdf = pt / (pr + pt);
		s.flags = CRT_BXDF_SPECULAR | CRT_BXDF_TRANSMISSION;
		s.eta = etap;
		return true;
	}

	const vec3 wm = microfacetSampleWm(b.alpha, wo, u);
	const float R = frDielectric(dot(wo, wm), eta);
	const float T = 1.0 - R;
	const float pr = R;
	const float pt = T;
	if (pr == 0.0 && pt == 0.0) {
		return false;
	}
	if (uc < pr / (pr + pt)) {
		const vec3 wi = bxdfReflect(wo, wm);
		if (!bxdfSameHemisphere(wo, wi)) {
			return false;
		}
		s.pdf = microfacetPdf(b.alpha, wo, wm) / (4.0 * abs(dot(wo, wm))) * pr / (pr + pt);
		s.f = spectrumConst(microfacetD(b.alpha, wm) * microfacetG(b.alpha, wo, wi) * R / (4.0 * wi.z * wo.z));
		s.wi = wi;
		s.flags = CRT_BXDF_GLOSSY | CRT_BXDF_REFLECTION;
		return true;
	}
	float etap;
	vec3 wi;
	const bool tir = !refractDirection(wo, wm, eta, etap, wi);
	if (bxdfSameHemisphere(wo, wi) || wi.z == 0.0 || tir) {
		return false;
	}
	const float denom = (dot(wi, wm) + dot(wo, wm) / etap) * (dot(wi, wm) + dot(wo, wm) / etap);
	const float dwmDwi = abs(dot(wi, wm)) / denom;
	s.pdf = microfacetPdf(b.alpha, wo, wm) * dwmDwi * pt / (pr + pt);
	float ft = T * microfacetD(b.alpha, wm) * microfacetG(b.alpha, wo, wi) * abs(dot(wi, wm) * dot(wo, wm) / (wi.z * wo.z * denom));
	if (mode == CRT_TRANSPORT_RADIANCE) {
		ft /= etap * etap;
	}
	s.f = spectrumConst(ft);
	s.wi = wi;
	s.flags = CRT_BXDF_GLOSSY | CRT_BXDF_TRANSMISSION;
	s.eta = etap;
	return true;
}

Spectrum dielectricF(BxdfMaterial b, vec3 wo, vec3 wi, uint mode)
{
	const float eta = b.eta;
	if (eta == 1.0 || effectivelySmooth(b.alpha)) {
		return spectrumConst(0.0);
	}
	const float cosThetaO = wo.z;
	const float cosThetaI = wi.z;
	const bool reflect = cosThetaI * cosThetaO > 0.0;
	float etap = 1.0;
	if (!reflect) {
		etap = cosThetaO > 0.0 ? eta : (1.0 / eta);
	}
	vec3 wm = wi * etap + wo;
	if (cosThetaI == 0.0 || cosThetaO == 0.0 || dot(wm, wm) == 0.0) {
		return spectrumConst(0.0);
	}
	wm = bxdfFaceForward(normalize(wm), vec3(0.0, 0.0, 1.0));
	if (dot(wm, wi) * cosThetaI < 0.0 || dot(wm, wo) * cosThetaO < 0.0) {
		return spectrumConst(0.0);
	}
	const float F = frDielectric(dot(wo, wm), eta);
	if (reflect) {
		return spectrumConst(microfacetD(b.alpha, wm) * microfacetG(b.alpha, wo, wi) * F / abs(4.0 * cosThetaI * cosThetaO));
	}
	const float d = dot(wi, wm) + dot(wo, wm) / etap;
	const float denom = d * d * cosThetaI * cosThetaO;
	float ft = microfacetD(b.alpha, wm) * (1.0 - F) * microfacetG(b.alpha, wo, wi) * abs(dot(wi, wm) * dot(wo, wm) / denom);
	if (mode == CRT_TRANSPORT_RADIANCE) {
		ft /= etap * etap;
	}
	return spectrumConst(ft);
}

float dielectricPdf(BxdfMaterial b, vec3 wo, vec3 wi)
{
	const float eta = b.eta;
	if (eta == 1.0 || effectivelySmooth(b.alpha)) {
		return 0.0;
	}
	const float cosThetaO = wo.z;
	const float cosThetaI = wi.z;
	const bool reflect = cosThetaI * cosThetaO > 0.0;
	float etap = 1.0;
	if (!reflect) {
		etap = cosThetaO > 0.0 ? eta : (1.0 / eta);
	}
	vec3 wm = wi * etap + wo;
	if (cosThetaI == 0.0 || cosThetaO == 0.0 || dot(wm, wm) == 0.0) {
		return 0.0;
	}
	wm = bxdfFaceForward(normalize(wm), vec3(0.0, 0.0, 1.0));
	if (dot(wm, wi) * cosThetaI < 0.0 || dot(wm, wo) * cosThetaO < 0.0) {
		return 0.0;
	}
	const float R = frDielectric(dot(wo, wm), eta);
	const float T = 1.0 - R;
	const float pr = R;
	const float pt = T;
	if (pr == 0.0 && pt == 0.0) {
		return 0.0;
	}
	if (reflect) {
		return microfacetPdf(b.alpha, wo, wm) / (4.0 * abs(dot(wo, wm))) * pr / (pr + pt);
	}
	const float d = dot(wi, wm) + dot(wo, wm) / etap;
	const float dwmDwi = abs(dot(wi, wm)) / (d * d);
	return microfacetPdf(b.alpha, wo, wm) * dwmDwi * pt / (pr + pt);
}

//---------------------------------------------------------------- the conductor

bool conductorSample(BxdfMaterial b, vec3 wo, vec2 u, out BxdfSample s)
{
	s.f = spectrumConst(0.0);
	s.wi = vec3(0.0, 0.0, 1.0);
	s.pdf = 0.0;
	s.flags = 0u;
	s.eta = 1.0;
	// the two lobes share one evaluation of the complex Fresnel term, at the end, so the kernel inlines it once: the
	// mirror's at the reflected direction, the rough lobe's at its microfacet normal
	float cosF;
	float factor;
	if (effectivelySmooth(b.alpha)) {
		s.wi = vec3(-wo.x, -wo.y, wo.z);
		s.pdf = 1.0;
		s.flags = CRT_BXDF_SPECULAR | CRT_BXDF_REFLECTION;
		cosF = abs(s.wi.z);
		factor = 0.0;
	} else {
		if (wo.z == 0.0) {
			return false;
		}
		const vec3 wm = microfacetSampleWm(b.alpha, wo, u);
		const vec3 wi = bxdfReflect(wo, wm);
		if (!bxdfSameHemisphere(wo, wi)) {
			return false;
		}
		const float pdf = microfacetPdf(b.alpha, wo, wm) / (4.0 * abs(dot(wo, wm)));
		const float cosThetaO = abs(wo.z);
		const float cosThetaI = abs(wi.z);
		if (cosThetaI == 0.0 || cosThetaO == 0.0) {
			return false;
		}
		s.wi = wi;
		s.pdf = pdf;
		s.flags = CRT_BXDF_GLOSSY | CRT_BXDF_REFLECTION;
		cosF = abs(dot(wo, wm));
		factor = microfacetD(b.alpha, wm) * microfacetG(b.alpha, wo, wi) / (4.0 * cosThetaI * cosThetaO);
	}
	const Spectrum F = frComplexSpectrum(cosF, b.conductorEta, b.conductorK);
	s.f = factor == 0.0 ? spectrumDivide(F, abs(s.wi.z)) : spectrumScale(F, factor);
	return true;
}

Spectrum conductorF(BxdfMaterial b, vec3 wo, vec3 wi)
{
	if (!bxdfSameHemisphere(wo, wi) || effectivelySmooth(b.alpha)) {
		return spectrumConst(0.0);
	}
	const float cosThetaO = abs(wo.z);
	const float cosThetaI = abs(wi.z);
	if (cosThetaI == 0.0 || cosThetaO == 0.0) {
		return spectrumConst(0.0);
	}
	vec3 wm = wi + wo;
	if (dot(wm, wm) == 0.0) {
		return spectrumConst(0.0);
	}
	wm = normalize(wm);
	const Spectrum F = frComplexSpectrum(abs(dot(wo, wm)), b.conductorEta, b.conductorK);
	return spectrumScale(F, microfacetD(b.alpha, wm) * microfacetG(b.alpha, wo, wi) / (4.0 * cosThetaI * cosThetaO));
}

float conductorPdf(BxdfMaterial b, vec3 wo, vec3 wi)
{
	if (!bxdfSameHemisphere(wo, wi) || effectivelySmooth(b.alpha)) {
		return 0.0;
	}
	vec3 wm = wo + wi;
	if (dot(wm, wm) == 0.0) {
		return 0.0;
	}
	wm = bxdfFaceForward(normalize(wm), vec3(0.0, 0.0, 1.0));
	return microfacetPdf(b.alpha, wo, wm) / (4.0 * abs(dot(wo, wm)));
}

//---------------------------------------------------------------- by type

bool bxdfSample(BxdfMaterial b, vec3 wo, float uc, vec2 u, uint mode, out BxdfSample s)
{
	if (b.type == CRT_MATERIAL_CONDUCTOR) {
		return conductorSample(b, wo, u, s);
	}
	return dielectricSample(b, wo, uc, u, mode, s);
}

Spectrum bxdfF(BxdfMaterial b, vec3 wo, vec3 wi, uint mode)
{
	if (b.type == CRT_MATERIAL_CONDUCTOR) {
		return conductorF(b, wo, wi);
	}
	return dielectricF(b, wo, wi, mode);
}

float bxdfPdf(BxdfMaterial b, vec3 wo, vec3 wi, uint mode)
{
	if (b.type == CRT_MATERIAL_CONDUCTOR) {
		return conductorPdf(b, wo, wi);
	}
	return dielectricPdf(b, wo, wi);
}

#endif
