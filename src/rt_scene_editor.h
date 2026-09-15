#pragma once

// The live, persistent list of scene objects the raytracer owns - spheres and cameras - and
// the "Scene" panel that browses the whole scene.
//
// It owns the spheres and cameras outright as plain data (rt_scene_types.h); a render copies
// what it needs when it starts, and compares later to know when to start over. The glTF
// models and their mesh nodes it also lists are not owned and not copied: they are read
// fresh out of VulkanEngine::m_models each time the panel draws, so importing or removing a
// model is reflected immediately. Scene files themselves are the engine's concern
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

class RaytraceSceneEditor {
public:
	// seeded with the sibling project's hardcoded 4-sphere scene and one camera at the free
	// camera's starting pose
	RaytraceSceneEditor();

	void drawPanel(VulkanEngine* engine);

	const std::vector<SceneSphere>& spheres() const { return m_spheres; }
	const std::vector<SceneCamera>& cameras() const { return m_cameras; }

	// by stable id; null once the object has been deleted or the list replaced. The pointer
	// is only good until the next mutation - do not keep it across frames
	SceneSphere* findSphere(uint64_t id);
	const SceneSphere* findSphere(uint64_t id) const;
	SceneCamera* findCamera(uint64_t id);
	const SceneCamera* findCamera(uint64_t id) const;

	// the load path: swaps in whole new lists. Every object gets a fresh id, and both go
	// through the same change tracking every manual edit uses, so consumers cannot miss it
	void replaceSpheres(std::vector<SceneSphere>&& spheres);
	void replaceCameras(std::vector<SceneCamera>&& cameras);

	// appends and returns the new id
	uint64_t addCamera(SceneCamera camera);

	// the first-person edit path: the raster camera writes its pose into a scene camera every
	// frame. Marks a change only when the pose actually moved, so a resting camera does not
	// restart renders
	void setCameraPose(uint64_t id, const glm::vec3& position, const glm::quat& orientation);

	// incremented by every mutation that a render depends on: add, delete, sphere, material
	// or camera parameters, a gizmo drag, a loaded file. A consumer that derives data from the
	// scene (the renderer's restart-on-change, the raster preview spheres' material buffers)
	// stores the last revision it acted on and compares. Nothing ever resets it, so adding a
	// consumer cannot starve another
	uint64_t revision() const { return m_revision; }

	bool* visibilityFlag() { return &m_showPanel; }

private:
	void drawModels(VulkanEngine* engine);
	void drawSelectedSphere(VulkanEngine* engine, int index);
	void drawSelectedCamera(VulkanEngine* engine, int index);
	void beginSphereGizmo(TransformGizmo& gizmo, uint64_t id);
	void beginCameraGizmo(TransformGizmo& gizmo, uint64_t id);
	void addSphere(SceneSphere sphere);

	void markChanged() { m_revision++; }

	// which row of the unified object list is selected. Rows are cameras, then spheres, then
	// glTF mesh nodes, so a selection is identified by kind rather than by a bare index - the
	// glTF side is rebuilt from live engine state every frame and can change length underneath us
	enum class SelectionKind {
		None,
		Camera,
		Sphere,
		GltfNode
	};

	std::vector<SceneSphere> m_spheres;
	std::vector<SceneCamera> m_cameras;
	SelectionKind m_selectionKind { SelectionKind::None };
	int m_selectionIndex { 0 };
	int m_nextSphereNumber { 1 };
	int m_nextCameraNumber { 1 };
	// spheres and cameras share one id space, so the gizmo's opaque target id cannot collide
	uint64_t m_nextId { 1 };
	uint64_t m_revision { 1 };
	bool m_showPanel { true };
};
