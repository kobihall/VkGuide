#include "vk_engine.h"

#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // abort
#include <iostream>
#include <vector>

#include <vk_types.h>
#include <vk_initializers.h>

void VulkanEngine::init()
{
    initGLFW();
	
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

void VulkanEngine::cleanup()
{	
	if (m_isInitialized) {
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