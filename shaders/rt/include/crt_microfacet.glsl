// THE TROWBRIDGE-REITZ DISTRIBUTION (PBR 4ed 9.6), anisotropic, in pbrt's local shading frame (z the outward normal):
// the line-for-line mirror of src/bxdf.cpp TrowbridgeReitz. docs/plans/pbrt-bsdf-library.md.
//
// Requires nothing.

#ifndef CRT_MICROFACET_GLSL
#define CRT_MICROFACET_GLSL

#define CRT_MICROFACET_PI 3.14159265358979323846

float localCos2Theta(vec3 w)
{
	return w.z * w.z;
}

float localSin2Theta(vec3 w)
{
	return max(0.0, 1.0 - localCos2Theta(w));
}

float localTan2Theta(vec3 w)
{
	return localSin2Theta(w) / localCos2Theta(w);
}

float localCosPhi(vec3 w)
{
	const float sinTheta = sqrt(localSin2Theta(w));
	return sinTheta == 0.0 ? 1.0 : clamp(w.x / sinTheta, -1.0, 1.0);
}

float localSinPhi(vec3 w)
{
	const float sinTheta = sqrt(localSin2Theta(w));
	return sinTheta == 0.0 ? 0.0 : clamp(w.y / sinTheta, -1.0, 1.0);
}

// alpha x and y, the constructor's floor applied: trowbridgeReitz()
vec2 trowbridgeReitz(float ax, float ay)
{
	if (max(ax, ay) >= 1e-3) {
		return vec2(max(ax, 1e-4), max(ay, 1e-4));
	}
	return vec2(ax, ay);
}

bool effectivelySmooth(vec2 alpha)
{
	return max(alpha.x, alpha.y) < 1e-3;
}

float microfacetD(vec2 alpha, vec3 wm)
{
	const float t2 = localTan2Theta(wm);
	if (isinf(t2)) {
		return 0.0;
	}
	const float cos4Theta = localCos2Theta(wm) * localCos2Theta(wm);
	if (cos4Theta < 1e-16) {
		return 0.0;
	}
	const float cx = localCosPhi(wm) / alpha.x;
	const float sy = localSinPhi(wm) / alpha.y;
	const float e = t2 * (cx * cx + sy * sy);
	return 1.0 / (CRT_MICROFACET_PI * alpha.x * alpha.y * cos4Theta * (1.0 + e) * (1.0 + e));
}

float microfacetLambda(vec2 alpha, vec3 w)
{
	const float t2 = localTan2Theta(w);
	if (isinf(t2)) {
		return 0.0;
	}
	const float cx = localCosPhi(w) * alpha.x;
	const float sy = localSinPhi(w) * alpha.y;
	const float alpha2 = cx * cx + sy * sy;
	return (sqrt(1.0 + alpha2 * t2) - 1.0) / 2.0;
}

float microfacetG1(vec2 alpha, vec3 w)
{
	return 1.0 / (1.0 + microfacetLambda(alpha, w));
}

float microfacetG(vec2 alpha, vec3 wo, vec3 wi)
{
	return 1.0 / (1.0 + microfacetLambda(alpha, wo) + microfacetLambda(alpha, wi));
}

// the density of the normals visible from w
float microfacetPdf(vec2 alpha, vec3 w, vec3 wm)
{
	return microfacetG1(alpha, w) / abs(w.z) * microfacetD(alpha, wm) * abs(dot(w, wm));
}

vec3 microfacetSampleWm(vec2 alpha, vec3 w, vec2 u)
{
	vec3 wh = normalize(vec3(alpha.x * w.x, alpha.y * w.y, w.z));
	if (wh.z < 0.0) {
		wh = -wh;
	}
	const vec3 t1 = wh.z < 0.99999 ? normalize(cross(vec3(0.0, 0.0, 1.0), wh)) : vec3(1.0, 0.0, 0.0);
	const vec3 t2 = cross(wh, t1);
	const float r = sqrt(u.x);
	const float theta = 2.0 * CRT_MICROFACET_PI * u.y;
	vec2 p = vec2(r * cos(theta), r * sin(theta));
	const float h = sqrt(1.0 - p.x * p.x);
	const float t = (1.0 + wh.z) / 2.0;
	p.y = (1.0 - t) * h + t * p.y;
	const float pz = sqrt(max(0.0, 1.0 - (p.x * p.x + p.y * p.y)));
	const vec3 nh = p.x * t1 + p.y * t2 + pz * wh;
	return normalize(vec3(alpha.x * nh.x, alpha.y * nh.y, max(1e-6, nh.z)));
}

#endif
