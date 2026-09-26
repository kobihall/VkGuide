#pragma once

// The light list: every light the path tracer's next-event estimation can sample, packed as the
// GPU reads it (shaders/rt/include/crt_common.glsl GpuLight, LightHeader; sampled by
// shaders/rt/include/crt_light.glsl). Built with the rest of the traced scene by buildSceneAccel()
// (rt_accel.h), whenever an object, a light or the background changes.
//
// Four kinds of source, in this order in the list:
//  - the punctual lights (SceneLight), first, so the delta lights are lights[0, deltaCount)
//  - analytic shapes whose material emits: one record per shape, six for a box, none for the
//    infinite plane, which has no finite area to sample
//  - glTF triangles whose material emits, one record per triangle
//  - the environment map, when it is the background
// Each gets a power - what it sends into the scene - and an alias table picks among them in
// proportion to it (pbrt-v4's PowerLightSampler). The delta lights get a second table over
// themselves alone, which the BSDF strategy uses, since nothing else can reach them.
//
// An emitter the list leaves out (the infinite plane, a black light) is still correct: kernel 04
// finds no record for it and counts whatever a BSDF sample finds on it at full weight.

#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include <light_sampling.h>
#include <rt_scene_types.h>

// crt_common.glsl CRT_LIGHT_*: AreaLightKind for the first four
enum class GpuLightKind : uint32_t {
	Sphere = 0,
	Rectangle = 1,
	Cylinder = 2,
	Triangle = 3,
	Point = 4,
	Spot = 5,
	Directional = 6,
	Environment = 7
};

// crt_common.glsl CRT_NO_LIGHT: the index of no light
inline constexpr uint32_t GPU_NO_LIGHT = 0xFFFFFFFFu;

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
	uint32_t pad[3] {};
};
static_assert(sizeof(GpuLight) == 96);

// 32 bytes, crt_common.glsl LightHeader
struct GpuLightHeader {
	uint32_t count { 0 };
	uint32_t deltaCount { 0 };
	uint32_t environment { GPU_NO_LIGHT };
	uint32_t pad0 { 0 };
	float invTotalPower { 0.f };
	float invDeltaPower { 0.f };
	float pad1 { 0.f };
	float pad2 { 0.f };
};
static_assert(sizeof(GpuLightHeader) == 32);

struct RaytraceLightList {
	GpuLightHeader header;
	std::vector<GpuLight> lights;
	// per emitting mesh instance, one entry per triangle of its mesh (by the triangle's index in the
	// mesh): its record in `lights`, or GPU_NO_LIGHT. GpuInstance::lightBase finds an instance's run
	std::vector<uint32_t> triangleLights;

	// for the panel
	uint32_t punctualLights { 0 };
	uint32_t shapeLights { 0 };
	uint32_t triangleLightCount { 0 };
	bool environment { false };
	float totalPower { 0.f };
};

// Collects the lights one at a time, then finish() prices the ones that depend on the scene's size
// and builds the alias tables. Punctual lights must be added before anything else
class LightListBuilder {
public:
	void addPunctual(const SceneLight& light);
	// an analytic shape's emitting surfaces (shapeLightGeometry()), all with the same radiance. The
	// first record's index, or GPU_NO_LIGHT when the shape adds none
	uint32_t addShape(ShapeKind kind, const glm::mat4& objectToWorld, const glm::vec3& radiance);
	// one emitting triangle of a placed mesh, in world space: its record's index, or GPU_NO_LIGHT for
	// a triangle with no area or no emission. `attributeIndex` is its global attribute record,
	// `material` its index in the scene's materials (for the alpha test of a cut-out emitter),
	// `emissiveLayer` its texture, -1 for none, and `textureMean` that texture's mean luminance over it
	uint32_t addTriangle(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& radiance, uint32_t attributeIndex, uint32_t material, int emissiveLayer, float textureMean);
	// `luminanceIntegral` is EnvironmentDistribution::luminanceIntegral
	void addEnvironment(float luminanceIntegral, float intensity);

	// a run of triangleLights entries for one mesh instance, all GPU_NO_LIGHT until set
	uint32_t beginTriangleRun(uint32_t triangleCount);
	void setTriangleLight(uint32_t runBase, uint32_t triangle, uint32_t light);

	// `sceneRadius` is the radius of a sphere around the scene's geometry: a directional light and
	// the environment deliver power in proportion to the area they fall on (PBR 4ed 12.4, 12.5)
	RaytraceLightList finish(float sceneRadius);

private:
	RaytraceLightList m_list;
	// per record: power already known, or a power per unit of scene cross-section (pi r^2) for the
	// directional lights and the environment
	std::vector<float> m_crossSectionPower;
};
