#pragma once

// Every loaded model's material textures (base colour, normal, metal/rough, emissive) in one 2D
// array image, so the path tracer can pick a triangle's texture with a plain layer index.
//
// Why an array rather than an array *of descriptors*: indexing a descriptor array by a value
// that varies per invocation needs VK_EXT_descriptor_indexing and non-uniform indexing, which
// MoltenVK only offers through Metal argument buffers and which this project (Vulkan 1.2 + KHR
// extensions, see CLAUDE.md) does not enable. One sampler2DArray at a fixed binding needs none
// of that: the layer is an ordinary integer the shader computes.
//
// The cost is that every texture is resampled to one common size. That is done on the GPU with
// vkCmdBlitImage, so a rebuild is one immediate submit and no CPU image work at all - except for the
// emissive textures, whose layers are read back once so the light list can price each emitting
// triangle by how bright its texture is over it (meanLuminance()).

#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <vk_types.h>

class VulkanEngine;

class RaytraceTextureArray {
public:
	// the square size every source texture is blitted to. Big enough for the base-colour detail
	// a path trace resolves, small enough that a scene's worth of them is a few tens of MB
	static constexpr uint32_t LAYER_SIZE = 512;
	// a hard cap on layers, so a pathological scene cannot try to allocate gigabytes: 1 MB a layer,
	// so 256 MB at most, and only the layers a scene uses are allocated. Sponza needs 69. Textures
	// past it are dropped, base colours last, and the surfaces using them fall back to their factors
	static constexpr uint32_t MAX_LAYERS = 256;

	// allocates the one-layer fallback, which is what the array is whenever no model has a
	// texture. The binding must always name a valid view, so the array is never absent
	void init(VulkanEngine* engine);
	// requires the device to be idle
	void destroy(VulkanEngine* engine);

	// rebuilds from every model currently loaded in the engine. Called from the same scene-change
	// path that rebuilds the mesh data, so the layers the scene's materials name agree by
	// construction. Waits for the device: a model import is a menu-driven action
	void rebuild(VulkanEngine* engine);

	// the layer holding a glTF image, or -1 for one this array does not have (no texture on the
	// material, or the layer cap reached)
	int layerOf(VkImage image) const;

	// always valid, always in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, never null
	const AllocatedImage& image() const { return m_array; }
	uint32_t layerCount() const { return m_layerCount; }

	// The mean linear luminance of an emissive layer over one triangle's uv footprint, from 16
	// stratified points: its share of the triangle's power in the light list. 1 for a layer that is
	// not an emissive texture. Only prices the light selection - the GPU applies the texel itself
	float meanLuminance(int layer, const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2) const;

private:
	void destroyArray(VulkanEngine* engine);
	// the array image plus its VK_IMAGE_VIEW_TYPE_2D_ARRAY view. VulkanEngine::createImage()
	// builds single-layer 2D images only, so this is the one place that spells out both
	void createArray(VulkanEngine* engine, uint32_t layers);

	AllocatedImage m_array {};
	uint32_t m_layerCount { 1 };
	// source glTF image -> its layer. Keyed by VkImage because that is what a GLTFMaterial holds
	// and what makes two materials sharing one texture share one layer, whatever each uses it for
	std::unordered_map<VkImage, int> m_layers;
	// the layers some material uses as its emissive texture, read back as rgba8 (sRGB)
	std::unordered_map<int, std::vector<uint8_t>> m_emissiveTexels;
};
