#pragma once

// A general-purpose compute pass: one compute shader, the shape of the one descriptor set it
// binds (any mix of images and buffers, at set 0), and an optional push-constant block of any
// size. Built once with ComputePassBuilder, recorded any number of times with
// dispatchComputePass() / dispatchComputePassOver().
//
// The pass owns the layouts and the pipeline; the caller owns the descriptor set. Which
// resources actually get bound varies per use (per frame, per effect, per call site), so the
// caller allocates a set against `setLayout` - normally from the current frame's
// DescriptorAllocatorGrowable, which is reset every frame - writes it with DescriptorWriter, and
// hands it to the dispatch. Push constants are a raw byte block the caller supplies as a
// pointer, the same plain-struct-memcpy'd-in convention every other push constant here uses.
//
// The caller also owns every image layout transition and memory barrier around the dispatch,
// since only it knows where each resource is coming from and going to.

#include <vk_types.h>
#include <vk_descriptors.h>

struct ComputePass {
	// VK_NULL_HANDLE when the shader binds nothing (push constants only)
	VkDescriptorSetLayout setLayout { VK_NULL_HANDLE };
	VkPipelineLayout pipelineLayout { VK_NULL_HANDLE };
	VkPipeline pipeline { VK_NULL_HANDLE };
	// 0 if the shader takes none
	uint32_t pushConstantSize { 0 };
	// the shader's local_size, so dispatchComputePassOver() can size a dispatch from the domain
	// it should cover. Not validated against the shader - keep the two in sync by hand
	VkExtent3D workgroupSize { 16, 16, 1 };

	void destroy(VkDevice device);
};

class ComputePassBuilder {
public:
	// shaderPath is a compiled .comp.spv. Loading it happens in build()
	explicit ComputePassBuilder(std::string shaderPath);

	// bindings are all at set 0, one descriptor each, visible to the compute stage
	ComputePassBuilder& addBinding(uint32_t binding, VkDescriptorType type);
	ComputePassBuilder& setPushConstantSize(uint32_t bytes);
	template <typename T>
	ComputePassBuilder& setPushConstants() { return setPushConstantSize(sizeof(T)); }
	// defaults to 16 x 16 x 1, the convention every .comp file in shaders/ follows
	ComputePassBuilder& setWorkgroupSize(uint32_t x, uint32_t y = 1, uint32_t z = 1);

	// aborts if the shader cannot be loaded or any vulkan object fails to build, matching the
	// checkVkResult convention: a missing shader is a broken build, not recoverable input
	ComputePass build(VkDevice device);

private:
	std::string m_shaderPath;
	DescriptorLayoutBuilder m_layoutBuilder;
	uint32_t m_pushConstantSize { 0 };
	VkExtent3D m_workgroupSize { 16, 16, 1 };
};

// the number of workgroups needed to cover `domain` invocations at `workgroupSize` each
VkExtent3D computeGroupCount(VkExtent3D domain, VkExtent3D workgroupSize);

// binds the pipeline, binds `set` at index 0 (skipped when the pass has no bindings), pushes
// pass.pushConstantSize bytes from pushData (skipped when the pass has none - pushData must
// otherwise be non-null), and dispatches groupCount workgroups
void dispatchComputePass(VkCommandBuffer cmd, const ComputePass& pass, VkDescriptorSet set, const void* pushData, VkExtent3D groupCount);

// the same, dispatching enough workgroups to cover `domain` invocations - typically an image's
// extent. The shader still has to bounds-check, since the last workgroup along each axis
// usually overhangs
void dispatchComputePassOver(VkCommandBuffer cmd, const ComputePass& pass, VkDescriptorSet set, const void* pushData, VkExtent3D domain);
