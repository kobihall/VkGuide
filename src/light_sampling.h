#pragma once

// Light sampling for the GPU path tracer's next-event estimation, as plain C++: the routines of
// shaders/rt/include/crt_light.glsl, mirrored line for line so that bin/light_test can check them
// against brute force without a GPU. A change to one side must be made to the other.
//
// Every routine follows pbrt-v4 (PBR 4ed chapters 6 and 12; src/pbrt/shapes.h, lights.h and
// util/sampling.h), which is where the numerically careful forms come from:
//  - a sphere: uniformly within the cone it subtends (Sphere::Sample), by area from inside it
//  - a rectangle: uniformly in the solid angle it subtends (Urena et al. 2013,
//    SampleSphericalRectangle), by area where that angle is too small or too large to be stable
//  - a triangle: uniformly in solid angle (Arvo 1995, SampleSphericalTriangle), by area likewise
//  - a capped cylinder: by area (Cylinder::Sample, Disk::Sample)
//  - an environment map: a piecewise-constant 2D distribution over its texels
//    (PiecewiseConstant2D), plus pbrt's MIS-compensated copy of it (ImageInfiniteLight)
//  - point, spot and directional lights: their one direction, with glTF's range window and cone
//    falloff (KHR_lights_punctual)
// and the alias table that picks one light in proportion to its power (PowerLightSampler).
//
// pbrt's cosine "warp product" on rectangles and triangles is deliberately left out. Under MIS the
// emissive kernel evaluates the pdf of a light that a BSDF sample hit, and with the warp that needs
// the inverse of the spherical-rectangle map, which pbrt notes does not always agree with the
// forward one. Without it the pdf of either shape is 1 / solid angle.

#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include <shape.h>

inline constexpr float LIGHT_PI = 3.14159265358979323846f;
// the tracer's "no hit" distance (shaders/rt/include/crt_common.glsl CRT_INFINITY), which a sample
// of a directional or environment light reports as its distance
inline constexpr float LIGHT_INFINITY = 1e30f;
// pbrt-v4's Triangle::MinSphericalSampleArea / MaxSphericalSampleArea, used for rectangles too: a
// shape subtending less than this is sampled by area (the spherical construction loses precision),
// and one subtending nearly a hemisphere as well (it degenerates as the reference point reaches the
// shape's plane)
inline constexpr float LIGHT_MIN_SPHERICAL_SOLID_ANGLE = 1e-3f;
inline constexpr float LIGHT_MAX_SPHERICAL_SOLID_ANGLE = 6.22f;

// One sample of a light as seen from a reference point
struct LightSample {
	// on the light's surface; unused for a directional or environment light
	glm::vec3 point { 0.f };
	// the surface's geometric normal there (either side; every emitter here is two-sided)
	glm::vec3 normal { 0.f };
	// unit length, from the reference point towards the light
	glm::vec3 direction { 0.f };
	// from the reference point to `point`, or LIGHT_INFINITY
	float distance { 0.f };
	// with respect to solid angle at the reference point; 1 for a delta light
	float pdf { 0.f };
};

// pbrt's AngleBetween(): the angle between two unit vectors, accurate near 0 and near pi
float lightAngleBetween(const glm::vec3& a, const glm::vec3& b);
// pbrt's DifferenceOfProducts() / SumOfProducts(): a * b -+ c * d with the rounding error of the
// second product recovered by an fma
float differenceOfProducts(float a, float b, float c, float d);
float sumOfProducts(float a, float b, float c, float d);
// a right-handed orthonormal basis with `z` as its third axis (Duff et al. 2017, pbrt's
// CoordinateSystem), so a sample in local coordinates maps to world as x * u + y * v + z * z
void lightFrameFromZ(const glm::vec3& z, glm::vec3& x, glm::vec3& y);

// the balance (exponent 1) or power (exponent 2) heuristic's weight for a sample drawn from the
// technique with density `pdf` against the one with `otherPdf` (PBR 4ed eq. 2.14, 2.15)
float misWeight(float pdf, float otherPdf, float exponent);

//---------------------------------------------------------------- sphere

bool sampleSphereLight(const glm::vec3& center, float radius, const glm::vec3& ref, const glm::vec2& u, LightSample& out);
// the density sampleSphereLight() gives the direction towards `lightPoint`, the nearest point of the
// sphere along it (a hit point, for the emissive kernel's MIS weight)
float sphereLightPdf(const glm::vec3& center, float radius, const glm::vec3& ref, const glm::vec3& lightPoint);

//---------------------------------------------------------------- rectangle

// `corner` and two perpendicular edges from it
float rectangleSolidAngle(const glm::vec3& corner, const glm::vec3& edgeU, const glm::vec3& edgeV, const glm::vec3& ref);
bool sampleRectangleLight(const glm::vec3& corner, const glm::vec3& edgeU, const glm::vec3& edgeV, const glm::vec3& ref, const glm::vec2& u, LightSample& out);
float rectangleLightPdf(const glm::vec3& corner, const glm::vec3& edgeU, const glm::vec3& edgeV, const glm::vec3& ref, const glm::vec3& lightPoint);

//---------------------------------------------------------------- triangle

float triangleSolidAngle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& ref);
// `barycentrics` of the sampled point, for looking up its uv
bool sampleTriangleLight(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& ref, const glm::vec2& u, LightSample& out, glm::vec3& barycentrics);
float triangleLightPdf(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& ref, const glm::vec3& lightPoint);

//---------------------------------------------------------------- capped cylinder

// the unit cylinder of shape.h (radius 1 about y, y in [-0.5, 0.5], both caps) placed at `center`
// with unit axes `axisX` and `axisY` (axisZ = cross(axisX, axisY)), `radius` and full `height`
bool sampleCylinderLight(const glm::vec3& center, const glm::vec3& axisX, const glm::vec3& axisY, float radius, float height, const glm::vec3& ref, const glm::vec2& u, LightSample& out);
float cylinderLightPdf(const glm::vec3& center, const glm::vec3& axisX, const glm::vec3& axisY, float radius, float height, const glm::vec3& ref, const glm::vec3& lightPoint);

//---------------------------------------------------------------- area lights, by kind

// the tags shaders/rt/include/crt_light.glsl gives them (CRT_LIGHT_*), append only
enum class AreaLightKind : uint32_t {
	Sphere,
	Rectangle,
	Cylinder,
	Triangle
};

// One emitting surface in world space, the way the light list records it
struct AreaLightGeometry {
	AreaLightKind kind { AreaLightKind::Sphere };
	// sphere: the centre. Rectangle: a corner. Cylinder: the centre. Triangle: the first vertex
	glm::vec3 a { 0.f };
	// rectangle: the edge from `a` along u. Cylinder: the unit x axis. Triangle: the second vertex
	glm::vec3 b { 0.f };
	// rectangle: the edge from `a` along v. Cylinder: the unit y (length) axis. Triangle: the third
	glm::vec3 c { 0.f };
	// sphere and cylinder
	float radius { 0.f };
	// cylinder, end to end
	float height { 0.f };
	// of the emitting surface, world space
	float area { 0.f };
};

// The emitting surfaces a placed shape contributes: one for a sphere, a quad or a cylinder, six for
// a box - in face order -x, +x, -y, +y, -z, +z, which is how the intersect kernel names the face it
// hit - and none for the infinite plane, which has no finite area to sample
std::vector<AreaLightGeometry> shapeLightGeometry(ShapeKind kind, const glm::mat4& objectToWorld);
// the box face (index into shapeLightGeometry()) an object-space point on the unit box lies on
uint32_t boxFaceOf(const glm::vec3& objectPoint);

bool sampleAreaLight(const AreaLightGeometry& light, const glm::vec3& ref, const glm::vec2& u, LightSample& out, glm::vec3& barycentrics);
float areaLightPdf(const AreaLightGeometry& light, const glm::vec3& ref, const glm::vec3& lightPoint);

//---------------------------------------------------------------- punctual lights

// glTF's recommended spot cone falloff: 1 inside the inner cone, 0 outside the outer, a squared
// ramp in cos(angle) between them
float spotFalloff(float cosAngle, float cosOuter, float cosInner);
// glTF's recommended window on the inverse-square law when a light has a range; 1 for range 0
float rangeWindow(float distance, float range);

//---------------------------------------------------------------- alias table

// Vose's alias method as pbrt-v4's AliasTable builds it: bin i is kept with probability q and
// otherwise gives way to its alias, so one uniform number picks outcome i with probability
// weights[i] / sum in O(1)
struct AliasEntry {
	float q { 1.f };
	uint32_t alias { 0 };
};

// empty when the weights sum to zero
std::vector<AliasEntry> buildAliasTable(std::span<const float> weights);
uint32_t sampleAliasTable(std::span<const AliasEntry> table, float u);

//---------------------------------------------------------------- environment map

// pbrt-v4's PiecewiseConstant2D over [0,1]^2 in the form the GPU reads it: a marginal cdf over the
// rows and a conditional cdf per row, both normalised, and the unnormalised function values the pdf
// comes from
struct PiecewiseConstant2D {
	uint32_t width { 0 };
	uint32_t height { 0 };
	// per cell, by row: the tabulated value (non-negative)
	std::vector<float> func;
	// per row: width + 1 entries from 0 to 1
	std::vector<float> rowCdf;
	// height + 1 entries from 0 to 1
	std::vector<float> marginalCdf;
	// the function's integral over the unit square: the pdf is func / integral
	float integral { 0.f };
};

PiecewiseConstant2D buildPiecewiseConstant2D(std::span<const float> values, uint32_t width, uint32_t height);
// a point of the unit square with density `pdf` (0 for a table that is all zero)
glm::vec2 samplePiecewiseConstant2D(const PiecewiseConstant2D& table, const glm::vec2& u, float& pdf);
float piecewiseConstant2DPdf(const PiecewiseConstant2D& table, const glm::vec2& uv);

// the tracer's equirectangular mapping (shaders/equirect.glsl): u turns right from -z towards +x,
// v runs from +y at the top to -y at the bottom
glm::vec2 equirectUvOf(const glm::vec3& direction);
glm::vec3 equirectDirectionOf(const glm::vec2& uv);

// What sampling an equirectangular environment map needs, built once when the map loads
struct EnvironmentDistribution {
	// proportional to luminance x sin(theta), so its solid-angle density follows the luminance
	PiecewiseConstant2D full;
	// the same with the mean subtracted and clamped at zero: pbrt's MIS compensation, which spends
	// no light samples on directions a BSDF sample covers well enough. Only valid under MIS
	PiecewiseConstant2D compensated;
	// the integral of the map's luminance over the sphere, the environment's share of the light
	// selection before the scene's size and the intensity are applied
	float luminanceIntegral { 0.f };
};

// from linear rgba floats, row 0 the top of the map. The table has the map's resolution, halved
// until it is at most `maxWidth` wide; each cell averages its texels and a one-texel border (wrapping
// both ways, like the renderer's sampler), so a cell is never zero where the bilinearly filtered map
// is not
EnvironmentDistribution buildEnvironmentDistribution(const float* rgba, uint32_t width, uint32_t height, uint32_t maxWidth);

// THE MAP'S GPU COPY is half float, whose largest finite value is 65504. A brighter texel, such as
// the unclipped lamp in moon_lab_4k.hdr at 466944, would upload as infinity: the tables above,
// built from the file's floats, would still send most light samples to it, and every sample that
// met it would be dropped as non-finite. So a map is stored divided by the smallest power of two
// that brings its peak within range, and every shader that reads it multiplies the scale back in
// with the environment intensity. Dividing by a power of two moves only the exponent, so the
// stored values carry half float's full precision; a map that fits has scale 1 and uploads as it
// always did.
inline constexpr float HALF_FLOAT_MAX = 65504.f;
// the brightest channel of any texel of an rgba map, alpha aside; non-finite values are skipped
float environmentPeak(const float* rgba, size_t pixelCount);
// the power of two a map with this peak is stored divided by: 1 up to HALF_FLOAT_MAX
float environmentStorageScale(float peak);
// the map as rgba16f, two packed uint32 per texel, colour divided by `scale` and alpha kept
std::vector<uint32_t> packEnvironmentTexels(const float* rgba, size_t pixelCount, float scale);

// a direction towards the environment and its solid-angle density; false where the table cannot
// sample (all zero, or a pole)
bool sampleEnvironment(const PiecewiseConstant2D& table, const glm::vec2& u, glm::vec3& direction, float& pdf);
float environmentPdf(const PiecewiseConstant2D& table, const glm::vec3& direction);
