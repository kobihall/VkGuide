// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_display.h>
#include <vk_loader.h>
#include <camera.h>
#include <vk_tonemap.h>
#include <rt_job.h>
#include <rt_scene_editor.h>
#include <vk_gizmo.h>

#include <filesystem>
#include <map>

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

// the raster camera's vertical field of view. Shared so the cpu raytracer frames its render
// the same way the viewport it was aimed from does
constexpr float CAMERA_VERTICAL_FOV_DEGREES = 70.f;

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

struct RenderObject {
	uint32_t indexCount;
	uint32_t firstIndex;
	VkBuffer indexBuffer;
	
	MaterialInstance* material;
	Bounds bounds;
	glm::mat4 transform;
	VkDeviceAddress vertexBufferAddress;
};

struct DrawContext {
	std::vector<RenderObject> opaqueSurfaces;
	std::vector<RenderObject> transparentSurfaces;
};

struct MeshNode : public Node {
	std::shared_ptr<MeshAsset> mesh;
	virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx) override;
};

struct GLTFMetallic_Roughness {
	MaterialPipeline opaquePipeline;
	MaterialPipeline transparentPipeline;

	VkDescriptorSetLayout materialLayout;

	struct MaterialConstants {
		glm::vec4 colorFactors;
		glm::vec4 metalRoughFactors;
		//padding, we need it anyway for uniform buffers
		glm::vec4 extra[14];
	};

	struct MaterialResources {
		AllocatedImage colorImage;
		VkSampler colorSampler;
		AllocatedImage metalRoughImage;
		VkSampler metalRoughSampler;
		VkBuffer dataBuffer;
		uint32_t dataBufferOffset;
	};

	DescriptorWriter writer;

	void buildPipelines(VulkanEngine* engine);
	void clearResources(VkDevice device);

	MaterialInstance writeMaterial(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator);
};

struct EngineStats {
	float frameTime;
	int triangleCount;
	int drawcallCount;
	float sceneUpdateTime;
	float meshDrawTime;
};

bool is_visible(const RenderObject& obj, const glm::mat4& viewproj);


class VulkanEngine {
public:

	bool m_isInitialized{ false };
	int m_frameNumber {0};
	bool m_stop_rendering{ false };

	DeletionQueue m_mainDeletionQueue;

	VkExtent2D m_windowExtent{ 1700 , 900 };

	GLFWwindow* m_window = nullptr;
	bool m_resizeRequested{ false };
	double m_lastMouseX{ 0.0 };
	double m_lastMouseY{ 0.0 };
	bool m_firstMouse{ true };

	// while capture is active the camera owns the mouse and the keyboard, and nothing else sees them
	bool m_cameraCaptureActive{ false };
	double m_capturedCursorX{ 0.0 };
	double m_capturedCursorY{ 0.0 };

	VkInstance m_instance;
	VkAllocationCallbacks* m_allocator = nullptr;
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

	std::string m_rootPath;

	VkQueue m_graphicsQueue;
	uint32_t m_graphicsQueueFamily;

	VmaAllocator m_memAllocator;

	DescriptorAllocatorGrowable m_globalDescriptorAllocator;
	DisplayRegistry m_displayRegistry;

	VkDescriptorSet m_drawImageDescriptors;
	VkDescriptorSetLayout m_drawImageDescriptorLayout;

	//VkPipeline m_computePipeline; //unused for now, instead shaders are in m_backgroundEffects
	VkPipelineLayout m_computePipelineLayout;

	// linear rgba32f -> displayable rgba8, shared by the raytracer outputs
	TonemapPass m_tonemapPass;

	VkPipelineLayout m_meshPipelineLayout;
	VkPipeline m_meshPipeline;

	// scene
	Camera m_mainCamera;
	RaytraceSceneEditor m_raytraceScene;
	RaytraceJob m_raytraceJob;
	GPUSceneData m_sceneData;
	VkDescriptorSetLayout m_gpuSceneDataDescriptorLayout;
	DrawContext m_mainDrawContext;
	// the scene's glTF models, keyed by a unique name derived from the file stem. Ordered, so
	// the browser, the mesh data and a saved file list them stably. Each is placed as authored
	// (identity root transform); the spheres in m_raytraceScene and m_environmentMap are the
	// rest of the scene, and openScene()/saveScene() move all three together
	std::map<std::string, std::shared_ptr<LoadedGLTF>> m_models;

	// the models as the cpu raytracer sees them, rebuilt by rebuildSceneDerivedData() exactly
	// when m_models changes and shared immutably with every render snapshot - see RaytraceMeshData
	std::shared_ptr<const RaytraceMeshData> m_raytraceMeshData;
	// bumped whenever m_models changes, so anything that derives data from the models (a future
	// simulation plane's intersections, say) can compare against the revision it last acted on
	uint64_t m_sceneRevision { 0 };

	// hdr environment lighting, part of the scene. Nothing samples it yet - the compute
	// raytracer's miss branch is the planned consumer
	AllocatedImage m_environmentMap {};
	VkExtent2D m_environmentMapExtent { 0, 0 };
	std::filesystem::path m_environmentMapPath;

	// the file the current scene was opened from or last saved to; empty for an unsaved scene
	std::filesystem::path m_scenePath;
	// the outcome of the last File-menu action, shown in the Scene panel
	IoResult m_lastFileResult;

	// the one viewport gizmo, shared by every object type that offers "Edit Transform"
	TransformGizmo m_transformGizmo;

	// a unit sphere at the origin, drawn once per raytracer sphere with a per-object transform
	std::shared_ptr<MeshAsset> m_sphereMesh;
	// rebuilt every frame in drawRaytraceSpheres(); RenderObject holds raw pointers into it, so
	// it is sized once up front and not appended to while those pointers are being taken
	std::vector<MaterialInstance> m_sphereMaterials;

	// textures
	AllocatedImage m_whiteImage;
	AllocatedImage m_blackImage;
	AllocatedImage m_greyImage;
	AllocatedImage m_errorCheckerboardImage;

	VkSampler m_defaultSamplerLinear;
	VkSampler m_defaultSamplerNearest;
	VkDescriptorSetLayout m_singleImageDescriptorLayout;

	// materials
	MaterialInstance m_defaultData;
	GLTFMetallic_Roughness m_metalRoughMaterial;

	EngineStats m_stats;

	// immediate submit structures
	VkFence m_immFence;
	VkCommandBuffer m_immCommandBuffer;
	VkCommandPool m_immCommandPool;

	std::vector<ComputeEffect> m_backgroundEffects;
	int m_currentBackgroundEffect{0};

	// a File-menu click only records the action; the native dialog it needs runs a modal loop,
	// so runPendingFileAction() executes it once the imgui frame is finished
	enum class FileAction {
		None,
		NewScene,
		OpenScene,
		SaveScene,
		SaveSceneAs,
		ImportGltf,
		SetEnvironmentMap,
		ClearEnvironmentMap
	};
	FileAction m_pendingFileAction { FileAction::None };

	// toggled from the "Windows" menu
	bool m_showBackgroundWindow{ true };
	bool m_showStatsWindow{ true };
	bool m_showDemoWindow{ false };
	// the raytracer's two control panels own their own flags, since they own their own windows

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	GPUMeshBuffers uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);
	AllocatedBuffer createBuffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
	void destroyBuffer(const AllocatedBuffer& buffer);
	AllocatedImage createImage(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	AllocatedImage createImage(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	void destroyImage(const AllocatedImage& img);

	// the scene as a whole: models + environment map + spheres. Failures never abort and leave
	// what is loaded untouched; a partially readable scene reports what it could not load
	void newScene();
	IoResult openScene(const std::filesystem::path& path);
	IoResult saveScene(const std::filesystem::path& path);

	// the scene's parts. importGltf() adds a model (models are keyed uniquely, so the same file
	// can be imported twice); loadEnvironmentMap() takes a Radiance .hdr into m_environmentMap
	// (rgba16f), replacing any previous one
	IoResult importGltf(const std::filesystem::path& path);
	void removeGltf(const std::string& name);
	void clearModels();
	IoResult loadEnvironmentMap(const std::filesystem::path& path);
	void clearEnvironmentMap();

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

	void setCameraCapture(bool active);

	void drawFileMenu();
	void runPendingFileAction();
	void destroyEnvironmentMap();
	// m_raytraceMeshData and m_sceneRevision, after any change to m_models
	void rebuildSceneDerivedData();

	// the raster camera's projection in OpenGL clip convention (y up). updateScene() flips y for
	// vulkan; the gizmo wants it as-is
	glm::mat4 rasterProjection() const;

	void updateScene();
	void drawRaytraceSpheres();
	void drawBackground(VkCommandBuffer cmd);
	void drawGeometry(VkCommandBuffer cmd);
	void drawImgui(VkCommandBuffer cmd, VkImageView targetImageView);

	static void glfw_error_callback(int error, const char* description);
};