#pragma once 
#include <vk_types.h>

namespace vkutil {

    bool load_shader_module(const std::string& filePath, VkDevice device, VkShaderModule* outShaderModule);
};

class PipelineBuilder {
public:
    std::vector<VkPipelineShaderStageCreateInfo> m_shaderStages;
   
    VkPipelineInputAssemblyStateCreateInfo m_inputAssembly;
    VkPipelineRasterizationStateCreateInfo m_rasterizer;
    VkPipelineColorBlendAttachmentState m_colorBlendAttachment;
    VkPipelineMultisampleStateCreateInfo m_multisampling;
    VkPipelineLayout m_pipelineLayout;
    VkPipelineDepthStencilStateCreateInfo m_depthStencil;
    VkPipelineRenderingCreateInfo m_renderInfo;
    VkFormat m_colorAttachmentformat;

	PipelineBuilder(){ clear(); }

    void clear();

    VkPipeline buildPipeline(VkDevice device);

    void setShaders(VkShaderModule vertexShader, VkShaderModule fragmentShader);
    void setInputTopology(VkPrimitiveTopology topology);
    void setPolygonMode(VkPolygonMode mode);
    void setCullMode(VkCullModeFlags cullMode, VkFrontFace frontFace);
    void setMultisamplingNone();
    void disableBlending();
    void enableBlendingAdditive();
    void enableBlendingAlphablend();
    void setColorAttachmentFormat(VkFormat format);
    void setDepthFormat(VkFormat format);
    void disableDepthtest();
    void enableDepthtest(bool depthWriteEnable, VkCompareOp op);
};
