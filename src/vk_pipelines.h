#pragma once 
#include <vk_types.h>

namespace vkutil {

    bool load_shader_module(const std::string& filePath, VkDevice device, VkShaderModule* outShaderModule);
};