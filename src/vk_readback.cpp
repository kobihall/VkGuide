#include <vk_readback.h>

#include <cstring>

#include <vk_engine.h>
#include <vk_images.h>

bool readbackImage(VulkanEngine* engine, VkImage image, VkImageLayout layout, VkExtent3D extent, uint32_t layerCount, VkDeviceSize bytesPerTexel, std::vector<uint8_t>& out)
{
	if (image == VK_NULL_HANDLE || layerCount == 0 || extent.width == 0 || extent.height == 0) {
		return false;
	}
	vkDeviceWaitIdle(engine->m_device);
	const VkDeviceSize layerBytes = (VkDeviceSize)extent.width * extent.height * bytesPerTexel;
	const VkDeviceSize bytes = layerBytes * layerCount;
	const AllocatedBuffer staging = engine->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		//the copy reads in GENERAL or TRANSFER_SRC; an image kept in SHADER_READ_ONLY (the display image) is moved
		//there and back
		const bool moved = layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		if (moved) {
			vkutil::transition_image(cmd, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}
		VkBufferImageCopy region {};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layerCount };
		region.imageExtent = extent;
		vkCmdCopyImageToBuffer(cmd, image, moved ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : layout, staging.buffer, 1, &region);
		if (moved) {
			vkutil::transition_image(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
		}
	});
	vmaInvalidateAllocation(engine->m_memAllocator, staging.allocation, 0, bytes);
	out.assign((const uint8_t*)staging.info.pMappedData, (const uint8_t*)staging.info.pMappedData + bytes);
	engine->destroyBuffer(staging);
	return true;
}

bool readbackBuffer(VulkanEngine* engine, VkBuffer buffer, VkDeviceSize bytes, std::vector<uint8_t>& out)
{
	if (buffer == VK_NULL_HANDLE || bytes == 0) {
		return false;
	}
	vkDeviceWaitIdle(engine->m_device);
	const AllocatedBuffer staging = engine->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy copy {};
		copy.size = bytes;
		vkCmdCopyBuffer(cmd, buffer, staging.buffer, 1, &copy);
	});
	vmaInvalidateAllocation(engine->m_memAllocator, staging.allocation, 0, bytes);
	out.assign((const uint8_t*)staging.info.pMappedData, (const uint8_t*)staging.info.pMappedData + bytes);
	engine->destroyBuffer(staging);
	return true;
}
