#pragma once

#include <vk_types.h>

namespace vkinit {

	VkCommandPoolCreateInfo command_pool_create_info(uint32_t queueFamilyIndex, VkCommandPoolCreateFlags flags = 0);
	VkCommandBufferAllocateInfo command_buffer_allocate_info(VkCommandPool pool, uint32_t count = 1);

	VkFenceCreateInfo fence_create_info(VkFenceCreateFlags flags = 0);
	VkSemaphoreCreateInfo semaphore_create_info(VkSemaphoreCreateFlags flags = 0);
	VkSemaphoreSubmitInfo semaphore_submit_info(VkPipelineStageFlags2 stageMask, VkSemaphore semaphore);

	VkCommandBufferBeginInfo command_buffer_begin_info(VkCommandBufferUsageFlags flags = 0);
	VkCommandBufferSubmitInfo command_buffer_submit_info(VkCommandBuffer cmd);

	VkSubmitInfo2 submit_info(VkCommandBufferSubmitInfo* cmd, VkSemaphoreSubmitInfo* signalSemaphoreInfo, VkSemaphoreSubmitInfo* waitSemaphoreInfo);

	VkImageSubresourceRange image_subresource_range(VkImageAspectFlags aspectMask);

	class VkFunctionLoader {
	public:
		static VkFunctionLoader& get_instance();
	
		// Function pointers for synchronization2 and other Vulkan features
		PFN_vkCmdPipelineBarrier2KHR vkCmdPipelineBarrier2KHR = nullptr;
		PFN_vkQueueSubmit2KHR vkQueueSubmit2KHR = nullptr;
	
		// Initialize function pointers
		void load_functions(VkDevice device);
	
	private:
		VkFunctionLoader() = default;  // Private constructor for Singleton pattern
	};
}