#pragma once

// The live, persistent side of the raytracer's scene state - mutated every frame its panel is
// open, and the source buildRaytraceScene() copies from when a render starts.
//
// It owns the sphere list outright. The glTF objects it also lists are not owned and not
// copied: they are read fresh out of VulkanEngine::m_loadedScenes each time the panel draws,
// so loading a different scene at runtime is reflected immediately.

#include <rt_hittable.h>
#include <rt_types.h>

class VulkanEngine;

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

	// set by every path that adds, removes or edits a sphere or its material. Nothing in this feature consumes
	// it - the CPU raytracer re-reads the whole list at render time - but the planned GPU
	// raytracer needs to know when to re-upload its sphere buffer and reset its accumulation
	bool isDirty() const { return m_dirty; }
	void clearDirty() { m_dirty = false; }

	bool* visibilityFlag() { return &m_showPanel; }

private:
	void drawSphereParams(int index);

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
	bool m_dirty { true };
	bool m_showPanel { true };
};
