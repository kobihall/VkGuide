#include <vk_tonemap.h>

#include <vk_descriptors.h>
#include <vk_engine.h>

namespace {

// shaders/tonemap.comp's push constants
struct TonemapPushConstants {
	float scale;
	uint32_t mode;
	uint32_t colormap;
	float decades;
	glm::vec4 tint;
};
static_assert(sizeof(TonemapPushConstants) == 32);

}

void TonemapPass::init(VulkanEngine* engine)
{
	m_pass = ComputePassBuilder(engine->m_rootPath + "shaders/tonemap.comp.spv")
		.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.setPushConstants<TonemapPushConstants>()
		.build(engine->m_device);
}

void TonemapPass::destroy(VkDevice device)
{
	m_pass.destroy(device);
}

void TonemapPass::dispatch(VkCommandBuffer cmd, VkDevice device, DescriptorAllocatorGrowable& allocator, const AllocatedImage& linearImage, const AllocatedImage& displayImage, float scale,
	const TonemapDisplay& display)
{
	const VkDescriptorSet set = allocator.allocate(device, m_pass.setLayout);

	DescriptorWriter writer;
	writer.writeImage(0, linearImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(1, displayImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.updateSet(device, set);

	const TonemapPushConstants pushConstants { scale, (uint32_t)display.mode, display.colormap, display.decades, glm::vec4(display.tint, 1.f) };
	dispatchComputePassOver(cmd, m_pass, set, &pushConstants, displayImage.imageExtent);
}
