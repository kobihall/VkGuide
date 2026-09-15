#include <vk_compute.h>

#include <vk_pipelines.h>

void ComputePass::destroy(VkDevice device)
{
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
	if (setLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
	}
	*this = ComputePass {};
}

ComputePassBuilder::ComputePassBuilder(std::string shaderPath)
	: m_shaderPath(std::move(shaderPath))
{
}

ComputePassBuilder& ComputePassBuilder::addBinding(uint32_t binding, VkDescriptorType type)
{
	m_layoutBuilder.addBinding(binding, type);
	return *this;
}

ComputePassBuilder& ComputePassBuilder::setPushConstantSize(uint32_t bytes)
{
	m_pushConstantSize = bytes;
	return *this;
}

ComputePassBuilder& ComputePassBuilder::setWorkgroupSize(uint32_t x, uint32_t y, uint32_t z)
{
	m_workgroupSize = { x, y, z };
	return *this;
}

ComputePass ComputePassBuilder::build(VkDevice device)
{
	ComputePass pass;
	pass.pushConstantSize = m_pushConstantSize;
	pass.workgroupSize = m_workgroupSize;

	if (!m_layoutBuilder.bindings.empty()) {
		pass.setLayout = m_layoutBuilder.build(device, VK_SHADER_STAGE_COMPUTE_BIT);
	}

	VkPushConstantRange pushConstant {};
	pushConstant.offset = 0;
	pushConstant.size = m_pushConstantSize;
	pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkPipelineLayoutCreateInfo layoutInfo {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = pass.setLayout != VK_NULL_HANDLE ? 1 : 0;
	layoutInfo.pSetLayouts = &pass.setLayout;
	layoutInfo.pushConstantRangeCount = m_pushConstantSize > 0 ? 1 : 0;
	layoutInfo.pPushConstantRanges = &pushConstant;

	checkVkResult(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pass.pipelineLayout));

	VkShaderModule shader;
	if (!vkutil::load_shader_module(m_shaderPath, device, &shader)) {
		fmt::println("Failed to load compute shader '{}'", m_shaderPath);
		abort();
	}

	VkPipelineShaderStageCreateInfo stageInfo {};
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = shader;
	stageInfo.pName = "main";

	VkComputePipelineCreateInfo pipelineInfo {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.layout = pass.pipelineLayout;
	pipelineInfo.stage = stageInfo;

	checkVkResult(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pass.pipeline));

	//the pipeline holds its own copy of the code
	vkDestroyShaderModule(device, shader, nullptr);

	return pass;
}

VkExtent3D computeGroupCount(VkExtent3D domain, VkExtent3D workgroupSize)
{
	auto ceilDiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
	return {
		ceilDiv(domain.width, workgroupSize.width),
		ceilDiv(domain.height, workgroupSize.height),
		ceilDiv(domain.depth, workgroupSize.depth)
	};
}

namespace {

//everything a dispatch needs bound and pushed, shared by the direct and indirect entry points
void bindComputePass(VkCommandBuffer cmd, const ComputePass& pass, VkDescriptorSet set, const void* pushData)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline);

	if (pass.setLayout != VK_NULL_HANDLE) {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipelineLayout, 0, 1, &set, 0, nullptr);
	}

	if (pass.pushConstantSize > 0) {
		if (pushData == nullptr) {
			fmt::println("dispatchComputePass: the pass takes {} bytes of push constants but none were supplied", pass.pushConstantSize);
			abort();
		}
		vkCmdPushConstants(cmd, pass.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pass.pushConstantSize, pushData);
	}
}

}

void dispatchComputePass(VkCommandBuffer cmd, const ComputePass& pass, VkDescriptorSet set, const void* pushData, VkExtent3D groupCount)
{
	bindComputePass(cmd, pass, set, pushData);
	vkCmdDispatch(cmd, groupCount.width, groupCount.height, groupCount.depth);
}

void dispatchComputePassIndirect(VkCommandBuffer cmd, const ComputePass& pass, VkDescriptorSet set, const void* pushData, VkBuffer argumentBuffer, VkDeviceSize argumentOffset)
{
	bindComputePass(cmd, pass, set, pushData);
	vkCmdDispatchIndirect(cmd, argumentBuffer, argumentOffset);
}

void dispatchComputePassOver(VkCommandBuffer cmd, const ComputePass& pass, VkDescriptorSet set, const void* pushData, VkExtent3D domain)
{
	dispatchComputePass(cmd, pass, set, pushData, computeGroupCount(domain, pass.workgroupSize));
}
