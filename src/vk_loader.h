#pragma once

#include <vk_types.h>
#include <vk_descriptors.h>
#include <unordered_map>
#include <filesystem>

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

struct GLTFMaterial {
	MaterialInstance data;

	// the raw pbr factors this material was authored with, kept on the cpu side alongside the
	// gpu uniform they are also written into. No consumer yet - retained so a future feature
	// that shades glTF geometry on the cpu does not have to re-parse the source file for them
	glm::vec4 colorFactors { 1.f };
	glm::vec2 metalRoughFactors { 0.f };
};

struct Bounds {
	glm::vec3 origin;
	float sphereRadius;
	glm::vec3 extents;
};

struct GeoSurface {
	uint32_t startIndex;
	uint32_t count;
	Bounds bounds;
	std::shared_ptr<GLTFMaterial> material;
};

struct MeshAsset {
	std::string name;

	std::vector<GeoSurface> surfaces;
	GPUMeshBuffers meshBuffers;

	// the same geometry that was handed to uploadMesh(), retained rather than discarded once
	// it is on the gpu. Costs ram proportional to total loaded mesh data - judged negligible
	// at this project's scale - and exists for future cpu-side geometry work (mesh raytracing,
	// plane/mesh intersection). Nothing reads it today
	std::vector<Vertex> cpuVertices;
	std::vector<uint32_t> cpuIndices;
};

//forward declaration
class VulkanEngine;

struct LoadedGLTF : public IRenderable {

	// storage for all the data on a given glTF file
	std::unordered_map<std::string, std::shared_ptr<MeshAsset>> meshes;
	std::unordered_map<std::string, std::shared_ptr<Node>> nodes;
	std::unordered_map<std::string, AllocatedImage> images;
	std::unordered_map<std::string, std::shared_ptr<GLTFMaterial>> materials;

	// nodes that dont have a parent, for iterating through the file in tree order
	std::vector<std::shared_ptr<Node>> topNodes;

	std::vector<VkSampler> samplers;

	DescriptorAllocatorGrowable descriptorPool;

	AllocatedBuffer materialDataBuffer;

	VulkanEngine* creator;

	// the file this was loaded from, absolute - what a saved scene records for it
	std::filesystem::path sourcePath;

	~LoadedGLTF() { clearAll(); };

	virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx);

private:

	void clearAll();
};

std::optional<std::vector<std::shared_ptr<MeshAsset>>> loadGltfMeshes(VulkanEngine* engine, std::filesystem::path filePath);

// callable at any time, not just at init. On failure outError (if given) receives a message
// suitable for showing to the user
std::optional<std::shared_ptr<LoadedGLTF>> loadGltf(VulkanEngine* engine, std::string_view filePath, std::string* outError = nullptr);

// directory is the glTF file's own folder, which relative image uris are resolved against
std::optional<AllocatedImage> load_image(VulkanEngine* engine, fastgltf::Asset& asset, fastgltf::Image& image, const std::filesystem::path& directory);