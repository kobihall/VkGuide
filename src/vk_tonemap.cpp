#include <vk_tonemap.h>

#include <vk_descriptors.h>
#include <vk_engine.h>
#include <vk_pipelines.h>

namespace {

struct TonemapPushConstants {
	float scale;
};

constexpr uint32_t WORKGROUP_SIZE = 16;

}

void TonemapPass::init(VulkanEngine* engine)
{
	const VkDevice device = engine->m_device;

	{
		DescriptorLayoutBuilder builder;
		builder.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		builder.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		m_descriptorLayout = builder.build(device, VK_SHADER_STAGE_COMPUTE_BIT);
	}

	VkPushConstantRange pushConstant {};
	pushConstant.offset = 0;
	pushConstant.size = sizeof(TonemapPushConstants);
	pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkPipelineLayoutCreateInfo layoutInfo {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.pSetLayouts = &m_descriptorLayout;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pPushConstantRanges = &pushConstant;
	layoutInfo.pushConstantRangeCount = 1;

	checkVkResult(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_pipelineLayout));

	VkShaderModule shader;
	const std::string shaderPath = engine->m_rootPath + "shaders/tonemap.comp.spv";
	if (!vkutil::load_shader_module(shaderPath, device, &shader)) {
		fmt::println("Error when building the tonemap compute shader");
		abort();
	}

	VkPipelineShaderStageCreateInfo stageInfo {};
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = shader;
	stageInfo.pName = "main";

	VkComputePipelineCreateInfo pipelineInfo {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.layout = m_pipelineLayout;
	pipelineInfo.stage = stageInfo;

	checkVkResult(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline));

	vkDestroyShaderModule(device, shader, nullptr);
}

void TonemapPass::destroy(VkDevice device)
{
	vkDestroyPipeline(device, m_pipeline, nullptr);
	vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
	vkDestroyDescriptorSetLayout(device, m_descriptorLayout, nullptr);
}

void TonemapPass::dispatch(VkCommandBuffer cmd, VkDevice device, DescriptorAllocatorGrowable& allocator, const AllocatedImage& linearImage, const AllocatedImage& displayImage, float scale)
{
	const VkDescriptorSet set = allocator.allocate(device, m_descriptorLayout);

	DescriptorWriter writer;
	writer.writeImage(0, linearImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(1, displayImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.updateSet(device, set);

	const TonemapPushConstants pushConstants { scale };

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TonemapPushConstants), &pushConstants);

	const uint32_t groupsX = (displayImage.imageExtent.width + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
	const uint32_t groupsY = (displayImage.imageExtent.height + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
	vkCmdDispatch(cmd, groupsX, groupsY, 1);
}
