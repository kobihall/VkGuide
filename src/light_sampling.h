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

// THE SAMPLE WARPS the cameras share (shaders/rt/include/crt_warp.glsl): pbrt-v4's SampleUniformTriangle(), the barycentrics
// of a point uniform over a triangle, and SampleUniformDiskConcentric(), Shirley and Chiu's square-to-disk map
glm::vec3 sampleUniformTriangle(const glm::vec2& u);
glm::vec2 sampleUniformDiskConcentric(const glm::vec2& u);

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
// pbrt-v4's spot falloff (SpotLight::I()): a smoothstep in cos(angle) over the same range
float spotFalloffSmooth(float cosAngle, float cosOuter, float cosInner);
// pbrt-v4's equal-area mapping between the unit square and the sphere (Clarberg 2008; PBR 4ed 3.8.3), which a
// goniometric light's image is laid out in: equal areas of the square are equal solid angles
glm::vec3 equalAreaSquareToSphere(const glm::vec2& p);
glm::vec2 equalAreaSphereToSquare(const glm::vec3& d);

// The solid angle a spot light's intensity covers, its falloff weighed in, so its power is that times its intensity: the
// inner cone in full and the ring between the cones at the falloff's mean over cos(angle), a third for glTF's squared
// ramp and a half for pbrt's smoothstep (pbrt-v4's SpotLight::Phi)
float spotSolidAngle(float cosOuter, float cosInner, bool smoothstep);
// pbrt-v4's frustum of a projection light: the half-extents at unit distance of an image `aspect` wide per unit of
// height, its field of view along the shorter side
glm::vec2 projectionExtents(float fovDegrees, float aspect);
// What a projection's (`projection`) or a goniometric light's image of `width` x `height` linear values, row 0 its top,
// integrates to over the directions it covers: each texel times the solid angle it subtends through the frustum, or
// 4 pi over the texel count on the equal-area square. Its largest value in `peak`
float lightImageSolidAngle(bool projection, float fovDegrees, uint32_t width, uint32_t height, const float* values, float* peak = nullptr);

// glTF's recommended window on the inverse-square law when a light has a range; 1 for range 0
float rangeWindow(float distance, float range);

//---------------------------------------------------------------- emitted rays

// One ray leaving a light, which light paths start with (PBR 3ed 16.1.2; pbrt-v4's SampleLe() and PDF_Le()): its
// origin, an area light's surface normal there (zero for every other light), its unit direction, and the density of
// each: per unit of area (1 for a light at a point) and per unit of solid angle (1 for a directional light). Mirrored
// line for line by crt_light.glsl's sampleLe() and pdfLe(), which add the radiance, the textures and the spectra
struct LightLeSample {
	glm::vec3 origin { 0.f };
	glm::vec3 normal { 0.f };
	glm::vec3 direction { 0.f, 0.f, 1.f };
	float pdfPos { 0.f };
	float pdfDir { 0.f };
	// a triangle light's point, for its texture
	glm::vec3 barycentrics { 0.f };
};

// the sides an area light emits from: both, or only the one its normal faces, or only the other (a record flagged
// reversed, src/rt_lights.cpp)
enum class EmitSides : uint32_t {
	Both,
	Front,
	Back
};

// a direction about +z with density cos(theta) / pi, through the concentric disk (pbrt-v4's SampleCosineHemisphere())
glm::vec3 sampleCosineHemisphere(const glm::vec2& u);
// a direction uniform on the sphere (pbrt-v4's SampleUniformSphere()), density 1 / (4 pi)
glm::vec3 sampleUniformSphere(const glm::vec2& u);
// a direction uniform in the cone of half-angle acos(cosMax) about +z (pbrt-v4's SampleUniformCone())
glm::vec3 sampleUniformCone(const glm::vec2& u, float cosMax);

// an area light's emitting surface's area
float areaLightArea(const AreaLightGeometry& light);
// a point uniform by area and its normal, then a direction cosine-weighted about the side it leaves from. Two-sided, the
// side is picked with probability 1/2 and uDir.x stretched back over [0, 1)
bool sampleAreaLe(const AreaLightGeometry& light, EmitSides sides, const glm::vec2& uPos, const glm::vec2& uDir, LightLeSample& out);
// the densities sampleAreaLe() gives the ray leaving a point of normal `normal` along the unit `direction`
void areaPdfLe(const AreaLightGeometry& light, EmitSides sides, const glm::vec3& normal, const glm::vec3& direction, float& pdfPos, float& pdfDir);
// a point light's: from its position, every direction alike
bool samplePointLe(const glm::vec3& position, const glm::vec2& uDir, LightLeSample& out);
// a spot light's: uniform in its outer cone about `axis`, the direction it shines
bool sampleSpotLe(const glm::vec3& position, const glm::vec3& axis, float cosOuter, const glm::vec2& uDir, LightLeSample& out);
float spotPdfDir(const glm::vec3& axis, float cosOuter, const glm::vec3& direction);
// a light at infinity, from the unit direction `towardLight`: an origin uniform on the disk of the scene's bounding
// sphere (center, radius) square to it, behind the scene, pbrt-v4's DistantLight::SampleLe()
glm::vec3 infiniteLightOrigin(const glm::vec4& sceneSphere, const glm::vec3& towardLight, const glm::vec2& uPos);
float infiniteLightPdfPos(const glm::vec4& sceneSphere);
struct PiecewiseConstant2D;
// What a projection or goniometric light's emitted directions are drawn by: its image of `width` x `height` linear
// values, row 0 its top, weighed by the solid angle each texel covers per unit of the image's area. A projection's
// texels subtend cos^3 of their angle off the axis on its image plane at unit distance, half `extents` across; a
// goniometric light's equal-area image covers 4 pi / texels with each
PiecewiseConstant2D buildLightImageDistribution(bool projection, const glm::vec2& extents, uint32_t width, uint32_t height, const float* values);

//---------------------------------------------------------------- lasers

// A laser's beam as its light records carry it (docs/plans/completed/physoptics-laser-sources.md; crt_laser.glsl mirrors
// these line for line): the reference plane through `center` square to the unit `axis`, the way the beam leaves, the
// beam's unit `xAxis` in it, and per transverse axis the 1/e^2 waist radius, the distance from the waist forward to the
// plane (positive when the waist is behind it, inside the housing) and the 1/e^2 far-field half angle. Along one axis
// the beam is the Gaussian phase-space density over the position x in the plane and the slope s (the tangent of the
// angle off the axis)
//
//     p(x, s) = N(x - d s; w0 / 2) N(s; theta / 2)
//
// its Wigner function, which is positive for a Gaussian beam and sheared by free space, so rays drawn from it carry the
// beam's second moments exactly through any paraxial system, focus included (Bastiaans 1979). x and y are independent
struct LaserEmitter {
	glm::vec3 center { 0.f };
	glm::vec3 axis { 0.f, 0.f, 1.f };
	glm::vec3 xAxis { 1.f, 0.f, 0.f };
	glm::vec2 waist { 1.f };
	glm::vec2 waistDistance { 0.f };
	glm::vec2 halfAngle { 1.f };
};

// two independent standard normals from two uniform numbers in [0, 1): the Box-Muller transform, which keeps a stratified
// sampler's strata as strata of the beam's cross section
glm::vec2 sampleStandardNormal2(const glm::vec2& u);
// the product of two normal densities of standard deviations `sigma`, at `v`
float gaussian2(const glm::vec2& v, const glm::vec2& sigma);
// A ray leaving the laser: a waist point and a slope per axis, the point carried along the slope to the plane. pdfPos is
// the point's density in the plane (the beam's intensity there per watt), pdfDir the direction's given the point, per
// unit solid angle, and the normal is the axis, so L cos / (pdfPos pdfDir) is the power
bool sampleLaserLe(const LaserEmitter& laser, const glm::vec2& uPos, const glm::vec2& uDir, LightLeSample& out);
// the densities of a ray leaving the plane at `origin` along the unit `direction`
void laserPdfLe(const LaserEmitter& laser, const glm::vec3& origin, const glm::vec3& direction, float& pdfPos, float& pdfDir);
// The beam's radiance per watt along the unit `direction` through `point`, anywhere on that ray: p(x, s) / cos^4 at the
// ray's crossing of the plane, since d(slope) = d(solid angle) / cos^3 and radiance divides by one cosine more. 0 for a
// direction back into the housing
float laserRadiance(const LaserEmitter& laser, const glm::vec3& point, const glm::vec3& direction);

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
// THE GPU'S COPY in binding 65, ImageDistributions (shaders/rt/include/crt_image_table.glsl), appended to `out`: four
// words (the width and the height as uint bits, the integral, 0), then the marginal cdf (height + 1 values), the row cdfs
// (height x (width + 1)) and the function (height x width). Returns where it starts, in floats
uint32_t packPiecewiseConstant2D(const PiecewiseConstant2D& table, std::vector<float>& out);

// the tracer's equirectangular mapping (shaders/equirect.glsl): u turns right from -z towards +x,
// v runs from +y at the top to -y at the bottom
glm::vec2 equirectUvOf(const glm::vec3& direction);
glm::vec3 equirectDirectionOf(const glm::vec2& uv);
// the map turned about +y (shaders/equirect.glsl): a world direction in the map's frame, and back. `rotation` is the
// angle's cosine and sine (environmentRotation()); a positive angle turns the map counterclockwise seen from above
glm::vec3 environmentToMap(const glm::vec3& direction, const glm::vec2& rotation);
glm::vec3 environmentFromMap(const glm::vec3& direction, const glm::vec2& rotation);
// the cosine and sine of an angle in degrees, as the shaders take a rotation
glm::vec2 environmentRotation(float degrees);

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

// a direction towards the environment, turned by `rotation`, and its solid-angle density; false where the table cannot
// sample (all zero, or a pole)
bool sampleEnvironment(const PiecewiseConstant2D& table, const glm::vec2& u, const glm::vec2& rotation, glm::vec3& direction, float& pdf);
float environmentPdf(const PiecewiseConstant2D& table, const glm::vec3& direction, const glm::vec2& rotation);
