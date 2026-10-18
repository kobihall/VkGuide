#pragma once

// The raytracer's scene model and render settings, as plain data.
//
// Everything here is what a scene *is* to the raytracer - analytic shapes and their materials, the
// cameras it can be rendered from, the loaded glTF geometry, how to render it - with no
// behaviour attached. The GPU path tracer builds its BVHs from it (rt_accel.h); the editor, the gizmo
// adapters, the scene file and the raster preview all work on these structs directly. Every
// struct compares with ==, which is how the renderer notices that the thing it is rendering has
// changed under it.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rt_material.h>
#include <shape.h>

//---------------------------------------------------------------- shapes

// One editable dimension of a shape: what the editor and the scene file call it, and which axes of
// SceneShape::size it drives (bit 0 x, bit 1 y, bit 2 z). A cylinder's radius drives x and z at
// once, which is what keeps it round
struct ShapeParam {
	const char* name;
	uint8_t axes;
};

// Everything that differs between the kinds of shape outside the tracer itself, as data, so the
// editor, the gizmo, the scene file and the preview all treat every kind through the one table.
// Adding a kind is a row here, a unit primitive in shape.h / crt_shape.glsl and a preview mesh
struct ShapeTraits {
	// the scene file's type name, and the prefix of a new object's name
	const char* name;
	// the Add menu's entry, and the scene tree's folder
	const char* label;
	const char* plural;
	// the sizes the user edits; empty for a shape with none (the infinite plane)
	std::span<const ShapeParam> params;
	// false for a shape rotating would not change (the sphere): its orientation is neither shown,
	// edited, saved nor applied
	bool rotatable;
	// where a new one is placed, and at what size
	glm::vec3 defaultPosition;
	glm::vec3 defaultSize;
};

inline constexpr ShapeParam SPHERE_PARAMS[] = { { "radius", 0b111 } };
inline constexpr ShapeParam QUAD_PARAMS[] = { { "width", 0b001 }, { "length", 0b100 } };
inline constexpr ShapeParam BOX_PARAMS[] = { { "width", 0b001 }, { "height", 0b010 }, { "depth", 0b100 } };
inline constexpr ShapeParam CYLINDER_PARAMS[] = { { "radius", 0b101 }, { "height", 0b010 } };

// by ShapeKind; the sizes are the unit primitives' scale (shape.h), so a radius is a radius and a
// width a width
inline constexpr ShapeTraits SHAPE_TRAITS[] = {
	{ "sphere", "Sphere", "Spheres", SPHERE_PARAMS, false, { 0.f, 0.f, -1.f }, glm::vec3(0.5f) },
	// a level floor just below the default sphere
	{ "plane", "Plane", "Planes", {}, true, { 0.f, -0.5f, 0.f }, glm::vec3(1.f) },
	{ "quad", "Quad", "Quads", QUAD_PARAMS, true, { 0.f, 0.f, -1.f }, glm::vec3(1.f) },
	{ "box", "Box", "Boxes", BOX_PARAMS, true, { 0.f, 0.f, -1.f }, glm::vec3(1.f) },
	{ "cylinder", "Cylinder", "Cylinders", CYLINDER_PARAMS, true, { 0.f, 0.f, -1.f }, { 0.5f, 1.f, 0.5f } },
};
static_assert(sizeof(SHAPE_TRAITS) / sizeof(SHAPE_TRAITS[0]) == SHAPE_KIND_COUNT);

inline const ShapeTraits& shapeTraits(ShapeKind kind)
{
	return SHAPE_TRAITS[(size_t)kind];
}

// A placed analytic shape: its kind's unit primitive (shape.h) scaled by size, rotated and moved.
// One struct for every kind, so every list, lookup, gizmo adapter and file record is written once
struct SceneShape {
	// stable across edits and reorders, assigned by the editor and never saved. The gizmo targets
	// a shape by it, so deleting or reloading the shape ends the edit rather than dangling
	uint64_t id { 0 };
	std::string name;
	ShapeKind kind { ShapeKind::Sphere };
	glm::vec3 position { 0.f, 0.f, -1.f };
	glm::quat orientation { 1.f, 0.f, 0.f, 0.f };
	// the unit primitive's scale along its own axes. What each axis means is the kind's params; an
	// axis none of them drives stays 1
	glm::vec3 size { 1.f };
	// the library entry it is shaded with (RaytraceSceneEditor::materials()). On a shape a material is
	// packed without its textures, which shapes cannot carry until docs/plans/pbrt-textures.md
	uint64_t materialId { 0 };

	// the unit primitive -> world
	glm::mat4 objectToWorld() const
	{
		const glm::mat4 rotation = shapeTraits(kind).rotatable ? glm::mat4_cast(orientation) : glm::mat4(1.f);
		return glm::translate(glm::mat4(1.f), position) * rotation * glm::scale(glm::mat4(1.f), size);
	}

	bool operator==(const SceneShape&) const = default;
};

// a param's value: every axis it drives holds the same one
inline float shapeParamValue(const SceneShape& shape, const ShapeParam& param)
{
	for (int axis = 0; axis < 3; axis++) {
		if (param.axes & (1u << axis)) {
			return shape.size[axis];
		}
	}
	return 1.f;
}

inline void setShapeParam(SceneShape& shape, const ShapeParam& param, float value)
{
	for (int axis = 0; axis < 3; axis++) {
		if (param.axes & (1u << axis)) {
			shape.size[axis] = value;
		}
	}
}

inline SceneShape makeShape(ShapeKind kind)
{
	SceneShape shape;
	shape.kind = kind;
	shape.position = shapeTraits(kind).defaultPosition;
	shape.size = shapeTraits(kind).defaultSize;
	return shape;
}

//---------------------------------------------------------------- cameras

// A camera the scene can be rendered from: a placeable object like a shape, with the lens
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

//---------------------------------------------------------------- punctual lights

// glTF's KHR_lights_punctual three, the values saved in the scene file by name (lightKindName())
enum class LightKind : uint8_t {
	Point,
	Spot,
	Directional
};

inline constexpr LightKind LIGHT_KINDS[] = {
	LightKind::Point,
	LightKind::Spot,
	LightKind::Directional,
};

// the scene file's type name, and the prefix of a new light's name
inline const char* lightKindName(LightKind kind)
{
	switch (kind) {
	case LightKind::Point:
		return "point";
	case LightKind::Spot:
		return "spot";
	case LightKind::Directional:
		return "directional";
	}
	return "unknown";
}

// the Add menu's entry
inline const char* lightKindLabel(LightKind kind)
{
	switch (kind) {
	case LightKind::Point:
		return "Point light";
	case LightKind::Spot:
		return "Spot light";
	case LightKind::Directional:
		return "Directional light";
	}
	return "Light";
}

// glTF gives point and spot intensity in candela and directional intensity in lux; the tracer
// works in radiometric units, with an environment map's texels taken as radiance in W/(sr m^2).
// 683 lm/W is the luminous efficacy of 555 nm light, the factor Blender's glTF exporter uses
inline constexpr float PHOTOMETRIC_TO_RADIOMETRIC = 1.f / 683.f;

// A punctual light: a point, a spot or a directional light, a scene object like a camera or a
// shape. It has no surface, so no ray can ever hit one - a BSDF sample never finds it, and the path
// tracer reaches it only through shadow rays, whichever direct-lighting strategy is selected. A
// light a glTF file carries becomes one of these on import, at its node's transform
struct SceneLight {
	// stable across edits and reorders, assigned by the editor and never saved; shares one id space
	// with the shapes, cameras and mesh objects
	uint64_t id { 0 };
	std::string name;
	LightKind kind { LightKind::Point };
	glm::vec3 position { 0.f, 2.f, 0.f };
	// a spot or directional light shines down its local -z, as glTF's do. The identity points it
	// down -z, the way the free camera looks at yaw 0
	glm::quat orientation { 1.f, 0.f, 0.f, 0.f };
	// linear rgb
	glm::vec3 color { 1.f };
	// point and spot: radiant intensity in W/sr. Directional: irradiance in W/m^2
	float intensity { 10.f };
	// point and spot: the distance glTF's recommended window brings the light to zero at; 0 for an
	// unwindowed inverse-square falloff
	float range { 0.f };
	// spot: full intensity inside the inner cone, none outside the outer, in degrees from the axis
	float innerConeDegrees { 20.f };
	float outerConeDegrees { 30.f };
	// the model this light was imported with, which removing the model removes it with; empty for a
	// light made in the editor
	std::string modelKey;

	// the direction the light shines (spot) or travels (directional)
	glm::vec3 direction() const { return orientation * glm::vec3(0.f, 0.f, -1.f); }
	// light space -> world
	glm::mat4 transform() const { return glm::translate(glm::mat4(1.f), position) * glm::mat4_cast(orientation); }

	bool operator==(const SceneLight&) const = default;
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
	// the library entry each of its surfaces' glTF materials became when the model was imported
	Gltf,
	// one library material for the whole object, in place of every surface's glTF material
	Override
};

// One mesh-bearing glTF node placed in the scene: a first-class object like a shape or a
// camera, with the same stable id, the same gizmo editing and the same delete.
//
// The geometry itself stays in the loaded model, named by modelKey + nodeIndex; everything the
// user can edit about the *placed* object - its name, where it sits, whether it is shown, how it
// is shaded - lives here. Deleting an object therefore removes it from the scene and leaves the
// model loaded, which is what lets one imported file be placed more than once.
struct SceneMeshObject {
	// stable across edits and reorders, assigned by the editor; shares one id space with the
	// shapes and cameras so the gizmo's opaque target id cannot collide
	uint64_t id { 0 };
	std::string name;
	// into VulkanEngine::m_models, and into that model's forEachMeshNode() walk order
	std::string modelKey;
	uint32_t nodeIndex { 0 };
	// world transform, seeded from the node's authored transform at import and owned here after
	glm::mat4 transform { 1.f };
	bool visible { true };
	MeshMaterialMode materialMode { MeshMaterialMode::Gltf };
	// the override's library entry; 0 until the object is first switched to override
	uint64_t materialId { 0 };

	bool operator==(const SceneMeshObject&) const = default;
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

// the render resolution is normally picked from this fixed list rather than dragged: every entry
// is a sane, recognisable size. A custom size can still be typed in, clamped to
// [MIN_RENDER_DIMENSION, MAX_RENDER_DIMENSION] so it can never be degenerate or absurd
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

// the bounds a typed-in custom resolution is held to. The lower one keeps a render from
// collapsing to nothing; the upper one is well past any display, and the render guard
// (WORK_PER_FRAME_BUDGET) is what actually stops a size that is merely too expensive
inline constexpr int MIN_RENDER_DIMENSION = 2;
inline constexpr int MAX_RENDER_DIMENSION = 8192;

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
	// start the render over whenever what it renders changes: the camera moves, a shape or
	// the environment is edited, a setting is changed. With unlimited samples this is a live
	// view of the scene
	bool restartOnChange { true };

	bool operator==(const RenderSettings&) const = default;
};
