// LIGHT SAMPLING, for next-event estimation and multiple importance sampling.
//
// The light list (src/rt_lights.h) holds every light kernel 06 can sample: the punctual lights,
// the area lights (analytic shapes and glTF triangles that emit), and the environment map. Kernel
// 06 picks one in proportion to its power and samples a direction towards it; kernels 03 and 04 ask
// the same code what density that sampling would have given a direction a BSDF sample found, which
// is what the MIS weight is made of. Both halves come from this file so they can never disagree.
//
// Mirrored line for line by src/light_sampling.cpp, where tests/light_test.cpp checks every
// routine against brute force - a change to one side must be made to the other, then run
// bin/light_test. Each follows pbrt-v4 (PBR 4ed chapters 6 and 12): a sphere by the cone it
// subtends, a rectangle and a triangle uniformly in solid angle (by area when that angle is tiny or
// nearly a hemisphere), a cylinder by area, the environment map through a piecewise-constant 2D
// table and its MIS-compensated copy, punctual lights by glTF's falloffs.
//
// Requires crt_common.glsl, crt_random.glsl and equirect.glsl; the including kernel declares the
// LightBuffer binding (and EnvironmentSampling, TriangleAttributes and MaterialTextures for the
// kinds that read them).

// src/light_sampling.h LIGHT_MIN_SPHERICAL_SOLID_ANGLE / LIGHT_MAX_SPHERICAL_SOLID_ANGLE
#include "crt_material.glsl"
#include "crt_warp.glsl"
#include "crt_shape.glsl"

#define CRT_MIN_SPHERICAL_SOLID_ANGLE 1e-3
#define CRT_MAX_SPHERICAL_SOLID_ANGLE 6.22
// the largest float below 1
#define CRT_ONE_MINUS_EPSILON 0.99999994

struct LightSample {
	vec3 point;
	vec3 normal;
	// unit length, from the reference point towards the light
	vec3 direction;
	// to `point`, or CRT_INFINITY for a directional or environment light
	float distance;
	// with respect to solid angle; 1 for a delta light
	float pdf;
};

//---------------------------------------------------------------- shared

// the kind without its flags
uint lightKindOf(GpuLight light)
{
	return light.kind & CRT_LIGHT_KIND_MASK;
}


float lightSafeSqrt(float x)
{
	return sqrt(max(x, 0.0));
}

float lightAngleBetween(vec3 a, vec3 b)
{
	if (dot(a, b) < 0.0) {
		return CRT_PI - 2.0 * asin(clamp(length(a + b) / 2.0, -1.0, 1.0));
	}
	return 2.0 * asin(clamp(length(b - a) / 2.0, -1.0, 1.0));
}

// differenceOfProducts() and sumOfProducts() are in crt_common.glsl, which the watertight triangle test shares

void lightFrameFromZ(vec3 z, out vec3 x, out vec3 y)
{
	const float zSign = z.z >= 0.0 ? 1.0 : -1.0;
	const float a = -1.0 / (zSign + z.z);
	const float b = z.x * z.y * a;
	x = vec3(1.0 + zSign * z.x * z.x * a, zSign * b, -zSign * z.x);
	y = vec3(b, zSign + z.y * z.y * a, -z.y);
}

// the balance (exponent 1) or power (exponent 2) heuristic's weight for a sample from the technique
// of density `pdf` against the one of density `otherPdf` (PBR 4ed eq. 2.14, 2.15)
float misWeight(float pdf, float otherPdf, float exponent)
{
	const float f = exponent >= 2.0 ? pdf * pdf : pdf;
	const float g = exponent >= 2.0 ? otherPdf * otherPdf : otherPdf;
	return f + g > 0.0 ? f / (f + g) : 0.0;
}

float areaToSolidAngle(float pdfArea, vec3 ref, vec3 point, vec3 normal)
{
	const vec3 toLight = point - ref;
	const float distanceSquared = dot(toLight, toLight);
	if (distanceSquared <= 0.0) {
		return 0.0;
	}
	const float cosLight = abs(dot(normal, toLight)) / sqrt(distanceSquared);
	if (cosLight <= 0.0) {
		return 0.0;
	}
	return pdfArea * distanceSquared / cosLight;
}

bool finishAreaSample(vec3 ref, vec3 point, vec3 normal, float pdfArea, out LightSample s)
{
	const vec3 toLight = point - ref;
	const float distanceSquared = dot(toLight, toLight);
	s.point = point;
	s.normal = normal;
	s.distance = sqrt(distanceSquared);
	s.direction = distanceSquared > 0.0 ? toLight / s.distance : vec3(0.0, 0.0, 1.0);
	s.pdf = areaToSolidAngle(pdfArea, ref, point, normal);
	return distanceSquared > 0.0 && s.pdf > 0.0;
}

//---------------------------------------------------------------- sphere

float sphereLightPdf(vec3 center, float radius, vec3 ref, vec3 lightPoint)
{
	const vec3 toCenter = center - ref;
	const float distanceSquared = dot(toCenter, toCenter);
	if (distanceSquared <= radius * radius) {
		return areaToSolidAngle(1.0 / (4.0 * CRT_PI * radius * radius), ref, lightPoint, normalize(lightPoint - center));
	}
	const float sin2ThetaMax = radius * radius / distanceSquared;
	const float cosThetaMax = lightSafeSqrt(1.0 - sin2ThetaMax);
	float oneMinusCosThetaMax = 1.0 - cosThetaMax;
	if (sin2ThetaMax < 0.00068523) {
		oneMinusCosThetaMax = sin2ThetaMax / 2.0;
	}
	return 1.0 / (2.0 * CRT_PI * oneMinusCosThetaMax);
}

bool sampleSphereLight(vec3 center, float radius, vec3 ref, vec2 u, out LightSample s)
{
	const vec3 toCenter = center - ref;
	const float distanceSquared = dot(toCenter, toCenter);

	// from inside, every point is visible: uniformly by area
	if (distanceSquared <= radius * radius) {
		const float z = 1.0 - 2.0 * u.x;
		const float r = lightSafeSqrt(1.0 - z * z);
		const float phi = 2.0 * CRT_PI * u.y;
		const vec3 normal = vec3(r * cos(phi), r * sin(phi), z);
		if (!finishAreaSample(ref, center + radius * normal, normal, 1.0 / (4.0 * CRT_PI * radius * radius), s)) {
			return false;
		}
		s.pdf = sphereLightPdf(center, radius, ref, s.point);
		return s.pdf > 0.0;
	}

	// from outside, uniformly in the cone it subtends
	const float sinThetaMax = radius / sqrt(distanceSquared);
	const float sin2ThetaMax = sinThetaMax * sinThetaMax;
	const float cosThetaMax = lightSafeSqrt(1.0 - sin2ThetaMax);
	float oneMinusCosThetaMax = 1.0 - cosThetaMax;

	float cosTheta = (cosThetaMax - 1.0) * u.x + 1.0;
	float sin2Theta = 1.0 - cosTheta * cosTheta;
	if (sin2ThetaMax < 0.00068523) {
		// a small cone by its Taylor series: 1 - cos(theta) cancels catastrophically
		sin2Theta = sin2ThetaMax * u.x;
		cosTheta = sqrt(1.0 - sin2Theta);
		oneMinusCosThetaMax = sin2ThetaMax / 2.0;
	}

	const float cosAlpha = sin2Theta / sinThetaMax + cosTheta * lightSafeSqrt(1.0 - sin2Theta / sin2ThetaMax);
	const float sinAlpha = lightSafeSqrt(1.0 - cosAlpha * cosAlpha);
	const float phi = u.y * 2.0 * CRT_PI;
	const vec3 w = vec3(sinAlpha * cos(phi), sinAlpha * sin(phi), cosAlpha);
	const vec3 axis = normalize(toCenter);
	vec3 frameX;
	vec3 frameY;
	lightFrameFromZ(axis, frameX, frameY);
	const vec3 normal = -(w.x * frameX + w.y * frameY + w.z * axis);
	const vec3 point = center + radius * normal;

	const vec3 toLight = point - ref;
	s.point = point;
	s.normal = normal;
	s.distance = length(toLight);
	s.direction = s.distance > 0.0 ? toLight / s.distance : axis;
	s.pdf = 1.0 / (2.0 * CRT_PI * oneMinusCosThetaMax);
	return s.distance > 0.0;
}

//---------------------------------------------------------------- rectangle

// Urena et al. 2013, as pbrt-v4's SampleSphericalRectangle() sets it up
struct SphericalRectangle {
	vec3 x;
	vec3 y;
	vec3 z;
	float x0;
	float y0;
	float z0;
	float x1;
	float y1;
	float b0;
	float b1;
	float g0;
	float g1;
	float g2;
	float g3;
	float solidAngle;
};

SphericalRectangle setupSphericalRectangle(vec3 corner, vec3 edgeU, vec3 edgeV, vec3 ref)
{
	SphericalRectangle r;
	const float lengthU = length(edgeU);
	const float lengthV = length(edgeV);
	r.x = edgeU / lengthU;
	r.y = edgeV / lengthV;
	r.z = cross(r.x, r.y);
	const vec3 d = corner - ref;
	r.x0 = dot(d, r.x);
	r.y0 = dot(d, r.y);
	r.z0 = dot(d, r.z);
	if (r.z0 > 0.0) {
		r.z = -r.z;
		r.z0 = -r.z0;
	}
	r.x1 = r.x0 + lengthU;
	r.y1 = r.y0 + lengthV;
	r.b0 = 0.0;
	r.b1 = 0.0;
	r.g0 = 0.0;
	r.g1 = 0.0;
	r.g2 = 0.0;
	r.g3 = 0.0;
	r.solidAngle = 0.0;
	// in the rectangle's plane: seen edge-on, or from inside itself
	if (!(r.z0 < -1e-7 * (lengthU + lengthV))) {
		return r;
	}

	const vec3 v00 = vec3(r.x0, r.y0, r.z0);
	const vec3 v01 = vec3(r.x0, r.y1, r.z0);
	const vec3 v10 = vec3(r.x1, r.y0, r.z0);
	const vec3 v11 = vec3(r.x1, r.y1, r.z0);
	const vec3 n0 = normalize(cross(v00, v10));
	const vec3 n1 = normalize(cross(v10, v11));
	const vec3 n2 = normalize(cross(v11, v01));
	const vec3 n3 = normalize(cross(v01, v00));
	r.g0 = lightAngleBetween(-n0, n1);
	r.g1 = lightAngleBetween(-n1, n2);
	r.g2 = lightAngleBetween(-n2, n3);
	r.g3 = lightAngleBetween(-n3, n0);
	r.b0 = n0.z;
	r.b1 = n2.z;
	r.solidAngle = max(r.g0 + r.g1 + r.g2 + r.g3 - 2.0 * CRT_PI, 0.0);
	return r;
}

bool sphericalRectangleUsable(SphericalRectangle r)
{
	return r.solidAngle >= CRT_MIN_SPHERICAL_SOLID_ANGLE && r.solidAngle <= CRT_MAX_SPHERICAL_SOLID_ANGLE;
}

bool sampleRectangleLight(vec3 corner, vec3 edgeU, vec3 edgeV, vec3 ref, vec2 u, out LightSample s)
{
	const vec3 normal = normalize(cross(edgeU, edgeV));
	const SphericalRectangle r = setupSphericalRectangle(corner, edgeU, edgeV, ref);
	if (!sphericalRectangleUsable(r)) {
		const float area = length(cross(edgeU, edgeV));
		return finishAreaSample(ref, corner + u.x * edgeU + u.y * edgeV, normal, 1.0 / area, s);
	}

	const float au = u.x * (r.g0 + r.g1 - 2.0 * CRT_PI) + (u.x - 1.0) * (r.g2 + r.g3);
	const float sinAu = sin(au);
	const float fu = (cos(au) * r.b0 - r.b1) / (abs(sinAu) > 1e-20 ? sinAu : 1e-20);
	float cu = 1.0 / sqrt(fu * fu + r.b0 * r.b0);
	cu = fu >= 0.0 ? cu : -cu;
	cu = clamp(cu, -CRT_ONE_MINUS_EPSILON, CRT_ONE_MINUS_EPSILON);
	float xu = -(cu * r.z0) / lightSafeSqrt(1.0 - cu * cu);
	xu = clamp(xu, r.x0, r.x1);
	const float dd = sqrt(xu * xu + r.z0 * r.z0);
	const float h0 = r.y0 / sqrt(dd * dd + r.y0 * r.y0);
	const float h1 = r.y1 / sqrt(dd * dd + r.y1 * r.y1);
	const float hv = h0 + u.y * (h1 - h0);
	const float hvSquared = hv * hv;
	const float yv = hvSquared < 1.0 - 1e-6 ? (hv * dd) / sqrt(1.0 - hvSquared) : r.y1;

	const vec3 point = ref + xu * r.x + yv * r.y + r.z0 * r.z;
	const vec3 toLight = point - ref;
	s.point = point;
	s.normal = normal;
	s.distance = length(toLight);
	s.direction = s.distance > 0.0 ? toLight / s.distance : -r.z;
	s.pdf = 1.0 / r.solidAngle;
	return s.distance > 0.0;
}

float rectangleLightPdf(vec3 corner, vec3 edgeU, vec3 edgeV, vec3 ref, vec3 lightPoint)
{
	const SphericalRectangle r = setupSphericalRectangle(corner, edgeU, edgeV, ref);
	if (!sphericalRectangleUsable(r)) {
		const float area = length(cross(edgeU, edgeV));
		return areaToSolidAngle(1.0 / area, ref, lightPoint, normalize(cross(edgeU, edgeV)));
	}
	return 1.0 / r.solidAngle;
}

//---------------------------------------------------------------- triangle

float triangleSolidAngle(vec3 v0, vec3 v1, vec3 v2, vec3 ref)
{
	const vec3 a = normalize(v0 - ref);
	const vec3 b = normalize(v1 - ref);
	const vec3 c = normalize(v2 - ref);
	return abs(2.0 * atan(dot(a, cross(b, c)), 1.0 + dot(a, b) + dot(a, c) + dot(b, c)));
}

// pbrt-v4's SampleSphericalTriangle(): barycentrics of a point uniform in the subtended solid angle
bool sampleSphericalTriangle(vec3 v0, vec3 v1, vec3 v2, vec3 ref, vec2 u, out vec3 barycentrics)
{
	barycentrics = vec3(1.0 / 3.0);
	const vec3 a = normalize(v0 - ref);
	const vec3 b = normalize(v1 - ref);
	const vec3 c = normalize(v2 - ref);

	vec3 nAB = cross(a, b);
	vec3 nBC = cross(b, c);
	vec3 nCA = cross(c, a);
	if (dot(nAB, nAB) == 0.0 || dot(nBC, nBC) == 0.0 || dot(nCA, nCA) == 0.0) {
		return false;
	}
	nAB = normalize(nAB);
	nBC = normalize(nBC);
	nCA = normalize(nCA);

	const float alpha = lightAngleBetween(nAB, -nCA);
	const float beta = lightAngleBetween(nBC, -nAB);
	const float gamma = lightAngleBetween(nCA, -nBC);

	const float areaPi = alpha + beta + gamma;
	const float subAreaPi = CRT_PI + u.x * (areaPi - CRT_PI);
	const float cosAlpha = cos(alpha);
	const float sinAlpha = sin(alpha);
	const float sinPhi = sin(subAreaPi) * cosAlpha - cos(subAreaPi) * sinAlpha;
	const float cosPhi = cos(subAreaPi) * cosAlpha + sin(subAreaPi) * sinAlpha;
	const float k1 = cosPhi + cosAlpha;
	const float k2 = sinPhi - sinAlpha * dot(a, b);
	float cosBp = (k2 + differenceOfProducts(k2, cosPhi, k1, sinPhi) * cosAlpha) / (sumOfProducts(k2, sinPhi, k1, cosPhi) * sinAlpha);
	// NaN for a triangle filling nearly the whole hemisphere, which the solid-angle bound keeps on
	// area sampling; this keeps a stray one finite
	cosBp = (cosBp == cosBp && abs(cosBp) < CRT_INFINITY) ? clamp(cosBp, -1.0, 1.0) : 1.0;
	const float sinBp = lightSafeSqrt(1.0 - cosBp * cosBp);
	const vec3 cp = cosBp * a + sinBp * normalize(c - dot(c, a) * a);

	const float cosTheta = 1.0 - u.y * (1.0 - dot(cp, b));
	const float sinTheta = lightSafeSqrt(1.0 - cosTheta * cosTheta);
	const vec3 w = cosTheta * b + sinTheta * normalize(cp - dot(cp, b) * b);

	const vec3 e1 = v1 - v0;
	const vec3 e2 = v2 - v0;
	const vec3 s1 = cross(w, e2);
	const float divisor = dot(s1, e1);
	if (divisor == 0.0) {
		return true;
	}
	const float invDivisor = 1.0 / divisor;
	const vec3 sv = ref - v0;
	float b1 = dot(sv, s1) * invDivisor;
	float b2 = dot(w, cross(sv, e1)) * invDivisor;
	b1 = clamp(b1, 0.0, 1.0);
	b2 = clamp(b2, 0.0, 1.0);
	if (b1 + b2 > 1.0) {
		const float sum = b1 + b2;
		b1 /= sum;
		b2 /= sum;
	}
	barycentrics = vec3(1.0 - b1 - b2, b1, b2);
	return true;
}

bool sampleTriangleLight(vec3 v0, vec3 v1, vec3 v2, vec3 ref, vec2 u, out LightSample s, out vec3 barycentrics)
{
	barycentrics = vec3(1.0 / 3.0);
	const vec3 crossed = cross(v1 - v0, v2 - v0);
	const float crossLength = length(crossed);
	if (!(crossLength > 0.0)) {
		s.pdf = 0.0;
		return false;
	}
	const vec3 normal = crossed / crossLength;
	const float solidAngle = triangleSolidAngle(v0, v1, v2, ref);
	if (!(solidAngle >= CRT_MIN_SPHERICAL_SOLID_ANGLE && solidAngle <= CRT_MAX_SPHERICAL_SOLID_ANGLE)) {
		barycentrics = sampleUniformTriangle(u);
		const vec3 point = barycentrics.x * v0 + barycentrics.y * v1 + barycentrics.z * v2;
		return finishAreaSample(ref, point, normal, 2.0 / crossLength, s);
	}

	if (!sampleSphericalTriangle(v0, v1, v2, ref, u, barycentrics)) {
		s.pdf = 0.0;
		return false;
	}
	const vec3 point = barycentrics.x * v0 + barycentrics.y * v1 + barycentrics.z * v2;
	const vec3 toLight = point - ref;
	s.point = point;
	s.normal = normal;
	s.distance = length(toLight);
	s.direction = s.distance > 0.0 ? toLight / s.distance : normal;
	s.pdf = 1.0 / solidAngle;
	return s.distance > 0.0;
}

float triangleLightPdf(vec3 v0, vec3 v1, vec3 v2, vec3 ref, vec3 lightPoint)
{
	const vec3 crossed = cross(v1 - v0, v2 - v0);
	const float crossLength = length(crossed);
	if (!(crossLength > 0.0)) {
		return 0.0;
	}
	const float solidAngle = triangleSolidAngle(v0, v1, v2, ref);
	if (!(solidAngle >= CRT_MIN_SPHERICAL_SOLID_ANGLE && solidAngle <= CRT_MAX_SPHERICAL_SOLID_ANGLE)) {
		return areaToSolidAngle(2.0 / crossLength, ref, lightPoint, crossed / crossLength);
	}
	return 1.0 / solidAngle;
}

//---------------------------------------------------------------- capped cylinder

float cylinderLightPdf(vec3 center, vec3 axisX, vec3 axisY, float radius, float height, vec3 ref, vec3 lightPoint)
{
	const vec3 axisZ = cross(axisX, axisY);
	const float area = 2.0 * CRT_PI * radius * height + 2.0 * CRT_PI * radius * radius;
	const vec3 local = lightPoint - center;
	const float along = dot(local, axisY) / height;
	const float lx = dot(local, axisX) / radius;
	const float lz = dot(local, axisZ) / radius;
	const float radial = sqrt(lx * lx + lz * lz);
	vec3 normal;
	if (abs(0.5 - abs(along)) < abs(1.0 - radial)) {
		normal = along >= 0.0 ? axisY : -axisY;
	} else {
		normal = radial > 0.0 ? (lx * axisX + lz * axisZ) / radial : axisX;
	}
	return areaToSolidAngle(1.0 / area, ref, lightPoint, normal);
}

bool sampleCylinderLight(vec3 center, vec3 axisX, vec3 axisY, float radius, float height, vec3 ref, vec2 u, out LightSample s)
{
	const vec3 axisZ = cross(axisX, axisY);
	const float sideArea = 2.0 * CRT_PI * radius * height;
	const float capArea = CRT_PI * radius * radius;
	const float area = sideArea + 2.0 * capArea;

	const float t = u.x * area;
	vec3 point;
	vec3 normal;
	if (t < sideArea) {
		const float phi = 2.0 * CRT_PI * min(t / sideArea, CRT_ONE_MINUS_EPSILON);
		const vec3 radial = cos(phi) * axisX + sin(phi) * axisZ;
		point = center + radius * radial + height * (u.y - 0.5) * axisY;
		normal = radial;
	} else {
		const float capU = (t - sideArea) / capArea;
		const bool top = capU < 1.0;
		const vec2 disk = sampleUniformDiskConcentric(vec2(min(top ? capU : capU - 1.0, CRT_ONE_MINUS_EPSILON), u.y));
		const float side = top ? 0.5 : -0.5;
		point = center + radius * (disk.x * axisX + disk.y * axisZ) + height * side * axisY;
		normal = top ? axisY : -axisY;
	}
	if (!finishAreaSample(ref, point, normal, 1.0 / area, s)) {
		return false;
	}
	s.pdf = cylinderLightPdf(center, axisX, axisY, radius, height, ref, s.point);
	return s.pdf > 0.0;
}

//---------------------------------------------------------------- punctual lights

float spotFalloff(float cosAngle, float cosOuter, float cosInner)
{
	const float scale = 1.0 / max(0.001, cosInner - cosOuter);
	const float t = clamp((cosAngle - cosOuter) * scale, 0.0, 1.0);
	return t * t;
}

// pbrt-v4's SpotLight::I(): a smoothstep in cos(angle) over the same range
float spotFalloffSmooth(float cosAngle, float cosOuter, float cosInner)
{
	const float scale = 1.0 / max(0.001, cosInner - cosOuter);
	const float t = clamp((cosAngle - cosOuter) * scale, 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

// a spot light's falloff by its flag
float spotFalloffOf(uint kind, float cosAngle, float cosOuter, float cosInner)
{
	return (kind & CRT_LIGHT_FLAG_SMOOTHSTEP) != 0u ? spotFalloffSmooth(cosAngle, cosOuter, cosInner) : spotFalloff(cosAngle, cosOuter, cosInner);
}

// What a projection or goniometric light sends towards `direction` (unit, from the light), as a multiple of its
// intensity: the image's texel where the direction pierces the frustum's image plane (0 outside it, or behind), or the
// distribution's at the direction in the light's frame (PBR 4ed 12.2.2, 12.2.3). Its image is the texture library's,
// read at the finest level
vec3 imagedLightFactor(GpuLight light, vec3 direction)
{
	const uint bits = floatBitsToUint(light.a.w);
	const uint texture = bits & 0xFFFFFu;
	const uint samplerIndex = 2u * (bits >> 20u);
	const vec3 xAxis = light.b.xyz;
	const vec3 yAxis = light.c.xyz;
	// the light shines down its -z, which a mirrored frame turns around
	const vec3 zAxis = (light.kind & CRT_LIGHT_FLAG_MIRRORED) != 0u ? -cross(xAxis, yAxis) : cross(xAxis, yAxis);
	const vec3 local = vec3(dot(direction, xAxis), dot(direction, yAxis), dot(direction, zAxis));
	vec2 uv;
	if (lightKindOf(light) == CRT_LIGHT_PROJECTION) {
		if (!(local.z < 0.0)) {
			return vec3(0.0);
		}
		const vec2 screen = local.xy / -local.z;
		if (abs(screen.x) > light.b.w || abs(screen.y) > light.c.w) {
			return vec3(0.0);
		}
		uv = vec2(0.5 * (screen.x / light.b.w + 1.0), 0.5 * (1.0 - screen.y / light.c.w));
	} else {
		uv = equalAreaSphereToSquare(local);
	}
	if (!CRT_TEXTURES) {
		return vec3(1.0);
	}
	const vec3 texel = textureLod(sampler2D(textures[nonuniformEXT(texture)], textureSamplers[nonuniformEXT(samplerIndex)]), uv, 0.0).rgb;
	// a goniometric light's distribution has no color of its own: its image's channels averaged, as pbrt-v4 reads it
	if (lightKindOf(light) == CRT_LIGHT_GONIOMETRIC) {
		return vec3((texel.r + texel.g + texel.b) / 3.0);
	}
	return texel;
}

float rangeWindow(float distance, float range)
{
	if (range <= 0.0) {
		return 1.0;
	}
	const float ratio = distance / range;
	const float ratio2 = ratio * ratio;
	return clamp(1.0 - ratio2 * ratio2, 0.0, 1.0);
}

//---------------------------------------------------------------- environment map

// the offsets of one of environmentSampling's two tables: 0 the full table, 1 the compensated one
uint envTableBase(uint table)
{
	return table * ((envHeight + 1u) + envHeight * (envWidth + 1u) + envHeight * envWidth);
}

float envTableIntegral(uint table)
{
	return table == 0u ? envIntegral : envCompensatedIntegral;
}

// pbrt's FindInterval() on the cdf of `count` + 1 entries at envData[base]
uint envFindInterval(uint base, uint count, float u)
{
	int length_ = int(count) - 1;
	int first = 1;
	while (length_ > 0) {
		const int half_ = length_ >> 1;
		const int middle = first + half_;
		if (envData[base + uint(middle)] <= u) {
			first = middle + 1;
			length_ -= half_ + 1;
		} else {
			length_ = half_;
		}
	}
	return uint(clamp(first - 1, 0, int(count) - 1));
}

float envSampleCdf(uint base, uint count, float u, out uint offset)
{
	offset = envFindInterval(base, count, u);
	float du = u - envData[base + offset];
	const float width = envData[base + offset + 1u] - envData[base + offset];
	if (width > 0.0) {
		du /= width;
	}
	return min((float(offset) + du) / float(count), CRT_ONE_MINUS_EPSILON);
}

bool sampleEnvironment(uint table, vec2 u, vec2 rotation, out vec3 direction, out float pdf)
{
	direction = vec3(0.0, 1.0, 0.0);
	pdf = 0.0;
	const uint base = envTableBase(table);
	const float integral = envTableIntegral(table);
	if (envWidth == 0u || envHeight == 0u || !(integral > 0.0)) {
		return false;
	}
	uint row;
	const float v = envSampleCdf(base, envHeight, u.y, row);
	uint column;
	const uint rowCdfBase = base + envHeight + 1u + row * (envWidth + 1u);
	const float uu = envSampleCdf(rowCdfBase, envWidth, u.x, column);
	const uint funcBase = base + envHeight + 1u + envHeight * (envWidth + 1u);
	const float uvPdf = envData[funcBase + row * envWidth + column] / integral;

	const float cosLatitude = cos((0.5 - v) * CRT_PI);
	if (!(uvPdf > 0.0) || !(cosLatitude > 1e-6)) {
		return false;
	}
	direction = environmentFromMap(equirectDirectionOf(vec2(uu, v)), rotation);
	// du dv covers 2 pi^2 cos(latitude) of solid angle
	pdf = uvPdf / (2.0 * CRT_PI * CRT_PI * cosLatitude);
	return true;
}

float environmentPdf(uint table, vec3 direction, vec2 rotation)
{
	const float integral = envTableIntegral(table);
	const float cosLatitude = lightSafeSqrt(1.0 - direction.y * direction.y);
	if (envWidth == 0u || envHeight == 0u || !(integral > 0.0) || !(cosLatitude > 1e-6)) {
		return 0.0;
	}
	const vec2 uv = equirectUv(environmentToMap(direction, rotation));
	const uint column = uint(clamp(int(uv.x * float(envWidth)), 0, int(envWidth) - 1));
	const uint row = uint(clamp(int(uv.y * float(envHeight)), 0, int(envHeight) - 1));
	const uint funcBase = envTableBase(table) + envHeight + 1u + envHeight * (envWidth + 1u);
	return envData[funcBase + row * envWidth + column] / integral / (2.0 * CRT_PI * CRT_PI * cosLatitude);
}

//---------------------------------------------------------------- the light list

// the probability the full list's alias table picks light `index`
float lightPmf(uint index)
{
	return lights[index].power * lightHeader.invTotalPower;
}

// the probability the delta lights' alias table picks light `index` (< deltaCount)
float deltaLightPmf(uint index)
{
	return lights[index].power * lightHeader.invDeltaPower;
}

// Vose's alias method: one draw picks a light in proportion to its power
uint sampleLightIndex(float u)
{
	const uint count = lightHeader.count;
	const uint offset = min(uint(u * float(count)), count - 1u);
	const float up = min(u * float(count) - float(offset), CRT_ONE_MINUS_EPSILON);
	return up < lights[offset].aliasQ ? offset : lights[offset].aliasIndex;
}

uint sampleDeltaLightIndex(float u)
{
	const uint count = lightHeader.deltaCount;
	const uint offset = min(uint(u * float(count)), count - 1u);
	const float up = min(u * float(count) - float(offset), CRT_ONE_MINUS_EPSILON);
	return up < lights[offset].deltaAliasQ ? offset : lights[offset].deltaAliasIndex;
}

// `kind` with or without flags
bool isDeltaLight(uint kind)
{
	kind &= CRT_LIGHT_KIND_MASK;
	return kind == CRT_LIGHT_PROJECTION || kind == CRT_LIGHT_GONIOMETRIC || kind == CRT_LIGHT_POINT || kind == CRT_LIGHT_SPOT || kind == CRT_LIGHT_DIRECTIONAL;
}

// what a light emits before any texture or falloff: its RGB radiance in the exact RGB build, its packed reference times
// its scale in a spectral one (src/rt_lights.h GpuLight::spectrum)
Spectrum lightSpectrum(GpuLight light)
{
#ifndef CRT_SPECTRAL
	return light.radiance;
#else
	return spectrumScale(spectrumFromRef(sceneSpectrumRef(light.spectrum), vec3(1.0)), uintBitsToFloat(light.spectrumScale));
#endif
}

#ifndef CRT_SPECTRAL
// the radiance a triangle light emits at a point: its emission times its emissive texel there, and
// nothing where its material's alpha test cuts the surface away - a BSDF sample can never hit such a
// point, so a light sample must not find light there either (crt_traverse.glsl cutAway())
vec3 triangleLightRadiance(GpuLight light, vec3 barycentrics)
{
	const int layer = floatBitsToInt(light.b.w);
	const ClassicMaterial material = classicMaterial(floatBitsToUint(light.c.w));
	const bool cutout = material.alphaCutoff > 0.0 && material.albedoLayer >= 0;
	const bool textured = CRT_TEXTURES && (light.kind & CRT_LIGHT_FLAG_TEXTURED) != 0u;
	if (layer < 0 && !cutout && !textured) {
		return light.radiance;
	}
	const GpuTriangleAttributes attributes = triangleAttributes[floatBitsToUint(light.a.w)];
	const vec2 uv = barycentrics.x * vec2(attributes.n0.w, attributes.vAndSurface.x) + barycentrics.y * vec2(attributes.n1.w, attributes.vAndSurface.y) + barycentrics.z * vec2(attributes.n2.w, attributes.vAndSurface.z);
	if (cutout && textureLod(materialTextures, vec3(uv, float(material.albedoLayer)), 0.0).a < material.alphaCutoff) {
		return vec3(0.0);
	}
	vec3 radiance = layer < 0 ? light.radiance : light.radiance * srgbToLinear(textureLod(materialTextures, vec3(uv, float(layer)), 0.0).rgb);
	if (textured) {
		radiance *= materialEmissionTexture(floatBitsToUint(light.c.w), textureSiteAt(uv));
	}
	return radiance;
}
#endif

#ifdef CRT_SPECTRAL
// a triangle light's texel at a point, in the working space, as the tint of its emission; false where its material's
// alpha test cuts the surface away
bool triangleLightTint(GpuLight light, vec3 barycentrics, out vec3 tint)
{
	tint = vec3(1.0);
	const int layer = floatBitsToInt(light.b.w);
	const ClassicMaterial material = classicMaterial(floatBitsToUint(light.c.w));
	const bool cutout = material.alphaCutoff > 0.0 && material.albedoLayer >= 0;
	const bool textured = CRT_TEXTURES && (light.kind & CRT_LIGHT_FLAG_TEXTURED) != 0u;
	if (layer < 0 && !cutout && !textured) {
		return true;
	}
	const GpuTriangleAttributes attributes = triangleAttributes[floatBitsToUint(light.a.w)];
	const vec2 uv = barycentrics.x * vec2(attributes.n0.w, attributes.vAndSurface.x) + barycentrics.y * vec2(attributes.n1.w, attributes.vAndSurface.y) + barycentrics.z * vec2(attributes.n2.w, attributes.vAndSurface.z);
	if (cutout && textureLod(materialTextures, vec3(uv, float(material.albedoLayer)), 0.0).a < material.alphaCutoff) {
		return false;
	}
	if (layer >= 0) {
		tint = srgbToWorking(srgbToLinear(textureLod(materialTextures, vec3(uv, float(layer)), 0.0).rgb));
	}
	if (textured) {
		tint *= srgbToWorking(materialEmissionTexture(floatBitsToUint(light.c.w), textureSiteAt(uv)));
	}
	return true;
}
#endif

// A one-sided area light's sample faces the reference point: the light emits along its front normal, the sampler's
// normal or, for a record flagged CRT_LIGHT_FLAG_REVERSED, its opposite (src/rt_lights.cpp)
bool areaSampleFacesReference(GpuLight light, LightSample s)
{
	if ((light.kind & CRT_LIGHT_FLAG_ONE_SIDED) == 0u) {
		return true;
	}
	const vec3 front = (light.kind & CRT_LIGHT_FLAG_REVERSED) != 0u ? -s.normal : s.normal;
	return dot(front, s.direction) < 0.0;
}

// A textured shape emitter's texel at a sampled point, as a multiple of its emission: the point carried into its
// instance's object space, the shape's uv there, and its material's emission textures (crt_texture.glsl)
vec3 shapeLightTexture(GpuLight light, vec3 point)
{
	if (!CRT_TEXTURES || (light.kind & CRT_LIGHT_FLAG_TEXTURED) == 0u) {
		return vec3(1.0);
	}
	const uint kind = lightKindOf(light);
	const uint slot = floatBitsToUint(kind == CRT_LIGHT_SPHERE ? light.b.w : kind == CRT_LIGHT_RECTANGLE ? light.a.w : light.c.w);
	const GpuInstance instance = instances[slot];
	const vec3 p = toObjectPoint(instance, point);
	const uint shape = instance.triangleBase;
	return materialEmissionTexture(instance.materialBase, textureSiteAt(shapeUv(shape, shapePart(shape, p), p)));
}

// The one direction from `ref` towards a point, spot or directional light, how far away it is and
// what arrives along it - a delta distribution, so there is no density to report. Kept apart from
// sampleLight() so a kernel that samples delta lights alone reads no other light's resources.
// `lightNormal` is the surface normal at the light, which a delta light does not have: zero
bool sampleDeltaLight(GpuLight light, vec3 ref, out vec3 direction, out float distance, out Spectrum radiance, out vec3 lightNormal)
{
	direction = vec3(0.0, 1.0, 0.0);
	distance = 0.0;
	radiance = spectrumConst(0.0);
	lightNormal = vec3(0.0);
#ifndef CRT_SPECTRAL
	if (lightKindOf(light) == CRT_LIGHT_DIRECTIONAL) {
		direction = -light.a.xyz;
		distance = CRT_INFINITY;
		radiance = light.radiance;
		return true;
	}
	const vec3 toLight = light.a.xyz - ref;
	const float distanceSquared = dot(toLight, toLight);
	if (!(distanceSquared > 0.0)) {
		return false;
	}
	distance = sqrt(distanceSquared);
	direction = toLight / distance;
	const uint deltaKind = lightKindOf(light);
	if (deltaKind == CRT_LIGHT_PROJECTION || deltaKind == CRT_LIGHT_GONIOMETRIC) {
		// no range: pbrt's imaged lights fall off as the inverse square alone
		radiance = light.radiance * imagedLightFactor(light, -direction) / distanceSquared;
		return true;
	}
	radiance = light.radiance * rangeWindow(distance, light.a.w) / distanceSquared;
	if (lightKindOf(light) == CRT_LIGHT_SPOT) {
		radiance *= spotFalloffOf(light.kind, dot(light.b.xyz, -direction), light.b.w, light.c.x);
	}
#else
	// the spectrum is evaluated once, after the geometry, so the shader inlines it once
	float falloff = 1.0;
	if (lightKindOf(light) == CRT_LIGHT_DIRECTIONAL) {
		direction = -light.a.xyz;
		distance = CRT_INFINITY;
	} else {
		const vec3 toLight = light.a.xyz - ref;
		const float distanceSquared = dot(toLight, toLight);
		if (!(distanceSquared > 0.0)) {
			return false;
		}
		distance = sqrt(distanceSquared);
		direction = toLight / distance;
		falloff = rangeWindow(distance, light.a.w) / distanceSquared;
		if (lightKindOf(light) == CRT_LIGHT_SPOT) {
			falloff *= spotFalloffOf(light.kind, dot(light.b.xyz, -direction), light.b.w, light.c.x);
		}
		if (lightKindOf(light) == CRT_LIGHT_PROJECTION || lightKindOf(light) == CRT_LIGHT_GONIOMETRIC) {
			// the image's texel tints the light's spectrum, as an emissive texel does
			radiance = spectrumScale(spectrumFromRefOrRgb(sceneSpectrumRef(light.spectrum), srgbToWorking(imagedLightFactor(light, -direction)), CRT_ROLE_ILLUMINANT),
				uintBitsToFloat(light.spectrumScale) / distanceSquared);
			return true;
		}
	}
	radiance = spectrumScale(lightSpectrum(light), falloff);
#endif
	return true;
}

// A direction from `ref` towards light `index`, what arrives along it, its solid-angle density
// (1 for a delta light) and how far the shadow ray may travel before it would reach the light.
// `lightNormal` is the surface normal at the point sampled on it, which the shadow ray's far end is
// offset by (zero for every light that has no surface: delta lights, the environment). `environmentTable` is 1 under MIS (the compensated table), 0 otherwise. False for
// no sample
#ifdef CRT_SPECTRAL
// THE SPECTRAL BUILD'S sampleLight(): the same geometry, with the emission evaluated once at the end, whatever the kind,
// so the kernel inlines one evaluation of a spectrum rather than one per kind. What a kind emits is a reference, a
// tint (a triangle's texel) and a scale, or for the environment its texel as an RGB emitter
bool sampleLight(uint index, vec3 ref, vec2 u, uint environmentTable, out vec3 direction, out float distance, out Spectrum radiance, out float pdf, out vec3 lightNormal)
{
	const GpuLight light = lights[index];
	direction = vec3(0.0, 1.0, 0.0);
	distance = 0.0;
	radiance = spectrumConst(0.0);
	pdf = 0.0;
	lightNormal = vec3(0.0);
	uint emission = sceneSpectrumRef(light.spectrum);
	vec3 tint = vec3(1.0);
	float scale = uintBitsToFloat(light.spectrumScale);

	if (isDeltaLight(light.kind)) {
		pdf = 1.0;
		float falloff = 1.0;
		if (lightKindOf(light) == CRT_LIGHT_DIRECTIONAL) {
			direction = -light.a.xyz;
			distance = CRT_INFINITY;
		} else {
			const vec3 toLight = light.a.xyz - ref;
			const float distanceSquared = dot(toLight, toLight);
			if (!(distanceSquared > 0.0)) {
				return false;
			}
			distance = sqrt(distanceSquared);
			direction = toLight / distance;
			falloff = rangeWindow(distance, light.a.w) / distanceSquared;
			if (lightKindOf(light) == CRT_LIGHT_SPOT) {
				falloff *= spotFalloffOf(light.kind, dot(light.b.xyz, -direction), light.b.w, light.c.x);
			}
			if (lightKindOf(light) == CRT_LIGHT_PROJECTION || lightKindOf(light) == CRT_LIGHT_GONIOMETRIC) {
				falloff = 1.0 / distanceSquared;
				tint = srgbToWorking(imagedLightFactor(light, -direction));
			}
		}
		scale *= falloff;
	} else if (lightKindOf(light) == CRT_LIGHT_ENVIRONMENT) {
		if (!sampleEnvironment(environmentTable, u, environmentRotation(), direction, pdf)) {
			return false;
		}
		distance = CRT_INFINITY;
		emission = CRT_SPECTRAL_NONE;
		tint = srgbToWorking(textureLod(environmentMap, equirectUv(environmentToMap(direction, environmentRotation())), 0.0).rgb);
		scale = pc.environmentIntensity;
	} else {
		LightSample s;
		vec3 barycentrics = vec3(0.0);
		bool ok = false;
		switch (lightKindOf(light)) {
		case CRT_LIGHT_SPHERE:
			ok = sampleSphereLight(light.a.xyz, light.a.w, ref, u, s);
			break;
		case CRT_LIGHT_RECTANGLE:
			ok = sampleRectangleLight(light.a.xyz, light.b.xyz, light.c.xyz, ref, u, s);
			break;
		case CRT_LIGHT_CYLINDER:
			ok = sampleCylinderLight(light.a.xyz, light.b.xyz, light.c.xyz, light.a.w, light.b.w, ref, u, s);
			break;
		case CRT_LIGHT_TRIANGLE:
			ok = sampleTriangleLight(light.a.xyz, light.b.xyz, light.c.xyz, ref, u, s, barycentrics);
			if (ok && !triangleLightTint(light, barycentrics, tint)) {
				return false;
			}
			break;
		}
		if (!ok || !areaSampleFacesReference(light, s)) {
			return false;
		}
		if (lightKindOf(light) != CRT_LIGHT_TRIANGLE) {
			tint *= srgbToWorking(shapeLightTexture(light, s.point));
		}
		direction = s.direction;
		distance = s.distance;
		pdf = s.pdf;
		lightNormal = s.normal;
		if (!(pdf > 0.0)) {
			return false;
		}
	}
	radiance = spectrumScale(spectrumFromRefOrRgb(emission, tint, CRT_ROLE_ILLUMINANT), scale);
	return true;
}
#else
bool sampleLight(uint index, vec3 ref, vec2 u, uint environmentTable, out vec3 direction, out float distance, out vec3 radiance, out float pdf, out vec3 lightNormal)
{
	const GpuLight light = lights[index];
	direction = vec3(0.0, 1.0, 0.0);
	distance = 0.0;
	radiance = vec3(0.0);
	pdf = 0.0;
	lightNormal = vec3(0.0);
	if (isDeltaLight(light.kind)) {
		pdf = 1.0;
		return sampleDeltaLight(light, ref, direction, distance, radiance, lightNormal);
	}

	LightSample s;
	vec3 barycentrics = vec3(0.0);
	bool ok = false;
	switch (lightKindOf(light)) {
	case CRT_LIGHT_SPHERE:
		ok = sampleSphereLight(light.a.xyz, light.a.w, ref, u, s);
		radiance = light.radiance;
		break;
	case CRT_LIGHT_RECTANGLE:
		ok = sampleRectangleLight(light.a.xyz, light.b.xyz, light.c.xyz, ref, u, s);
		radiance = light.radiance;
		break;
	case CRT_LIGHT_CYLINDER:
		ok = sampleCylinderLight(light.a.xyz, light.b.xyz, light.c.xyz, light.a.w, light.b.w, ref, u, s);
		radiance = light.radiance;
		break;
	case CRT_LIGHT_TRIANGLE:
		ok = sampleTriangleLight(light.a.xyz, light.b.xyz, light.c.xyz, ref, u, s, barycentrics);
		if (ok) {
			radiance = triangleLightRadiance(light, barycentrics);
		}
		break;
	case CRT_LIGHT_ENVIRONMENT:
		if (!sampleEnvironment(environmentTable, u, environmentRotation(), direction, pdf)) {
			return false;
		}
		distance = CRT_INFINITY;
		radiance = textureLod(environmentMap, equirectUv(environmentToMap(direction, environmentRotation())), 0.0).rgb * pc.environmentIntensity;
		return true;
	}
	if (!ok || !areaSampleFacesReference(light, s)) {
		return false;
	}
	if (lightKindOf(light) != CRT_LIGHT_TRIANGLE) {
		radiance *= shapeLightTexture(light, s.point);
	}
	direction = s.direction;
	distance = s.distance;
	pdf = s.pdf;
	lightNormal = s.normal;
	return pdf > 0.0;
}
#endif

// The density sampleLight() gives the direction from `ref` to `lightPoint` on area light `index`:
// what kernel 04 weighs a BSDF sample that hit the light against
float areaLightPdf(uint index, vec3 ref, vec3 lightPoint)
{
	const GpuLight light = lights[index];
	switch (lightKindOf(light)) {
	case CRT_LIGHT_SPHERE:
		return sphereLightPdf(light.a.xyz, light.a.w, ref, lightPoint);
	case CRT_LIGHT_RECTANGLE:
		return rectangleLightPdf(light.a.xyz, light.b.xyz, light.c.xyz, ref, lightPoint);
	case CRT_LIGHT_CYLINDER:
		return cylinderLightPdf(light.a.xyz, light.b.xyz, light.c.xyz, light.a.w, light.b.w, ref, lightPoint);
	case CRT_LIGHT_TRIANGLE:
		return triangleLightPdf(light.a.xyz, light.b.xyz, light.c.xyz, ref, lightPoint);
	}
	return 0.0;
}
