#pragma once

// The raytracer's scene model and render settings, as plain data.
//
// Everything here is what a scene *is* to the raytracer - spheres with tagged materials, the
// loaded glTF geometry, a camera snapshot, how to render it - with no behaviour attached. Both
// backends read it: the GPU path tracer uploads it as flat buffers, and the CPU backend (kept
// behind a switch until the GPU one is trusted) constructs its own intersection/scattering
// objects from it in buildRaytraceScene(). The editor, the gizmo adapter, the scene file and the
// raster preview spheres all work on these structs directly, so retiring the CPU backend deletes
// its classes without touching any of them.
//
// Float throughout: the GPU works in float and the editor's widgets are float widgets. The CPU
// backend widens to double at conversion time and keeps its ported double math unchanged.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

//---------------------------------------------------------------- materials and spheres

enum class MaterialType : uint8_t {
	Lambertian,
	Metal,
	Phong,
	Dielectric
};

inline constexpr MaterialType MATERIAL_TYPES[] = {
	MaterialType::Lambertian,
	MaterialType::Metal,
	MaterialType::Phong,
	MaterialType::Dielectric,
};

inline const char* materialTypeName(MaterialType type)
{
	switch (type) {
	case MaterialType::Lambertian:
		return "lambertian";
	case MaterialType::Metal:
		return "metal";
	case MaterialType::Phong:
		return "phong";
	case MaterialType::Dielectric:
		return "dielectric";
	}
	return "unknown";
}

// every type's parameters are stored, whichever type is selected, so switching a sphere's
// material type and back keeps its values
struct SphereMaterial {
	MaterialType type { MaterialType::Lambertian };
	// lambertian, metal, phong
	glm::vec3 albedo { 0.7f };
	// metal: radius of the perturbation ball around the mirror direction, 0..1
	float fuzz { 0.3f };
	// phong: 0..1, mapped to a lobe exponent of 1..1000 by the scatter
	float smoothness { 0.5f };
	// dielectric: index of refraction. 1.0 vacuum, 1.33 water, 1.5 glass, 2.42 diamond
	float ir { 1.5f };
};

inline SphereMaterial makeSphereMaterial(MaterialType type, const glm::vec3& albedo = glm::vec3(0.7f))
{
	SphereMaterial material;
	material.type = type;
	material.albedo = albedo;
	return material;
}

// the single flat colour a material is best shown as outside the raytracer - used by the raster
// preview spheres, which have no path tracer to resolve a real appearance with. A dielectric has
// no albedo of its own (it attenuates by white), so a pale tint reads as glass rather than as a
// plain white diffuse sphere
inline glm::vec3 materialPreviewColor(const SphereMaterial& material)
{
	return material.type == MaterialType::Dielectric ? glm::vec3(0.75f, 0.85f, 1.f) : material.albedo;
}

struct SceneSphere {
	// stable across edits and reorders, assigned by the editor and never saved. The gizmo targets
	// a sphere by it, so deleting or reloading the sphere ends the edit rather than dangling
	uint64_t id { 0 };
	std::string name;
	glm::vec3 center { 0.f, 0.f, -1.f };
	float radius { 0.5f };
	SphereMaterial material;
};

//---------------------------------------------------------------- glTF geometry

struct MeshAsset;

// One glTF GeoSurface placed in the world: which retained mesh it indexes into, that surface's
// index range, the owning node's world transform, and the raw material factors the surface was
// authored with. Geometry stays in object space - a consumer that needs world-space triangles
// applies worldTransform itself. Per surface rather than per node because a surface is the
// finest granularity that has exactly one material.
//
// Deliberately without an intersection routine: nothing traces triangles yet. This is the
// bottom-level/top-level split a future BVH or GPU mesh path would want anyway.
struct RTMeshInstance {
	// the node's name, suffixed with the surface index when the node's mesh has several surfaces
	std::string name;
	// into RaytraceMeshData::meshes
	size_t meshIndex { 0 };
	// into that mesh's cpuIndices
	uint32_t firstIndex { 0 };
	uint32_t indexCount { 0 };
	glm::mat4 worldTransform { 1.f };
	glm::vec4 colorFactors { 1.f };
	glm::vec2 metalRoughFactors { 0.f };
};

// The loaded glTF scene as the raytracer sees it: every unique mesh once, plus one instance per
// mesh-bearing node and surface. Built once per scene load by buildRaytraceMeshData() and shared
// immutably (shared_ptr<const>) with every render snapshot taken afterwards, instead of being
// rebuilt on every Render click.
//
// The MeshAssets are held alive here past a scene replacement, but LoadedGLTF::clearAll() will
// already have destroyed their GPU buffers by then. Raytracer code must NEVER read
// MeshAsset::meshBuffers through this - only cpuVertices and cpuIndices, which stay valid.
struct RaytraceMeshData {
	std::vector<std::shared_ptr<const MeshAsset>> meshes;
	std::vector<RTMeshInstance> instances;
	size_t triangleCount { 0 };
};

//---------------------------------------------------------------- camera

// The raster camera's state at the instant Render was clicked. Captured by value so a render
// never reads VulkanEngine::m_mainCamera, which keeps moving while the render runs.
struct RTCameraSnapshot {
	glm::vec3 lookFrom { 0.f, 0.f, 0.f };
	glm::vec3 lookAt { 0.f, 0.f, -1.f };
	glm::vec3 vUp { 0.f, 1.f, 0.f };
	float vfovDegrees { 70.f };
	// a zero aperture makes the lens sampling a no-op
	float aperture { 0.f };
	float focusDistance { 1.f };
};

//---------------------------------------------------------------- render settings

// the render resolution is picked from this fixed list rather than typed in or dragged: every
// entry is a sane, recognisable size, and there is no way to land on a degenerate one
struct ResolutionPreset {
	const char* label;
	int width;
	int height;
};

inline constexpr ResolutionPreset RESOLUTION_PRESETS[] = {
	{ "320 x 180", 320, 180 },
	{ "640 x 360", 640, 360 },
	{ "800 x 600", 800, 600 },
	{ "960 x 540", 960, 540 },
	{ "1280 x 720", 1280, 720 },
	{ "1600 x 900", 1600, 900 },
	{ "1920 x 1080", 1920, 1080 },
};

inline constexpr int DEFAULT_RESOLUTION_PRESET = 1;

// How a render is made. Owned by RaytraceRenderer and saved with the scene; both backends read
// it, and the panel greys out the rows the selected backend ignores
struct RenderSettings {
	int width { RESOLUTION_PRESETS[DEFAULT_RESOLUTION_PRESET].width };
	int height { RESOLUTION_PRESETS[DEFAULT_RESOLUTION_PRESET].height };
	// render at the viewport's current draw resolution instead of width x height, captured at
	// Render like everything else, so the output is pixel-for-pixel what the raster pass shows
	bool matchViewport { false };
	// jitter each primary ray within its pixel and average maxSamples of them - supersampling,
	// not Vulkan MSAA. Off traces a single un-jittered ray per pixel and stops there
	bool antialiasing { true };
	// samples per pixel this render takes: the CPU backend's per-pixel loop count, the GPU
	// backend's stopping point
	int maxSamples { 8 };
	int rayDepth { 8 };
	// seeds the render with `seed` instead of a random draw, so the same scene and settings
	// reproduce the same image - useful when comparing renders or debugging. Both backends
	// honour it; they do not produce the same image as each other
	bool useFixedSeed { false };
	uint32_t seed { 1 };

	// GPU only: samples per pixel added each engine frame (a K-times-larger path pool, not K loops)
	int samplesPerFrame { 1 };
	// GPU only: after minBouncesBeforeRoulette, a path survives with probability max(throughput)
	// and is reweighted, which is unbiased and makes late bounces cheap. Off for CPU comparisons
	bool russianRoulette { true };
	int minBouncesBeforeRoulette { 3 };

	// the thin lens, both backends. A zero aperture is a pinhole; focusDistance is where the
	// image plane sits and must stay above zero
	float aperture { 0.f };
	float focusDistance { 6.f };

	// display only: TonemapPass's scale. Scales the picture, not the light (that is the scene's
	// environmentIntensity)
	float exposure { 1.f };
};
