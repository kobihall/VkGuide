#pragma once

// The bench on the GPU (docs/plans/completed/physoptics-bench-and-mounts.md): the breadboard's mesh at the scene's size, the
// post, and the mount library's models that a stack uses, uploaded through loadImportedModel(); what they draw into the
// raster preview; and what they add to the traced scene. The stacks themselves are the editor's
// (RaytraceSceneEditor::benchStacks()), assembled by optic_bench.h; this only puts them on screen and in the TLAS.
//
// Nothing is uploaded until the scene has a breadboard or a mounted optic, so a scene without them draws and traces
// exactly what it did before.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <optic_bench.h>
#include <optic_models.h>
#include <rt_accel.h>

class VulkanEngine;
class RaytraceSceneEditor;
struct LoadedModel;
struct MeshAsset;
struct MeshNode;

class BenchScene {
public:
	// Uploads what the scene's bench needs this frame: the breadboard (again when its size changed), the post, and each
	// model a stack names. Returns whether the meshes the tracer needs changed, which the engine answers by rebuilding
	// the tracer's mesh data. Call outside a frame's command recording: an upload is an immediate submit
	bool update(VulkanEngine* engine);
	// the breadboard, the posts and every stack's parts into the main draw context; a blocked fork in red
	void draw(VulkanEngine* engine);
	// releases everything uploaded; before the device goes
	void clear();

	// what the tracer's mesh data must hold: the breadboard's mesh and every uploaded model's parts
	const std::vector<std::shared_ptr<const MeshAsset>>& tracedMeshes() const { return m_tracedMeshes; }
	// the bench's instances, finishes and tags, for buildSceneAccel()
	BenchTrace traceInput(const RaytraceSceneEditor& editor, const RaytraceMeshData& meshData) const;
	// moves with every upload and release, for RasterKey: the paused preview must not keep showing an old table
	uint64_t revision() const { return m_revision; }

private:
	// a part's mesh and the finish of each of its surfaces
	struct PartMesh {
		std::shared_ptr<MeshAsset> mesh;
		// in its model, which m_models keeps alive
		MeshNode* node { nullptr };
		std::vector<MountFinish> finishes;
	};
	const PartMesh* findPart(const std::string& model, const std::string& part) const;
	// keeps a model alive until the frames that may still draw it are done
	void retire(VulkanEngine* engine, std::shared_ptr<LoadedModel> model);

	std::shared_ptr<LoadedModel> m_table;
	BenchSettings m_tableFor;
	std::shared_ptr<LoadedModel> m_post;
	// a single red material, for a fork with no free hole
	std::shared_ptr<LoadedModel> m_marker;
	std::map<std::string, std::shared_ptr<LoadedModel>> m_models;
	std::map<std::pair<std::string, std::string>, PartMesh> m_parts;
	std::vector<std::shared_ptr<const MeshAsset>> m_tracedMeshes;
	uint64_t m_revision { 0 };
};
