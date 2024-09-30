// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>

class VulkanEngine {
public:

	bool m_isInitialized{ false };
	int m_frameNumber {0};

	VkExtent2D m_windowExtent{ 1700 , 900 };

	GLFWwindow* m_window = nullptr;

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	//draw loop
	void draw();

	//run main loop
	void run();

private:
	void initGLFW();
	static void glfw_error_callback(int error, const char* description);
};