#pragma once 

#include <vulkan/vulkan.h>

namespace vkutil {

	void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout);

	void copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D dstSize);

	// one global VkMemoryBarrier2: everything `srcStage` wrote with `srcAccess` is visible to
	// `dstStage`'s `dstAccess`. For buffer hand-offs between passes (compute -> compute, a
	// transfer-reset buffer -> compute, a shader-written argument buffer -> indirect dispatch),
	// where no image layout is involved. VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT is the stage that
	// reads dispatch arguments as well as draw arguments, despite the name
	void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
};