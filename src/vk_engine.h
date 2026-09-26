// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_display.h>
#include <vk_loader.h>
#include <camera.h>
#include <vk_compute.h>
#include <vk_tonemap.h>
#include <rt_renderer.h>
#include <rt_scene_editor.h>
#include <rt_textures.h>
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

// the push-constant block shared by gradient_color.comp and sky.comp: four raw vec4s each
// shader reads what it likes from
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

// one selectable compute effect that fills the draw image before the raster pass. Effects bind
// different resources and take differently shaped push constants, so each carries the two
// pieces of code that know its shape: how to record it and how to edit its settings
struct BackgroundEffect {
	const char* name;
	ComputePass pass;
	// allocates this frame's descriptor set, fills the push constants and dispatches, with the
	// draw image in VK_IMAGE_LAYOUT_GENERAL
	std::function<void(VkCommandBuffer cmd, const ComputePass& pass)> record;
	// the effect's imgui controls, inside the "background" window
	std::function<void()> drawSettings;
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

	// this node's own surfaces at an explicit world transform, without recursing into its
	// children: each mesh-bearing child is its own scene object carrying its own transform, so
	// the scene's mesh objects draw through this rather than through Draw()
	void drawSurfaces(const glm::mat4& worldMatrix, DrawContext& ctx) const;

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
	// limits worth checking new work against - maxPushConstantsSize, maxComputeWorkGroup* -
	// printed once at startup
	VkPhysicalDeviceProperties m_gpuProperties {};
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

	// linear rgba32f -> displayable rgba8, shared by the raytracer outputs
	TonemapPass m_tonemapPass;

	VkPipelineLayout m_meshPipelineLayout;
	VkPipeline m_meshPipeline;

	// scene
	Camera m_mainCamera;
	RaytraceSceneEditor m_raytraceScene;
	// the raytracer: its panel, settings, the GPU path tracer and the output window
	RaytraceRenderer m_raytracer;
	// the scene camera the viewport is currently flying (first-person edit): every frame the
	// viewport's pose is written into it. 0 = none, the viewport is a free camera
	uint64_t m_firstPersonCameraId { 0 };
	GPUSceneData m_sceneData;
	VkDescriptorSetLayout m_gpuSceneDataDescriptorLayout;
	DrawContext m_mainDrawContext;
	// the scene's glTF models, keyed by a unique name derived from the file stem. Ordered, so
	// the browser, the mesh data and a saved file list them stably. Each is placed as authored
	// (identity root transform); the shapes in m_raytraceScene and m_environmentMap are the
	// rest of the scene, and openScene()/saveScene() move all three together
	std::map<std::string, std::shared_ptr<LoadedGLTF>> m_models;

	// the models as the raytracer sees them, rebuilt by rebuildSceneDerivedData() exactly when
	// m_models changes and shared immutably with every render snapshot - see RaytraceMeshData
	std::shared_ptr<const RaytraceMeshData> m_raytraceMeshData;
	// every loaded model's mesh nodes by (model key, node index), so a SceneMeshObject resolves to
	// the geometry it places without re-walking every node tree each frame. Raw pointers into
	// m_models, so this is rebuilt by the same path, and only by it
	std::map<std::pair<std::string, uint32_t>, const MeshNode*> m_meshNodes;
	// every model's base-colour textures in one array image, for the path tracer's triangles.
	// Rebuilt alongside the mesh data, so the layers a triangle's material names always exist
	RaytraceTextureArray m_raytraceTextures;
	// bumped whenever m_models changes, so anything that derives data from the models (a future
	// simulation plane's intersections, say) can compare against the revision it last acted on
	uint64_t m_sceneRevision { 0 };

	// hdr environment lighting, part of the scene: the "environment" background effect shows it
	// behind the models and the GPU path tracer lights the render with it on a miss
	AllocatedImage m_environmentMap {};
	VkExtent2D m_environmentMapExtent { 0, 0 };
	std::filesystem::path m_environmentMapPath;
	// the power of two m_environmentMap's texels are divided by, so that a peak brighter than half
	// float's 65504 stays finite (environmentStorageScale()); 1 for most maps. Every shader that
	// reads the map multiplies it back in with the intensity. The "Environment Map" window shows
	// the stored values, darker by this factor
	float m_environmentMapScale { 1.f };
	// the map's sampling tables for the path tracer's light sampling (light_sampling.h), built from
	// the decoded pixels when the map loads and shared immutably with every render that samples it
	std::shared_ptr<const EnvironmentDistribution> m_environmentDistribution;
	// scales the map as a light source, so the viewport and the render agree on how bright the
	// sky is. A scene property, saved with the scene; the picture's brightness is the
	// renderer's exposure setting
	float m_environmentIntensity { 1.f };
	// a solid colour a ray that leaves the scene sees in the path tracer, replacing the environment
	// map or the sky gradient - the black void of a scene lit only by its own lights. A scene
	// property, saved with the scene
	bool m_solidBackground { false };
	glm::vec3 m_backgroundColor { 0.f };

	// the file the current scene was opened from or last saved to; empty for an unsaved scene
	std::filesystem::path m_scenePath;
	// the outcome of the last File-menu action, shown in the Scene panel
	IoResult m_lastFileResult;

	// the one viewport gizmo, shared by every object type that offers "Edit Transform"
	TransformGizmo m_transformGizmo;

	// by ShapeKind: the unit primitive's preview mesh (shape_mesh.h), drawn once per raytracer
	// shape of that kind under the shape's own objectToWorld()
	std::array<std::shared_ptr<MeshAsset>, SHAPE_KIND_COUNT> m_shapeMeshes;
	// the preview shapes' material data, one persistent slot per frame in flight, rewritten
	// only when the editor's revision has moved on since the slot was last written. The
	// buffer and the descriptor sets belong to the slot's frames alone, which are provably
	// finished when the slot comes round again, so a rewrite never races the GPU
	struct PreviewShapeSlot {
		AllocatedBuffer materialBuffer {};
		// shapes the buffer has room for
		size_t capacity { 0 };
		DescriptorAllocatorGrowable descriptors;
		// RenderObject holds raw pointers into it, so it is never appended to while in use
		std::vector<MaterialInstance> materials;
		// the editor revision the slot was written at; 0 = never
		uint64_t revision { 0 };
	};
	PreviewShapeSlot m_previewShapeSlots[FRAME_OVERLAP];

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

	std::vector<BackgroundEffect> m_backgroundEffects;
	int m_currentBackgroundEffect{0};
	// each effect's editable settings, owned here so the effect closures can capture `this`
	// rather than a pointer into the vector above
	ComputePushConstants m_gradientParams {};
	ComputePushConstants m_skyParams {};

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

	// the scene as a whole: models + environment map + shapes. Failures never abort and leave
	// what is loaded untouched; a partially readable scene reports what it could not load
	void newScene();
	IoResult openScene(const std::filesystem::path& path);
	IoResult saveScene(const std::filesystem::path& path);

	// the raster camera's projection in OpenGL clip convention (y up). updateScene() flips y for
	// vulkan; the gizmo wants it as-is. Uses the flown camera's fov during a first-person edit
	glm::mat4 rasterProjection() const;
	float rasterVerticalFov() const;

	// the viewport's pose as a scene camera - what "Add Camera" creates
	SceneCamera cameraFromViewport() const;
	// first-person edit: the viewport takes the camera's pose and fov, and drives the camera
	// until endFirstPersonEdit(); deleting the camera ends it too
	void beginFirstPersonEdit(uint64_t cameraId);
	void endFirstPersonEdit();

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
	// the draw and depth images, at the window's size in points; recreated on resize
	void createDrawImages(VkExtent2D extent);
	void destroyDrawImages();

	void setCameraCapture(bool active);

	void drawFileMenu();
	void runPendingFileAction();
	void destroyEnvironmentMap();
	// m_raytraceMeshData and m_sceneRevision, after any change to m_models
	void rebuildSceneDerivedData();


	// the free camera's movement and, in a first-person edit, its pose into the scene camera.
	// At the top of the frame, before anything reads the camera - the gizmo, the panels, the
	// raster pass - so they all see the same pose
	void updateViewportCamera();
	// wireframe frustums for the scene's cameras, drawn over the viewport through imgui's
	// background draw list; the render camera brighter, the flown one not at all
	void drawCameraOverlays();
	// markers for the punctual lights over the viewport, the same way: a point light's star, a spot
	// light's cone, a directional light's arrow
	void drawLightOverlays();

	// CPU-only scene preparation: camera, the models' draw list, the scene uniform's values.
	// Touches no GPU resource, so it runs before the frame's fence wait and overlaps the GPU
	// finishing the previous frame
	void updateScene();
	// appends the editor's shapes to the draw list. Writes this frame slot's material buffer
	// when the shapes changed, so it must run after the slot's fence wait
	void drawRaytraceShapes();
	void drawBackground(VkCommandBuffer cmd);
	// the shared shape of gradient_color.comp and sky.comp: the draw image at binding 0 and a
	// ComputePushConstants block, dispatched over the whole draw image
	void recordDrawImageEffect(VkCommandBuffer cmd, const ComputePass& pass, const ComputePushConstants& params);
	void drawGeometry(VkCommandBuffer cmd);
	void drawImgui(VkCommandBuffer cmd, VkImageView targetImageView);

	static void glfw_error_callback(int error, const char* description);
};