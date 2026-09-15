#pragma once

// The one-shot snapshot handed to the raytrace worker thread, and the functions that build it
// and the scene-wide mesh data it shares.
//
// This is the thread-safety boundary of the whole feature: buildRaytraceScene() runs on the
// main thread while Render is being clicked, copies everything the render needs out of live
// engine and editor state, and the worker then reads nothing else. Editing a sphere or moving
// the camera while a render is in flight therefore does not affect that render - the same
// "each render is a snapshot of settings at that moment" behaviour the sibling project had by
// virtue of being synchronous.

#include <rt_hittable.h>
#include <rt_types.h>

class VulkanEngine;
class RaytraceSceneEditor;
struct MeshNode;

struct RaytraceScene {
	// deep copies, materials included, not shared with the editor - see buildRaytraceScene()
	std::vector<std::shared_ptr<sphere>> spheres;

	// shared with VulkanEngine::m_raytraceMeshData rather than copied: it is immutable and
	// cpu-only, so a render keeps the scene it was started against alive even if a different
	// file is loaded mid-render. Nothing in this feature intersects it - see RaytraceMeshData
	std::shared_ptr<const RaytraceMeshData> meshData;

	RTCameraSnapshot camera;
	RenderSettings settings;

	// flat linear scan over spheres, mirroring the sibling project's hittable_list::hit().
	// With a handful of user-placed spheres there is no acceleration structure to justify
	bool hit(const ray& r, double t_min, double t_max, hit_record& rec) const;
};

// Walks every loaded glTF scene's node tree and visits each mesh-bearing node. Shared by the
// scene browser (which wants node names) and buildRaytraceMeshData() (which wants geometry),
// so both see exactly the same set of objects.
void forEachMeshNode(VulkanEngine* engine, const std::function<void(const MeshNode& node)>& visit);

// Bakes the currently loaded glTF scenes into object-space mesh data. Called from the engine's
// scene load path, once per scene change - never per render, which is what it used to cost
std::shared_ptr<const RaytraceMeshData> buildRaytraceMeshData(VulkanEngine* engine);

// The raster camera as it is right now, plus the lens settings, as the thing a render is *of*.
// Both backends call this at Render, so they fire the same primary rays for the same click.
// When cameras enter the scene graph this grows a "which camera" argument; today the free
// camera is the only source
RTCameraSnapshot captureCameraSnapshot(VulkanEngine* engine, const RenderSettings& settings);

RaytraceScene buildRaytraceScene(VulkanEngine* engine, const RaytraceSceneEditor& editor, const RenderSettings& settings);
