#pragma once

// The loaded glTF models as the raytracer sees them: the walk that finds their nodes, the
// object-space mesh data derived from them, and the world-space triangle list the path tracer
// actually traces.
//
// The split is deliberate. buildRaytraceMeshData() is expensive and depends only on which
// models are loaded, so it runs once per import or removal. buildTriangleData() applies the
// placed objects' transforms and is rebuilt only when a model or an object changes - not per
// render, and certainly not per frame of a restarting render.

#include <functional>
#include <string>
#include <string_view>

#include <rt_scene_types.h>

class VulkanEngine;
class RaytraceTextureArray;
struct MeshNode;

// Walks every imported model's node tree and visits each mesh-bearing node, in a deterministic
// order: models by key, then nodes depth-first from each top node. `nodeIndex` counts mesh nodes
// within one model and is what a SceneMeshObject stores, so the same node keeps the same index
// for as long as that model stays loaded.
void forEachMeshNode(VulkanEngine* engine, const std::function<void(std::string_view modelKey, uint32_t nodeIndex, const MeshNode& node)>& visit);

// Bakes the currently loaded glTF models into object-space mesh data. Called from the engine's
// scene change path, once per change - never per render.
std::shared_ptr<const RaytraceMeshData> buildRaytraceMeshData(VulkanEngine* engine);

// The scene objects a freshly imported model contributes: one per mesh-bearing node, named after
// the node and placed at the transform it was authored with. Ids are left unassigned - the
// editor gives them theirs.
std::vector<SceneMeshObject> defaultMeshObjects(const RaytraceMeshData& meshData, const std::string& modelKey);

// Flattens every visible mesh object into world-space triangles and the materials they index.
// An object whose model has been removed, or whose node no longer exists, contributes nothing.
std::shared_ptr<const RaytraceTriangleData> buildTriangleData(const RaytraceMeshData& meshData,
	const std::vector<SceneMeshObject>& objects, const RaytraceTextureArray& textures);
