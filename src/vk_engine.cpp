#include "vk_engine.h"

#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // abort
#include <iostream>
#include <vector>

#include <vk_types.h>
#include <vk_initializers.h>
#include <vk_images.h>

#include <VkBootstrap.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

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

void VulkanEngine::draw()
{
	// wait until the gpu has finished rendering the last frame. Timeout of 1
	// second
	checkVkResult(vkWaitForFences(m_device, 1, &get_current_frame().renderFence, true, 1000000000));

	get_current_frame().deletionQueue.flush();

	checkVkResult(vkResetFences(m_device, 1, &get_current_frame().renderFence));

	//request image from the swapchain
	uint32_t swapchainImageIndex;
	checkVkResult(vkAcquireNextImageKHR(m_device, m_swapchain, 1000000000, get_current_frame().swapchainSemaphore, nullptr, &swapchainImageIndex));

	//naming it cmd for shorter writing
	VkCommandBuffer cmd = get_current_frame().mainCommandBuffer;

	// now that we are sure that the commands finished executing, we can safely
	// reset the command buffer to begin recording again.
	checkVkResult(vkResetCommandBuffer(cmd, 0));

	//begin the command buffer recording. We will use this command buffer exactly once, so we want to let vulkan know that
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	m_drawExtent.width = m_drawImage.imageExtent.width;
	m_drawExtent.height = m_drawImage.imageExtent.height;

	//start the command buffer recording
	checkVkResult(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

	// transition our main draw image into general layout so we can write into it
	// we will overwrite it all so we don't care about what was the older layout
	vkutil::transition_image(cmd, m_drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

	// flash the background color
	draw_background(cmd);

	// transition the draw image and the swapchain image into their correct transfer layouts
	vkutil::transition_image(cmd, m_drawImage.image,VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkutil::transition_image(cmd, m_swapchainImages[swapchainImageIndex],VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	// execute a copy from the draw image into the swapchain
	vkutil::copy_image_to_image(cmd, m_drawImage.image, m_swapchainImages[swapchainImageIndex], m_drawExtent, m_swapchainExtent);

	// set swapchain image layout to Present so we can show it on the screen
	vkutil::transition_image(cmd, m_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	//finalize the command buffer (we can no longer add commands, but it can now be executed)
	checkVkResult(vkEndCommandBuffer(cmd));

	//prepare the submission to the queue. 
	//we want to wait on the m_presentSemaphore, as that semaphore is signaled when the swapchain is ready
	//we will signal the m_renderSemaphore, to signal that rendering has finished

	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);

	VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,get_current_frame().swapchainSemaphore);
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, get_current_frame().renderSemaphore);	
	
	VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo,&signalInfo,&waitInfo);	

	//submit command buffer to the queue and execute it.
	// renderFence will now block until the graphic commands finish execution
	checkVkResult(vkinit::VkFunctionLoader::get_instance().vkQueueSubmit2KHR(m_graphicsQueue, 1, &submit, get_current_frame().renderFence));

	//prepare present
	// this will put the image we just rendered to into the visible window.
	// we want to wait on the _renderSemaphore for that, 
	// as its necessary that drawing commands have finished before the image is displayed to the user
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pNext = nullptr;
	presentInfo.pSwapchains = &m_swapchain;
	presentInfo.swapchainCount = 1;

	presentInfo.pWaitSemaphores = &get_current_frame().renderSemaphore;
	presentInfo.waitSemaphoreCount = 1;

	presentInfo.pImageIndices = &swapchainImageIndex;

	checkVkResult(vkQueuePresentKHR(m_graphicsQueue, &presentInfo));

	//increase the number of frames drawn
	m_frameNumber++;
}

void VulkanEngine::draw_background(VkCommandBuffer cmd)
{
	//make a clear-color from frame number. This will flash with a 120 frame period.
	VkClearColorValue clearValue;
	float flash = std::abs(std::sin(m_frameNumber / 120.f));
	clearValue = { { 0.0f, 0.0f, flash, 1.0f } };

	VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

	//clear image
	vkCmdClearColorImage(cmd, m_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
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

	VkPhysicalDeviceSynchronization2FeaturesKHR sync2_features{};
	sync2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
	sync2_features.synchronization2 = true;

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
		.add_required_extension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)
		.add_required_extension(VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME)
		.set_required_features_12(features)
		.set_surface(m_surface)
		.select();
	vkbErr(physdev_ret);

	VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering_features{};
	dynamic_rendering_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
	dynamic_rendering_features.dynamicRendering = VK_TRUE;
	dynamic_rendering_features.pNext = &sync2_features;

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

	vkinit::VkFunctionLoader::get_instance().load_functions(m_device);

	// use vkbootstrap to get a Graphics queue
	m_graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
	m_graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

	// initialize the memory allocator
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = m_chosenGPU;
    allocatorInfo.device = m_device;
    allocatorInfo.instance = m_instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocatorInfo, &m_memAllocator);

    m_mainDeletionQueue.push_function([&]() {
        vmaDestroyAllocator(m_memAllocator);
    });

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

	//draw image size will match the window
	VkExtent3D drawImageExtent = {
		m_windowExtent.width,
		m_windowExtent.height,
		1
	};

	//hardcoding the draw format to 32 bit float
	m_drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	m_drawImage.imageExtent = drawImageExtent;

	VkImageUsageFlags drawImageUsages{};
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	VkImageCreateInfo rimg_info = vkinit::image_create_info(m_drawImage.imageFormat, drawImageUsages, drawImageExtent);

	//for the draw image, we want to allocate it from gpu local memory
	VmaAllocationCreateInfo rimg_allocinfo = {};
	rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	//allocate and create the image
	vmaCreateImage(m_memAllocator, &rimg_info, &rimg_allocinfo, &m_drawImage.image, &m_drawImage.allocation, nullptr);

	//build a image-view for the draw image to use for rendering
	VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(m_drawImage.imageFormat, m_drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);

	checkVkResult(vkCreateImageView(m_device, &rview_info, nullptr, &m_drawImage.imageView));

	//add to deletion queues
	m_mainDeletionQueue.push_function([=]() {
		vkDestroyImageView(m_device, m_drawImage.imageView, nullptr);
		vmaDestroyImage(m_memAllocator, m_drawImage.image, m_drawImage.allocation);
	});
}

void VulkanEngine::initCommands()
{
	//create a command pool for commands submitted to the graphics queue.
	//we also want the pool to allow for resetting of individual command buffers
	VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(m_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
	
	for (int i = 0; i < FRAME_OVERLAP; i++) {
		checkVkResult(vkCreateCommandPool(m_device, &commandPoolInfo, nullptr, &m_frames[i].commandPool));

		// allocate the default command buffer that we will use for rendering
		VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(m_frames[i].commandPool, 1);

		checkVkResult(vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &m_frames[i].mainCommandBuffer));
	}
}

void VulkanEngine::initSyncStructures()
{
	//create syncronization structures
	//one fence to control when the gpu has finished rendering the frame,
	//and 2 semaphores to syncronize rendering with swapchain
	//we want the fence to start signalled so we can wait on it on the first frame
	VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
	VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		checkVkResult(vkCreateFence(m_device, &fenceCreateInfo, nullptr, &m_frames[i].renderFence));

		checkVkResult(vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &m_frames[i].swapchainSemaphore));
		checkVkResult(vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &m_frames[i].renderSemaphore));
	}
}

void VulkanEngine::cleanup()
{	
	if (m_isInitialized) {
		//make sure the gpu has stopped doing its things
		vkDeviceWaitIdle(m_device);

		for (int i = 0; i < FRAME_OVERLAP; i++) {
			vkDestroyCommandPool(m_device, m_frames[i].commandPool, nullptr);

			//destroy sync objects
			vkDestroyFence(m_device, m_frames[i].renderFence, nullptr);
			vkDestroySemaphore(m_device, m_frames[i].renderSemaphore, nullptr);
			vkDestroySemaphore(m_device ,m_frames[i].swapchainSemaphore, nullptr);

			m_frames[i].deletionQueue.flush();
		}

		m_mainDeletionQueue.flush();

		destroySwapchain();

		vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
		vkDestroyDevice(m_device, nullptr);

		vkb::destroy_debug_utils_messenger(m_instance, m_debugMessenger);
		vkDestroyInstance(m_instance, nullptr);

		glfwDestroyWindow(m_window);
        glfwTerminate();
	}
}

void VulkanEngine::glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}