#pragma once

// Reading GPU memory back to the host: the one place that does it, so every export (the render, its auxiliary
// outputs, the display image, and later the detector films of docs/plans/physoptics-data-export.md) invalidates
// the staging memory the same way. MoltenVK exposes no coherent host-visible memory, which is why every CPU write
// to mapped memory is flushed (CLAUDE.md); the read side needs vmaInvalidateAllocation() before the CPU looks.
//
// Both wait for the device to be idle and submit one immediate command buffer, so call them between renders or
// accept the stall of a running one.

#include <cstdint>
#include <vector>

#include <vk_types.h>

class VulkanEngine;

// The texels of `layerCount` array layers of an image in `layout` (one of GENERAL, TRANSFER_SRC_OPTIMAL or
// SHADER_READ_ONLY_OPTIMAL, which is moved through TRANSFER_SRC and back), layer after layer, row 0 first, each
// layer extent.width x extent.height x bytesPerTexel bytes
bool readbackImage(VulkanEngine* engine, VkImage image, VkImageLayout layout, VkExtent3D extent, uint32_t layerCount, VkDeviceSize bytesPerTexel, std::vector<uint8_t>& out);

// the first `bytes` of a buffer created with VK_BUFFER_USAGE_TRANSFER_SRC_BIT
bool readbackBuffer(VulkanEngine* engine, VkBuffer buffer, VkDeviceSize bytes, std::vector<uint8_t>& out);
