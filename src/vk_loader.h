#pragma once

#include <vk_types.h>
#include <vk_descriptors.h>
#include <unordered_map>
#include <filesystem>

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

// how a glTF material's base-colour alpha is read (glTF 2.0 alphaMode)
enum class GltfAlphaMode : uint8_t {
	Opaque,
	// cut out wherever alpha falls below alphaCutoff
	Mask,
	// blended. The path tracer has no partial coverage and treats it as opaque
	Blend
};

struct GLTFMaterial {
	MaterialInstance data;

	// the raw pbr factors this material was authored with, kept on the cpu side alongside the
	// gpu uniform they are also written into, so anything shading this geometry outside the
	// raster pipeline (the path tracer) does not have to re-parse the source file for them
	glm::vec4 colorFactors { 1.f };
	// x metallic, y roughness
	glm::vec2 metalRoughFactors { 0.f };
	// linear rgb, already multiplied by KHR_materials_emissive_strength
	glm::vec3 emissiveFactor { 0.f };
	float normalScale { 1.f };
	GltfAlphaMode alphaMode { GltfAlphaMode::Opaque };
	float alphaCutoff { 0.5f };

	// the textures, or null images for the ones the material has none of. The raster path reaches
	// the base colour and metal/rough through `data`'s descriptor set; the path tracer needs the
	// images themselves, to find which layers of RaytraceTextureArray hold them
	AllocatedImage baseColorImage {};
	AllocatedImage normalImage {};
	AllocatedImage metalRoughImage {};
	AllocatedImage emissiveImage {};
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
	// per cpuVertices entry: xyz the tangent, w the bitangent's sign (glTF TANGENT). Generated from
	// the uvs when the file has none; zero where there are no uvs either. Kept off the gpu vertex,
	// which the raster shaders read by buffer address, because only the path tracer's normal maps
	// use it. May be empty for a mesh built outside the loader
	std::vector<glm::vec4> cpuTangents;
};

// A KHR_lights_punctual light as the file places it. Imported as data only: the path tracer does
// not sample or hit these yet (docs/plans/shadow-rays-nee.md)
struct GltfPunctualLight {
	enum class Type : uint8_t {
		Directional,
		Point,
		Spot
	};

	std::string name;
	Type type { Type::Point };
	// linear rgb
	glm::vec3 color { 1.f };
	// candela for point and spot, lux for directional
	float intensity { 1.f };
	// 0 for infinite
	float range { 0.f };
	// spot only, radians
	float innerConeAngle { 0.f };
	float outerConeAngle { 0.7853982f };
	// the owning node's world transform: the light sits at its origin and shines down its -z
	glm::mat4 worldTransform { 1.f };
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

	// the file's KHR_lights_punctual lights, one per node that references one
	std::vector<GltfPunctualLight> lights;

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