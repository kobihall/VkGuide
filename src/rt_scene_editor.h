#pragma once

// The live, persistent list of scene objects the raytracer owns - spheres, cameras and the
// placed glTF mesh objects - and the "Scene" panel that browses and edits them.
//
// All three kinds are plain data (rt_scene_types.h) owned outright here; a render copies what it
// needs when it starts, and compares later to know when to start over. A mesh object does not own
// its geometry: it names a node of one of VulkanEngine::m_models, which the engine loads and
// unloads, and carries only what the user can edit about the placed instance. Deleting the
// object leaves the model loaded. Scene files themselves are the engine's concern
// (VulkanEngine::openScene()/saveScene(), driven from the File menu).

#include <rt_scene_types.h>
#include <vk_types.h>

class VulkanEngine;
class TransformGizmo;

// the per-object imgui controls, one per struct rather than a virtual per class. Each returns
// whether anything the render depends on changed; drawCameraParams() also reports a change to
// a display-only setting (exposure), which does not warrant restarting a render
bool drawSphereParams(SceneSphere& sphere);
bool drawMaterialParams(SphereMaterial& material);
bool drawCameraParams(SceneCamera& camera, bool& displayChanged);
bool drawMeshObjectParams(SceneMeshObject& object);

class RaytraceSceneEditor {
public:
	// an empty scene with one camera at the free camera's starting pose. The startup scene is a
	// file the engine opens (assets/scenes/sphere_scene.gltf), not something seeded here
	RaytraceSceneEditor();

	void drawPanel(VulkanEngine* engine);

	const std::vector<SceneSphere>& spheres() const { return m_spheres; }
	const std::vector<SceneCamera>& cameras() const { return m_cameras; }
	const std::vector<SceneMeshObject>& meshObjects() const { return m_meshObjects; }

	// by stable id; null once the object has been deleted or the list replaced. The pointer
	// is only good until the next mutation - do not keep it across frames
	SceneSphere* findSphere(uint64_t id);
	const SceneSphere* findSphere(uint64_t id) const;
	SceneCamera* findCamera(uint64_t id);
	const SceneCamera* findCamera(uint64_t id) const;
	SceneMeshObject* findMeshObject(uint64_t id);
	const SceneMeshObject* findMeshObject(uint64_t id) const;

	// the load path: swaps in whole new lists. Every object gets a fresh id, and both go
	// through the same change tracking every manual edit uses, so consumers cannot miss it
	void replaceSpheres(std::vector<SceneSphere>&& spheres);
	void replaceCameras(std::vector<SceneCamera>&& cameras);
	void replaceMeshObjects(std::vector<SceneMeshObject>&& objects);

	// appends and returns the new id
	uint64_t addCamera(SceneCamera camera);
	// the import path: the objects a freshly loaded model contributes (defaultMeshObjects())
	void addMeshObjects(std::vector<SceneMeshObject>&& objects);
	// the model-removal path: drops every object placing a node of that model, since the geometry
	// they name is about to stop existing
	void removeMeshObjectsOf(const std::string& modelKey);

	// the first-person edit path: the raster camera writes its pose into a scene camera every
	// frame. Marks a change only when the pose actually moved, so a resting camera does not
	// restart renders
	void setCameraPose(uint64_t id, const glm::vec3& position, const glm::quat& orientation);

	// incremented by every mutation that a render depends on: add, delete, sphere, material,
	// camera or mesh-object parameters, a gizmo drag, a loaded file. A consumer that derives data
	// from the scene (the renderer's restart-on-change and its triangle data, the raster preview
	// spheres' material buffers) stores the last revision it acted on and compares. Nothing ever
	// resets it, so adding a consumer cannot starve another
	uint64_t revision() const { return m_revision; }

	bool* visibilityFlag() { return &m_showPanel; }

private:
	// which row of the object tree is selected. Rows come from three separately owned lists, so a
	// selection is an id rather than an index: a list can be replaced or reordered under us
	// between frames, and an id that no longer resolves simply means nothing is selected
	enum class SelectionKind {
		None,
		Camera,
		Sphere,
		MeshObject
	};

	void drawToolbar(VulkanEngine* engine);
	void drawObjectTree(VulkanEngine* engine);
	void drawCameraRows();
	void drawSphereRows();
	void drawModelRows(VulkanEngine* engine);
	// one selectable row, with the shared rename-in-place and right-click context menu. Returns
	// false when the row deleted its own object, in which case the caller must stop touching it
	bool drawObjectRow(SelectionKind kind, uint64_t id, const std::string& name);
	void drawSelection(VulkanEngine* engine);
	void drawSelectedSphere(VulkanEngine* engine, SceneSphere& sphere);
	void drawSelectedCamera(VulkanEngine* engine, SceneCamera& camera);
	void drawSelectedMeshObject(VulkanEngine* engine, SceneMeshObject& object);
	void beginSphereGizmo(TransformGizmo& gizmo, uint64_t id);
	void beginCameraGizmo(TransformGizmo& gizmo, uint64_t id);
	void beginMeshGizmo(TransformGizmo& gizmo, uint64_t id);
	void addSphere(SceneSphere sphere);
	// the Add menu's two creating entries, also reachable from the context menu's Duplicate
	void createSphere();
	void createCamera(VulkanEngine* engine);
	void duplicateSelection();
	// removes whatever is selected, whichever kind it is, and clears the selection
	void deleteSelection();
	void select(SelectionKind kind, uint64_t id);

	void markChanged() { m_revision++; }

	std::vector<SceneSphere> m_spheres;
	std::vector<SceneCamera> m_cameras;
	std::vector<SceneMeshObject> m_meshObjects;

	SelectionKind m_selectionKind { SelectionKind::None };
	uint64_t m_selectionId { 0 };
	// the object whose name is being edited in place, 0 for none, and the edit buffer
	uint64_t m_renamingId { 0 };
	char m_renameBuffer[128] {};

	int m_nextSphereNumber { 1 };
	int m_nextCameraNumber { 1 };
	// spheres, cameras and mesh objects share one id space, so the gizmo's opaque target id
	// cannot collide
	uint64_t m_nextId { 1 };
	uint64_t m_revision { 1 };
	bool m_showPanel { true };
};
