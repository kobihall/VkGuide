#include <vk_tonemap.h>

#include <vk_descriptors.h>
#include <vk_engine.h>

namespace {

struct TonemapPushConstants {
	float scale;
};

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

void TonemapPass::dispatch(VkCommandBuffer cmd, VkDevice device, DescriptorAllocatorGrowable& allocator, const AllocatedImage& linearImage, const AllocatedImage& displayImage, float scale)
{
	const VkDescriptorSet set = allocator.allocate(device, m_pass.setLayout);

	DescriptorWriter writer;
	writer.writeImage(0, linearImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(1, displayImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.updateSet(device, set);

	const TonemapPushConstants pushConstants { scale };
	dispatchComputePassOver(cmd, m_pass, set, &pushConstants, displayImage.imageExtent);
}
