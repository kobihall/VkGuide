#pragma once

// THE TEXTURE LIBRARY (docs/plans/completed/pbrt-textures.md): every texture the path tracer samples through binding 27, each its
// own image at its own size (up to the Maximum texture size), with a full mip chain and its own sampler. What fills it:
// the images a glTF or pbrt model brings, image files the user loads for their own materials, and the images of
// projection and goniometric lights (an IES file becomes an equal-area square image here).
//
// The GPU side is one descriptor set, set 1 of every kernel that declares CrtBinding::Textures (crt_texture.glsl):
// binding 0 the distinct samplers, binding 27 the textures, a variable-count array written once whenever the library
// changes, not per dispatch. Its layout and pool are update-after-bind, which is what lifts MoltenVK's per-stage limit
// from 128 images to 1024 (Metal argument buffers, on by default since MoltenVK 1.3; see the plan's spike).
// Descriptor 0 is a white 1 x 1 texture, so a material can name "no texture" by index 0 as well.
//
// The legacy texture array (rt_textures.h, binding 12) still holds imported models' four glTF textures at 512 x 512,
// for renders under Texture storage "legacy array", which reproduces images from before the library bit for bit.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <texture_types.h>
#include <vk_types.h>

class VulkanEngine;

// Where a texture's texels come from: one image of a loaded model, by its index in the file, or a file on disk
struct TextureSource {
	// a model's image: the model's key in VulkanEngine::m_models and the image's index in LoadedModel::imagesByIndex
	std::string modelKey;
	int modelImage { -1 };
	// a file: PNG, JPEG, TGA, BMP (8-bit), HDR, EXR, PFM (float), or an IES LM-63 photometric file, which becomes the
	// equal-area image of its distribution (ies_loader.h)
	std::filesystem::path path;
	TextureEncoding encoding { TextureEncoding::Srgb };
	float gamma { 2.2f };
	// pbrt's "invert": 1 - value, applied as the file is read
	bool invert { false };

	bool isModelImage() const { return modelImage >= 0; }
	bool operator==(const TextureSource&) const = default;
};

struct TextureEntry {
	// stable, never 0, never saved: a material or light names its texture by it
	uint64_t id { 0 };
	std::string name;
	TextureSource source;
	TextureColorSpace colorSpace { TextureColorSpace::Srgb };
	TextureSampling sampling;

	// the GPU copy, once uploaded: its index in binding 27 and its sampler's in binding 0 of set 1
	AllocatedImage image {};
	VkImageView view { VK_NULL_HANDLE };
	uint32_t descriptor { 0 };
	uint32_t sampler { 0 };
	// as stored, after the size cap; the file's own size
	glm::uvec2 size { 0 };
	glm::uvec2 sourceSize { 0 };
	uint32_t levels { 0 };
	bool floatTexels { false };
	VkDeviceSize bytes { 0 };
	// why the texture could not be loaded; empty when it was
	std::string error;

	// THE CPU COPY: linear luminance (Rec. 709 weights) of the largest mip level no more than LUMINANCE_SIZE a side, row 0
	// the image's top (v = 0). What the light list prices a textured emitter by and a light image is integrated with
	glm::uvec2 luminanceSize { 0 };
	std::vector<float> luminance;
};

// How a projection or goniometric light's image is read (docs/plans/completed/pbrt-light-types.md): a color image as sRGB, a float
// image or an IES file's distribution as it is, clamped at its edges
struct LightImageRequest {
	TextureSource source;
	TextureColorSpace colorSpace { TextureColorSpace::Srgb };
	TextureSampling sampling;
};
LightImageRequest lightImageRequest(const std::filesystem::path& path);

class TextureLibrary {
public:
	// the largest side of the CPU luminance copy
	static constexpr uint32_t LUMINANCE_SIZE = 256;
	// the descriptors set 1 can hold: binding 27's variable count is at most this. MoltenVK 1.4.2 on the Radeon Pro 560X
	// supports a set of 1024 (vkGetDescriptorSetLayoutSupport), and not 2048. A texture past it reads as white
	static constexpr uint32_t MAX_TEXTURES = 1024;
	// distinct sampler states, binding 0 of set 1 (shaders/rt/include/crt_texture.glsl CRT_MAX_TEXTURE_SAMPLERS), each
	// twice: as its sampling says, and nearest-filtered, for the Texture filtering setting's "point"
	static constexpr uint32_t MAX_SAMPLERS = 64;

	void init(VulkanEngine* engine);
	// requires the device to be idle
	void destroy(VulkanEngine* engine);

	// The id of the entry for this source, color space and sampling, added (and loaded at the next update()) when there
	// is none. Two materials that read one image the same way share one entry
	uint64_t add(const TextureSource& source, TextureColorSpace colorSpace, const TextureSampling& sampling, std::string name = {});
	const TextureEntry* find(uint64_t id) const;
	// the entry add() would return for these, without adding one
	const TextureEntry* find(const TextureSource& source, TextureColorSpace colorSpace, const TextureSampling& sampling) const;
	std::span<const TextureEntry> entries() const { return m_entries; }
	// drops every entry of a model's images, at the next update()
	void removeModel(const std::string& modelKey);
	// drops the file entries no one uses any more: `used` says which ids are still referenced
	void removeUnused(const std::function<bool(uint64_t id)>& used);

	// Loads what was added, frees what was removed, and when either happened rewrites set 1, after waiting for the
	// device (a menu-driven change, like the texture array's rebuild). `maxSize` caps a texture's larger side; a change
	// to it reloads every texture. True when anything changed
	bool update(VulkanEngine* engine, uint32_t maxSize);
	// moves on every change that renumbers descriptors or changes what one holds: part of the scene's key
	uint64_t revision() const { return m_revision; }

	VkDescriptorSetLayout setLayout() const { return m_setLayout; }
	VkDescriptorSet set() const { return m_set; }
	VkDeviceSize bytes() const;

	// the mean luminance over one triangle's uv footprint, from 16 stratified points (as RaytraceTextureArray's), with
	// the texture's wrap: an emitter's share of its triangle's power. 1 for no texture
	float meanLuminance(uint64_t id, const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2) const;
	// the mean over the whole image
	float meanLuminance(uint64_t id) const;

private:
	void load(VulkanEngine* engine, TextureEntry& entry, uint32_t maxSize);
	void release(VulkanEngine* engine, TextureEntry& entry);
	uint32_t samplerIndex(VulkanEngine* engine, const TextureSampling& sampling);
	void writeSet(VulkanEngine* engine);

	std::vector<TextureEntry> m_entries;
	std::vector<TextureEntry> m_removed;
	uint64_t m_nextId { 1 };
	uint64_t m_revision { 1 };
	bool m_dirty { true };
	uint32_t m_maxSize { 0 };

	// the white 1 x 1 at descriptor 0
	AllocatedImage m_white {};
	std::vector<TextureSampling> m_samplerStates;
	// two per state: its own filters, then nearest
	std::vector<VkSampler> m_samplers;

	VkDescriptorSetLayout m_setLayout { VK_NULL_HANDLE };
	VkDescriptorPool m_pool { VK_NULL_HANDLE };
	VkDescriptorSet m_set { VK_NULL_HANDLE };
};
