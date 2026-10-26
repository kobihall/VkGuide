#include <bxdf.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr float PI_F = 3.14159265358979323846f;

// pbrt's spherical geometry in the local frame (util/vecmath.h)
float cos2Theta(glm::vec3 w)
{
	return w.z * w.z;
}

float sin2Theta(glm::vec3 w)
{
	return std::max(0.f, 1.f - cos2Theta(w));
}

float tan2Theta(glm::vec3 w)
{
	return sin2Theta(w) / cos2Theta(w);
}

float cosPhi(glm::vec3 w)
{
	const float sinTheta = std::sqrt(sin2Theta(w));
	return sinTheta == 0.f ? 1.f : std::clamp(w.x / sinTheta, -1.f, 1.f);
}

float sinPhi(glm::vec3 w)
{
	const float sinTheta = std::sqrt(sin2Theta(w));
	return sinTheta == 0.f ? 0.f : std::clamp(w.y / sinTheta, -1.f, 1.f);
}

bool sameHemisphere(glm::vec3 a, glm::vec3 b)
{
	return a.z * b.z > 0.f;
}

glm::vec3 reflectAbout(glm::vec3 wo, glm::vec3 n)
{
	return -wo + 2.f * glm::dot(wo, n) * n;
}

glm::vec3 faceForward(glm::vec3 n, glm::vec3 v)
{
	return glm::dot(n, v) < 0.f ? -n : n;
}

float safeSqrt(float x)
{
	return std::sqrt(std::max(0.f, x));
}

// pbrt's pstd::complex, the operations FrComplex() uses
struct Complex {
	float re;
	float im;
};

Complex operator+(Complex a, Complex b)
{
	return { a.re + b.re, a.im + b.im };
}

Complex operator-(Complex a, Complex b)
{
	return { a.re - b.re, a.im - b.im };
}

Complex operator*(Complex a, Complex b)
{
	return { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re };
}

Complex operator/(Complex a, Complex b)
{
	const float scale = 1.f / (b.re * b.re + b.im * b.im);
	return { scale * (a.re * b.re + a.im * b.im), scale * (a.im * b.re - a.re * b.im) };
}

float norm(Complex z)
{
	return z.re * z.re + z.im * z.im;
}

Complex complexSqrt(Complex z)
{
	const float n = std::sqrt(norm(z));
	const float t1 = std::sqrt(0.5f * (n + std::abs(z.re)));
	const float t2 = 0.5f * z.im / t1;
	if (n == 0.f) {
		return { 0.f, 0.f };
	}
	if (z.re >= 0.f) {
		return { t1, t2 };
	}
	return { std::abs(t2), std::copysign(t1, z.im) };
}

}

float frDielectric(float cosThetaI, float eta)
{
	cosThetaI = std::clamp(cosThetaI, -1.f, 1.f);
	if (cosThetaI < 0.f) {
		eta = 1.f / eta;
		cosThetaI = -cosThetaI;
	}
	const float sin2ThetaI = 1.f - cosThetaI * cosThetaI;
	const float sin2ThetaT = sin2ThetaI / (eta * eta);
	if (sin2ThetaT >= 1.f) {
		return 1.f;
	}
	const float cosThetaT = safeSqrt(1.f - sin2ThetaT);
	const float rParallel = (eta * cosThetaI - cosThetaT) / (eta * cosThetaI + cosThetaT);
	const float rPerpendicular = (cosThetaI - eta * cosThetaT) / (cosThetaI + eta * cosThetaT);
	return (rParallel * rParallel + rPerpendicular * rPerpendicular) / 2.f;
}

float frComplex(float cosThetaI, float eta, float k)
{
	cosThetaI = std::clamp(cosThetaI, 0.f, 1.f);
	const Complex etaC { eta, k };
	const float sin2ThetaI = 1.f - cosThetaI * cosThetaI;
	const Complex sin2ThetaT = Complex { sin2ThetaI, 0.f } / (etaC * etaC);
	const Complex cosThetaT = complexSqrt(Complex { 1.f, 0.f } - sin2ThetaT);
	const Complex cosI { cosThetaI, 0.f };
	const Complex rParallel = (etaC * cosI - cosThetaT) / (etaC * cosI + cosThetaT);
	const Complex rPerpendicular = (cosI - etaC * cosThetaT) / (cosI + etaC * cosThetaT);
	return (norm(rParallel) + norm(rPerpendicular)) / 2.f;
}

glm::vec3 frComplex(float cosThetaI, glm::vec3 eta, glm::vec3 k)
{
	return glm::vec3(frComplex(cosThetaI, eta.x, k.x), frComplex(cosThetaI, eta.y, k.y), frComplex(cosThetaI, eta.z, k.z));
}

bool refractDirection(glm::vec3 wi, glm::vec3 n, float eta, float& etap, glm::vec3& wt)
{
	float cosThetaI = glm::dot(n, wi);
	if (cosThetaI < 0.f) {
		eta = 1.f / eta;
		cosThetaI = -cosThetaI;
		n = -n;
	}
	const float sin2ThetaI = std::max(0.f, 1.f - cosThetaI * cosThetaI);
	const float sin2ThetaT = sin2ThetaI / (eta * eta);
	if (sin2ThetaT >= 1.f) {
		return false;
	}
	const float cosThetaT = std::sqrt(1.f - sin2ThetaT);
	wt = -wi / eta + (cosThetaI / eta - cosThetaT) * n;
	etap = eta;
	return true;
}

//---------------------------------------------------------------- Trowbridge-Reitz

TrowbridgeReitz::TrowbridgeReitz(float ax, float ay) : alphaX(ax), alphaY(ay)
{
	if (!effectivelySmooth()) {
		alphaX = std::max(alphaX, 1e-4f);
		alphaY = std::max(alphaY, 1e-4f);
	}
}

float TrowbridgeReitz::D(glm::vec3 wm) const
{
	const float t2 = tan2Theta(wm);
	if (std::isinf(t2)) {
		return 0.f;
	}
	const float cos4Theta = cos2Theta(wm) * cos2Theta(wm);
	if (cos4Theta < 1e-16f) {
		return 0.f;
	}
	const float cx = cosPhi(wm) / alphaX;
	const float sy = sinPhi(wm) / alphaY;
	const float e = t2 * (cx * cx + sy * sy);
	return 1.f / (PI_F * alphaX * alphaY * cos4Theta * (1.f + e) * (1.f + e));
}

float TrowbridgeReitz::lambda(glm::vec3 w) const
{
	const float t2 = tan2Theta(w);
	if (std::isinf(t2)) {
		return 0.f;
	}
	const float cx = cosPhi(w) * alphaX;
	const float sy = sinPhi(w) * alphaY;
	const float alpha2 = cx * cx + sy * sy;
	return (std::sqrt(1.f + alpha2 * t2) - 1.f) / 2.f;
}

float TrowbridgeReitz::D(glm::vec3 w, glm::vec3 wm) const
{
	return G1(w) / std::abs(w.z) * D(wm) * std::abs(glm::dot(w, wm));
}

glm::vec3 TrowbridgeReitz::sampleWm(glm::vec3 w, glm::vec2 u) const
{
	glm::vec3 wh = glm::normalize(glm::vec3(alphaX * w.x, alphaY * w.y, w.z));
	if (wh.z < 0.f) {
		wh = -wh;
	}
	const glm::vec3 t1 = wh.z < 0.99999f ? glm::normalize(glm::cross(glm::vec3(0.f, 0.f, 1.f), wh)) : glm::vec3(1.f, 0.f, 0.f);
	const glm::vec3 t2 = glm::cross(wh, t1);
	//pbrt's SampleUniformDiskPolar
	const float r = std::sqrt(u.x);
	const float theta = 2.f * PI_F * u.y;
	glm::vec2 p(r * std::cos(theta), r * std::sin(theta));
	const float h = std::sqrt(1.f - p.x * p.x);
	const float t = (1.f + wh.z) / 2.f;
	p.y = (1.f - t) * h + t * p.y;
	const float pz = std::sqrt(std::max(0.f, 1.f - (p.x * p.x + p.y * p.y)));
	const glm::vec3 nh = p.x * t1 + p.y * t2 + pz * wh;
	return glm::normalize(glm::vec3(alphaX * nh.x, alphaY * nh.y, std::max(1e-6f, nh.z)));
}

float TrowbridgeReitz::roughnessToAlpha(float roughness)
{
	return std::sqrt(roughness);
}

//---------------------------------------------------------------- the dielectric

uint32_t DielectricBxdf::flags() const
{
	const uint32_t f = eta == 1.f ? BXDF_TRANSMISSION : (BXDF_REFLECTION | BXDF_TRANSMISSION);
	return f | (distribution.effectivelySmooth() ? BXDF_SPECULAR : BXDF_GLOSSY);
}

bool DielectricBxdf::sample(glm::vec3 wo, float uc, glm::vec2 u, TransportMode mode, BxdfSample& out) const
{
	if (eta == 1.f || distribution.effectivelySmooth()) {
		const float R = frDielectric(wo.z, eta);
		const float T = 1.f - R;
		const float pr = R;
		const float pt = T;
		if (pr == 0.f && pt == 0.f) {
			return false;
		}
		if (uc < pr / (pr + pt)) {
			const glm::vec3 wi(-wo.x, -wo.y, wo.z);
			out = { glm::vec3(R / std::abs(wi.z)), wi, pr / (pr + pt), BXDF_SPECULAR | BXDF_REFLECTION, 1.f };
			return true;
		}
		glm::vec3 wi;
		float etap = 1.f;
		if (!refractDirection(wo, glm::vec3(0.f, 0.f, 1.f), eta, etap, wi)) {
			return false;
		}
		glm::vec3 ft(T / std::abs(wi.z));
		if (mode == TransportMode::Radiance) {
			ft /= etap * etap;
		}
		out = { ft, wi, pt / (pr + pt), BXDF_SPECULAR | BXDF_TRANSMISSION, etap };
		return true;
	}

	const glm::vec3 wm = distribution.sampleWm(wo, u);
	const float R = frDielectric(glm::dot(wo, wm), eta);
	const float T = 1.f - R;
	const float pr = R;
	const float pt = T;
	if (pr == 0.f && pt == 0.f) {
		return false;
	}
	if (uc < pr / (pr + pt)) {
		const glm::vec3 wi = reflectAbout(wo, wm);
		if (!sameHemisphere(wo, wi)) {
			return false;
		}
		const float pdf = distribution.pdf(wo, wm) / (4.f * std::abs(glm::dot(wo, wm))) * pr / (pr + pt);
		const float f = distribution.D(wm) * distribution.G(wo, wi) * R / (4.f * wi.z * wo.z);
		out = { glm::vec3(f), wi, pdf, BXDF_GLOSSY | BXDF_REFLECTION, 1.f };
		return true;
	}
	float etap = 1.f;
	glm::vec3 wi;
	const bool tir = !refractDirection(wo, wm, eta, etap, wi);
	if (sameHemisphere(wo, wi) || wi.z == 0.f || tir) {
		return false;
	}
	const float denom = (glm::dot(wi, wm) + glm::dot(wo, wm) / etap) * (glm::dot(wi, wm) + glm::dot(wo, wm) / etap);
	const float dwmDwi = std::abs(glm::dot(wi, wm)) / denom;
	const float pdf = distribution.pdf(wo, wm) * dwmDwi * pt / (pr + pt);
	float ft = T * distribution.D(wm) * distribution.G(wo, wi) * std::abs(glm::dot(wi, wm) * glm::dot(wo, wm) / (wi.z * wo.z * denom));
	if (mode == TransportMode::Radiance) {
		ft /= etap * etap;
	}
	out = { glm::vec3(ft), wi, pdf, BXDF_GLOSSY | BXDF_TRANSMISSION, etap };
	return true;
}

glm::vec3 DielectricBxdf::f(glm::vec3 wo, glm::vec3 wi, TransportMode mode) const
{
	if (eta == 1.f || distribution.effectivelySmooth()) {
		return glm::vec3(0.f);
	}
	//the generalized half vector
	const float cosThetaO = wo.z;
	const float cosThetaI = wi.z;
	const bool reflect = cosThetaI * cosThetaO > 0.f;
	float etap = 1.f;
	if (!reflect) {
		etap = cosThetaO > 0.f ? eta : (1.f / eta);
	}
	glm::vec3 wm = wi * etap + wo;
	if (cosThetaI == 0.f || cosThetaO == 0.f || glm::dot(wm, wm) == 0.f) {
		return glm::vec3(0.f);
	}
	wm = faceForward(glm::normalize(wm), glm::vec3(0.f, 0.f, 1.f));
	//back-facing microfacets
	if (glm::dot(wm, wi) * cosThetaI < 0.f || glm::dot(wm, wo) * cosThetaO < 0.f) {
		return glm::vec3(0.f);
	}
	const float F = frDielectric(glm::dot(wo, wm), eta);
	if (reflect) {
		return glm::vec3(distribution.D(wm) * distribution.G(wo, wi) * F / std::abs(4.f * cosThetaI * cosThetaO));
	}
	const float d = glm::dot(wi, wm) + glm::dot(wo, wm) / etap;
	const float denom = d * d * cosThetaI * cosThetaO;
	float ft = distribution.D(wm) * (1.f - F) * distribution.G(wo, wi) * std::abs(glm::dot(wi, wm) * glm::dot(wo, wm) / denom);
	if (mode == TransportMode::Radiance) {
		ft /= etap * etap;
	}
	return glm::vec3(ft);
}

float DielectricBxdf::pdf(glm::vec3 wo, glm::vec3 wi, TransportMode mode) const
{
	(void)mode;
	if (eta == 1.f || distribution.effectivelySmooth()) {
		return 0.f;
	}
	const float cosThetaO = wo.z;
	const float cosThetaI = wi.z;
	const bool reflect = cosThetaI * cosThetaO > 0.f;
	float etap = 1.f;
	if (!reflect) {
		etap = cosThetaO > 0.f ? eta : (1.f / eta);
	}
	glm::vec3 wm = wi * etap + wo;
	if (cosThetaI == 0.f || cosThetaO == 0.f || glm::dot(wm, wm) == 0.f) {
		return 0.f;
	}
	wm = faceForward(glm::normalize(wm), glm::vec3(0.f, 0.f, 1.f));
	if (glm::dot(wm, wi) * cosThetaI < 0.f || glm::dot(wm, wo) * cosThetaO < 0.f) {
		return 0.f;
	}
	const float R = frDielectric(glm::dot(wo, wm), eta);
	const float T = 1.f - R;
	const float pr = R;
	const float pt = T;
	if (pr == 0.f && pt == 0.f) {
		return 0.f;
	}
	if (reflect) {
		return distribution.pdf(wo, wm) / (4.f * std::abs(glm::dot(wo, wm))) * pr / (pr + pt);
	}
	const float d = glm::dot(wi, wm) + glm::dot(wo, wm) / etap;
	const float dwmDwi = std::abs(glm::dot(wi, wm)) / (d * d);
	return distribution.pdf(wo, wm) * dwmDwi * pt / (pr + pt);
}

//---------------------------------------------------------------- the conductor

uint32_t ConductorBxdf::flags() const
{
	return BXDF_REFLECTION | (distribution.effectivelySmooth() ? BXDF_SPECULAR : BXDF_GLOSSY);
}

bool ConductorBxdf::sample(glm::vec3 wo, float uc, glm::vec2 u, TransportMode mode, BxdfSample& out) const
{
	(void)uc;
	(void)mode;
	if (distribution.effectivelySmooth()) {
		const glm::vec3 wi(-wo.x, -wo.y, wo.z);
		out = { frComplex(std::abs(wi.z), eta, k) / std::abs(wi.z), wi, 1.f, BXDF_SPECULAR | BXDF_REFLECTION, 1.f };
		return true;
	}
	if (wo.z == 0.f) {
		return false;
	}
	const glm::vec3 wm = distribution.sampleWm(wo, u);
	const glm::vec3 wi = reflectAbout(wo, wm);
	if (!sameHemisphere(wo, wi)) {
		return false;
	}
	const float pdf = distribution.pdf(wo, wm) / (4.f * std::abs(glm::dot(wo, wm)));
	const float cosThetaO = std::abs(wo.z);
	const float cosThetaI = std::abs(wi.z);
	if (cosThetaI == 0.f || cosThetaO == 0.f) {
		return false;
	}
	const glm::vec3 F = frComplex(std::abs(glm::dot(wo, wm)), eta, k);
	out = { F * (distribution.D(wm) * distribution.G(wo, wi) / (4.f * cosThetaI * cosThetaO)), wi, pdf, BXDF_GLOSSY | BXDF_REFLECTION, 1.f };
	return true;
}

glm::vec3 ConductorBxdf::f(glm::vec3 wo, glm::vec3 wi, TransportMode mode) const
{
	(void)mode;
	if (!sameHemisphere(wo, wi) || distribution.effectivelySmooth()) {
		return glm::vec3(0.f);
	}
	const float cosThetaO = std::abs(wo.z);
	const float cosThetaI = std::abs(wi.z);
	if (cosThetaI == 0.f || cosThetaO == 0.f) {
		return glm::vec3(0.f);
	}
	glm::vec3 wm = wi + wo;
	if (glm::dot(wm, wm) == 0.f) {
		return glm::vec3(0.f);
	}
	wm = glm::normalize(wm);
	const glm::vec3 F = frComplex(std::abs(glm::dot(wo, wm)), eta, k);
	return F * (distribution.D(wm) * distribution.G(wo, wi) / (4.f * cosThetaI * cosThetaO));
}

float ConductorBxdf::pdf(glm::vec3 wo, glm::vec3 wi, TransportMode mode) const
{
	(void)mode;
	if (!sameHemisphere(wo, wi) || distribution.effectivelySmooth()) {
		return 0.f;
	}
	glm::vec3 wm = wo + wi;
	if (glm::dot(wm, wm) == 0.f) {
		return 0.f;
	}
	wm = faceForward(glm::normalize(wm), glm::vec3(0.f, 0.f, 1.f));
	return distribution.pdf(wo, wm) / (4.f * std::abs(glm::dot(wo, wm)));
}

ConductorBxdf ConductorBxdf::fromReflectance(const TrowbridgeReitz& distribution, glm::vec3 reflectance)
{
	//pbrt: avoid the r == 0 NaN
	const glm::vec3 r = glm::clamp(reflectance, glm::vec3(0.f), glm::vec3(0.9999f));
	ConductorBxdf bxdf;
	bxdf.distribution = distribution;
	bxdf.eta = glm::vec3(1.f);
	bxdf.k = 2.f * glm::sqrt(r) / glm::sqrt(glm::max(glm::vec3(1.f) - r, glm::vec3(0.f)));
	return bxdf;
}
