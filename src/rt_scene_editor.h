#pragma once

// The live, persistent sphere list, and the "Scene" panel that browses the whole scene.
//
// It owns the spheres outright - they are the raytracer's geometry, and buildRaytraceScene()
// copies them when a render starts. The glTF models and their mesh nodes it also lists are not
// owned and not copied: they are read fresh out of VulkanEngine::m_models each time the panel
// draws, so importing or removing a model is reflected immediately. Scene files themselves are
// the engine's concern (VulkanEngine::openScene()/saveScene(), driven from the File menu).

#include <rt_hittable.h>
#include <rt_types.h>
#include <vk_types.h>

class VulkanEngine;
class TransformGizmo;

struct SceneSphere {
	std::string name;
	std::shared_ptr<sphere> object;
};

class RaytraceSceneEditor {
public:
	// seeded with the sibling project's hardcoded 4-sphere scene
	RaytraceSceneEditor();

	void drawPanel(VulkanEngine* engine);

	const std::vector<SceneSphere>& spheres() const { return m_spheres; }

	// swaps in a whole new sphere list - the load path. Goes through the same change tracking
	// every manual edit uses, so consumers cannot miss it
	void replaceSpheres(std::vector<SceneSphere>&& spheres);

	// incremented by every mutation: add, delete, sphere or material parameters, a gizmo drag, a
	// loaded file. A consumer that derives data from the sphere list (the planned GPU raytracer's
	// sphere buffer and accumulation reset, the raster preview spheres) stores the last revision
	// it acted on and compares. Nothing ever resets it, so adding a consumer cannot starve another
	uint64_t revision() const { return m_revision; }

	bool* visibilityFlag() { return &m_showPanel; }

private:
	void drawModels(VulkanEngine* engine);
	void drawSphereParams(VulkanEngine* engine, int index);
	void beginSphereGizmo(TransformGizmo& gizmo, const std::shared_ptr<sphere>& target);

	void markChanged() { m_revision++; }

	// which row of the unified object list is selected. Rows are spheres first, then glTF
	// mesh nodes, so a selection is identified by kind rather than by a bare index - the glTF
	// side is rebuilt from live engine state every frame and can change length underneath us
	enum class SelectionKind {
		None,
		Sphere,
		GltfNode
	};

	std::vector<SceneSphere> m_spheres;
	SelectionKind m_selectionKind { SelectionKind::None };
	int m_selectionIndex { 0 };
	int m_nextSphereNumber { 1 };
	uint64_t m_revision { 1 };
	bool m_showPanel { true };
};
