// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_loader.h>

struct DeletionQueue
{
	std::deque<std::function<void()>> deletors;

	void push_function(std::function<void()>&& function) {
		deletors.push_back(function);
	}

	void flush() {
		// reverse iterate the deletion queue to execute all the functions
		for (auto it = deletors.rbegin(); it != deletors.rend(); it++) {
			(*it)(); //call functors
		}

		deletors.clear();
	}
};


struct FrameData {
	VkCommandPool commandPool;
	VkCommandBuffer mainCommandBuffer;
	DescriptorAllocatorGrowable frameDescriptors;
	VkSemaphore swapchainSemaphore, renderSemaphore;
	VkFence renderFence;
	DeletionQueue deletionQueue;
};

constexpr unsigned int FRAME_OVERLAP = 2;

struct ComputePushConstants {
	glm::vec4 data1;
	glm::vec4 data2;
	glm::vec4 data3;
	glm::vec4 data4;
};

struct GPUSceneData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewproj;
    glm::vec4 ambientColor;
    glm::vec4 sunlightDirection; // w for sun power
    glm::vec4 sunlightColor;
};

struct ComputeEffect {
    const char* name;

	VkPipeline pipeline;
	VkPipelineLayout layout;

	ComputePushConstants data;
};


class VulkanEngine {
public:

	bool m_isInitialized{ false };
	int m_frameNumber {0};
	bool m_stop_rendering{ false };

	DeletionQueue m_mainDeletionQueue;

	VkExtent2D m_windowExtent{ 1700 , 900 };

	GLFWwindow* m_window = nullptr;
	bool m_resizeRequested;

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

	AllocatedImage m_drawImage;
	AllocatedImage m_depthImage;
	VkExtent2D m_drawExtent;
	float m_renderScale = 1.f;

	FrameData m_frames[FRAME_OVERLAP];
	FrameData& getCurrentFrame() { return m_frames[m_frameNumber % FRAME_OVERLAP]; };

	VkQueue m_graphicsQueue;
	uint32_t m_graphicsQueueFamily;

	VmaAllocator m_memAllocator;

	DescriptorAllocator globalDescriptorAllocator;

	VkDescriptorSet m_drawImageDescriptors;
	VkDescriptorSetLayout m_drawImageDescriptorLayout;

	//VkPipeline m_computePipeline; //unused for now, instead shaders are in m_backgroundEffects
	VkPipelineLayout m_computePipelineLayout;

	VkPipelineLayout m_meshPipelineLayout;
	VkPipeline m_meshPipeline;

	// scene
	GPUSceneData m_sceneData;
	VkDescriptorSetLayout m_gpuSceneDataDescriptorLayout;

	// meshes
	std::vector<std::shared_ptr<MeshAsset>> testMeshes;

	// immediate submit structures
    VkFence m_immFence;
    VkCommandBuffer m_immCommandBuffer;
    VkCommandPool m_immCommandPool;

	std::vector<ComputeEffect> m_backgroundEffects;
	int m_currentBackgroundEffect{0};

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	GPUMeshBuffers uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);

	//draw loop
	void draw();

	//submit immdediate mode functions to a command buffer
	void immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function);

	//run main loop
	void run();

private:
	void initGLFW();
	void initVulkan();
	void initSwapchain();
	void initCommands();
	void initSyncStructures();
	void initDescriptors();
	void initPipeline();
	void initComputePipelines();
	void initMeshPipeline();
	void initIMGUI();
	void initDefaultData();

	void createSwapchain(uint32_t width, uint32_t height);
	void destroySwapchain();
	void resizeSwapchain();

	AllocatedBuffer createBuffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
	void destroy_buffer(const AllocatedBuffer& buffer);


	void drawBackground(VkCommandBuffer cmd);
	void drawGeometry(VkCommandBuffer cmd);
	void drawImgui(VkCommandBuffer cmd, VkImageView targetImageView);

	static void glfw_error_callback(int error, const char* description);
};