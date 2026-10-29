// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <command_line.h>
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
#include <gpu_selftest.h>
#include <mount_library.h>

#include <chrono>
#include <filesystem>
#include <optional>
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
	// the raster pass's per-instance world matrices (drawGeometry()), host-visible and kept mapped; grown when a
	// frame draws more instances than it holds
	AllocatedBuffer instanceBuffer {};
	size_t instanceCapacity { 0 };
	// the occlusion queries this frame slot issued, one per entry of MeshDrawList::opaque in occlusionEntries, read
	// back when the slot comes round again; occlusionGeneration is the MeshDrawList::generation they were issued for
	VkQueryPool occlusionQueries { VK_NULL_HANDLE };
	uint32_t occlusionCapacity { 0 };
	std::vector<uint32_t> occlusionEntries;
	uint64_t occlusionGeneration { 0 };
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

	bool operator==(const ComputePushConstants&) const = default;
};

// When the raster preview stops drawing a picture identical to the last one (VulkanEngine::draw()): whenever nothing it
// shows has changed, only while the raytracer renders, or never
enum class RasterPauseMode : uint8_t {
	Always,
	WhileRendering,
	Never,
};

// Everything the raster preview's picture depends on (VulkanEngine::rasterKey()). Equal keys draw equal pictures, so a
// frame whose key matches the last ones keeps the draw image as it is
struct RasterKey {
	glm::mat4 viewproj { 0.f };
	uint32_t width { 0 };
	uint32_t height { 0 };
	// VulkanEngine::m_drawImagesCreated: recreated on a resize, with undefined contents. A count, not the handle, which
	// MoltenVK can hand out again for a new image
	uint64_t drawImages { 0 };
	uint64_t editorRevision { 0 };
	uint64_t sceneRevision { 0 };
	int backgroundEffect { -1 };
	ComputePushConstants gradient {};
	ComputePushConstants sky {};
	// VulkanEngine::m_environmentMapsLoaded
	uint64_t environmentMaps { 0 };
	float environmentExposure { 0.f };
	bool occlusionCulling { false };

	bool operator==(const RasterKey&) const = default;
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
	VkBuffer vertexBuffer;
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

// The scene's mesh objects as the raster pass draws them (src/raster_draw_list.cpp, compiled -O2 in every
// configuration, since a pbrt scene places 160,000 objects). Built only when the scene or the models change, not every
// frame: every visible object's surfaces in draw order (by material, then index buffer), each opaque one with its box
// in world space, which cull() tests against the view frustum each frame. Raw pointers into the models, valid as long
// as VulkanEngine::m_meshNodes is
struct MeshDrawList {
	std::vector<RenderObject> opaque;
	std::vector<std::pair<glm::vec3, glm::vec3>> boxes;
	// per opaque surface: tested for occlusion (OCCLUSION_MIN_TRIANGLES, OCCLUSION_MAX_QUERIES), and its box passed no
	// samples in the last occlusion query read back (VulkanEngine::drawGeometry())
	std::vector<uint8_t> queried;
	std::vector<uint8_t> occluded;
	std::vector<RenderObject> transparent;
	// moves on every rebuild, so occlusion results for an older list are dropped rather than applied to the wrong entries
	uint64_t generation { 0 };
	// what it was built from: RaytraceSceneEditor::contentRevision() and VulkanEngine::m_sceneRevision
	uint64_t editorRevision { ~0ull };
	uint64_t sceneRevision { ~0ull };

	void rebuild(const std::vector<SceneMeshObject>& objects, const std::map<std::pair<std::string, uint32_t>, const MeshNode*>& nodes);
	// a surface of at least this many triangles is tested for occlusion; below it, the query's draw costs about as much
	// as the surface's own
	static constexpr uint32_t OCCLUSION_MIN_TRIANGLES = 4096;
	// at most this many surfaces are tested, those of the most triangles: each query is a draw of its own, and Metal's
	// visibility buffer holds 32,768 (pbrt's landscape has 90,000 surfaces over the minimum)
	static constexpr uint32_t OCCLUSION_MAX_QUERIES = 4096;
	struct CullResult {
		uint32_t occluded { 0 };
		uint64_t occludedTriangles { 0 };
	};
	// the opaque surfaces whose box is inside the frustum of `viewproj`, appended in draw order to `out`, less those
	// `occluded` marks when `skipOccluded`. `queries` gets the entries to test for occlusion this frame: those in view
	// that `queried` marks, unless their box holds the eye
	CullResult cull(const glm::mat4& viewproj, const glm::vec3& eye, bool skipOccluded, std::vector<const RenderObject*>& out, std::vector<uint32_t>& queries) const;
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
	// surfaces in view the occlusion queries skipped this frame, and their triangles
	int occludedCount;
	uint64_t occludedTriangles;
	float sceneUpdateTime;
	float meshDrawTime;
};

bool is_visible(const RenderObject& obj, const glm::mat4& viewproj);


class VulkanEngine {
public:

	bool m_isInitialized{ false };
	int m_frameNumber {0};
	bool m_stop_rendering{ false };
	// VKGUIDE_WINDOW=minimized: the window starts minimized and the loop renders anyway, where it
	// would otherwise pause while the window is hidden
	bool m_renderWhenHidden{ false };

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
	// the largest single allocation the device allows (maxMemoryAllocationSize) and its largest device-local heap: a
	// render whose geometry needs more is refused (RaytraceRenderer::startRender())
	VkDeviceSize m_maxAllocationBytes { 0 };
	VkDeviceSize m_deviceLocalBytes { 0 };
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
	// the shader-against-mirror checks of docs/plans/completed/pbrt-verification-harness.md: the
	// Windows -> GPU self-tests window, and VKGUIDE_SELFTEST=1 for a run from a script
	GpuSelfTestPanel m_selfTests;
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
	std::map<std::string, std::shared_ptr<LoadedModel>> m_models;
	// the optomechanical models (mount_library.h): fixed resources loaded at startup, outside m_models, so
	// they are never saved with a scene or listed under Models
	MountLibrary m_mountLibrary;

	// the models as the raytracer sees them, rebuilt by rebuildSceneDerivedData() exactly when
	// m_models changes and shared immutably with every render snapshot - see RaytraceMeshData
	std::shared_ptr<const RaytraceMeshData> m_raytraceMeshData;
	// every loaded model's mesh nodes by (model key, node index), so a SceneMeshObject resolves to
	// the geometry it places without re-walking every node tree each frame. Raw pointers into
	// m_models, so this is rebuilt by the same path, and only by it
	std::map<std::pair<std::string, uint32_t>, const MeshNode*> m_meshNodes;
	// the mesh objects' surfaces as the raster pass draws them, rebuilt in updateScene() only when the editor's content
	// revision or m_sceneRevision moves
	MeshDrawList m_meshDrawList;
	// skip the surfaces whose box was hidden at the last occlusion query (drawGeometry()); the Stats window's checkbox
	bool m_occlusionCulling { true };
	// the Stats window's "Pause raster preview": draw() keeps the last picture while rasterKey() holds still
	RasterPauseMode m_rasterPauseMode { RasterPauseMode::Always };
	RasterKey m_lastRasterKey {};
	// frames in a row whose key matched the one before
	uint32_t m_rasterStableFrames { 0 };
	bool m_rasterPaused { false };
	// how many times createDrawImages() and the environment map upload have run, for rasterKey()
	uint64_t m_drawImagesCreated { 0 };
	uint64_t m_environmentMapsLoaded { 0 };
	// draws a world-space box with no color or depth writes, for the occlusion queries (shaders/occlusion_box.vert)
	MaterialPipeline m_occlusionBoxPipeline {};
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
	// the model whose environment m_environmentMap is (a pbrt file's infinite light), which removing the model
	// removes; empty for a map from a file or none
	std::string m_environmentModelKey;
	// the camera each pbrt model's import added, which "Use pbrt settings" renders from
	std::map<std::string, uint64_t> m_modelCameras;
	// the import report window (Windows -> Import Report): which model it shows
	bool m_showImportReport { false };
	std::string m_importReportModel;
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
		ImportPbrt,
		SetEnvironmentMap,
		ClearEnvironmentMap,
		SaveReference,
		LoadReference,
		ExportRender
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
	// a 2D array image of `layers` layers, viewed as one array (an image2DArray in a shader)
	AllocatedImage createImageArray(VkExtent3D size, uint32_t layers, VkFormat format, VkImageUsageFlags usage);
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

	// the scene's parts. importModel() adds a model, a glTF file or a pbrt-v4 scene (.pbrt, .pbrt.gz) by its name
	// (models are keyed uniquely, so the same file can be imported twice); loadEnvironmentMap() takes a Radiance
	// .hdr or an OpenEXR file into m_environmentMap (rgba16f), replacing any previous one
	IoResult importModel(const std::filesystem::path& path);
	IoResult importGltf(const std::filesystem::path& path);
	// a pbrt-v4 scene (docs/plans/completed/pbrt-scene-import.md): its meshes, materials, lights, shapes, camera and
	// environment, with the import report shown in its window
	IoResult importPbrt(const std::filesystem::path& path);
	// applies what a pbrt model's file asks a render to be (resolution, samples, sampler, filter, depth, clamp) and
	// renders from its camera: "Use pbrt settings" in the import report, and the batch mode for a .pbrt file
	void usePbrtSettings(const std::string& modelKey);
	void removeModel(const std::string& name);
	void clearModels();
	IoResult loadEnvironmentMap(const std::filesystem::path& path);
	void clearEnvironmentMap();

	//draw loop
	void draw();

	//submit immdediate mode functions to a command buffer
	void immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function);

	//run main loop
	void run();
	// the arguments main() parsed, before init(): --render asks for the batch mode
	void setCommandLine(CommandLine commandLine) { m_commandLine = std::move(commandLine); }
	//what main() returns: nonzero when VKGUIDE_SELFTEST=1 ran and a self-test failed
	int exitCode() const { return m_exitCode; }

private:
	int m_exitCode { 0 };

	// THE BATCH MODE (--render, src/command_line.h): opens the scene, renders it to its sample target with a fixed
	// seed, writes the result and quits. For the verification scripts (tools/verify/)
	struct BatchRun {
		std::filesystem::path output;
		uint32_t samples { 1 };
		enum class Phase { Start, Rendering, Settling } phase { Phase::Start };
		uint64_t settleFrame { 0 };
		double timeoutSeconds { 1800.0 };
		std::chrono::steady_clock::time_point started;
	};
	std::optional<BatchRun> m_batch;
	CommandLine m_commandLine;
	// opens the scene the command line names and applies its options; false (with m_exitCode set) when it cannot run
	bool beginBatch();
	// once per frame while a batch run is active
	void stepBatch();
	void initGLFW();
	void initVulkan();
	void initSwapchain();
	void initCommands();
	void initSyncStructures();
	void initDescriptors();
	void initPipeline();
	void initComputePipelines();
	void initMeshPipeline();
	void initOcclusionPipeline();
	RasterKey rasterKey() const;
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
	// what every import does once its file is a model: key it, rebuild what derives from the models, add its
	// materials to the library and place its nodes. Returns the key
	std::string addModel(std::shared_ptr<LoadedModel> model, const std::filesystem::path& path);
	// float rgba pixels as the environment map; `path` is what a saved scene records, empty for a map a model brought
	IoResult installEnvironmentMap(const float* pixels, uint32_t width, uint32_t height, const std::string& label, const std::filesystem::path& path);
	void drawImportReport();
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