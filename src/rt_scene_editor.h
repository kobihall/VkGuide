#pragma once

// The live, persistent list of scene objects the raytracer owns - analytic shapes, cameras, punctual
// lights and the placed glTF mesh objects - the material library they are shaded with, the "Scene"
// panel that browses and edits the objects, and the "Materials" window that edits the library.
//
// All of it is plain data (rt_scene_types.h, rt_material.h) owned outright here; a render copies what it
// needs when it starts, and compares later to know when to start over. A mesh object does not own
// its geometry: it names a node of one of VulkanEngine::m_models, which the engine loads and
// unloads, and carries only what the user can edit about the placed instance. Deleting the
// object leaves the model loaded. Scene files themselves are the engine's concern
// (VulkanEngine::openScene()/saveScene(), driven from the File menu).

#include <array>
#include <functional>
#include <string_view>

#include <rt_scene_types.h>
#include <vk_types.h>

class VulkanEngine;
class TransformGizmo;

// the per-object imgui controls, one per struct rather than a virtual per class. Each returns
// whether anything the render depends on changed; drawCameraParams() also reports a change to
// a display-only setting (exposure), which does not warrant restarting a render
bool drawShapeParams(SceneShape& shape);
bool drawCameraParams(SceneCamera& camera, bool& displayChanged);
bool drawLightParams(SceneLight& light);
bool drawMeshObjectParams(SceneMeshObject& object);

class RaytraceSceneEditor {
public:
	// an empty scene with one camera at the free camera's starting pose. The startup scene is a
	// file the engine opens (assets/scenes/sphere_scene.gltf), not something seeded here
	RaytraceSceneEditor();

	void drawPanel(VulkanEngine* engine);

	const std::vector<SceneShape>& shapes() const { return m_shapes; }
	const std::vector<SceneCamera>& cameras() const { return m_cameras; }
	const std::vector<SceneLight>& lights() const { return m_lights; }
	const std::vector<SceneMeshObject>& meshObjects() const { return m_meshObjects; }
	// THE MATERIAL LIBRARY: user materials, and one entry per glTF material of every loaded model
	const std::vector<MaterialDef>& materials() const { return m_materials; }

	// by stable id; null once the object has been deleted or the list replaced. The pointer
	// is only good until the next mutation - do not keep it across frames
	SceneShape* findShape(uint64_t id);
	const SceneShape* findShape(uint64_t id) const;
	SceneCamera* findCamera(uint64_t id);
	const SceneCamera* findCamera(uint64_t id) const;
	SceneLight* findLight(uint64_t id);
	const SceneLight* findLight(uint64_t id) const;
	SceneMeshObject* findMeshObject(uint64_t id);
	const SceneMeshObject* findMeshObject(uint64_t id) const;
	MaterialDef* findMaterial(uint64_t id);
	const MaterialDef* findMaterial(uint64_t id) const;
	// the entry a model's import made for its glTF material at `index`, or null
	MaterialDef* findModelMaterial(std::string_view modelKey, uint32_t index);
	const MaterialDef* findModelMaterial(std::string_view modelKey, uint32_t index) const;

	// the load path: swaps in whole new lists. Every object gets a fresh id, and both go
	// through the same change tracking every manual edit uses, so consumers cannot miss it
	void replaceShapes(std::vector<SceneShape>&& shapes);
	void replaceCameras(std::vector<SceneCamera>&& cameras);
	void replaceLights(std::vector<SceneLight>&& lights);
	void replaceMeshObjects(std::vector<SceneMeshObject>&& objects);
	void replaceMaterials(std::vector<MaterialDef>&& materials);

	// THE LIGHT GROUPS (docs/plans/completed/pbrt-aovs-and-image-output.md): up to MAX_LIGHT_GROUPS named groups.
	// A light, an emissive material and the background each carry the index of one, so a render that keeps the
	// groups' sums can export and relight them apart. Group 0 is the default, so a scene that touches none has one
	const std::vector<std::string>& lightGroupNames() const { return m_lightGroupNames; }
	// the group the background belongs to (the environment map, the sky gradient or the solid colour)
	uint32_t environmentGroup() const { return m_environmentGroup; }
	// the load path: the names and the background's group. A light or material in a group the list does not have
	// moves to group 0
	void setLightGroups(std::vector<std::string> names, uint32_t environmentGroup);
	// the group list's editor, in the Raytrace Render panel: rename, add, remove, and the background's group
	void drawLightGroupEditor();
	// a combo over the names; true when the choice changed. A group the list no longer has shows as group 0
	bool drawLightGroupCombo(const char* label, uint32_t& group) const;

	// appends a library entry, gives it an id and returns the id
	uint64_t addMaterial(MaterialDef material);
	// the model-removal path: drops the entries that model's import made. One an object still
	// references becomes a user material instead, without its textures, and its name is returned
	std::vector<std::string> removeMaterialsOf(const std::string& modelKey);
	// the texture-array rebuild path: `refresh` re-derives a glTF entry's read-only parameters and
	// says whether it changed anything
	void refreshModelMaterials(const std::function<bool(MaterialDef&)>& refresh);
	// the shapes and override mesh objects that reference the entry
	size_t materialUsers(uint64_t id) const;
	// shows the Materials window with the entry selected
	void openMaterial(uint64_t id);

	// appends and returns the new id
	uint64_t addCamera(SceneCamera camera);
	// the import path: the objects a freshly loaded model contributes (defaultMeshObjects())
	void addMeshObjects(std::vector<SceneMeshObject>&& objects);
	// the model-removal path: drops every object placing a node of that model, since the geometry
	// they name is about to stop existing
	void removeMeshObjectsOf(const std::string& modelKey);
	// the import path's lights: the KHR_lights_punctual lights a freshly loaded model carries
	void addLights(std::vector<SceneLight>&& lights);
	// the analytic shapes an imported model brings (a pbrt file's spheres), and their removal with it
	void addShapes(std::vector<SceneShape>&& shapes);
	void removeShapesOf(const std::string& modelKey);
	// and the removal path's: the lights imported with that model go with it
	void removeLightsOf(const std::string& modelKey);

	// the first-person edit path: the raster camera writes its pose into a scene camera every
	// frame. Marks a change only when the pose actually moved, so a resting camera does not
	// restart renders
	void setCameraPose(uint64_t id, const glm::vec3& position, const glm::quat& orientation);

	// incremented by every mutation that a render depends on: add, delete, shape, material,
	// camera or mesh-object parameters, a gizmo drag, a loaded file. A consumer that derives data
	// from the scene (the renderer's restart-on-change and its triangle data, the raster preview
	// shapes' material buffers) stores the last revision it acted on and compares. Nothing ever
	// resets it, so adding a consumer cannot starve another
	uint64_t revision() const { return m_revision; }
	// moves with every change but a camera's: what the acceleration structure and the light list depend on, so the
	// renderer can tell in one compare that a frame changed nothing it builds from
	uint64_t contentRevision() const { return m_contentRevision; }

	bool* visibilityFlag() { return &m_showPanel; }

	// the "Materials" window, under Windows
	void drawMaterialsWindow(VulkanEngine* engine);
	bool* materialsVisibilityFlag() { return &m_showMaterials; }

private:
	// which row of the object tree is selected. Rows come from three separately owned lists, so a
	// selection is an id rather than an index: a list can be replaced or reordered under us
	// between frames, and an id that no longer resolves simply means nothing is selected
	enum class SelectionKind {
		None,
		Camera,
		Shape,
		Light,
		MeshObject
	};

	void drawToolbar(VulkanEngine* engine);
	void drawObjectTree(VulkanEngine* engine);
	void drawCameraRows();
	// the Lights folder: every punctual light, whatever its kind
	void drawLightRows();
	// the Geometry folder: every shape, in one sub-folder per kind
	void drawGeometryRows();
	void drawModelRows(VulkanEngine* engine);
	// one selectable row, with the shared rename-in-place and right-click context menu. Returns
	// false when the row deleted its own object, in which case the caller must stop touching it
	bool drawObjectRow(SelectionKind kind, uint64_t id, const std::string& name);
	void drawSelection(VulkanEngine* engine);
	void drawSelectedShape(VulkanEngine* engine, SceneShape& shape);
	void drawSelectedCamera(VulkanEngine* engine, SceneCamera& camera);
	void drawSelectedLight(VulkanEngine* engine, SceneLight& light);
	void drawSelectedMeshObject(VulkanEngine* engine, SceneMeshObject& object);
	// a material combo over the whole library, with Edit and New from this. Returns whether the
	// reference changed
	bool drawMaterialReference(uint64_t& materialId, const std::string& ownerName);
	// the Materials window's parts
	void drawMaterialTree();
	void drawMaterialDetails(VulkanEngine* engine, MaterialDef& material);
	// a user material that copies `source`: its parameters, without the origin, the edits or the
	// textures a user material cannot keep yet
	MaterialDef userCopyOf(const MaterialDef& source, std::string name) const;
	// a new user material for a new object, named after it
	uint64_t addMaterialFor(const std::string& ownerName);
	void beginShapeGizmo(TransformGizmo& gizmo, uint64_t id);
	void beginCameraGizmo(TransformGizmo& gizmo, uint64_t id);
	void beginLightGizmo(TransformGizmo& gizmo, uint64_t id);
	void beginMeshGizmo(TransformGizmo& gizmo, uint64_t id);
	void addShape(SceneShape shape);
	// the Add menu's creating entries
	void createShape(ShapeKind kind);
	void createCamera(VulkanEngine* engine);
	void createLight(VulkanEngine* engine, LightKind kind);
	void addLight(SceneLight light);
	void duplicateSelection();
	// removes whatever is selected, whichever kind it is, and clears the selection
	void deleteSelection();
	void select(SelectionKind kind, uint64_t id);

	void markChanged()
	{
		m_revision++;
		m_contentRevision++;
	}
	// a change to a camera alone (its pose, lens or exposure), which restarts a render but rebuilds nothing
	void markCameraChanged() { m_revision++; }

	std::vector<SceneShape> m_shapes;
	std::vector<SceneCamera> m_cameras;
	std::vector<SceneLight> m_lights;
	std::vector<SceneMeshObject> m_meshObjects;
	std::vector<MaterialDef> m_materials;
	std::vector<std::string> m_lightGroupNames { "Default" };
	uint32_t m_environmentGroup { 0 };

	SelectionKind m_selectionKind { SelectionKind::None };
	uint64_t m_selectionId { 0 };
	// the object whose name is being edited in place, 0 for none, and the edit buffer
	uint64_t m_renamingId { 0 };
	char m_renameBuffer[128] {};

	// by ShapeKind: the number the next new shape of that kind is named with
	std::array<int, SHAPE_KIND_COUNT> m_nextShapeNumber {};
	int m_nextCameraNumber { 1 };
	int m_nextLightNumber { 1 };
	// the Materials window's selection, 0 for none
	uint64_t m_selectedMaterialId { 0 };
	bool m_showMaterials { true };

	// shapes, cameras, lights, mesh objects and materials share one id space, so the gizmo's opaque
	// target id cannot collide
	uint64_t m_nextId { 1 };
	uint64_t m_revision { 1 };
	uint64_t m_contentRevision { 1 };
	// drawModelRows()'s list of each model's mesh objects, by index into m_meshObjects, as of m_modelRowsRevision
	std::map<std::string, std::vector<size_t>> m_modelRows;
	uint64_t m_modelRowsRevision { 0 };
	bool m_showPanel { true };
};
