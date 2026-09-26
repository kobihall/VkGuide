#include <light_sampling.h>

#include <algorithm>
#include <cmath>
#include <numeric>

#include <glm/packing.hpp>

namespace {

float safeSqrt(float x)
{
	return std::sqrt(std::max(x, 0.f));
}

float safeAsin(float x)
{
	return std::asin(std::clamp(x, -1.f, 1.f));
}

// the largest float below 1, so a sample coordinate never reaches the far edge
constexpr float ONE_MINUS_EPSILON = 0x1.fffffep-1f;

// pbrt's GramSchmidt(): v with its component along the unit vector w removed
glm::vec3 gramSchmidt(const glm::vec3& v, const glm::vec3& w)
{
	return v - glm::dot(v, w) * w;
}

// the area-measure density `pdfArea` at `point`, seen from `ref`, as a solid-angle density; 0 when
// the surface is seen exactly edge-on
float areaToSolidAngle(float pdfArea, const glm::vec3& ref, const glm::vec3& point, const glm::vec3& normal)
{
	const glm::vec3 toLight = point - ref;
	const float distanceSquared = glm::dot(toLight, toLight);
	if (distanceSquared <= 0.f) {
		return 0.f;
	}
	const float cosLight = std::abs(glm::dot(normal, toLight)) / std::sqrt(distanceSquared);
	if (cosLight <= 0.f) {
		return 0.f;
	}
	return pdfArea * distanceSquared / cosLight;
}

// fills `out` for a point sampled by area with density `pdfArea`; false where the result is unusable
bool finishAreaSample(const glm::vec3& ref, const glm::vec3& point, const glm::vec3& normal, float pdfArea, LightSample& out)
{
	const glm::vec3 toLight = point - ref;
	const float distanceSquared = glm::dot(toLight, toLight);
	if (distanceSquared <= 0.f) {
		return false;
	}
	out.point = point;
	out.normal = normal;
	out.distance = std::sqrt(distanceSquared);
	out.direction = toLight / out.distance;
	out.pdf = areaToSolidAngle(pdfArea, ref, point, normal);
	return out.pdf > 0.f;
}

//---------------------------------------------------------------- spherical rectangle

// Urena et al. 2013 as pbrt-v4's SampleSphericalRectangle() sets it up: the rectangle in a frame
// whose z points from it towards the reference point, the four edge-plane normals, the interior
// angles and from them the solid angle
struct SphericalRectangle {
	glm::vec3 x { 0.f };
	glm::vec3 y { 0.f };
	glm::vec3 z { 0.f };
	float x0 { 0.f };
	float y0 { 0.f };
	float z0 { 0.f };
	float x1 { 0.f };
	float y1 { 0.f };
	float b0 { 0.f };
	float b1 { 0.f };
	float g0 { 0.f };
	float g1 { 0.f };
	float g2 { 0.f };
	float g3 { 0.f };
	float solidAngle { 0.f };
};

SphericalRectangle setupSphericalRectangle(const glm::vec3& corner, const glm::vec3& edgeU, const glm::vec3& edgeV, const glm::vec3& ref)
{
	SphericalRectangle r;
	const float lengthU = glm::length(edgeU);
	const float lengthV = glm::length(edgeV);
	r.x = edgeU / lengthU;
	r.y = edgeV / lengthV;
	r.z = glm::cross(r.x, r.y);
	const glm::vec3 d = corner - ref;
	r.x0 = glm::dot(d, r.x);
	r.y0 = glm::dot(d, r.y);
	r.z0 = glm::dot(d, r.z);
	//z points away from the rectangle, towards the reference point
	if (r.z0 > 0.f) {
		r.z = -r.z;
		r.z0 = -r.z0;
	}
	r.x1 = r.x0 + lengthU;
	r.y1 = r.y0 + lengthV;
	//in the rectangle's plane it is seen edge-on, or from inside itself: no solid angle to sample
	if (!(r.z0 < -1e-7f * (lengthU + lengthV))) {
		return r;
	}

	const glm::vec3 v00(r.x0, r.y0, r.z0);
	const glm::vec3 v01(r.x0, r.y1, r.z0);
	const glm::vec3 v10(r.x1, r.y0, r.z0);
	const glm::vec3 v11(r.x1, r.y1, r.z0);
	const glm::vec3 n0 = glm::normalize(glm::cross(v00, v10));
	const glm::vec3 n1 = glm::normalize(glm::cross(v10, v11));
	const glm::vec3 n2 = glm::normalize(glm::cross(v11, v01));
	const glm::vec3 n3 = glm::normalize(glm::cross(v01, v00));
	r.g0 = lightAngleBetween(-n0, n1);
	r.g1 = lightAngleBetween(-n1, n2);
	r.g2 = lightAngleBetween(-n2, n3);
	r.g3 = lightAngleBetween(-n3, n0);
	r.b0 = n0.z;
	r.b1 = n2.z;
	r.solidAngle = std::max(r.g0 + r.g1 + r.g2 + r.g3 - 2.f * LIGHT_PI, 0.f);
	return r;
}

bool sphericalRectangleUsable(const SphericalRectangle& r)
{
	return r.solidAngle >= LIGHT_MIN_SPHERICAL_SOLID_ANGLE && r.solidAngle <= LIGHT_MAX_SPHERICAL_SOLID_ANGLE;
}

//---------------------------------------------------------------- spherical triangle

// pbrt-v4's SampleSphericalTriangle(): the barycentrics of a point uniformly distributed in the
// solid angle the triangle subtends from `ref`. False for a degenerate triangle
bool sampleSphericalTriangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& ref, const glm::vec2& u, glm::vec3& barycentrics)
{
	const glm::vec3 a = glm::normalize(v0 - ref);
	const glm::vec3 b = glm::normalize(v1 - ref);
	const glm::vec3 c = glm::normalize(v2 - ref);

	glm::vec3 nAB = glm::cross(a, b);
	glm::vec3 nBC = glm::cross(b, c);
	glm::vec3 nCA = glm::cross(c, a);
	if (glm::dot(nAB, nAB) == 0.f || glm::dot(nBC, nBC) == 0.f || glm::dot(nCA, nCA) == 0.f) {
		return false;
	}
	nAB = glm::normalize(nAB);
	nBC = glm::normalize(nBC);
	nCA = glm::normalize(nCA);

	//the spherical triangle's angles at its vertices
	const float alpha = lightAngleBetween(nAB, -nCA);
	const float beta = lightAngleBetween(nBC, -nAB);
	const float gamma = lightAngleBetween(nCA, -nBC);

	//a uniformly chosen sub-area A' of the whole, and the point c' along the arc a-c that bounds it
	const float areaPi = alpha + beta + gamma;
	const float subAreaPi = LIGHT_PI + u.x * (areaPi - LIGHT_PI);
	const float cosAlpha = std::cos(alpha);
	const float sinAlpha = std::sin(alpha);
	const float sinPhi = std::sin(subAreaPi) * cosAlpha - std::cos(subAreaPi) * sinAlpha;
	const float cosPhi = std::cos(subAreaPi) * cosAlpha + std::sin(subAreaPi) * sinAlpha;
	const float k1 = cosPhi + cosAlpha;
	const float k2 = sinPhi - sinAlpha * glm::dot(a, b);
	float cosBp = (k2 + differenceOfProducts(k2, cosPhi, k1, sinPhi) * cosAlpha) / (sumOfProducts(k2, sinPhi, k1, cosPhi) * sinAlpha);
	//NaN when the triangle fills nearly the whole hemisphere; the caller's solid-angle bound keeps
	//such triangles on area sampling, and this keeps a stray one finite
	cosBp = std::isfinite(cosBp) ? std::clamp(cosBp, -1.f, 1.f) : 1.f;
	const float sinBp = safeSqrt(1.f - cosBp * cosBp);
	const glm::vec3 cp = cosBp * a + sinBp * glm::normalize(gramSchmidt(c, a));

	//then a point along the arc from b to c', uniformly in the area it sweeps
	const float cosTheta = 1.f - u.y * (1.f - glm::dot(cp, b));
	const float sinTheta = safeSqrt(1.f - cosTheta * cosTheta);
	const glm::vec3 w = cosTheta * b + sinTheta * glm::normalize(gramSchmidt(cp, b));

	//the barycentrics of that direction's hit on the triangle
	const glm::vec3 e1 = v1 - v0;
	const glm::vec3 e2 = v2 - v0;
	const glm::vec3 s1 = glm::cross(w, e2);
	const float divisor = glm::dot(s1, e1);
	if (divisor == 0.f) {
		barycentrics = glm::vec3(1.f / 3.f);
		return true;
	}
	const float invDivisor = 1.f / divisor;
	const glm::vec3 s = ref - v0;
	float b1 = glm::dot(s, s1) * invDivisor;
	float b2 = glm::dot(w, glm::cross(s, e1)) * invDivisor;
	b1 = std::clamp(b1, 0.f, 1.f);
	b2 = std::clamp(b2, 0.f, 1.f);
	if (b1 + b2 > 1.f) {
		const float sum = b1 + b2;
		b1 /= sum;
		b2 /= sum;
	}
	barycentrics = glm::vec3(1.f - b1 - b2, b1, b2);
	return true;
}

// pbrt-v4's SampleUniformTriangle(): barycentrics uniform over the triangle's area
glm::vec3 sampleUniformTriangle(const glm::vec2& u)
{
	float b0;
	float b1;
	if (u.x < u.y) {
		b0 = u.x / 2.f;
		b1 = u.y - b0;
	} else {
		b1 = u.y / 2.f;
		b0 = u.x - b1;
	}
	return glm::vec3(b0, b1, 1.f - b0 - b1);
}

// pbrt-v4's SampleUniformDiskConcentric(): Shirley and Chiu's low-distortion square-to-disk map
glm::vec2 sampleUniformDiskConcentric(const glm::vec2& u)
{
	const glm::vec2 offset = 2.f * u - 1.f;
	if (offset.x == 0.f && offset.y == 0.f) {
		return glm::vec2(0.f);
	}
	float r;
	float theta;
	if (std::abs(offset.x) > std::abs(offset.y)) {
		r = offset.x;
		theta = (LIGHT_PI / 4.f) * (offset.y / offset.x);
	} else {
		r = offset.y;
		theta = LIGHT_PI / 2.f - (LIGHT_PI / 4.f) * (offset.x / offset.y);
	}
	return r * glm::vec2(std::cos(theta), std::sin(theta));
}

// pbrt's FindInterval(): the largest index i in [0, size - 2] with cdf[i] <= u, by bisection
uint32_t findInterval(const float* cdf, uint32_t size, float u)
{
	int32_t length = (int32_t)size - 2;
	int32_t first = 1;
	while (length > 0) {
		const int32_t half = length >> 1;
		const int32_t middle = first + half;
		if (cdf[middle] <= u) {
			first = middle + 1;
			length -= half + 1;
		} else {
			length = half;
		}
	}
	return (uint32_t)std::clamp(first - 1, 0, (int32_t)size - 2);
}

// pbrt-v4's PiecewiseConstant1D::Sample() over [0,1], on one cdf of `count` + 1 entries
float sampleCdf(const float* cdf, uint32_t count, float u, uint32_t& offset)
{
	offset = findInterval(cdf, count + 1, u);
	float du = u - cdf[offset];
	if (cdf[offset + 1] - cdf[offset] > 0.f) {
		du /= cdf[offset + 1] - cdf[offset];
	}
	return std::min(((float)offset + du) / (float)count, ONE_MINUS_EPSILON);
}

float luminanceOf(float r, float g, float b)
{
	return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

}

//---------------------------------------------------------------- shared

float lightAngleBetween(const glm::vec3& a, const glm::vec3& b)
{
	if (glm::dot(a, b) < 0.f) {
		return LIGHT_PI - 2.f * safeAsin(glm::length(a + b) / 2.f);
	}
	return 2.f * safeAsin(glm::length(b - a) / 2.f);
}

float differenceOfProducts(float a, float b, float c, float d)
{
	const float cd = c * d;
	const float difference = std::fma(a, b, -cd);
	const float error = std::fma(-c, d, cd);
	return difference + error;
}

float sumOfProducts(float a, float b, float c, float d)
{
	const float cd = c * d;
	const float sum = std::fma(a, b, cd);
	const float error = std::fma(c, d, -cd);
	return sum + error;
}

void lightFrameFromZ(const glm::vec3& z, glm::vec3& x, glm::vec3& y)
{
	const float zSign = z.z >= 0.f ? 1.f : -1.f;
	const float a = -1.f / (zSign + z.z);
	const float b = z.x * z.y * a;
	x = glm::vec3(1.f + zSign * z.x * z.x * a, zSign * b, -zSign * z.x);
	y = glm::vec3(b, zSign + z.y * z.y * a, -z.y);
}

float misWeight(float pdf, float otherPdf, float exponent)
{
	const float f = exponent >= 2.f ? pdf * pdf : pdf;
	const float g = exponent >= 2.f ? otherPdf * otherPdf : otherPdf;
	return f + g > 0.f ? f / (f + g) : 0.f;
}

//---------------------------------------------------------------- sphere

bool sampleSphereLight(const glm::vec3& center, float radius, const glm::vec3& ref, const glm::vec2& u, LightSample& out)
{
	const glm::vec3 toCenter = center - ref;
	const float distanceSquared = glm::dot(toCenter, toCenter);

	//from inside the sphere every point of it is visible: sample it uniformly by area
	if (distanceSquared <= radius * radius) {
		const float z = 1.f - 2.f * u.x;
		const float r = safeSqrt(1.f - z * z);
		const float phi = 2.f * LIGHT_PI * u.y;
		const glm::vec3 normal(r * std::cos(phi), r * std::sin(phi), z);
		if (!finishAreaSample(ref, center + radius * normal, normal, 1.f / (4.f * LIGHT_PI * radius * radius), out)) {
			return false;
		}
		//through the pdf function, so the emissive kernel's MIS weight sees exactly this density
		out.pdf = sphereLightPdf(center, radius, ref, out.point);
		return out.pdf > 0.f;
	}

	//from outside, uniformly inside the cone of directions it subtends
	const float sinThetaMax = radius / std::sqrt(distanceSquared);
	const float sin2ThetaMax = sinThetaMax * sinThetaMax;
	const float cosThetaMax = safeSqrt(1.f - sin2ThetaMax);
	float oneMinusCosThetaMax = 1.f - cosThetaMax;

	float cosTheta = (cosThetaMax - 1.f) * u.x + 1.f;
	float sin2Theta = 1.f - cosTheta * cosTheta;
	//a small cone by its Taylor series: 1 - cos(theta) cancels catastrophically below ~1.5 degrees
	if (sin2ThetaMax < 0.00068523f) {
		sin2Theta = sin2ThetaMax * u.x;
		cosTheta = std::sqrt(1.f - sin2Theta);
		oneMinusCosThetaMax = sin2ThetaMax / 2.f;
	}

	//the angle at the centre between the axis and the sampled point, then the point
	const float cosAlpha = sin2Theta / sinThetaMax + cosTheta * safeSqrt(1.f - sin2Theta / sin2ThetaMax);
	const float sinAlpha = safeSqrt(1.f - cosAlpha * cosAlpha);
	const float phi = u.y * 2.f * LIGHT_PI;
	const glm::vec3 w(sinAlpha * std::cos(phi), sinAlpha * std::sin(phi), cosAlpha);
	const glm::vec3 axis = glm::normalize(toCenter);
	glm::vec3 frameX;
	glm::vec3 frameY;
	lightFrameFromZ(axis, frameX, frameY);
	const glm::vec3 normal = -(w.x * frameX + w.y * frameY + w.z * axis);
	const glm::vec3 point = center + radius * normal;

	const glm::vec3 toLight = point - ref;
	out.point = point;
	out.normal = normal;
	out.distance = glm::length(toLight);
	if (!(out.distance > 0.f)) {
		return false;
	}
	out.direction = toLight / out.distance;
	out.pdf = 1.f / (2.f * LIGHT_PI * oneMinusCosThetaMax);
	return true;
}

float sphereLightPdf(const glm::vec3& center, float radius, const glm::vec3& ref, const glm::vec3& lightPoint)
{
	const glm::vec3 toCenter = center - ref;
	const float distanceSquared = glm::dot(toCenter, toCenter);
	if (distanceSquared <= radius * radius) {
		return areaToSolidAngle(1.f / (4.f * LIGHT_PI * radius * radius), ref, lightPoint, glm::normalize(lightPoint - center));
	}
	const float sin2ThetaMax = radius * radius / distanceSquared;
	const float cosThetaMax = safeSqrt(1.f - sin2ThetaMax);
	float oneMinusCosThetaMax = 1.f - cosThetaMax;
	if (sin2ThetaMax < 0.00068523f) {
		oneMinusCosThetaMax = sin2ThetaMax / 2.f;
	}
	return 1.f / (2.f * LIGHT_PI * oneMinusCosThetaMax);
}

//---------------------------------------------------------------- rectangle

float rectangleSolidAngle(const glm::vec3& corner, const glm::vec3& edgeU, const glm::vec3& edgeV, const glm::vec3& ref)
{
	return setupSphericalRectangle(corner, edgeU, edgeV, ref).solidAngle;
}

bool sampleRectangleLight(const glm::vec3& corner, const glm::vec3& edgeU, const glm::vec3& edgeV, const glm::vec3& ref, const glm::vec2& u, LightSample& out)
{
	const glm::vec3 normal = glm::normalize(glm::cross(edgeU, edgeV));
	const SphericalRectangle r = setupSphericalRectangle(corner, edgeU, edgeV, ref);
	if (!sphericalRectangleUsable(r)) {
		const float area = glm::length(glm::cross(edgeU, edgeV));
		return finishAreaSample(ref, corner + u.x * edgeU + u.y * edgeV, normal, 1.f / area, out);
	}

	//the x coordinate through the sub-area u.x of the whole, then y uniformly in solid angle along it
	const float au = u.x * (r.g0 + r.g1 - 2.f * LIGHT_PI) + (u.x - 1.f) * (r.g2 + r.g3);
	const float sinAu = std::sin(au);
	const float fu = (std::cos(au) * r.b0 - r.b1) / (std::abs(sinAu) > 1e-20f ? sinAu : 1e-20f);
	float cu = 1.f / std::sqrt(fu * fu + r.b0 * r.b0);
	cu = fu >= 0.f ? cu : -cu;
	cu = std::clamp(cu, -ONE_MINUS_EPSILON, ONE_MINUS_EPSILON);
	float xu = -(cu * r.z0) / safeSqrt(1.f - cu * cu);
	xu = std::clamp(xu, r.x0, r.x1);
	const float dd = std::sqrt(xu * xu + r.z0 * r.z0);
	const float h0 = r.y0 / std::sqrt(dd * dd + r.y0 * r.y0);
	const float h1 = r.y1 / std::sqrt(dd * dd + r.y1 * r.y1);
	const float hv = h0 + u.y * (h1 - h0);
	const float hvSquared = hv * hv;
	const float yv = hvSquared < 1.f - 1e-6f ? (hv * dd) / std::sqrt(1.f - hvSquared) : r.y1;

	const glm::vec3 point = ref + xu * r.x + yv * r.y + r.z0 * r.z;
	const glm::vec3 toLight = point - ref;
	out.point = point;
	out.normal = normal;
	out.distance = glm::length(toLight);
	if (!(out.distance > 0.f)) {
		return false;
	}
	out.direction = toLight / out.distance;
	out.pdf = 1.f / r.solidAngle;
	return true;
}

float rectangleLightPdf(const glm::vec3& corner, const glm::vec3& edgeU, const glm::vec3& edgeV, const glm::vec3& ref, const glm::vec3& lightPoint)
{
	const SphericalRectangle r = setupSphericalRectangle(corner, edgeU, edgeV, ref);
	if (!sphericalRectangleUsable(r)) {
		const float area = glm::length(glm::cross(edgeU, edgeV));
		return areaToSolidAngle(1.f / area, ref, lightPoint, glm::normalize(glm::cross(edgeU, edgeV)));
	}
	return 1.f / r.solidAngle;
}

//---------------------------------------------------------------- triangle

float triangleSolidAngle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& ref)
{
	//pbrt's SphericalTriangleArea() (Van Oosterom and Strackee 1983)
	const glm::vec3 a = glm::normalize(v0 - ref);
	const glm::vec3 b = glm::normalize(v1 - ref);
	const glm::vec3 c = glm::normalize(v2 - ref);
	return std::abs(2.f * std::atan2(glm::dot(a, glm::cross(b, c)), 1.f + glm::dot(a, b) + glm::dot(a, c) + glm::dot(b, c)));
}

bool sampleTriangleLight(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& ref, const glm::vec2& u, LightSample& out, glm::vec3& barycentrics)
{
	const glm::vec3 cross = glm::cross(v1 - v0, v2 - v0);
	const float crossLength = glm::length(cross);
	if (!(crossLength > 0.f)) {
		return false;
	}
	const glm::vec3 normal = cross / crossLength;
	const float solidAngle = triangleSolidAngle(v0, v1, v2, ref);
	if (!(solidAngle >= LIGHT_MIN_SPHERICAL_SOLID_ANGLE && solidAngle <= LIGHT_MAX_SPHERICAL_SOLID_ANGLE)) {
		barycentrics = sampleUniformTriangle(u);
		const glm::vec3 point = barycentrics.x * v0 + barycentrics.y * v1 + barycentrics.z * v2;
		return finishAreaSample(ref, point, normal, 2.f / crossLength, out);
	}

	if (!sampleSphericalTriangle(v0, v1, v2, ref, u, barycentrics)) {
		return false;
	}
	const glm::vec3 point = barycentrics.x * v0 + barycentrics.y * v1 + barycentrics.z * v2;
	const glm::vec3 toLight = point - ref;
	out.point = point;
	out.normal = normal;
	out.distance = glm::length(toLight);
	if (!(out.distance > 0.f)) {
		return false;
	}
	out.direction = toLight / out.distance;
	//one over the same solid angle the pdf function computes, rather than the angle excess the
	//construction above used: the two agree mathematically, and the emissive kernel's MIS weight
	//needs this density exactly
	out.pdf = 1.f / solidAngle;
	return true;
}

float triangleLightPdf(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& ref, const glm::vec3& lightPoint)
{
	const glm::vec3 cross = glm::cross(v1 - v0, v2 - v0);
	const float crossLength = glm::length(cross);
	if (!(crossLength > 0.f)) {
		return 0.f;
	}
	const float solidAngle = triangleSolidAngle(v0, v1, v2, ref);
	if (!(solidAngle >= LIGHT_MIN_SPHERICAL_SOLID_ANGLE && solidAngle <= LIGHT_MAX_SPHERICAL_SOLID_ANGLE)) {
		return areaToSolidAngle(2.f / crossLength, ref, lightPoint, cross / crossLength);
	}
	return 1.f / solidAngle;
}

//---------------------------------------------------------------- capped cylinder

bool sampleCylinderLight(const glm::vec3& center, const glm::vec3& axisX, const glm::vec3& axisY, float radius, float height, const glm::vec3& ref, const glm::vec2& u, LightSample& out)
{
	const glm::vec3 axisZ = glm::cross(axisX, axisY);
	const float sideArea = 2.f * LIGHT_PI * radius * height;
	const float capArea = LIGHT_PI * radius * radius;
	const float area = sideArea + 2.f * capArea;

	//u.x picks the side or a cap by area and is then stretched back over [0, 1) for that part
	const float t = u.x * area;
	glm::vec3 point;
	glm::vec3 normal;
	if (t < sideArea) {
		const float phi = 2.f * LIGHT_PI * std::min(t / sideArea, ONE_MINUS_EPSILON);
		const glm::vec3 radial = std::cos(phi) * axisX + std::sin(phi) * axisZ;
		point = center + radius * radial + height * (u.y - 0.5f) * axisY;
		normal = radial;
	} else {
		const float capU = (t - sideArea) / capArea;
		const bool top = capU < 1.f;
		const glm::vec2 disk = sampleUniformDiskConcentric(glm::vec2(std::min(top ? capU : capU - 1.f, ONE_MINUS_EPSILON), u.y));
		const float side = top ? 0.5f : -0.5f;
		point = center + radius * (disk.x * axisX + disk.y * axisZ) + height * side * axisY;
		normal = top ? axisY : -axisY;
	}
	if (!finishAreaSample(ref, point, normal, 1.f / area, out)) {
		return false;
	}
	//through the pdf function, which finds the surface from the point: at a grazing angle the
	//generating normal and the recovered one give visibly different cosines, and the emissive kernel's
	//MIS weight can only recover the second
	out.pdf = cylinderLightPdf(center, axisX, axisY, radius, height, ref, out.point);
	return out.pdf > 0.f;
}

float cylinderLightPdf(const glm::vec3& center, const glm::vec3& axisX, const glm::vec3& axisY, float radius, float height, const glm::vec3& ref, const glm::vec3& lightPoint)
{
	const glm::vec3 axisZ = glm::cross(axisX, axisY);
	const float area = 2.f * LIGHT_PI * radius * height + 2.f * LIGHT_PI * radius * radius;

	//which surface the point is on, the way shapeNormal() decides it: the nearer of the rim and a cap
	const glm::vec3 local = lightPoint - center;
	const float along = glm::dot(local, axisY) / height;
	const float lx = glm::dot(local, axisX) / radius;
	const float lz = glm::dot(local, axisZ) / radius;
	const float radial = std::sqrt(lx * lx + lz * lz);
	glm::vec3 normal;
	if (std::abs(0.5f - std::abs(along)) < std::abs(1.f - radial)) {
		normal = along >= 0.f ? axisY : -axisY;
	} else {
		normal = radial > 0.f ? (lx * axisX + lz * axisZ) / radial : axisX;
	}
	return areaToSolidAngle(1.f / area, ref, lightPoint, normal);
}

//---------------------------------------------------------------- area lights, by kind

std::vector<AreaLightGeometry> shapeLightGeometry(ShapeKind kind, const glm::mat4& objectToWorld)
{
	auto point = [&](float x, float y, float z) { return glm::vec3(objectToWorld * glm::vec4(x, y, z, 1.f)); };
	auto vector = [&](float x, float y, float z) { return glm::vec3(objectToWorld * glm::vec4(x, y, z, 0.f)); };

	std::vector<AreaLightGeometry> out;
	switch (kind) {
	case ShapeKind::Sphere: {
		//a sphere's size is one radius on every axis (SPHERE_PARAMS) and it is never rotated
		AreaLightGeometry light;
		light.kind = AreaLightKind::Sphere;
		light.a = point(0.f, 0.f, 0.f);
		light.radius = glm::length(vector(1.f, 0.f, 0.f));
		light.area = 4.f * LIGHT_PI * light.radius * light.radius;
		out.push_back(light);
		break;
	}
	case ShapeKind::Plane:
		break;
	case ShapeKind::Quad: {
		//scaled along its own axes and then rotated, so always a rectangle
		AreaLightGeometry light;
		light.kind = AreaLightKind::Rectangle;
		light.a = point(-0.5f, 0.f, -0.5f);
		light.b = vector(1.f, 0.f, 0.f);
		light.c = vector(0.f, 0.f, 1.f);
		light.area = glm::length(glm::cross(light.b, light.c));
		out.push_back(light);
		break;
	}
	case ShapeKind::Box: {
		for (uint32_t face = 0; face < 6; face++) {
			const uint32_t axis = face / 2;
			const float side = (face & 1u) ? 0.5f : -0.5f;
			//the face's two in-plane axes
			const uint32_t axisU = (axis + 1) % 3;
			const uint32_t axisV = (axis + 2) % 3;
			glm::vec3 corner(-0.5f);
			corner[axis] = side;
			glm::vec3 edgeU(0.f);
			edgeU[axisU] = 1.f;
			glm::vec3 edgeV(0.f);
			edgeV[axisV] = 1.f;
			AreaLightGeometry light;
			light.kind = AreaLightKind::Rectangle;
			light.a = point(corner.x, corner.y, corner.z);
			light.b = vector(edgeU.x, edgeU.y, edgeU.z);
			light.c = vector(edgeV.x, edgeV.y, edgeV.z);
			light.area = glm::length(glm::cross(light.b, light.c));
			out.push_back(light);
		}
		break;
	}
	case ShapeKind::Cylinder: {
		//the radius drives x and z together (CYLINDER_PARAMS), so the cross-section stays a circle
		AreaLightGeometry light;
		light.kind = AreaLightKind::Cylinder;
		light.a = point(0.f, 0.f, 0.f);
		const glm::vec3 x = vector(1.f, 0.f, 0.f);
		const glm::vec3 y = vector(0.f, 1.f, 0.f);
		light.radius = glm::length(x);
		light.height = glm::length(y);
		light.b = x / light.radius;
		light.c = y / light.height;
		light.area = 2.f * LIGHT_PI * light.radius * light.height + 2.f * LIGHT_PI * light.radius * light.radius;
		out.push_back(light);
		break;
	}
	}
	return out;
}

uint32_t boxFaceOf(const glm::vec3& objectPoint)
{
	//the axis the point lies furthest out along, as shapeNormal() decides the face
	const glm::vec3 a = glm::abs(objectPoint);
	if (a.x >= a.y && a.x >= a.z) {
		return objectPoint.x >= 0.f ? 1u : 0u;
	}
	if (a.y >= a.z) {
		return objectPoint.y >= 0.f ? 3u : 2u;
	}
	return objectPoint.z >= 0.f ? 5u : 4u;
}

bool sampleAreaLight(const AreaLightGeometry& light, const glm::vec3& ref, const glm::vec2& u, LightSample& out, glm::vec3& barycentrics)
{
	barycentrics = glm::vec3(0.f);
	switch (light.kind) {
	case AreaLightKind::Sphere:
		return sampleSphereLight(light.a, light.radius, ref, u, out);
	case AreaLightKind::Rectangle:
		return sampleRectangleLight(light.a, light.b, light.c, ref, u, out);
	case AreaLightKind::Cylinder:
		return sampleCylinderLight(light.a, light.b, light.c, light.radius, light.height, ref, u, out);
	case AreaLightKind::Triangle:
		return sampleTriangleLight(light.a, light.b, light.c, ref, u, out, barycentrics);
	}
	return false;
}

float areaLightPdf(const AreaLightGeometry& light, const glm::vec3& ref, const glm::vec3& lightPoint)
{
	switch (light.kind) {
	case AreaLightKind::Sphere:
		return sphereLightPdf(light.a, light.radius, ref, lightPoint);
	case AreaLightKind::Rectangle:
		return rectangleLightPdf(light.a, light.b, light.c, ref, lightPoint);
	case AreaLightKind::Cylinder:
		return cylinderLightPdf(light.a, light.b, light.c, light.radius, light.height, ref, lightPoint);
	case AreaLightKind::Triangle:
		return triangleLightPdf(light.a, light.b, light.c, ref, lightPoint);
	}
	return 0.f;
}

//---------------------------------------------------------------- punctual lights

float spotFalloff(float cosAngle, float cosOuter, float cosInner)
{
	const float scale = 1.f / std::max(0.001f, cosInner - cosOuter);
	const float t = std::clamp((cosAngle - cosOuter) * scale, 0.f, 1.f);
	return t * t;
}

float rangeWindow(float distance, float range)
{
	if (range <= 0.f) {
		return 1.f;
	}
	const float ratio = distance / range;
	const float ratio2 = ratio * ratio;
	return std::clamp(1.f - ratio2 * ratio2, 0.f, 1.f);
}

//---------------------------------------------------------------- alias table

std::vector<AliasEntry> buildAliasTable(std::span<const float> weights)
{
	const double sum = std::accumulate(weights.begin(), weights.end(), 0.0);
	if (weights.empty() || !(sum > 0.0)) {
		return {};
	}
	std::vector<AliasEntry> table(weights.size());

	//every outcome's probability scaled so the average is 1: those under 1 are topped up from those
	//over it, one pair at a time
	struct Outcome {
		double scaled;
		uint32_t index;
	};
	std::vector<Outcome> under;
	std::vector<Outcome> over;
	for (uint32_t i = 0; i < weights.size(); i++) {
		const double scaled = (double)weights[i] / sum * (double)weights.size();
		(scaled < 1.0 ? under : over).push_back({ scaled, i });
	}
	while (!under.empty() && !over.empty()) {
		const Outcome small = under.back();
		const Outcome large = over.back();
		under.pop_back();
		over.pop_back();
		table[small.index].q = (float)small.scaled;
		table[small.index].alias = large.index;
		const double excess = small.scaled + large.scaled - 1.0;
		(excess < 1.0 ? under : over).push_back({ excess, large.index });
	}
	//what remains is 1 up to rounding
	for (const Outcome& outcome : over) {
		table[outcome.index].q = 1.f;
		table[outcome.index].alias = outcome.index;
	}
	for (const Outcome& outcome : under) {
		table[outcome.index].q = 1.f;
		table[outcome.index].alias = outcome.index;
	}
	return table;
}

uint32_t sampleAliasTable(std::span<const AliasEntry> table, float u)
{
	const uint32_t count = (uint32_t)table.size();
	const uint32_t offset = std::min((uint32_t)(u * (float)count), count - 1);
	const float up = std::min(u * (float)count - (float)offset, ONE_MINUS_EPSILON);
	return up < table[offset].q ? offset : table[offset].alias;
}

//---------------------------------------------------------------- environment map

PiecewiseConstant2D buildPiecewiseConstant2D(std::span<const float> values, uint32_t width, uint32_t height)
{
	PiecewiseConstant2D table;
	table.width = width;
	table.height = height;
	table.func.assign(values.begin(), values.end());
	for (float& value : table.func) {
		value = std::abs(value);
	}
	table.rowCdf.assign((size_t)height * (width + 1), 0.f);
	table.marginalCdf.assign(height + 1, 0.f);

	//each row's cdf, normalised, and the row's integral as the marginal's function (pbrt's
	//PiecewiseConstant1D over [0, 1]); accumulated in double so a 2048-wide row of tiny values does
	//not lose its tail
	std::vector<float> rowIntegral(height, 0.f);
	for (uint32_t row = 0; row < height; row++) {
		float* cdf = &table.rowCdf[(size_t)row * (width + 1)];
		const float* func = &table.func[(size_t)row * width];
		double running = 0.0;
		cdf[0] = 0.f;
		for (uint32_t i = 1; i <= width; i++) {
			running += (double)func[i - 1] / (double)width;
			cdf[i] = (float)running;
		}
		rowIntegral[row] = (float)running;
		for (uint32_t i = 1; i <= width; i++) {
			cdf[i] = running > 0.0 ? (float)((double)cdf[i] / running) : (float)i / (float)width;
		}
	}
	double running = 0.0;
	table.marginalCdf[0] = 0.f;
	for (uint32_t i = 1; i <= height; i++) {
		running += (double)rowIntegral[i - 1] / (double)height;
		table.marginalCdf[i] = (float)running;
	}
	table.integral = (float)running;
	for (uint32_t i = 1; i <= height; i++) {
		table.marginalCdf[i] = running > 0.0 ? (float)((double)table.marginalCdf[i] / running) : (float)i / (float)height;
	}
	return table;
}

glm::vec2 samplePiecewiseConstant2D(const PiecewiseConstant2D& table, const glm::vec2& u, float& pdf)
{
	uint32_t row;
	const float v = sampleCdf(table.marginalCdf.data(), table.height, u.y, row);
	uint32_t column;
	const float uu = sampleCdf(&table.rowCdf[(size_t)row * (table.width + 1)], table.width, u.x, column);
	pdf = table.integral > 0.f ? table.func[(size_t)row * table.width + column] / table.integral : 0.f;
	return glm::vec2(uu, v);
}

float piecewiseConstant2DPdf(const PiecewiseConstant2D& table, const glm::vec2& uv)
{
	const uint32_t column = (uint32_t)std::clamp((int32_t)(uv.x * (float)table.width), 0, (int32_t)table.width - 1);
	const uint32_t row = (uint32_t)std::clamp((int32_t)(uv.y * (float)table.height), 0, (int32_t)table.height - 1);
	return table.integral > 0.f ? table.func[(size_t)row * table.width + column] / table.integral : 0.f;
}

glm::vec2 equirectUvOf(const glm::vec3& direction)
{
	const float u = 0.5f + std::atan2(direction.x, -direction.z) / (2.f * LIGHT_PI);
	const float v = 0.5f - std::asin(std::clamp(direction.y, -1.f, 1.f)) / LIGHT_PI;
	return glm::vec2(u, v);
}

glm::vec3 equirectDirectionOf(const glm::vec2& uv)
{
	const float phi = (uv.x - 0.5f) * 2.f * LIGHT_PI;
	const float latitude = (0.5f - uv.y) * LIGHT_PI;
	const float cosLatitude = std::cos(latitude);
	return glm::vec3(cosLatitude * std::sin(phi), std::sin(latitude), -cosLatitude * std::cos(phi));
}

EnvironmentDistribution buildEnvironmentDistribution(const float* rgba, uint32_t width, uint32_t height, uint32_t maxWidth)
{
	EnvironmentDistribution out;
	if (rgba == nullptr || width == 0 || height == 0) {
		return out;
	}

	std::vector<float> luminance((size_t)width * height);
	double integral = 0.0;
	for (uint32_t y = 0; y < height; y++) {
		//each texel's solid angle: 2 pi / width across, pi / height down, shrunk by cos(latitude)
		const double latitude = (0.5 - ((double)y + 0.5) / (double)height) * (double)LIGHT_PI;
		const double texelSolidAngle = (2.0 * LIGHT_PI / width) * (LIGHT_PI / height) * std::cos(latitude);
		for (uint32_t x = 0; x < width; x++) {
			const float* texel = rgba + ((size_t)y * width + x) * 4;
			const float value = std::max(luminanceOf(texel[0], texel[1], texel[2]), 0.f);
			luminance[(size_t)y * width + x] = std::isfinite(value) ? value : 0.f;
			integral += luminance[(size_t)y * width + x] * texelSolidAngle;
		}
	}
	out.luminanceIntegral = (float)integral;

	//the table's cells: the map's texels, in blocks of factor x factor until it is narrow enough
	uint32_t factor = 1;
	while (width / factor > std::max(maxWidth, 1u) && factor < width) {
		factor *= 2;
	}
	const uint32_t cellsX = std::max(width / factor, 1u);
	const uint32_t cellsY = std::max(height / factor, 1u);

	std::vector<float> values((size_t)cellsX * cellsY);
	for (uint32_t cy = 0; cy < cellsY; cy++) {
		const uint32_t y0 = cy * height / cellsY;
		const uint32_t y1 = (cy + 1) * height / cellsY;
		//sin(theta) at the cell's centre turns the uv density into one over solid angle
		const float sinTheta = std::cos((0.5f - ((float)cy + 0.5f) / (float)cellsY) * LIGHT_PI);
		for (uint32_t cx = 0; cx < cellsX; cx++) {
			const uint32_t x0 = cx * width / cellsX;
			const uint32_t x1 = (cx + 1) * width / cellsX;
			//the block and a one-texel border, wrapping both ways as the renderer's sampler does
			//(VK_SAMPLER_ADDRESS_MODE_REPEAT): bilinear filtering spreads a texel half a texel into
			//its neighbours, so a cell whose own texels are black but whose neighbour is not still has
			//radiance, and must still have probability
			double sum = 0.0;
			uint32_t count = 0;
			for (int32_t y = (int32_t)y0 - 1; y <= (int32_t)y1; y++) {
				const uint32_t row = (uint32_t)((y + (int32_t)height) % (int32_t)height);
				for (int32_t x = (int32_t)x0 - 1; x <= (int32_t)x1; x++) {
					const uint32_t column = (uint32_t)((x + (int32_t)width) % (int32_t)width);
					sum += luminance[(size_t)row * width + column];
					count++;
				}
			}
			values[(size_t)cy * cellsX + cx] = (float)(sum / (double)count) * sinTheta;
		}
	}
	out.full = buildPiecewiseConstant2D(values, cellsX, cellsY);

	//pbrt-v4's MIS compensation: subtract the average and clamp, so the light samples go where the
	//map is brighter than a BSDF sample would find by itself
	const double average = std::accumulate(values.begin(), values.end(), 0.0) / (double)values.size();
	bool anyLeft = false;
	for (float& value : values) {
		value = std::max((float)(value - average), 0.f);
		anyLeft |= value > 0.f;
	}
	if (!anyLeft) {
		std::fill(values.begin(), values.end(), 1.f);
	}
	out.compensated = buildPiecewiseConstant2D(values, cellsX, cellsY);
	return out;
}

float environmentPeak(const float* rgba, size_t pixelCount)
{
	float peak = 0.f;
	for (size_t i = 0; i < pixelCount; i++) {
		for (int c = 0; c < 3; c++) {
			const float value = rgba[i * 4 + c];
			if (std::isfinite(value)) {
				peak = std::max(peak, value);
			}
		}
	}
	return peak;
}

float environmentStorageScale(float peak)
{
	float scale = 1.f;
	//exact: each doubling moves the exponent, and peak / scale is exact for the same reason
	while (std::isfinite(peak) && peak / scale > HALF_FLOAT_MAX) {
		scale *= 2.f;
	}
	return scale;
}

std::vector<uint32_t> packEnvironmentTexels(const float* rgba, size_t pixelCount, float scale)
{
	const float inverse = 1.f / scale;
	std::vector<uint32_t> packed(pixelCount * 2);
	for (size_t i = 0; i < pixelCount; i++) {
		packed[i * 2 + 0] = glm::packHalf2x16(glm::vec2(rgba[i * 4 + 0] * inverse, rgba[i * 4 + 1] * inverse));
		packed[i * 2 + 1] = glm::packHalf2x16(glm::vec2(rgba[i * 4 + 2] * inverse, rgba[i * 4 + 3]));
	}
	return packed;
}

bool sampleEnvironment(const PiecewiseConstant2D& table, const glm::vec2& u, glm::vec3& direction, float& pdf)
{
	float uvPdf;
	const glm::vec2 uv = samplePiecewiseConstant2D(table, u, uvPdf);
	const float cosLatitude = std::cos((0.5f - uv.y) * LIGHT_PI);
	if (!(uvPdf > 0.f) || !(cosLatitude > 1e-6f)) {
		return false;
	}
	direction = equirectDirectionOf(uv);
	//du dv covers 2 pi^2 cos(latitude) of solid angle
	pdf = uvPdf / (2.f * LIGHT_PI * LIGHT_PI * cosLatitude);
	return true;
}

float environmentPdf(const PiecewiseConstant2D& table, const glm::vec3& direction)
{
	const float cosLatitude = safeSqrt(1.f - direction.y * direction.y);
	if (!(cosLatitude > 1e-6f)) {
		return 0.f;
	}
	return piecewiseConstant2DPdf(table, equirectUvOf(direction)) / (2.f * LIGHT_PI * LIGHT_PI * cosLatitude);
}
