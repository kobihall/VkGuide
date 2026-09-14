#pragma once

// Converts a linear rgba32f radiance image into an rgba8 image that can be shown directly:
// scale, zero out NaNs, clamp to [0,1], gamma 2 (shaders/tonemap.comp).
//
// Every raytracer output goes through this one pass - the CPU raytracer uploads its finished
// buffer and runs it once, and the planned compute raytracer runs it on its accumulation image
// each frame - so the two produce directly comparable images rather than each applying its own
// display transform.
//
// Built by hand in the same shape as the engine's background compute effects. When
// docs/plans/compute-pipeline-general.md's ComputePass exists, this is a natural first thing to
// move onto it.

#include <vk_types.h>

class VulkanEngine;
struct DescriptorAllocatorGrowable;

class TonemapPass {
public:
	static constexpr VkFormat LINEAR_FORMAT = VK_FORMAT_R32G32B32A32_SFLOAT;
	static constexpr VkFormat DISPLAY_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

	void init(VulkanEngine* engine);
	void destroy(VkDevice device);

	// records the dispatch into cmd. Both images need VK_IMAGE_USAGE_STORAGE_BIT and must already
	// be in VK_IMAGE_LAYOUT_GENERAL - the caller owns every layout transition, since only it knows
	// where each image is coming from and going to. The descriptor set is allocated from
	// `allocator`, which must not be reset before cmd has finished executing
	void dispatch(VkCommandBuffer cmd, VkDevice device, DescriptorAllocatorGrowable& allocator, const AllocatedImage& linearImage, const AllocatedImage& displayImage, float scale);

private:
	VkDescriptorSetLayout m_descriptorLayout { VK_NULL_HANDLE };
	VkPipelineLayout m_pipelineLayout { VK_NULL_HANDLE };
	VkPipeline m_pipeline { VK_NULL_HANDLE };
};
