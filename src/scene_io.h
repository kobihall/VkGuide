#pragma once

// Scene file save/load.
//
// One file captures the whole scene: the glTF and pbrt models it is composed of (by path - the model
// files themselves are untouched), the environment map (by path), the raytracer's shapes and the
// material library they reference. The file is a minimal but valid glTF 2.0 document whose single
// scene carries all of that in its spec-defined `extras` field, so any glTF tool can open it
// without erroring - it just sees an empty scene - and the spec requires readers to preserve
// extras they don't understand.
//
// Paths are written relative to the scene file's own directory when they share a root, so an
// assets folder can move as a unit, and made absolute again on load. Reading navigates the
// extras with simdjson, which fastgltf already links; writing hand-formats the small
// fixed-shape payload with fmt. The payload carries a "version" so a later schema change has
// something to branch on; every older version still loads - 1 (spheres only), 2 (no render
// settings), 3 (no cameras), 4 (no mesh objects), 5 (spheres rather than geometry) and 6 (no
// emissive material or background colour), 7 (no kernel selection or BVH settings), 8 (no pbr
// material), 9 (no punctual lights), 10 (no integrator or traversal), 11 (inline materials rather
// than a library: each object's becomes a user material named after it), 12 to 14 (render settings
// that a file without them reads as the defaults a scene of its time had), 15 (no auxiliary outputs,
// light groups or export settings) and 16 (no shape or environment brought by a model).
//
// Neither function touches the engine: they are pure file io over plain data, and failures come
// back as an IoResult rather than aborting, since a bad path is expected input.

#include <filesystem>
#include <optional>
#include <vector>

#include <rt_accel.h>
#include <rt_kernels.h>
#include <rt_scene_types.h>
#include <vk_types.h>

// A reference to a library entry as a file records it: a user material by its index in
// SceneDescription::materials, or a glTF material by its model's index in modelPaths and its index
// in that file. Neither set (both -1) for an object whose material went missing
struct SceneMaterialRef {
	int user { -1 };
	int model { -1 };
	uint32_t modelMaterial { 0 };
};

// one shape as a file records it: the shape, and its material by reference
struct SceneShapeRecord {
	SceneShape shape;
	SceneMaterialRef material;
	// version 17: the model it was imported with (a pbrt file's sphere), -1 for the scene's own
	int model { -1 };
};

// the parameters the user changed on a glTF material: all a file records of one
struct SceneMaterialOverride {
	int model { 0 };
	uint32_t modelMaterial { 0 };
	ParamSet params;
	// version 16: the light group (0 to 7) the material's emission is kept in
	uint32_t lightGroup { 0 };
};

// One placed mesh object as a file records it. Identical to SceneMeshObject except that the model
// is an index into SceneDescription::modelPaths rather than a key: the keys are assigned by
// VulkanEngine::importGltf() as it loads them and are not stable across a save and reload, while
// the order of the model list is.
struct SceneMeshObjectRecord {
	int model { 0 };
	uint32_t nodeIndex { 0 };
	std::string name;
	glm::mat4 transform { 1.f };
	bool visible { true };
	MeshMaterialMode materialMode { MeshMaterialMode::Model };
	// the override's material; unset in glTF mode
	SceneMaterialRef material;
};

// One punctual light as a file records it: the light, and - for one a glTF import brought in - its
// model as an index into SceneDescription::modelPaths, for the same reason a mesh object records
// one. -1 for a light made in the editor
struct SceneLightRecord {
	SceneLight light;
	int model { -1 };
};

struct SceneDescription {
	// absolute paths
	std::vector<std::filesystem::path> modelPaths;
	// empty when the scene has no environment map
	std::filesystem::path environmentMapPath;
	// version 17: the model whose environment the scene shows (a pbrt file's infinite light), -1 for none. Its
	// import recreates the map, so the file holds no path for it
	int environmentModel { -1 };
	std::vector<SceneShapeRecord> shapes;
	// version 12: the user materials, in full, with unassigned ids (RaytraceSceneEditor::addMaterial()
	// gives them theirs), and the edits to glTF materials. An older file's inline materials arrive
	// here already converted, one user material per object, named after it
	std::vector<MaterialDef> materials;
	std::vector<SceneMaterialOverride> materialOverrides;
	// the mesh objects placing the models' nodes. `hasMeshObjects` tells an absent list (a
	// version-4 file, which predates them - the loader then places every node at its authored
	// transform, as importing does) apart from a scene that genuinely has none because the user
	// deleted them all
	std::vector<SceneMeshObjectRecord> meshObjects;
	bool hasMeshObjects { false };
	// the cameras it can be rendered from, and which one the renderer had selected (an index
	// into cameras; -1 for none). Ids are not saved - the editor assigns them on load
	std::vector<SceneCamera> cameras;
	int renderCamera { -1 };
	// version 10: the punctual lights. `hasLights` tells an older file, whose models' own lights the
	// imports recreate, from a scene whose lights were all deleted
	std::vector<SceneLightRecord> lights;
	bool hasLights { false };
	// how to render it, and how bright the environment lights it
	RenderSettings render;
	float environmentIntensity { 1.f };
	// a solid colour replacing the environment map and the sky for missed rays; none by default
	std::optional<glm::vec3> backgroundColor;
	// version 8: which kernel variant runs in each stage of the wavefront, and how the BVH those
	// kernels read is built; version 11: the integrator and the traversal. Saved by ID, never by index, so a build that registers its variants
	// in another order still opens the file, and one that has dropped a variant falls back to the
	// slot's default with a warning rather than failing the load
	KernelSelection kernels { defaultKernelSelection() };
	AccelSettings accel { defaultAccelSettings() };
	// what was skipped on load: an unknown integrator, traversal, kernel slot, variant or builder
	std::vector<std::string> kernelWarnings;
	// version 16: the names of the light groups, the group the background belongs to, and what exporting a render
	// does. A file before 16 has one group, "Default"
	std::vector<std::string> lightGroupNames { "Default" };
	uint32_t environmentGroup { 0 };
	ExportSettings exportSettings;
	// version 18: how a monochromatic render is shown. Display only, like the export settings
	SpectralDisplay spectralDisplay;
};

IoResult saveSceneFile(const SceneDescription& scene, const std::filesystem::path& file);

// on success out holds the shapes (with unassigned ids - RaytraceSceneEditor::replaceShapes()
// gives them theirs) and absolute paths; on failure it is untouched
IoResult loadSceneFile(const std::filesystem::path& file, SceneDescription& out);
