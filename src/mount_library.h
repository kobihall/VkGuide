#pragma once

// The optomechanical model library on the GPU (docs/plans/completed/physoptics-optomechanical-models.md): every entry of
// mountModels() (optic_models.h), each read from assets/optical_models/meshes/ or built procedurally and uploaded
// through loadImportedModel() as a LoadedModel with one node per part and one surface per finish.
//
// Nothing is read or uploaded at startup. open() only notes which converted models' files are missing; a model is read
// the first time model() asks for it, and uploaded, with its decal texture, the first time meshes() does. A scene that
// uses three mounts pays for three.
//
// The library lives beside VulkanEngine::m_models, never in it: its models are fixed resources, so they are never
// saved with a scene and never listed under Models. Nothing draws or traces them until an optic uses one
// (physoptics-bench-and-mounts.md places them, and builds their BLASes through RaytraceBlasCache from these
// MeshAssets).

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <optic_models.h>

class VulkanEngine;
struct LoadedModel;

class MountLibrary {
public:
	// Opens the library on `directory` without reading it: a converted model whose .glb or .mates.json is missing
	// gets a warning now, since it will be its fallback box. Call once, after the raster materials exist
	void open(VulkanEngine* engine, const std::filesystem::path& directory);
	// releases the GPU meshes and forgets every model read; before the device goes
	void clear();

	// the model's parts, mates and motions, in its canonical frame (meters), read on the first call: its files, or
	// its procedural stand-in when they are missing or unreadable. Null for an unknown id
	const MountModel* model(std::string_view id);
	// the uploaded parts, uploaded on the first call: LoadedModel::topNodes holds one MeshNode per part, named as
	// MountPart::name. Null for an unknown id. The upload is an immediate submit, so call it outside a frame's
	// command recording
	std::shared_ptr<LoadedModel> meshes(std::string_view id);

	// one line per model that falls back to its box, for the Scene panel: those found missing at open(), and any
	// read since that failed
	const std::vector<std::string>& warnings() const { return m_warnings; }
	// how many models have been read and uploaded so far
	size_t readCount() const;
	size_t uploadedCount() const;

private:
	struct Entry {
		const MountModelInfo* info { nullptr };
		std::unique_ptr<MountModel> model;
		std::shared_ptr<LoadedModel> meshes;
	};
	Entry* find(std::string_view id);

	VulkanEngine* m_engine { nullptr };
	std::filesystem::path m_directory;
	std::vector<Entry> m_entries;
	std::vector<std::string> m_warnings;
};
