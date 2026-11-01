#include <texture_library.h>

#include <algorithm>
#include <cmath>

#include <glm/gtc/packing.hpp>
#include <stb_image.h>

#include <ies_loader.h>
#include <image_io.h>
#include <vk_engine.h>
#include <vk_images.h>
#include <vk_initializers.h>

namespace {

float srgbToLinearValue(float c)
{
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgbValue(float c)
{
	c = std::clamp(c, 0.f, 1.f);
	return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

std::string lowerExtension(const std::filesystem::path& path)
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return extension;
}

VkSamplerAddressMode addressMode(TextureWrap wrap)
{
	switch (wrap) {
	case TextureWrap::Repeat:
		return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	case TextureWrap::Clamp:
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case TextureWrap::Mirror:
		return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	case TextureWrap::Black:
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	}
	return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

// a uv wrapped as `wrap` addresses it, onto [0, 1); false outside the image for Black
bool wrapCoordinate(float& x, TextureWrap wrap)
{
	switch (wrap) {
	case TextureWrap::Repeat:
		x -= std::floor(x);
		return true;
	case TextureWrap::Clamp:
		x = std::clamp(x, 0.f, 0.99999994f);
		return true;
	case TextureWrap::Mirror: {
		const float period = x - 2.f * std::floor(x * 0.5f);
		x = period < 1.f ? period : std::min(2.f - period, 0.99999994f);
		return true;
	}
	case TextureWrap::Black:
		return x >= 0.f && x < 1.f;
	}
	return true;
}

uint32_t mipLevels(uint32_t width, uint32_t height)
{
	return (uint32_t)std::floor(std::log2((float)std::max(width, height))) + 1u;
}

// what a file decodes to before upload: 8-bit texels in the image's format, or linear floats
struct DecodedImage {
	uint32_t width { 0 };
	uint32_t height { 0 };
	std::vector<uint8_t> rgba8;
	std::vector<float> rgbaFloat;
};

bool decodeFile(const TextureSource& source, TextureColorSpace colorSpace, DecodedImage& out, std::string& error)
{
	const std::string extension = lowerExtension(source.path);
	if (extension == ".ies") {
		IesData ies;
		if (!loadIes(source.path, ies, &error)) {
			return false;
		}
		const uint32_t size = IES_IMAGE_SIZE;
		const std::vector<float> intensity = iesEqualAreaImage(ies, size);
		out.width = size;
		out.height = size;
		out.rgbaFloat.resize((size_t)size * size * 4);
		for (size_t i = 0; i < intensity.size(); i++) {
			out.rgbaFloat[i * 4 + 0] = intensity[i];
			out.rgbaFloat[i * 4 + 1] = intensity[i];
			out.rgbaFloat[i * 4 + 2] = intensity[i];
			out.rgbaFloat[i * 4 + 3] = 1.f;
		}
		return true;
	}
	if (extension == ".exr" || extension == ".pfm" || extension == ".hdr") {
		Image image;
		const ImageIoStatus status = loadImage(source.path, image);
		if (!status.ok) {
			error = status.message;
			return false;
		}
		out.width = image.width;
		out.height = image.height;
		out.rgbaFloat.assign(image.pixelCount() * 4, 1.f);
		for (size_t i = 0; i < image.pixelCount(); i++) {
			for (int c = 0; c < 3; c++) {
				const float v = image.rgb[i * 3 + c];
				out.rgbaFloat[i * 4 + c] = source.invert ? 1.f - v : v;
			}
		}
		return true;
	}
	int width = 0;
	int height = 0;
	int channels = 0;
	stbi_uc* data = stbi_load(source.path.string().c_str(), &width, &height, &channels, 4);
	if (data == nullptr) {
		error = fmt::format("{}: {}", source.path.filename().string(), stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unreadable");
		return false;
	}
	out.width = (uint32_t)width;
	out.height = (uint32_t)height;
	out.rgba8.assign(data, data + (size_t)width * height * 4);
	stbi_image_free(data);
	//the stored image is sRGB bytes for a color and the file's bytes for data, so only a file in another encoding, or
	//inverted, is converted: decoded to linear, then re-encoded for the format it is stored in
	const bool color = colorSpace == TextureColorSpace::Srgb;
	if (source.invert || (color && source.encoding != TextureEncoding::Srgb) || (!color && source.encoding != TextureEncoding::Linear)) {
		for (size_t i = 0; i < out.rgba8.size(); i++) {
			if (i % 4 == 3) {
				continue;
			}
			float v = (float)out.rgba8[i] / 255.f;
			if (source.encoding == TextureEncoding::Srgb) {
				v = srgbToLinearValue(v);
			} else if (source.encoding == TextureEncoding::Gamma) {
				v = std::pow(v, source.gamma);
			}
			if (source.invert) {
				v = 1.f - v;
			}
			const float stored = color ? linearToSrgbValue(v) : std::clamp(v, 0.f, 1.f);
			out.rgba8[i] = (uint8_t)std::lround(stored * 255.f);
		}
	}
	return true;
}

}

void TextureLibrary::init(VulkanEngine* engine)
{
	//set 1: the samplers, then the textures, a variable-count array that must be the set's last binding
	VkDescriptorSetLayoutBinding bindings[2] {};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	bindings[0].descriptorCount = MAX_SAMPLERS * 2;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].binding = 27;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bindings[1].descriptorCount = MAX_TEXTURES;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	const VkDescriptorBindingFlags flags[2] = {
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
	};
	VkDescriptorSetLayoutBindingFlagsCreateInfo flagInfo { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
	flagInfo.bindingCount = 2;
	flagInfo.pBindingFlags = flags;
	VkDescriptorSetLayoutCreateInfo layoutInfo { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layoutInfo.pNext = &flagInfo;
	//update-after-bind is what has MoltenVK put the set in a Metal argument buffer, lifting the per-stage limit of
	//128 images and 16 samplers
	layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
	layoutInfo.bindingCount = 2;
	layoutInfo.pBindings = bindings;
	//past the plain per-stage limits, so the device is asked first, as the specification requires
	VkDescriptorSetLayoutSupport support { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT };
	vkGetDescriptorSetLayoutSupport(engine->m_device, &layoutInfo, &support);
	if (!support.supported) {
		fmt::println("TextureLibrary: the device does not support a set of {} textures and {} samplers", MAX_TEXTURES, MAX_SAMPLERS * 2);
		abort();
	}
	checkVkResult(vkCreateDescriptorSetLayout(engine->m_device, &layoutInfo, nullptr, &m_setLayout));

	const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLER, MAX_SAMPLERS * 2 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_TEXTURES } };
	VkDescriptorPoolCreateInfo poolInfo { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = sizes;
	checkVkResult(vkCreateDescriptorPool(engine->m_device, &poolInfo, nullptr, &m_pool));

	uint32_t white = 0xFFFFFFFFu;
	m_white = engine->createImage(&white, VkExtent3D { 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
	samplerIndex(engine, TextureSampling {});
	m_dirty = true;
	update(engine, 4096);
}

void TextureLibrary::destroy(VulkanEngine* engine)
{
	for (TextureEntry& entry : m_entries) {
		release(engine, entry);
	}
	for (TextureEntry& entry : m_removed) {
		release(engine, entry);
	}
	m_entries.clear();
	m_removed.clear();
	for (VkSampler sampler : m_samplers) {
		vkDestroySampler(engine->m_device, sampler, nullptr);
	}
	m_samplers.clear();
	m_samplerStates.clear();
	if (m_white.image != VK_NULL_HANDLE) {
		engine->destroyImage(m_white);
		m_white = {};
	}
	if (m_pool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(engine->m_device, m_pool, nullptr);
		m_pool = VK_NULL_HANDLE;
		m_set = VK_NULL_HANDLE;
	}
	if (m_setLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(engine->m_device, m_setLayout, nullptr);
		m_setLayout = VK_NULL_HANDLE;
	}
}

uint64_t TextureLibrary::add(const TextureSource& source, TextureColorSpace colorSpace, const TextureSampling& sampling, std::string name)
{
	for (const TextureEntry& entry : m_entries) {
		if (entry.source == source && entry.colorSpace == colorSpace && entry.sampling == sampling) {
			return entry.id;
		}
	}
	TextureEntry entry;
	entry.id = m_nextId++;
	entry.name = name.empty() ? (source.isModelImage() ? fmt::format("{} image {}", source.modelKey, source.modelImage) : source.path.filename().string()) : std::move(name);
	entry.source = source;
	entry.colorSpace = colorSpace;
	entry.sampling = sampling;
	m_entries.push_back(std::move(entry));
	m_dirty = true;
	return m_entries.back().id;
}

const TextureEntry* TextureLibrary::find(uint64_t id) const
{
	if (id == 0) {
		return nullptr;
	}
	for (const TextureEntry& entry : m_entries) {
		if (entry.id == id) {
			return &entry;
		}
	}
	return nullptr;
}

const TextureEntry* TextureLibrary::find(const TextureSource& source, TextureColorSpace colorSpace, const TextureSampling& sampling) const
{
	for (const TextureEntry& entry : m_entries) {
		if (entry.source == source && entry.colorSpace == colorSpace && entry.sampling == sampling) {
			return &entry;
		}
	}
	return nullptr;
}

LightImageRequest lightImageRequest(const std::filesystem::path& path)
{
	LightImageRequest request;
	request.source.path = path;
	request.sampling.wrapU = TextureWrap::Clamp;
	request.sampling.wrapV = TextureWrap::Clamp;
	const std::string extension = lowerExtension(path);
	const bool linear = extension == ".ies" || extension == ".exr" || extension == ".pfm" || extension == ".hdr";
	request.colorSpace = linear ? TextureColorSpace::Linear : TextureColorSpace::Srgb;
	request.source.encoding = linear ? TextureEncoding::Linear : TextureEncoding::Srgb;
	return request;
}

void TextureLibrary::removeModel(const std::string& modelKey)
{
	for (auto it = m_entries.begin(); it != m_entries.end();) {
		if (it->source.isModelImage() && it->source.modelKey == modelKey) {
			m_removed.push_back(std::move(*it));
			it = m_entries.erase(it);
			m_dirty = true;
		} else {
			++it;
		}
	}
}

void TextureLibrary::removeUnused(const std::function<bool(uint64_t id)>& used)
{
	for (auto it = m_entries.begin(); it != m_entries.end();) {
		if (!it->source.isModelImage() && !used(it->id)) {
			m_removed.push_back(std::move(*it));
			it = m_entries.erase(it);
			m_dirty = true;
		} else {
			++it;
		}
	}
}

VkDeviceSize TextureLibrary::bytes() const
{
	VkDeviceSize total = 0;
	for (const TextureEntry& entry : m_entries) {
		total += entry.bytes;
	}
	return total;
}

uint32_t TextureLibrary::samplerIndex(VulkanEngine* engine, const TextureSampling& sampling)
{
	for (size_t i = 0; i < m_samplerStates.size(); i++) {
		if (m_samplerStates[i] == sampling) {
			return (uint32_t)i;
		}
	}
	if (m_samplerStates.size() >= MAX_SAMPLERS) {
		fmt::println("TextureLibrary: more than {} sampler states; a texture falls back to the first", MAX_SAMPLERS);
		return 0;
	}
	for (int nearest = 0; nearest < 2; nearest++) {
		VkSamplerCreateInfo info { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		info.magFilter = nearest || sampling.nearestMag ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
		info.minFilter = nearest || sampling.nearestMin ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
		info.mipmapMode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
		info.addressModeU = addressMode(sampling.wrapU);
		info.addressModeV = addressMode(sampling.wrapV);
		info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
		//anisotropy only matters to textureGrad, which the anisotropic filtering mode samples with; textureLod is isotropic
		info.anisotropyEnable = nearest ? VK_FALSE : VK_TRUE;
		info.maxAnisotropy = nearest ? 1.f : std::min(16.f, engine->m_gpuProperties.limits.maxSamplerAnisotropy);
		info.minLod = 0.f;
		info.maxLod = VK_LOD_CLAMP_NONE;
		VkSampler sampler;
		checkVkResult(vkCreateSampler(engine->m_device, &info, nullptr, &sampler));
		m_samplers.push_back(sampler);
	}
	m_samplerStates.push_back(sampling);
	return (uint32_t)m_samplerStates.size() - 1;
}

void TextureLibrary::release(VulkanEngine* engine, TextureEntry& entry)
{
	if (entry.image.image != VK_NULL_HANDLE) {
		if (entry.view != VK_NULL_HANDLE && entry.view != entry.image.imageView) {
			vkDestroyImageView(engine->m_device, entry.view, nullptr);
		}
		engine->destroyImage(entry.image);
	}
	entry.image = {};
	entry.view = VK_NULL_HANDLE;
	entry.bytes = 0;
}

void TextureLibrary::load(VulkanEngine* engine, TextureEntry& entry, uint32_t maxSize)
{
	release(engine, entry);
	entry.error.clear();
	entry.luminance.clear();
	entry.luminanceSize = glm::uvec2(0);
	entry.sampler = samplerIndex(engine, entry.sampling);

	//the source as a GPU image at its own size: a model's image is already one, a file is decoded and uploaded
	AllocatedImage source {};
	bool ownsSource = false;
	VkFormat format = entry.colorSpace == TextureColorSpace::Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	if (entry.source.isModelImage()) {
		const auto model = engine->m_models.find(entry.source.modelKey);
		if (model == engine->m_models.end() || model->second == nullptr || entry.source.modelImage >= (int)model->second->imagesByIndex.size()) {
			entry.error = "the model's image is gone";
			return;
		}
		source = model->second->imagesByIndex[(size_t)entry.source.modelImage];
		if (source.image == VK_NULL_HANDLE || source.imageFormat != VK_FORMAT_R8G8B8A8_UNORM) {
			entry.error = "the model's image is not an 8-bit color image";
			return;
		}
	} else {
		DecodedImage decoded;
		if (!decodeFile(entry.source, entry.colorSpace, decoded, entry.error)) {
			fmt::println("TextureLibrary: '{}' did not load: {}", entry.name, entry.error);
			return;
		}
		if (decoded.width == 0 || decoded.height == 0) {
			entry.error = "the image is empty";
			return;
		}
		const VkExtent3D extent { decoded.width, decoded.height, 1 };
		if (!decoded.rgbaFloat.empty()) {
			//half floats, which hold an HDR image's range; a texel past 65504 is clamped to it
			std::vector<uint16_t> halves(decoded.rgbaFloat.size());
			for (size_t i = 0; i < halves.size(); i++) {
				const float v = std::isfinite(decoded.rgbaFloat[i]) ? std::clamp(decoded.rgbaFloat[i], -65504.f, 65504.f) : 0.f;
				halves[i] = glm::packHalf1x16(v);
			}
			format = VK_FORMAT_R16G16B16A16_SFLOAT;
			source = engine->createImage(halves.data(), extent, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			entry.floatTexels = true;
		} else {
			source = engine->createImage(decoded.rgba8.data(), extent, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			entry.floatTexels = false;
		}
		ownsSource = true;
	}
	entry.sourceSize = glm::uvec2(source.imageExtent.width, source.imageExtent.height);

	//the full chain at the source's size, so every level is filtered from the one above it; then, when the source is
	//larger than the cap, only the levels within it are kept. An 8-bit color image is stored sRGB, so a blit between
	//levels filters linear values (Vulkan converts sRGB formats around a blit), and its first level is a copy of the
	//source's bytes, which are sRGB already
	const uint32_t width = entry.sourceSize.x;
	const uint32_t height = entry.sourceSize.y;
	const uint32_t levels = mipLevels(width, height);
	uint32_t skip = 0;
	while (std::max(width >> skip, height >> skip) > std::max(maxSize, 1u) && skip + 1 < levels) {
		skip++;
	}
	VkImageCreateInfo chainInfo = vkinit::image_create_info(format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VkExtent3D { width, height, 1 });
	chainInfo.mipLevels = levels;
	VmaAllocationCreateInfo allocInfo {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	AllocatedImage chain {};
	chain.imageFormat = format;
	chain.imageExtent = chainInfo.extent;
	checkVkResult(vmaCreateImage(engine->m_memAllocator, &chainInfo, &allocInfo, &chain.image, &chain.allocation, nullptr));

	AllocatedImage stored = chain;
	const uint32_t storedLevels = levels - skip;
	const VkExtent3D storedExtent { std::max(width >> skip, 1u), std::max(height >> skip, 1u), 1 };
	if (skip > 0) {
		VkImageCreateInfo storedInfo = vkinit::image_create_info(format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, storedExtent);
		storedInfo.mipLevels = storedLevels;
		stored = {};
		stored.imageFormat = format;
		stored.imageExtent = storedExtent;
		checkVkResult(vmaCreateImage(engine->m_memAllocator, &storedInfo, &allocInfo, &stored.image, &stored.allocation, nullptr));
	}

	auto levelBarrier = [](VkCommandBuffer cmd, VkImage image, uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to) {
		VkImageMemoryBarrier2 barrier { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
		barrier.oldLayout = from;
		barrier.newLayout = to;
		barrier.image = image;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1 };
		VkDependencyInfo dependency { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dependency.imageMemoryBarrierCount = 1;
		dependency.pImageMemoryBarriers = &barrier;
		vkinit::VkFunctionLoader::get_instance().vkCmdPipelineBarrier2KHR(cmd, &dependency);
	};

	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		const VkImageLayout sourceLayout = ownsSource ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		levelBarrier(cmd, source.image, 0, 1, sourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		levelBarrier(cmd, chain.image, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageCopy copy {};
		copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.extent = { width, height, 1 };
		vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		levelBarrier(cmd, source.image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceLayout);
		for (uint32_t level = 1; level < levels; level++) {
			levelBarrier(cmd, chain.image, level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			VkImageBlit blit {};
			blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
			blit.srcOffsets[1] = { (int32_t)std::max(width >> (level - 1), 1u), (int32_t)std::max(height >> (level - 1), 1u), 1 };
			blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
			blit.dstOffsets[1] = { (int32_t)std::max(width >> level, 1u), (int32_t)std::max(height >> level, 1u), 1 };
			vkCmdBlitImage(cmd, chain.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
		}
		levelBarrier(cmd, chain.image, levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		if (skip > 0) {
			levelBarrier(cmd, stored.image, 0, storedLevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			for (uint32_t level = 0; level < storedLevels; level++) {
				VkImageCopy levelCopy {};
				levelCopy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level + skip, 0, 1 };
				levelCopy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
				levelCopy.extent = { std::max(storedExtent.width >> level, 1u), std::max(storedExtent.height >> level, 1u), 1 };
				vkCmdCopyImage(cmd, chain.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stored.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &levelCopy);
			}
			levelBarrier(cmd, stored.image, 0, storedLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}
	});
	if (skip > 0) {
		vmaDestroyImage(engine->m_memAllocator, chain.image, chain.allocation);
	}
	if (ownsSource) {
		engine->destroyImage(source);
	}

	//the CPU copy: the largest level within LUMINANCE_SIZE, read back once
	uint32_t lumLevel = 0;
	while (std::max(storedExtent.width >> lumLevel, storedExtent.height >> lumLevel) > LUMINANCE_SIZE && lumLevel + 1 < storedLevels) {
		lumLevel++;
	}
	const uint32_t lumWidth = std::max(storedExtent.width >> lumLevel, 1u);
	const uint32_t lumHeight = std::max(storedExtent.height >> lumLevel, 1u);
	const VkDeviceSize texelBytes = entry.floatTexels ? 8 : 4;
	const VkDeviceSize readBytes = (VkDeviceSize)lumWidth * lumHeight * texelBytes;
	AllocatedBuffer readback = engine->createBuffer(readBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferImageCopy region {};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, lumLevel, 0, 1 };
		region.imageExtent = { lumWidth, lumHeight, 1 };
		vkCmdCopyImageToBuffer(cmd, stored.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
		levelBarrier(cmd, stored.image, 0, storedLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});
	vmaInvalidateAllocation(engine->m_memAllocator, readback.allocation, 0, readBytes);
	entry.luminanceSize = glm::uvec2(lumWidth, lumHeight);
	entry.luminance.resize((size_t)lumWidth * lumHeight);
	const uint8_t* mapped = (const uint8_t*)readback.info.pMappedData;
	for (size_t i = 0; i < entry.luminance.size(); i++) {
		glm::vec3 rgb;
		if (entry.floatTexels) {
			const uint16_t* half = (const uint16_t*)(mapped + i * 8);
			rgb = glm::vec3(glm::unpackHalf1x16(half[0]), glm::unpackHalf1x16(half[1]), glm::unpackHalf1x16(half[2]));
		} else {
			const uint8_t* texel = mapped + i * 4;
			rgb = glm::vec3(texel[0], texel[1], texel[2]) / 255.f;
			if (format == VK_FORMAT_R8G8B8A8_SRGB) {
				rgb = glm::vec3(srgbToLinearValue(rgb.r), srgbToLinearValue(rgb.g), srgbToLinearValue(rgb.b));
			}
		}
		entry.luminance[i] = 0.2126f * rgb.r + 0.7152f * rgb.g + 0.0722f * rgb.b;
	}
	engine->destroyBuffer(readback);

	VkImageViewCreateInfo viewInfo = vkinit::imageview_create_info(format, stored.image, VK_IMAGE_ASPECT_COLOR_BIT);
	viewInfo.subresourceRange.levelCount = storedLevels;
	checkVkResult(vkCreateImageView(engine->m_device, &viewInfo, nullptr, &stored.imageView));
	entry.image = stored;
	entry.view = stored.imageView;
	entry.size = glm::uvec2(storedExtent.width, storedExtent.height);
	entry.levels = storedLevels;
	VkDeviceSize bytes = 0;
	for (uint32_t level = 0; level < storedLevels; level++) {
		bytes += (VkDeviceSize)std::max(storedExtent.width >> level, 1u) * std::max(storedExtent.height >> level, 1u) * texelBytes;
	}
	entry.bytes = bytes;
}

void TextureLibrary::writeSet(VulkanEngine* engine)
{
	checkVkResult(vkResetDescriptorPool(engine->m_device, m_pool, 0));
	const uint32_t count = (uint32_t)std::min<size_t>(m_entries.size() + 1, MAX_TEXTURES);
	VkDescriptorSetVariableDescriptorCountAllocateInfo variable { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO };
	variable.descriptorSetCount = 1;
	variable.pDescriptorCounts = &count;
	VkDescriptorSetAllocateInfo allocInfo { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	allocInfo.pNext = &variable;
	allocInfo.descriptorPool = m_pool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &m_setLayout;
	checkVkResult(vkAllocateDescriptorSets(engine->m_device, &allocInfo, &m_set));

	std::vector<VkDescriptorImageInfo> samplers;
	for (VkSampler sampler : m_samplers) {
		samplers.push_back(VkDescriptorImageInfo { sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED });
	}
	std::vector<VkDescriptorImageInfo> images;
	images.push_back(VkDescriptorImageInfo { VK_NULL_HANDLE, m_white.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
	size_t overflow = 0;
	for (TextureEntry& entry : m_entries) {
		if (images.size() >= count) {
			entry.descriptor = 0;
			overflow++;
			continue;
		}
		//a texture that did not load reads as white, by descriptor 0
		entry.descriptor = entry.view != VK_NULL_HANDLE ? (uint32_t)images.size() : 0u;
		if (entry.view != VK_NULL_HANDLE) {
			images.push_back(VkDescriptorImageInfo { VK_NULL_HANDLE, entry.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
		}
	}
	VkWriteDescriptorSet writes[2] {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = m_set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = (uint32_t)samplers.size();
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	writes[0].pImageInfo = samplers.data();
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = m_set;
	writes[1].dstBinding = 27;
	writes[1].descriptorCount = (uint32_t)images.size();
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	writes[1].pImageInfo = images.data();
	vkUpdateDescriptorSets(engine->m_device, 2, writes, 0, nullptr);
	if (overflow > 0) {
		fmt::println("TextureLibrary: {} texture(s) past the {}-texture limit read as white", overflow, MAX_TEXTURES);
	}
}

bool TextureLibrary::update(VulkanEngine* engine, uint32_t maxSize)
{
	maxSize = std::max(maxSize, 1u);
	const bool resized = maxSize != m_maxSize;
	bool pending = m_dirty || resized || !m_removed.empty();
	if (!pending) {
		for (const TextureEntry& entry : m_entries) {
			if (entry.view == VK_NULL_HANDLE && entry.error.empty()) {
				pending = true;
				break;
			}
		}
	}
	if (!pending) {
		return false;
	}
	//frames in flight may still read the old images through set 1; a library change is a menu action, so a device-wide
	//wait is the plainly correct way to retire them
	vkDeviceWaitIdle(engine->m_device);
	for (TextureEntry& entry : m_removed) {
		release(engine, entry);
	}
	m_removed.clear();
	size_t loaded = 0;
	for (TextureEntry& entry : m_entries) {
		if (resized || (entry.view == VK_NULL_HANDLE && entry.error.empty())) {
			load(engine, entry, maxSize);
			loaded++;
		}
	}
	m_maxSize = maxSize;
	writeSet(engine);
	m_dirty = false;
	m_revision++;
	if (loaded > 0) {
		fmt::println("TextureLibrary: {} texture(s) loaded, {} in all, {:.1f} MB", loaded, m_entries.size(), (double)bytes() / (1024.0 * 1024.0));
	}
	return true;
}

float TextureLibrary::meanLuminance(uint64_t id, const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2) const
{
	const TextureEntry* entry = find(id);
	if (entry == nullptr || entry->luminance.empty()) {
		return 1.f;
	}
	const glm::uvec2 size = entry->luminanceSize;
	double sum = 0.0;
	for (int j = 0; j < 4; j++) {
		for (int i = 0; i < 4; i++) {
			const float su = std::sqrt(((float)i + 0.5f) / 4.f);
			const float v = ((float)j + 0.5f) / 4.f;
			glm::vec2 uv = (1.f - su) * uv0 + su * (1.f - v) * uv1 + su * v * uv2;
			if (!wrapCoordinate(uv.x, entry->sampling.wrapU) || !wrapCoordinate(uv.y, entry->sampling.wrapV)) {
				continue;
			}
			const uint32_t x = std::min((uint32_t)(uv.x * (float)size.x), size.x - 1);
			const uint32_t y = std::min((uint32_t)(uv.y * (float)size.y), size.y - 1);
			sum += entry->luminance[(size_t)y * size.x + x];
		}
	}
	return (float)(sum / 16.0);
}

float TextureLibrary::meanLuminance(uint64_t id) const
{
	const TextureEntry* entry = find(id);
	if (entry == nullptr || entry->luminance.empty()) {
		return 1.f;
	}
	double sum = 0.0;
	for (const float value : entry->luminance) {
		sum += value;
	}
	return (float)(sum / (double)entry->luminance.size());
}
