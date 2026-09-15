#pragma once

// The loaded glTF models as the raytracer sees them, and the walk that finds them.

#include <functional>

#include <rt_scene_types.h>

class VulkanEngine;
struct MeshNode;

// Walks every imported model's node tree and visits each mesh-bearing node. Shared by the
// scene browser (which wants node names) and buildRaytraceMeshData() (which wants geometry),
// so both see exactly the same set of objects.
void forEachMeshNode(VulkanEngine* engine, const std::function<void(const MeshNode& node)>& visit);

// Bakes the currently loaded glTF models into object-space mesh data. Called from the engine's
// scene change path, once per change - never per render. Nothing traces it yet; it is the
// input of the planned triangle extend stage
std::shared_ptr<const RaytraceMeshData> buildRaytraceMeshData(VulkanEngine* engine);
