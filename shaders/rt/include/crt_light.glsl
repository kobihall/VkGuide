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

float differenceOfProducts(float a, float b, float c, float d)
{
	const float cd = c * d;
	const float difference = fma(a, b, -cd);
	const float error = fma(-c, d, cd);
	return difference + error;
}

float sumOfProducts(float a, float b, float c, float d)
{
	const float cd = c * d;
	const float sum = fma(a, b, cd);
	const float error = fma(c, d, -cd);
	return sum + error;
}

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

vec3 sampleUniformTriangle(vec2 u)
{
	float b0;
	float b1;
	if (u.x < u.y) {
		b0 = u.x / 2.0;
		b1 = u.y - b0;
	} else {
		b1 = u.y / 2.0;
		b0 = u.x - b1;
	}
	return vec3(b0, b1, 1.0 - b0 - b1);
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

vec2 sampleUniformDiskConcentric(vec2 u)
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

vec3 equirectDirectionOf(vec2 uv)
{
	const float phi = (uv.x - 0.5) * 2.0 * CRT_PI;
	const float latitude = (0.5 - uv.y) * CRT_PI;
	const float cosLatitude = cos(latitude);
	return vec3(cosLatitude * sin(phi), sin(latitude), -cosLatitude * cos(phi));
}

bool sampleEnvironment(uint table, vec2 u, out vec3 direction, out float pdf)
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
	direction = equirectDirectionOf(vec2(uu, v));
	// du dv covers 2 pi^2 cos(latitude) of solid angle
	pdf = uvPdf / (2.0 * CRT_PI * CRT_PI * cosLatitude);
	return true;
}

float environmentPdf(uint table, vec3 direction)
{
	const float integral = envTableIntegral(table);
	const float cosLatitude = lightSafeSqrt(1.0 - direction.y * direction.y);
	if (envWidth == 0u || envHeight == 0u || !(integral > 0.0) || !(cosLatitude > 1e-6)) {
		return 0.0;
	}
	const vec2 uv = equirectUv(direction);
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

bool isDeltaLight(uint kind)
{
	return kind == CRT_LIGHT_POINT || kind == CRT_LIGHT_SPOT || kind == CRT_LIGHT_DIRECTIONAL;
}

// the radiance a triangle light emits at a point: its emission times its emissive texel there, and
// nothing where its material's alpha test cuts the surface away - a BSDF sample can never hit such a
// point, so a light sample must not find light there either (crt_traverse.glsl cutAway())
vec3 triangleLightRadiance(GpuLight light, vec3 barycentrics)
{
	const int layer = floatBitsToInt(light.b.w);
	const GpuMaterial material = materials[floatBitsToUint(light.c.w)];
	const bool cutout = material.alphaCutoff > 0.0 && material.albedoLayer >= 0;
	if (layer < 0 && !cutout) {
		return light.radiance;
	}
	const GpuTriangleAttributes attributes = triangleAttributes[floatBitsToUint(light.a.w)];
	const vec2 uv = barycentrics.x * vec2(attributes.n0.w, attributes.vAndSurface.x) + barycentrics.y * vec2(attributes.n1.w, attributes.vAndSurface.y) + barycentrics.z * vec2(attributes.n2.w, attributes.vAndSurface.z);
	if (cutout && textureLod(materialTextures, vec3(uv, float(material.albedoLayer)), 0.0).a < material.alphaCutoff) {
		return vec3(0.0);
	}
	return layer < 0 ? light.radiance : light.radiance * srgbToLinear(textureLod(materialTextures, vec3(uv, float(layer)), 0.0).rgb);
}

// The one direction from `ref` towards a point, spot or directional light, how far away it is and
// what arrives along it - a delta distribution, so there is no density to report. Kept apart from
// sampleLight() so a kernel that samples delta lights alone reads no other light's resources
bool sampleDeltaLight(GpuLight light, vec3 ref, out vec3 direction, out float distance, out vec3 radiance)
{
	direction = vec3(0.0, 1.0, 0.0);
	distance = 0.0;
	radiance = vec3(0.0);
	if (light.kind == CRT_LIGHT_DIRECTIONAL) {
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
	radiance = light.radiance * rangeWindow(distance, light.a.w) / distanceSquared;
	if (light.kind == CRT_LIGHT_SPOT) {
		radiance *= spotFalloff(dot(light.b.xyz, -direction), light.b.w, light.c.x);
	}
	return true;
}

// A direction from `ref` towards light `index`, what arrives along it, its solid-angle density
// (1 for a delta light) and how far the shadow ray may travel before it would reach the light.
// `environmentTable` is 1 under MIS (the compensated table), 0 otherwise. False for no sample
bool sampleLight(uint index, vec3 ref, vec2 u, uint environmentTable, out vec3 direction, out float distance, out vec3 radiance, out float pdf)
{
	const GpuLight light = lights[index];
	direction = vec3(0.0, 1.0, 0.0);
	distance = 0.0;
	radiance = vec3(0.0);
	pdf = 0.0;
	if (isDeltaLight(light.kind)) {
		pdf = 1.0;
		return sampleDeltaLight(light, ref, direction, distance, radiance);
	}

	LightSample s;
	vec3 barycentrics = vec3(0.0);
	bool ok = false;
	switch (light.kind) {
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
		if (!sampleEnvironment(environmentTable, u, direction, pdf)) {
			return false;
		}
		distance = CRT_INFINITY;
		radiance = textureLod(environmentMap, equirectUv(direction), 0.0).rgb * pc.environmentIntensity;
		return true;
	}
	if (!ok) {
		return false;
	}
	direction = s.direction;
	distance = s.distance;
	pdf = s.pdf;
	return pdf > 0.0;
}

// The density sampleLight() gives the direction from `ref` to `lightPoint` on area light `index`:
// what kernel 04 weighs a BSDF sample that hit the light against
float areaLightPdf(uint index, vec3 ref, vec3 lightPoint)
{
	const GpuLight light = lights[index];
	switch (light.kind) {
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
