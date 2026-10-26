// THE FRESNEL EQUATIONS (PBR 4ed 9.3), the line-for-line mirror of src/bxdf.cpp: a dielectric's exact unpolarized
// reflectance with total internal reflection, and a conductor's from its complex index eta + i k, with pbrt's
// complex arithmetic (a vec2 is re, im). docs/plans/pbrt-bsdf-library.md.
//
// Requires nothing.

#ifndef CRT_FRESNEL_GLSL
#define CRT_FRESNEL_GLSL

float frDielectric(float cosThetaI, float eta)
{
	cosThetaI = clamp(cosThetaI, -1.0, 1.0);
	if (cosThetaI < 0.0) {
		eta = 1.0 / eta;
		cosThetaI = -cosThetaI;
	}
	const float sin2ThetaI = 1.0 - cosThetaI * cosThetaI;
	const float sin2ThetaT = sin2ThetaI / (eta * eta);
	if (sin2ThetaT >= 1.0) {
		return 1.0;
	}
	const float cosThetaT = sqrt(max(0.0, 1.0 - sin2ThetaT));
	const float rParallel = (eta * cosThetaI - cosThetaT) / (eta * cosThetaI + cosThetaT);
	const float rPerpendicular = (cosThetaI - eta * cosThetaT) / (cosThetaI + eta * cosThetaT);
	return (rParallel * rParallel + rPerpendicular * rPerpendicular) / 2.0;
}

vec2 complexMul(vec2 a, vec2 b)
{
	return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

vec2 complexDiv(vec2 a, vec2 b)
{
	const float scale = 1.0 / (b.x * b.x + b.y * b.y);
	return vec2(scale * (a.x * b.x + a.y * b.y), scale * (a.y * b.x - a.x * b.y));
}

float complexNorm(vec2 z)
{
	return z.x * z.x + z.y * z.y;
}

vec2 complexSqrt(vec2 z)
{
	const float n = sqrt(complexNorm(z));
	const float t1 = sqrt(0.5 * (n + abs(z.x)));
	const float t2 = 0.5 * z.y / t1;
	if (n == 0.0) {
		return vec2(0.0);
	}
	if (z.x >= 0.0) {
		return vec2(t1, t2);
	}
	// copysign(t1, z.im), which keeps the sign of a -0 as a comparison would not
	return vec2(abs(t2), uintBitsToFloat((floatBitsToUint(t1) & 0x7FFFFFFFu) | (floatBitsToUint(z.y) & 0x80000000u)));
}

float frComplex(float cosThetaI, float eta, float k)
{
	cosThetaI = clamp(cosThetaI, 0.0, 1.0);
	const vec2 etaC = vec2(eta, k);
	const float sin2ThetaI = 1.0 - cosThetaI * cosThetaI;
	const vec2 sin2ThetaT = complexDiv(vec2(sin2ThetaI, 0.0), complexMul(etaC, etaC));
	const vec2 cosThetaT = complexSqrt(vec2(1.0, 0.0) - sin2ThetaT);
	const vec2 cosI = vec2(cosThetaI, 0.0);
	const vec2 rParallel = complexDiv(complexMul(etaC, cosI) - cosThetaT, complexMul(etaC, cosI) + cosThetaT);
	const vec2 rPerpendicular = complexDiv(cosI - complexMul(etaC, cosThetaT), cosI + complexMul(etaC, cosThetaT));
	return (complexNorm(rParallel) + complexNorm(rPerpendicular)) / 2.0;
}

// Snell's law about n (the side wi is on decides which way eta reads); false for total internal reflection
bool refractDirection(vec3 wi, vec3 n, float eta, out float etap, out vec3 wt)
{
	etap = 1.0;
	wt = vec3(0.0);
	float cosThetaI = dot(n, wi);
	if (cosThetaI < 0.0) {
		eta = 1.0 / eta;
		cosThetaI = -cosThetaI;
		n = -n;
	}
	const float sin2ThetaI = max(0.0, 1.0 - cosThetaI * cosThetaI);
	const float sin2ThetaT = sin2ThetaI / (eta * eta);
	if (sin2ThetaT >= 1.0) {
		return false;
	}
	const float cosThetaT = sqrt(1.0 - sin2ThetaT);
	wt = -wi / eta + (cosThetaI / eta - cosThetaT) * n;
	etap = eta;
	return true;
}

#endif
