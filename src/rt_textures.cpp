#include <rt_textures.h>

#include <vector>

#include <vk_engine.h>
#include <vk_images.h>
#include <vk_initializers.h>

void RaytraceTextureArray::init(VulkanEngine* engine)
{
	createArray(engine, 1);
	//rebuild() runs only when the models change, so a scene with none - a cornell box of analytic
	//shapes, say - would bind this array while it is still UNDEFINED. The layer is never sampled
	//there (an untextured material's albedoLayer is -1), but a descriptor still has to name an
	//image in the layout it was written with
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_array.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});
}

void RaytraceTextureArray::createArray(VulkanEngine* engine, uint32_t layers)
{
	m_layerCount = layers;

	const VkExtent3D extent { LAYER_SIZE, LAYER_SIZE, 1 };
	m_array.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
	m_array.imageExtent = extent;

	//TRANSFER_SRC is not needed to render: it is what lets the array be read back and inspected,
	//which is the only way to tell "the blits went wrong" from "the shading went wrong"
	VkImageCreateInfo imageInfo = vkinit::image_create_info(m_array.imageFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, extent);
	imageInfo.arrayLayers = layers;

	VmaAllocationCreateInfo allocInfo {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocInfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	checkVkResult(vmaCreateImage(engine->m_memAllocator, &imageInfo, &allocInfo, &m_array.image, &m_array.allocation, nullptr));

	VkImageViewCreateInfo viewInfo = vkinit::imageview_create_info(m_array.imageFormat, m_array.image, VK_IMAGE_ASPECT_COLOR_BIT);
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.subresourceRange.layerCount = layers;
	checkVkResult(vkCreateImageView(engine->m_device, &viewInfo, nullptr, &m_array.imageView));
}

void RaytraceTextureArray::destroyArray(VulkanEngine* engine)
{
	if (m_array.image == VK_NULL_HANDLE) {
		return;
	}
	engine->destroyImage(m_array);
	m_array = {};
	m_layerCount = 0;
	m_layers.clear();
}

void RaytraceTextureArray::destroy(VulkanEngine* engine)
{
	destroyArray(engine);
}

int RaytraceTextureArray::layerOf(VkImage image) const
{
	const auto found = m_layers.find(image);
	return found == m_layers.end() ? -1 : found->second;
}

void RaytraceTextureArray::rebuild(VulkanEngine* engine)
{
	//every distinct texture image across every model's materials: base colour, normal, metal/rough
	//and emissive alike, since the layer is only a number and the shader knows what it samples it
	//for. Two materials sharing one texture share one layer. Kept as whole AllocatedImages, since
	//the blit needs each source's own extent to scale from
	std::vector<AllocatedImage> sources;
	std::unordered_map<VkImage, int> layers;
	size_t dropped = 0;

	auto add = [&](const AllocatedImage& image) {
		if (image.image == VK_NULL_HANDLE || layers.count(image.image) != 0) {
			return;
		}
		if (sources.size() >= MAX_LAYERS) {
			dropped++;
			return;
		}
		layers[image.image] = (int)sources.size();
		sources.push_back(image);
	};
	for (const auto& [name, model] : engine->m_models) {
		if (model == nullptr) {
			continue;
		}
		for (const auto& [materialName, material] : model->materials) {
			if (material == nullptr) {
				continue;
			}
			//base colour first, so a scene over the cap loses the finer maps before the colours
			add(material->baseColorImage);
		}
		for (const auto& [materialName, material] : model->materials) {
			if (material == nullptr) {
				continue;
			}
			add(material->normalImage);
			add(material->metalRoughImage);
			add(material->emissiveImage);
		}
	}
	if (dropped > 0) {
		fmt::println("RaytraceTextureArray: {} texture(s) over the {}-layer cap; those maps are ignored and the materials fall back to their factors", dropped, MAX_LAYERS);
	}

	//frames in flight may still sample the old array through a descriptor set written this frame
	//or last, and there is no cheap way to know. A model import is a menu-driven action, so a
	//device-wide wait is the obviously-correct way to retire it
	vkDeviceWaitIdle(engine->m_device);
	destroyArray(engine);
	createArray(engine, std::max<uint32_t>((uint32_t)sources.size(), 1));
	m_layers = std::move(layers);

	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_array.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		//an unused fallback layer is still read on a miss-index, so it starts opaque white rather
		//than as whatever the allocation held
		if (sources.empty()) {
			const VkClearColorValue white { { 1.f, 1.f, 1.f, 1.f } };
			const VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
			vkCmdClearColorImage(cmd, m_array.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
		}

		for (uint32_t layer = 0; layer < sources.size(); layer++) {
			//the glTF images live in SHADER_READ_ONLY_OPTIMAL for the raster material sets; they
			//are lent to the blit and handed straight back
			const AllocatedImage& source = sources[layer];
			const VkExtent2D sourceSize { source.imageExtent.width, source.imageExtent.height };
			vkutil::transition_image(cmd, source.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			vkutil::blit_image_to_layer(cmd, source.image, m_array.image, layer, sourceSize, { LAYER_SIZE, LAYER_SIZE });
			vkutil::transition_image(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}

		vkutil::transition_image(cmd, m_array.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	fmt::println("RaytraceTextureArray: {} layer(s) at {} x {}", m_layerCount, LAYER_SIZE, LAYER_SIZE);
}
