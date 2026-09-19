#pragma once

// The raytracer's scene model and render settings, as plain data.
//
// Everything here is what a scene *is* to the raytracer - spheres with tagged materials, the
// cameras it can be rendered from, the loaded glTF geometry, how to render it - with no
// behaviour attached. The GPU path tracer builds its BVHs from it (rt_accel.h); the editor, the gizmo
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
struct GLTFMaterial;

// One glTF GeoSurface of one mesh-bearing node, in the node's own object space: which retained
// mesh it indexes into, that surface's index range, and the material it was authored with. Per
// surface rather than per node because a surface is the finest granularity that has exactly one
// material.
//
// Geometry stays in object space here, and in the BVH built over it. A placed SceneMeshObject
// carries the world transform, which the path tracer's TLAS applies to the ray, never to the mesh.
struct RTMeshSurface {
	// into RaytraceMeshData::meshes
	size_t meshIndex { 0 };
	// into that mesh's cpuIndices
	uint32_t firstIndex { 0 };
	uint32_t indexCount { 0 };
	// held alive here so the factors and the base-colour image survive as long as this data does.
	// The MaterialInstance inside it belongs to the owning model's descriptor pool and must never
	// be used through this pointer - only colorFactors, metalRoughFactors and baseColorImage
	std::shared_ptr<const GLTFMaterial> material;
};

// One mesh-bearing glTF node of one loaded model, identified the way a SceneMeshObject refers to
// it: the model's key in VulkanEngine::m_models, and the node's position in the deterministic
// walk order forEachMeshNode() visits that model in.
struct RTMeshNode {
	std::string modelKey;
	uint32_t nodeIndex { 0 };
	// the source glTF node's name, for the browser
	std::string name;
	// the transform the node was authored with, which a newly imported object is placed at
	glm::mat4 authoredTransform { 1.f };
	std::vector<RTMeshSurface> surfaces;
	size_t triangleCount { 0 };
};

// The loaded glTF models as the raytracer sees them: every unique mesh once, plus every
// mesh-bearing node. Built once per change to VulkanEngine::m_models by buildRaytraceMeshData()
// and shared immutably (shared_ptr<const>), so moving an object never re-reads the models.
//
// The MeshAssets are held alive here past a scene replacement, but LoadedGLTF::clearAll() will
// already have destroyed their GPU buffers by then. Raytracer code must NEVER read
// MeshAsset::meshBuffers through this - only cpuVertices and cpuIndices, which stay valid.
struct RaytraceMeshData {
	std::vector<std::shared_ptr<const MeshAsset>> meshes;
	std::vector<RTMeshNode> nodes;
	size_t triangleCount { 0 };

	// the node a SceneMeshObject names, or null once its model has been removed
	const RTMeshNode* findNode(const std::string& modelKey, uint32_t nodeIndex) const
	{
		for (const RTMeshNode& node : nodes) {
			if (node.nodeIndex == nodeIndex && node.modelKey == modelKey) {
				return &node;
			}
		}
		return nullptr;
	}
};

//---------------------------------------------------------------- mesh objects

// How the raytracer shades a placed mesh object
enum class MeshMaterialMode : uint8_t {
	// the glTF material each of its surfaces was authored with: the base-colour factor modulated
	// by the base-colour texture, scattered as a diffuse surface
	Gltf,
	// one raytracer material (the same four a sphere offers) for the whole object, replacing the
	// glTF material and its texture entirely
	Override
};

// One mesh-bearing glTF node placed in the scene: a first-class object like a sphere or a
// camera, with the same stable id, the same gizmo editing and the same delete.
//
// The geometry itself stays in the loaded model, named by modelKey + nodeIndex; everything the
// user can edit about the *placed* object - its name, where it sits, whether it is shown, how it
// is shaded - lives here. Deleting an object therefore removes it from the scene and leaves the
// model loaded, which is what lets one imported file be placed more than once.
struct SceneMeshObject {
	// stable across edits and reorders, assigned by the editor; shares one id space with the
	// spheres and cameras so the gizmo's opaque target id cannot collide
	uint64_t id { 0 };
	std::string name;
	// into VulkanEngine::m_models, and into that model's forEachMeshNode() walk order
	std::string modelKey;
	uint32_t nodeIndex { 0 };
	// world transform, seeded from the node's authored transform at import and owned here after
	glm::mat4 transform { 1.f };
	bool visible { true };
	MeshMaterialMode materialMode { MeshMaterialMode::Gltf };
	SphereMaterial material;

	bool operator==(const SceneMeshObject&) const = default;
};

//---------------------------------------------------------------- mesh materials

// A material a triangle is shaded with: the same tagged material a sphere uses, plus the
// base-colour texture that modulates its albedo (a layer of the raytracer's texture array, -1
// for untextured)
struct RaytraceTriMaterial {
	SphereMaterial material;
	int albedoLayer { -1 };

	bool operator==(const RaytraceTriMaterial&) const = default;
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
