// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>

struct FrameData {
	VkCommandPool commandPool;
	VkCommandBuffer mainCommandBuffer;
	VkSemaphore swapchainSemaphore, renderSemaphore;
	VkFence renderFence;
};

constexpr unsigned int FRAME_OVERLAP = 2;


class VulkanEngine {
public:

	bool m_isInitialized{ false };
	int m_frameNumber {0};

	VkExtent2D m_windowExtent{ 1700 , 900 };

	GLFWwindow* m_window = nullptr;

	VkInstance m_instance;
	VkAllocationCallbacks* m_Allocator = NULL;
	VkDebugUtilsMessengerEXT m_debugMessenger;
	VkPhysicalDevice m_chosenGPU;
	VkDevice m_device;
	VkSurfaceKHR m_surface;

	VkSwapchainKHR m_swapchain;
	VkFormat m_swapchainImageFormat;
	std::vector<VkImage> m_swapchainImages;
	std::vector<VkImageView> m_swapchainImageViews;
	VkExtent2D m_swapchainExtent;

	FrameData m_frames[FRAME_OVERLAP];
	FrameData& get_current_frame() { return m_frames[m_frameNumber % FRAME_OVERLAP]; };

	VkQueue m_graphicsQueue;
	uint32_t m_graphicsQueueFamily;

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
	void initVulkan();
	void initSwapchain();
	void initCommands();
	void initSyncStructures();

	void createSwapchain(uint32_t width, uint32_t height);
	void destroySwapchain();

	static void glfw_error_callback(int error, const char* description);
};