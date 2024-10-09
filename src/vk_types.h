#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <span>
#include <array>
#include <functional>
#include <deque>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <VkBootstrap.h>

#include <VulkanMemoryAllocator/include/vk_mem_alloc.h>

#include <fmt/core.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

struct AllocatedImage {
    VkImage image;
    VkImageView imageView;
    VmaAllocation allocation;
    VkExtent3D imageExtent;
    VkFormat imageFormat;
};


template <typename T>
void vkbErr(vkb::Result<T> res){
    if(!res) {
        fmt::println("Vulkan bootstrap result error: {}", res.error().message());
        abort();
    }
};

#define checkVkResult(x)                                        \
do{                                                             \
    VkResult err = x;                                           \
	if (err) {                                                  \
        fmt::println("Detected Vulkan error: {}", string_VkResult(err)); \
        abort();                                                \
    }                                                           \
} while(0)