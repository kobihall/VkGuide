// PBRT-V4'S BXDFS, in its local shading frame (PBR 4ed 9.1.1): z is the OUTWARD shading normal, directions point away
// from the surface, and wo.z < 0 means the ray arrived from inside. The line-for-line mirror of src/bxdf.cpp.
// docs/plans/completed/pbrt-bsdf-library.md; the `glass`, `conductor`, `diffuse`, `thin_glass` and
// `diffuse_transmission` material types read their parameters into a BxdfMaterial (crt_scatter.glsl
// resolveBxdfMaterial()), and kernel 06's complex build alone evaluates them.
//
//   glass      smooth or rough dielectric with exact Fresnel (Walter et al. 2007), dividing radiance by eta^2 on
//              transmission. One index for every lane: a dispersive glass first terminates the path's secondary
//              wavelengths (crt_spectrum.glsl terminateSecondary()) and reads its index at the one that is left
//   conductor  smooth or rough metal with complex Fresnel per lane
//   diffuse    Lambertian, or Oren-Nayar when sigma > 0
//   diffuse_transmission  Lambertian reflection and transmission
//   thin_glass a thin pane: the Fresnel term summed over the inter-reflections of its two faces, transmitting straight on
//
// Requires crt_common.glsl, crt_random.glsl, crt_spectrum.glsl, crt_fresnel.glsl and crt_microfacet.glsl.

#ifndef CRT_BXDF_GLSL
#define CRT_BXDF_GLSL

#define CRT_BXDF_REFLECTION 1u
#define CRT_BXDF_TRANSMISSION 2u
#define CRT_BXDF_DIFFUSE 4u
#define CRT_BXDF_GLOSSY 8u
#define CRT_BXDF_SPECULAR 16u

#define CRT_TRANSPORT_RADIANCE 0u
#define CRT_TRANSPORT_IMPORTANCE 1u

struct BxdfMaterial {
	// CRT_MATERIAL_GLASS, CONDUCTOR, DIFFUSE, THIN_GLASS or DIFFUSE_TRANSMISSION
	uint type;
	// the distribution's alphas, trowbridgeReitz() applied
	vec2 alpha;
	// glass and thin glass: the relative index, outside over inside
	float eta;
	// diffuse: the reflectance; diffuse transmission: the reflectance and the transmittance
	Spectrum reflectance;
	Spectrum transmittance;
	// diffuse: Oren-Nayar's A and B, 1 and 0 for a Lambertian surface
	float orenA;
	float orenB;
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
	if (b.type == CRT_MATERIAL_DIFFUSE) {
		return spectrumMaxValue(b.reflectance) > 0.0 ? (CRT_BXDF_REFLECTION | CRT_BXDF_DIFFUSE) : 0u;
	}
	if (b.type == CRT_MATERIAL_DIFFUSE_TRANSMISSION) {
		return (spectrumMaxValue(b.reflectance) > 0.0 ? (CRT_BXDF_REFLECTION | CRT_BXDF_DIFFUSE) : 0u)
			| (spectrumMaxValue(b.transmittance) > 0.0 ? (CRT_BXDF_TRANSMISSION | CRT_BXDF_DIFFUSE) : 0u);
	}
	if (b.type == CRT_MATERIAL_THIN_GLASS) {
		return CRT_BXDF_REFLECTION | CRT_BXDF_TRANSMISSION | CRT_BXDF_SPECULAR;
	}
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

//---------------------------------------------------------------- the diffuse BxDFs

vec2 bxdfSampleDiskConcentric(vec2 u)
{
	const vec2 offset = 2.0 * u - 1.0;
	if (offset.x == 0.0 && offset.y == 0.0) {
		return vec2(0.0);
	}
	float r;
	float theta;
	if (abs(offset.x) > abs(offset.y)) {
		r = offset.x;
		theta = (CRT_PI / 4.0) * (offset.y / offset.x);
	} else {
		r = offset.y;
		theta = CRT_PI / 2.0 - (CRT_PI / 4.0) * (offset.x / offset.y);
	}
	return r * vec2(cos(theta), sin(theta));
}

vec3 bxdfSampleCosineHemisphere(vec2 u)
{
	const vec2 d = bxdfSampleDiskConcentric(u);
	return vec3(d, sqrt(max(0.0, 1.0 - d.x * d.x - d.y * d.y)));
}

float bxdfCosPhi(vec3 w)
{
	const float sinTheta = sqrt(max(0.0, 1.0 - w.z * w.z));
	return sinTheta == 0.0 ? 1.0 : clamp(w.x / sinTheta, -1.0, 1.0);
}

float bxdfSinPhi(vec3 w)
{
	const float sinTheta = sqrt(max(0.0, 1.0 - w.z * w.z));
	return sinTheta == 0.0 ? 0.0 : clamp(w.y / sinTheta, -1.0, 1.0);
}

Spectrum diffuseF(BxdfMaterial b, vec3 wo, vec3 wi)
{
	if (!bxdfSameHemisphere(wo, wi)) {
		return spectrumConst(0.0);
	}
	if (b.orenB == 0.0) {
		return spectrumScale(b.reflectance, 1.0 / CRT_PI);
	}
	// pbrt-v3's OrenNayar::f()
	const float sinThetaI = sqrt(max(0.0, 1.0 - wi.z * wi.z));
	const float sinThetaO = sqrt(max(0.0, 1.0 - wo.z * wo.z));
	float maxCos = 0.0;
	if (sinThetaI > 1e-4 && sinThetaO > 1e-4) {
		const float dCos = bxdfCosPhi(wi) * bxdfCosPhi(wo) + bxdfSinPhi(wi) * bxdfSinPhi(wo);
		maxCos = max(0.0, dCos);
	}
	float sinAlpha;
	float tanBeta;
	if (abs(wi.z) > abs(wo.z)) {
		sinAlpha = sinThetaO;
		tanBeta = sinThetaI / abs(wi.z);
	} else {
		sinAlpha = sinThetaI;
		tanBeta = sinThetaO / abs(wo.z);
	}
	return spectrumScale(b.reflectance, (b.orenA + b.orenB * maxCos * sinAlpha * tanBeta) / CRT_PI);
}

bool diffuseSample(BxdfMaterial b, vec3 wo, vec2 u, out BxdfSample s)
{
	vec3 wi = bxdfSampleCosineHemisphere(u);
	if (wo.z < 0.0) {
		wi.z = -wi.z;
	}
	s.wi = wi;
	s.pdf = abs(wi.z) / CRT_PI;
	s.f = diffuseF(b, wo, wi);
	s.flags = CRT_BXDF_DIFFUSE | CRT_BXDF_REFLECTION;
	s.eta = 1.0;
	return true;
}

float diffusePdf(vec3 wo, vec3 wi)
{
	return bxdfSameHemisphere(wo, wi) ? abs(wi.z) / CRT_PI : 0.0;
}

Spectrum diffuseTransmissionF(BxdfMaterial b, vec3 wo, vec3 wi)
{
	return spectrumScale(bxdfSameHemisphere(wo, wi) ? b.reflectance : b.transmittance, 1.0 / CRT_PI);
}

bool diffuseTransmissionSample(BxdfMaterial b, vec3 wo, float uc, vec2 u, out BxdfSample s)
{
	s.f = spectrumConst(0.0);
	s.wi = vec3(0.0, 0.0, 1.0);
	s.pdf = 0.0;
	s.flags = 0u;
	s.eta = 1.0;
	const float pr = spectrumMaxValue(b.reflectance);
	const float pt = spectrumMaxValue(b.transmittance);
	if (pr == 0.0 && pt == 0.0) {
		return false;
	}
	vec3 wi = bxdfSampleCosineHemisphere(u);
	if (uc < pr / (pr + pt)) {
		if (wo.z < 0.0) {
			wi.z = -wi.z;
		}
		s.f = spectrumScale(b.reflectance, 1.0 / CRT_PI);
		s.pdf = abs(wi.z) / CRT_PI * pr / (pr + pt);
		s.flags = CRT_BXDF_DIFFUSE | CRT_BXDF_REFLECTION;
	} else {
		if (wo.z > 0.0) {
			wi.z = -wi.z;
		}
		s.f = spectrumScale(b.transmittance, 1.0 / CRT_PI);
		s.pdf = abs(wi.z) / CRT_PI * pt / (pr + pt);
		s.flags = CRT_BXDF_DIFFUSE | CRT_BXDF_TRANSMISSION;
	}
	s.wi = wi;
	return true;
}

float diffuseTransmissionPdf(BxdfMaterial b, vec3 wo, vec3 wi)
{
	const float pr = spectrumMaxValue(b.reflectance);
	const float pt = spectrumMaxValue(b.transmittance);
	if (pr == 0.0 && pt == 0.0) {
		return 0.0;
	}
	return abs(wi.z) / CRT_PI * (bxdfSameHemisphere(wo, wi) ? pr : pt) / (pr + pt);
}

//---------------------------------------------------------------- the thin dielectric

bool thinDielectricSample(BxdfMaterial b, vec3 wo, float uc, out BxdfSample s)
{
	s.f = spectrumConst(0.0);
	s.wi = vec3(0.0, 0.0, 1.0);
	s.pdf = 0.0;
	s.flags = 0u;
	s.eta = 1.0;
	// PBR 4ed 9.5.1: the sum of the geometric series of reflections between the two interfaces
	float R = frDielectric(abs(wo.z), b.eta);
	float T = 1.0 - R;
	if (R < 1.0) {
		R += T * T * R / (1.0 - R * R);
		T = 1.0 - R;
	}
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
	s.wi = -wo;
	s.f = spectrumConst(T / abs(s.wi.z));
	s.pdf = pt / (pr + pt);
	s.flags = CRT_BXDF_SPECULAR | CRT_BXDF_TRANSMISSION;
	return true;
}

//---------------------------------------------------------------- by type

bool bxdfSample(BxdfMaterial b, vec3 wo, float uc, vec2 u, uint mode, out BxdfSample s)
{
	if (b.type == CRT_MATERIAL_CONDUCTOR) {
		return conductorSample(b, wo, u, s);
	}
	if (b.type == CRT_MATERIAL_DIFFUSE) {
		return diffuseSample(b, wo, u, s);
	}
	if (b.type == CRT_MATERIAL_DIFFUSE_TRANSMISSION) {
		return diffuseTransmissionSample(b, wo, uc, u, s);
	}
	if (b.type == CRT_MATERIAL_THIN_GLASS) {
		return thinDielectricSample(b, wo, uc, s);
	}
	return dielectricSample(b, wo, uc, u, mode, s);
}

Spectrum bxdfF(BxdfMaterial b, vec3 wo, vec3 wi, uint mode)
{
	if (b.type == CRT_MATERIAL_CONDUCTOR) {
		return conductorF(b, wo, wi);
	}
	if (b.type == CRT_MATERIAL_DIFFUSE) {
		return diffuseF(b, wo, wi);
	}
	if (b.type == CRT_MATERIAL_DIFFUSE_TRANSMISSION) {
		return diffuseTransmissionF(b, wo, wi);
	}
	if (b.type == CRT_MATERIAL_THIN_GLASS) {
		return spectrumConst(0.0);
	}
	return dielectricF(b, wo, wi, mode);
}

float bxdfPdf(BxdfMaterial b, vec3 wo, vec3 wi, uint mode)
{
	if (b.type == CRT_MATERIAL_CONDUCTOR) {
		return conductorPdf(b, wo, wi);
	}
	if (b.type == CRT_MATERIAL_DIFFUSE) {
		return diffusePdf(wo, wi);
	}
	if (b.type == CRT_MATERIAL_DIFFUSE_TRANSMISSION) {
		return diffuseTransmissionPdf(b, wo, wi);
	}
	if (b.type == CRT_MATERIAL_THIN_GLASS) {
		return 0.0;
	}
	return dielectricPdf(b, wo, wi);
}

//---------------------------------------------------------------- the mix material

// src/bxdf.cpp mixHash(): the float bits of the hit point and the outgoing direction folded through the PCG hash, so the
// same hit chooses the same child whenever it is evaluated
float mixHash(vec3 p, vec3 wo, uint salt)
{
	uint h = pcgHash(salt);
	h = pcgHash(h ^ floatBitsToUint(p.x));
	h = pcgHash(h ^ floatBitsToUint(p.y));
	h = pcgHash(h ^ floatBitsToUint(p.z));
	h = pcgHash(h ^ floatBitsToUint(wo.x));
	h = pcgHash(h ^ floatBitsToUint(wo.y));
	h = pcgHash(h ^ floatBitsToUint(wo.z));
	return float(h >> 8u) * (1.0 / 16777216.0);
}

bool mixChoosesSecond(float amount, vec3 p, vec3 wo, uint salt)
{
	if (amount <= 0.0) {
		return false;
	}
	if (amount >= 1.0) {
		return true;
	}
	return !(amount < mixHash(p, wo, salt));
}

#endif
