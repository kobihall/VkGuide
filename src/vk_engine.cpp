#include "vk_engine.h"

#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // abort
#include <iostream>
#include <vector>

#include <vk_types.h>
#include <vk_initializers.h>

#include <VkBootstrap.h>

constexpr bool b_UseValidationLayers = true;

void VulkanEngine::init()
{
    initGLFW();
	initVulkan();
	initSwapchain();
	initCommands();
	initSyncStructures();
	
	//everything went fine
	m_isInitialized = true;
}

void VulkanEngine::initGLFW()
{
    // Initialize GLFW and create a window with it. 
    glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
	{
		std::cerr << "Could not ini-talize GLFW!\n";
		return;
	}

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    m_window = glfwCreateWindow(m_windowExtent.width, m_windowExtent.height, "Vulkan", nullptr, nullptr);
    glfwSetWindowUserPointer(m_window, this);
    //glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback); // Define framebufferResizeCallback at a later point
}

void VulkanEngine::initVulkan()
{
	vkb::InstanceBuilder builder;

	//make the vulkan instance, with basic debug features
	auto inst_ret = builder.set_app_name("Example Vulkan Application")
		.request_validation_layers(b_UseValidationLayers)
		.use_default_debug_messenger()
		.require_api_version(1, 2, 0)
		.build();
	vkbErr(inst_ret);
	vkb::Instance vkb_inst = inst_ret.value();

	//grab the instance 
	m_instance = vkb_inst.instance;
	m_debugMessenger = vkb_inst.debug_messenger;

	glfwCreateWindowSurface(m_instance, m_window, m_Allocator, &m_surface);

	//vulkan 1.2 features
	VkPhysicalDeviceVulkan12Features features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	features.bufferDeviceAddress = true;
	features.descriptorIndexing = true;

	//use vkbootstrap to select a gpu. 
	//We want a gpu that can write to the GLFW surface and supports vulkan 1.3 with the correct features
	vkb::PhysicalDeviceSelector selector{ vkb_inst };
	auto physdev_ret = selector
		.set_minimum_version(1, 2)
		.add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)
		.set_required_features_12(features)
		.set_surface(m_surface)
		.select();
	vkbErr(physdev_ret);

	VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering_features{};
	dynamic_rendering_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
	dynamic_rendering_features.dynamicRendering = VK_TRUE;

	//create the final vulkan device
	vkb::PhysicalDevice physicalDevice = physdev_ret.value();
	vkb::DeviceBuilder deviceBuilder{ physicalDevice };
	auto dev_ret = deviceBuilder
		.add_pNext(&dynamic_rendering_features)
		.build();
	vkbErr(dev_ret);
	vkb::Device vkbDevice = dev_ret.value();

	// Get the VkDevice handle used in the rest of a vulkan application
	m_device = vkbDevice.device;
	m_chosenGPU = physicalDevice.physical_device;
}

void VulkanEngine::createSwapchain(uint32_t width, uint32_t height)
{
	vkb::SwapchainBuilder swapchainBuilder{ m_chosenGPU, m_device, m_surface};

	m_swapchainImageFormat = VK_FORMAT_B8G8R8_UNORM;
	auto swapchain_ret = swapchainBuilder
		.set_desired_format(VkSurfaceFormatKHR{ .format = m_swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
		.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
		.set_desired_extent(width, height)
		.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.build();
	vkbErr(swapchain_ret);

	vkb::Swapchain vkbSwapchain = swapchain_ret.value();
	m_swapchainExtent = vkbSwapchain.extent;
	//store swapchain and its related images
	m_swapchain = vkbSwapchain.swapchain;
	m_swapchainImages = vkbSwapchain.get_images().value();
	m_swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void VulkanEngine::destroySwapchain()
{
	vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
	// destroy swapchain resources
	for (int i = 0; i < m_swapchainImageViews.size(); i++) {
		vkDestroyImageView(m_device, m_swapchainImageViews[i], nullptr);
	}
}

void VulkanEngine::initSwapchain()
{
	createSwapchain(m_windowExtent.width, m_windowExtent.height);
}

void VulkanEngine::initCommands()
{
	//nothing yet
}

void VulkanEngine::initSyncStructures()
{
	//nothing yet
}

void VulkanEngine::cleanup()
{	
	if (m_isInitialized) {
		destroySwapchain();

		vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
		vkDestroyDevice(m_device, nullptr);

		vkb::destroy_debug_utils_messenger(m_instance, m_debugMessenger);
		vkDestroyInstance(m_instance, nullptr);

		glfwDestroyWindow(m_window);
        glfwTerminate();
	}
}

void VulkanEngine::draw()
{
	//nothing yet
}

void VulkanEngine::run()
{
	bool bQuit = false;

	//main loop
	while (!bQuit && !glfwWindowShouldClose(m_window))
	{
        glfwPollEvents();

		draw();
	}
}

void VulkanEngine::glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}