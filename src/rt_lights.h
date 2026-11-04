#pragma once

// The light list: every light the path tracer's next-event estimation can sample, packed as the
// GPU reads it (shaders/rt/include/crt_common.glsl GpuLight, LightHeader; sampled by
// shaders/rt/include/crt_light.glsl). Built with the rest of the traced scene by buildSceneAccel()
// (rt_accel.h), whenever an object, a light or the background changes.
//
// Five kinds of source, in this order in the list:
//  - the punctual lights (SceneLight), first, so the delta lights are lights[0, deltaCount)
//  - analytic shapes whose material emits: one record per shape, six for a box, none for the
//    infinite plane, which has no finite area to sample
//  - glTF triangles whose material emits, one record per triangle
//  - the environment map, when it is the background
//  - the lasers (docs/plans/completed/physoptics-laser-sources.md), last, two records each, from laserBase on
// Each gets a power - what it sends into the scene - and an alias table picks among them in
// proportion to it (pbrt-v4's PowerLightSampler). The delta lights get a second table over
// themselves alone, which the BSDF strategy uses, since nothing else can reach them. The lasers are in neither:
// next-event estimation never samples a laser, whose light leaves in a cone of milliradians that a shadow ray toward
// its aperture would almost never fall in. They have a third table of their own, which light tracing picks from beside
// the first, every light by its power over the total of both (sampleEmitterIndex(), crt_laser.glsl).
//
// An emitter the list leaves out (the infinite plane, a black light) is still correct: kernel 04
// finds no record for it and counts whatever a BSDF sample finds on it at full weight.

#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include <light_sampling.h>
#include <rt_scene_types.h>
#include <spectral.h>

// crt_common.glsl CRT_LIGHT_*: AreaLightKind for the first four
enum class GpuLightKind : uint32_t {
	Sphere = 0,
	Rectangle = 1,
	Cylinder = 2,
	Triangle = 3,
	Point = 4,
	Spot = 5,
	Directional = 6,
	Environment = 7,
	Projection = 8,
	Goniometric = 9,
	// a laser's two records: the first as every light's, the second its beam (crt_laser.glsl)
	Laser = 10,
	LaserBeam = 11
};

// GpuLight::kind holds the kind in its low 8 bits and these flags above (crt_common.glsl CRT_LIGHT_FLAG_*)
inline constexpr uint32_t GPU_LIGHT_KIND_MASK = 0xFFu;
inline constexpr uint32_t GPU_LIGHT_FLAG_SMOOTHSTEP = 1u << 8;
inline constexpr uint32_t GPU_LIGHT_FLAG_ONE_SIDED = 1u << 9;
inline constexpr uint32_t GPU_LIGHT_FLAG_REVERSED = 1u << 10;
// an area light whose emission reads a library texture: a shape's record names its instance (sphere b.w, rectangle a.w,
// cylinder c.w), a triangle's its material (c.w), whose texture table the sample's uv is looked up in
inline constexpr uint32_t GPU_LIGHT_FLAG_TEXTURED = 1u << 11;
// a projection or goniometric light whose image frame is left-handed: its z axis is -(b x c) (SceneLight::mirrored)
inline constexpr uint32_t GPU_LIGHT_FLAG_MIRRORED = 1u << 12;

// what a shape's records carry beyond its geometry and radiance: GPU_LIGHT_FLAG_*, the instance a textured one's samples
// look their uv up through, and its texture's mean, which scales its power
struct ShapeEmitter {
	uint32_t flags { 0 };
	uint32_t instance { 0 };
	float textureMean { 1.f };
};

// crt_common.glsl CRT_NO_LIGHT: the index of no light
inline constexpr uint32_t GPU_NO_LIGHT = 0xFFFFFFFFu;

// The light groups (0 to 7) are what every record carries beside its kind: the sums the render keeps by group
// (docs/plans/completed/pbrt-aovs-and-image-output.md)

// 96 bytes, crt_common.glsl GpuLight, whose comment lists what a, b and c hold for each kind
struct GpuLight {
	glm::vec4 a { 0.f };
	glm::vec4 b { 0.f };
	glm::vec4 c { 0.f };
	glm::vec3 radiance { 0.f };
	uint32_t kind { 0 };
	float power { 0.f };
	float aliasQ { 1.f };
	uint32_t aliasIndex { 0 };
	float deltaAliasQ { 1.f };
	uint32_t deltaAliasIndex { 0 };
	// its light group, 0 to 7 (docs/plans/completed/pbrt-aovs-and-image-output.md)
	uint32_t group { 0 };
	// what the spectral builds evaluate in place of `radiance` (docs/plans/completed/pbrt-spectral-rendering.md): the
	// emission's index in the scene's spectra (RaytraceSceneAccel::spectra, SPECTRAL_NONE for none) and the factor it is
	// multiplied by, a light's intensity or an emitter's strength
	uint32_t spectrum { SPECTRAL_NONE };
	float spectrumScale { 0.f };
};
static_assert(sizeof(GpuLight) == 96);

// 32 bytes, crt_common.glsl LightHeader
struct GpuLightHeader {
	uint32_t count { 0 };
	uint32_t deltaCount { 0 };
	uint32_t environment { GPU_NO_LIGHT };
	// the lasers' first record, and how many lasers there are (two records each); 0 and 0 without any, so a list without
	// a laser is the bytes it always was
	uint32_t laserBase { 0 };
	float invTotalPower { 0.f };
	float invDeltaPower { 0.f };
	uint32_t laserCount { 0 };
	// one over the power of every light the light paths start from: the first table's and the lasers'
	float invEmitPower { 0.f };
};
static_assert(sizeof(GpuLightHeader) == 32);

struct RaytraceLightList {
	GpuLightHeader header;
	std::vector<GpuLight> lights;
	// per emitting mesh instance, one entry per triangle of its mesh (by the triangle's index in the
	// mesh): its record in `lights`, or GPU_NO_LIGHT. GpuInstance::lightBase finds an instance's run
	std::vector<uint32_t> triangleLights;
	// THE START OF BINDING 65 (ImageDistributions): for each punctual light, lights[0, deltaCount), where its image's
	// emission table starts (as uint bits), or GPU_NO_LIGHT; then the tables (packPiecewiseConstant2D()). What
	// emitted rays of a projection or goniometric light are drawn by (crt_light_emission.glsl)
	std::vector<float> imageTables;

	// for the panel
	uint32_t punctualLights { 0 };
	uint32_t shapeLights { 0 };
	uint32_t triangleLightCount { 0 };
	bool environment { false };
	float totalPower { 0.f };
	float laserPower { 0.f };
};

// The solid angle a spot light's intensity covers, its falloff weighed in: its power over its intensity
float spotSolidAngle(float cosOuter, float cosInner, SpotFalloff falloff);

// A projection or goniometric light's image, as the light list reads it from the texture library: its descriptor and
// sampler, and what it integrates to (lightImageMeasure())
struct LightImage {
	bool valid { false };
	uint32_t descriptor { 0 };
	uint32_t sampler { 0 };
	// the solid angle its texels cover at unit intensity, each weighed by its value, and its brightest texel
	float solidAngle { 0.f };
	float peak { 0.f };
	// width over height
	float aspect { 1.f };
	// the library's luminance copy it was measured from, which emitted rays are drawn by (null for none)
	const float* luminance { nullptr };
	glm::uvec2 luminanceSize { 0 };
};

// a projection's or goniometric light's image of `width` x `height` linear luminances measured for its power
// (lightImageSolidAngle(), src/light_sampling.h), with its peak and its aspect
LightImage lightImageMeasure(bool projection, float fovDegrees, uint32_t width, uint32_t height, const float* luminance);

// THE PHOTOMETRIC UNITS. What `light`'s stored strength (W/sr, or W/m^2 for a directional light) is multiplied by to
// show it in `unit`: 683 lm/W times the luminance of its color, times the solid angle its intensity covers for a luminous
// power. For a projection or goniometric light `image` gives that solid angle, its image integrated over the directions it
// covers (lightImageMeasure()), and a candela its brightest texel; one whose image is not loaded yet reads as a point light
float lightUnitFactor(const SceneLight& light, LightUnit unit, const LightImage& image = {});
// the units a light of `kind` can be shown in, radiometric first
std::span<const LightUnit> lightUnits(LightKind kind);
const char* lightUnitLabel(LightKind kind, LightUnit unit);

// What an emitter's records carry for the spectral builds: its emission's index in the scene's spectra, the factor
// that multiplies it, and the price the light selection takes when its RGB has no luminance (PackedMaterial::
// emissionPrice), which is what keeps an emitter only a spectral build sees in the list
struct LightSpectrum {
	uint32_t index { SPECTRAL_NONE };
	float scale { 0.f };
	float price { 0.f };
};

// What a laser's records carry: its beam in world space (src/light_sampling.h), its output aperture's radius, the exact
// RGB color of a watt of it (src/optic_laser.h LaserBeam::rgb), its power in watts and its spectrum
struct LaserLight {
	LaserEmitter emitter;
	float apertureRadius { 0.f };
	glm::vec3 rgb { 1.f };
	float power { 0.f };
	LightSpectrum spectrum;
};

// Collects the lights one at a time, then finish() prices the ones that depend on the scene's size
// and builds the alias tables. Punctual lights must be added before anything else. A punctual light's spectrum goes
// into `spectra`, read in `context` (the working space) for its exact RGB color
class LightListBuilder {
public:
	explicit LightListBuilder(SceneSpectra* spectra = nullptr, SpectrumContext context = {}) : m_spectra(spectra), m_context(context) {}
	// `image` is a projection or goniometric light's; such a light without one is a point light of its intensity
	void addPunctual(const SceneLight& light, const LightImage& image = {});
	// an analytic shape's emitting surfaces (shapeLightGeometry()), all with the same radiance. The
	// first record's index, or GPU_NO_LIGHT when the shape adds none
	uint32_t addShape(ShapeKind kind, const glm::mat4& objectToWorld, const glm::vec3& radiance, uint32_t group = 0, const LightSpectrum& spectrum = {}, const ShapeEmitter& emitter = {});
	// one emitting triangle of a placed mesh, in world space: its record's index, or GPU_NO_LIGHT for
	// a triangle with no area or no emission. `attributeIndex` is its global attribute record,
	// `material` its index in the scene's materials (for the alpha test of a cut-out emitter),
	// `emissiveLayer` its texture, -1 for none, and `textureMean` that texture's mean luminance over it
	// `flags` the GPU_LIGHT_FLAG_* of a one-sided or library-textured emitter
	uint32_t addTriangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& radiance, uint32_t attributeIndex, uint32_t material, int emissiveLayer, float textureMean, uint32_t group = 0,
		const LightSpectrum& spectrum = {}, uint32_t flags = 0);
	// `luminanceIntegral` is EnvironmentDistribution::luminanceIntegral
	void addEnvironment(float luminanceIntegral, float intensity, uint32_t group = 0);
	// a laser, which finish() puts after everything else: its ordinal among the lasers, whose first record is then
	// laserBase + 2 ordinal, or GPU_NO_LIGHT for a laser with no power
	uint32_t addLaser(const LaserLight& laser);

	// a run of triangleLights entries for one mesh instance, all GPU_NO_LIGHT until set
	uint32_t beginTriangleRun(uint32_t triangleCount);
	void setTriangleLight(uint32_t runBase, uint32_t triangle, uint32_t light);

	// `sceneRadius` is the radius of a sphere around the scene's geometry: a directional light and
	// the environment deliver power in proportion to the area they fall on (PBR 4ed 12.4, 12.5)
	RaytraceLightList finish(float sceneRadius);

private:
	SceneSpectra* m_spectra { nullptr };
	SpectrumContext m_context;
	RaytraceLightList m_list;
	// per record: power already known, or a power per unit of scene cross-section (pi r^2) for the
	// directional lights and the environment
	std::vector<float> m_crossSectionPower;
	// the punctual lights' emission tables, by light
	std::vector<std::pair<uint32_t, PiecewiseConstant2D>> m_imageDistributions;
	// two records per laser
	std::vector<GpuLight> m_lasers;
};
