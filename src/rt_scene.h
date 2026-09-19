#pragma once

// The loaded glTF models as the raytracer sees them: the walk that finds their nodes, and the
// object-space mesh data derived from them. buildRaytraceMeshData() depends only on which models
// are loaded, so it runs once per import or removal; the BVHs the path tracer traces are built
// from it (rt_accel.h), and the placed objects' transforms are applied by the TLAS, never baked
// into the triangles.

#include <functional>
#include <string>
#include <string_view>

#include <rt_scene_types.h>

class VulkanEngine;
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
