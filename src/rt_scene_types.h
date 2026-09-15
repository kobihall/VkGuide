#pragma once

// The raytracer's scene model and render settings, as plain data.
//
// Everything here is what a scene *is* to the raytracer - spheres with tagged materials, the
// cameras it can be rendered from, the loaded glTF geometry, how to render it - with no
// behaviour attached. The GPU path tracer uploads it as flat buffers; the editor, the gizmo
// adapters, the scene file and the raster preview all work on these structs directly. Every
// struct compares with ==, which is how the renderer notices that the thing it is rendering has
// changed under it.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

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

	bool operator==(const SphereMaterial&) const = default;
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

	bool operator==(const SceneSphere&) const = default;
};

//---------------------------------------------------------------- cameras

// A camera the scene can be rendered from: a placeable object like a sphere, with the lens
// and display settings that belong to a camera rather than to a render. The raster viewport's
// free camera is not one of these - it is a tool for looking at the scene - but "first-person
// edit" lets it drive one. Orientation is a quaternion so the gizmo can rotate it freely; the
// identity looks down -z with +y up, the same as the free camera at yaw 0, pitch 0
struct SceneCamera {
	// stable across edits and reorders, assigned by the editor and never saved
	uint64_t id { 0 };
	std::string name;
	glm::vec3 position { 0.f, 0.f, 5.f };
	glm::quat orientation { 1.f, 0.f, 0.f, 0.f };
	float vfovDegrees { 70.f };
	// the thin lens: a zero aperture is a pinhole; focusDistance is where the image plane sits
	float aperture { 0.f };
	float focusDistance { 6.f };
	// display only: TonemapPass's scale for renders from this camera. Scales the picture, not
	// the light (that is the scene's environmentIntensity)
	float exposure { 1.f };

	glm::vec3 forward() const { return orientation * glm::vec3(0.f, 0.f, -1.f); }
	glm::vec3 up() const { return orientation * glm::vec3(0.f, 1.f, 0.f); }
	// camera space -> world
	glm::mat4 transform() const { return glm::translate(glm::mat4(1.f), position) * glm::mat4_cast(orientation); }

	bool operator==(const SceneCamera&) const = default;
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

//---------------------------------------------------------------- camera snapshot

// What the path tracer's ray generation needs of a camera, by value: the look-from / look-at /
// up form the ported RTIOW camera is built from, plus the lens. Everything about the camera
// that affects the rays and nothing that does not (exposure is applied at display time), so
// comparing two of these says whether a render has to start over
struct RTCameraSnapshot {
	glm::vec3 lookFrom { 0.f, 0.f, 0.f };
	glm::vec3 lookAt { 0.f, 0.f, -1.f };
	glm::vec3 vUp { 0.f, 1.f, 0.f };
	float vfovDegrees { 70.f };
	float aperture { 0.f };
	float focusDistance { 1.f };

	bool operator==(const RTCameraSnapshot&) const = default;
};

inline RTCameraSnapshot cameraSnapshot(const SceneCamera& camera)
{
	RTCameraSnapshot snapshot;
	snapshot.lookFrom = camera.position;
	snapshot.lookAt = camera.position + camera.forward();
	snapshot.vUp = camera.up();
	snapshot.vfovDegrees = camera.vfovDegrees;
	//a zero focus distance would collapse the whole image plane onto one point
	snapshot.aperture = glm::max(camera.aperture, 0.f);
	snapshot.focusDistance = glm::max(camera.focusDistance, 0.01f);
	return snapshot;
}

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

// How a render is made. Owned by RaytraceRenderer and saved with the scene. The camera's own
// settings (lens, exposure) live on the SceneCamera, and which camera renders is the
// renderer's choice
struct RenderSettings {
	int width { RESOLUTION_PRESETS[DEFAULT_RESOLUTION_PRESET].width };
	int height { RESOLUTION_PRESETS[DEFAULT_RESOLUTION_PRESET].height };
	// render at the viewport's current draw resolution instead of width x height, so the output
	// is pixel-for-pixel what the raster pass shows
	bool matchViewport { false };
	// jitter each primary ray within its pixel and accumulate samples. Off traces a single
	// un-jittered ray per pixel and stops there
	bool antialiasing { true };
	// samples per pixel a render takes before it stops by itself...
	int maxSamples { 64 };
	// ...unless it never does: keep refining until stopped or restarted
	bool unlimitedSamples { false };
	int rayDepth { 8 };
	// seeds the render with `seed` instead of a random draw, so the same scene and settings
	// reproduce the same image
	bool useFixedSeed { false };
	uint32_t seed { 1 };
	// samples per pixel added each engine frame (a K-times-larger path pool, not K loops)
	int samplesPerFrame { 1 };
	// after minBouncesBeforeRoulette, a path survives with probability max(throughput) and is
	// reweighted, which is unbiased and makes late bounces cheap
	bool russianRoulette { true };
	int minBouncesBeforeRoulette { 3 };
	// start the render over whenever what it renders changes: the camera moves, a sphere or
	// the environment is edited, a setting is changed. With unlimited samples this is a live
	// view of the scene
	bool restartOnChange { true };

	bool operator==(const RenderSettings&) const = default;
};
