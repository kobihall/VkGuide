#include "vk_engine.h"

#include <algorithm>
#include <image_io.h>
#include <rt_scene.h>
#include <pbrt_import.h>
#include <scene_io.h>
#include <shape_mesh.h>
#include <file_dialog.h>
#include <vk_ui.h>
#include <optics_ui.h>
#include <detectors_ui.h>
#include <optic_detectors.h>
#include <optic_laser.h>
#include <optic_mesh.h>

#include <fstream>
#include <sstream>

#include <fmt/ranges.h>

#include <stb_image.h>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <stdio.h>			// printf, fprintf
#include <stdlib.h>			// abort, getenv
#include <string.h>			// strcmp
#include <iostream>
#include <vector>
#include <thread>

#include <vk_types.h>
#include <vk_initializers.h>
#include <vk_images.h>
#include <vk_pipelines.h>

#include <VkBootstrap.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <imgui.h>
#include <implot.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>

constexpr bool b_UseValidationLayers = false;

constexpr const char* VIEWPORT_TEXTURE_NAME = "Viewport";

namespace {
// defined with the scene file helpers below
std::filesystem::path absoluteNormalized(const std::filesystem::path& path);
}

void VulkanEngine::init()
{
	m_rootPath = "../";
	fmt::println("loaded root path as: {}", m_rootPath);

	initGLFW();
	initVulkan();
	initSwapchain();
	initCommands();
	initSyncStructures();
	initDescriptors();
	initPipeline();
	initIMGUI();
	initDefaultData();
	m_mountLibrary.open(this, m_rootPath + "assets/optical_models/meshes");
	{
		std::vector<std::string> warnings;
		m_opticalLibrary.loadBundled(m_rootPath, warnings);
		for (const std::string& warning : warnings) {
			fmt::println("optical library: {}", warning);
		}
		setOpticsUiLibrary(&m_opticalLibrary);
	}

	//before the tracer: its descriptor sets always name the array's view, so it must exist first
	m_raytraceTextures.init(this);
	m_textureLibrary.init(this);
	//what the light panel's photometric units read of a light's image
	m_raytraceScene.lightImageSource = [this](const SceneLight& light) {
		if ((light.kind != LightKind::Projection && light.kind != LightKind::Goniometric) || light.image.empty()) {
			return LightImage {};
		}
		const LightImageRequest request = lightImageRequest(light.image);
		const TextureEntry* entry = m_textureLibrary.find(request.source, request.colorSpace, request.sampling);
		if (entry == nullptr || entry->luminance.empty()) {
			return LightImage {};
		}
		return lightImageMeasure(light.kind == LightKind::Projection, light.fovDegrees, entry->luminanceSize.x, entry->luminanceSize.y, entry->luminance.data());
	};
	m_raytracer.init(this);

	//the raster preview reaches the screen through the Viewport window, which samples the draw image
	m_displayRegistry.registerTexture(VIEWPORT_TEXTURE_NAME, m_drawImage.imageView, { m_drawImage.imageExtent.width, m_drawImage.imageExtent.height });
	registerWindows();

	//the startup scene is a file like any other, opened through the same path File > Open Scene
	//uses - nothing about it is hardcoded, so editing and saving it changes what the app starts
	//with. A missing or unreadable file is reported rather than fatal: the app is perfectly
	//usable with the empty scene openScene() leaves behind
	m_lastFileResult = openScene(m_rootPath + "assets/scenes/sphere_scene.gltf");
	if (!m_lastFileResult.ok) {
		fmt::println("{}", m_lastFileResult.message);
	}

	//everything went fine
	m_isInitialized = true;
}

void VulkanEngine::draw()
{
	// wait until the gpu has finished rendering the last frame (below)
	// cpu-only, so it overlaps the gpu finishing the previous frame
	updateScene();

	//30 seconds, not vkguide's 1: a frame of a scene of billions of triangles (pbrt's landscape) takes 2.5 s on the GPU, and
	//a timeout here aborts. A hung GPU still ends in an error, from the timeout or from macOS resetting the device
	checkVkResult(vkWaitForFences(m_device, 1, &getCurrentFrame().renderFence, true, 30000000000ull));

	getCurrentFrame().deletionQueue.flush();
	getCurrentFrame().frameDescriptors.clearPools(m_device);

	//the raster pass this slot timed when it last ran, FRAME_OVERLAP frames ago; a gap in the plot when it drew nothing
	m_stats.rasterGpuTime = NAN;
	if (getCurrentFrame().rasterTimed) {
		getCurrentFrame().rasterTimed = false;
		//value, availability pairs for the two queries
		uint64_t results[4] = {};
		const VkResult got = vkGetQueryPoolResults(m_device, getCurrentFrame().rasterTimestamps, 0, 2, sizeof(results), results, sizeof(uint64_t) * 2,
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		if (got == VK_SUCCESS && results[1] != 0 && results[3] != 0) {
			m_stats.rasterGpuTime = (float)(double(results[2] - results[0]) * m_timestampPeriodNs / 1.0e6);
		}
	}

	// after the fence wait: this may rewrite the frame slot's persistent shape-material buffer
	drawRaytraceShapes();
	//what the scene's capabilities draw beside its own objects: the bench's breadboard, posts and mounts
	forEachEnabledCapability(m_capabilities, [this](const CapabilityInfo& capability) {
		if (capability.rasterGeometry != nullptr) {
			capability.rasterGeometry(this);
		}
	});

	checkVkResult(vkResetFences(m_device, 1, &getCurrentFrame().renderFence));

	//request image from the swapchain
	uint32_t swapchainImageIndex;
	VkResult acquireImageResult = vkAcquireNextImageKHR(m_device, m_swapchain, 1000000000, getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex);
	if (acquireImageResult == VK_ERROR_OUT_OF_DATE_KHR) {
		m_resizeRequested = true;		
		return ;
	} else if (acquireImageResult != VK_SUCCESS && acquireImageResult != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("failed to acquire swap chain image!");
	}

	//naming it cmd for shorter writing
	VkCommandBuffer cmd = getCurrentFrame().mainCommandBuffer;

	// now that we are sure that the commands finished executing, we can safely
	// reset the command buffer to begin recording again.
	checkVkResult(vkResetCommandBuffer(cmd, 0));

	//begin the command buffer recording. We will use this command buffer exactly once, so we want to let vulkan know that
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	//the Viewport window chose this frame's extent; a resize since may have shrunk the image under it
	m_drawExtent.width = std::clamp(m_drawExtent.width, 1u, m_drawImage.imageExtent.width);
	m_drawExtent.height = std::clamp(m_drawExtent.height, 1u, m_drawImage.imageExtent.height);

	//the raster preview pauses, keeping last frame's picture in the draw image, once nothing it shows has changed for
	//FRAME_OVERLAP + 1 frames: a frame skips the surfaces its occlusion queries of FRAME_OVERLAP frames before found
	//hidden, so only after that many still frames is the last picture drawn from queries made at the same view
	const RasterKey rasterNow = rasterKey();
	m_rasterStableFrames = rasterNow == m_lastRasterKey ? m_rasterStableFrames + 1 : 0;
	m_lastRasterKey = rasterNow;
	const bool pauseAllowed = m_rasterPauseMode == RasterPauseMode::Always
		|| (m_rasterPauseMode == RasterPauseMode::WhileRendering && m_raytracer.renderRunning());
	m_rasterPaused = pauseAllowed && m_rasterStableFrames > FRAME_OVERLAP;
	//a preview nobody sees is not drawn; when it comes back on screen it is drawn afresh before it may pause again
	const bool rasterSkipped = !m_viewportVisible;
	if (rasterSkipped) {
		m_rasterStableFrames = 0;
	}

	//start the command buffer recording
	checkVkResult(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

	// this frame's GPU path tracing, first: it only touches its own buffers and the raytracer's
	// display image, which it leaves readable for drawImgui()
	m_raytracer.record(cmd, this, m_raytraceScene);

	if (rasterSkipped || m_rasterPaused) {
		//the draw image keeps last frame's picture in SHADER_READ_ONLY_OPTIMAL (below), which the Viewport window shows
		m_stats.meshDrawTime = 0.f;
	} else {
		const VkQueryPool timestamps = getCurrentFrame().rasterTimestamps;
		if (timestamps != VK_NULL_HANDLE) {
			vkCmdResetQueryPool(cmd, timestamps, 0, 2);
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
		}

		// transition our main draw image into general layout so we can write into it
		// we will overwrite it all so we don't care about what was the older layout
		vkutil::transition_image(cmd, m_drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		//also set depth image layout. ideally only once, but we do every frame for now
		vkutil::transition_image(cmd, m_depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

		// flash the background color
		drawBackground(cmd);

		vkutil::transition_image(cmd, m_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		drawGeometry(cmd);

		//the draw image is linear; the Viewport shows it sRGB-encoded, as the swapchain is UNORM. Only here, after a raster
		//pass drew it: a paused or skipped frame keeps last frame's encoded picture
		vkutil::transition_image(cmd, m_drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		//the solved beams drawn by drawGeometry(), composited over the linear picture before it is encoded
		if (m_beamPreviewDrawn) {
			const VkDescriptorSet set = getCurrentFrame().frameDescriptors.allocate(m_device, m_beamPreviewComposite.setLayout);
			DescriptorWriter writer;
			writer.writeImage(0, m_drawImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
			writer.writeImage(1, m_beamPreviewAccum.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
			writer.writeImage(2, m_beamPreviewReveal.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
			writer.updateSet(m_device, set);
			dispatchComputePassOver(cmd, m_beamPreviewComposite, set, nullptr, m_drawImage.imageExtent);
			vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		}
		recordDrawImageEffect(cmd, m_viewportEncodePass, ComputePushConstants {});

		// the Viewport window samples the draw image, so it has to be readable before drawImgui() below. Every image
		// registered with m_displayRegistry needs this same transition
		vkutil::transition_image(cmd, m_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		if (timestamps != VK_NULL_HANDLE) {
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1);
			getCurrentFrame().rasterTimed = true;
		}
	}

	//the ui covers the whole swapchain image and clears it first, so its old contents do not matter
	vkutil::transition_image(cmd, m_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	//draw imgui into the swapchain image
	drawImgui(cmd, m_swapchainImageViews[swapchainImageIndex]);

	// set swapchain image layout to Present so we can show it on the screen
	vkutil::transition_image(cmd, m_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	//finalize the command buffer (we can no longer add commands, but it can now be executed)
	checkVkResult(vkEndCommandBuffer(cmd));

	//prepare the submission to the queue. 
	//we want to wait on the m_presentSemaphore, as that semaphore is signaled when the swapchain is ready
	//we will signal the m_renderSemaphore, to signal that rendering has finished

	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);

	VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,getCurrentFrame().swapchainSemaphore);
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, getCurrentFrame().renderSemaphore);	
	
	VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo,&signalInfo,&waitInfo); 

	//submit command buffer to the queue and execute it.
	// renderFence will now block until the graphic commands finish execution
	checkVkResult(vkinit::VkFunctionLoader::get_instance().vkQueueSubmit2KHR(m_graphicsQueue, 1, &submit, getCurrentFrame().renderFence));

	//prepare present
	// this will put the image we just rendered to into the visible window.
	// we want to wait on the _renderSemaphore for that, 
	// as its necessary that drawing commands have finished before the image is displayed to the user
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pNext = nullptr;
	presentInfo.pSwapchains = &m_swapchain;
	presentInfo.swapchainCount = 1;

	presentInfo.pWaitSemaphores = &getCurrentFrame().renderSemaphore;
	presentInfo.waitSemaphoreCount = 1;

	presentInfo.pImageIndices = &swapchainImageIndex;

	VkResult presentResult = vkQueuePresentKHR(m_graphicsQueue, &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
		m_resizeRequested = true; 
	} else if (presentResult != VK_SUCCESS) {
		throw std::runtime_error("failed to present swap chain image!");
	}

	//increase the number of frames drawn
	m_frameNumber++;
}

void VulkanEngine::updateScene()
{
	//begin clock
	auto start = std::chrono::system_clock::now();

	//both lists, every frame: a frame whose raster pass is paused never reaches drawGeometry(), which used to be the only
	//place the transparent list was cleared, and an optic's glass preview drawn into it then piled up across frames and
	//bound descriptor sets from a frame slot whose pool had since been reset
	m_mainDrawContext.opaqueSurfaces.clear();
	m_mainDrawContext.transparentSurfaces.clear();

	//the mesh objects' surfaces change only when the scene or the models do, so the list is rebuilt then and not
	//every frame: a pbrt scene places tens of thousands of objects
	if (m_meshDrawList.editorRevision != m_raytraceScene.contentRevision() || m_meshDrawList.sceneRevision != m_sceneRevision) {
		m_meshDrawList.rebuild(m_raytraceScene.meshObjects(), m_meshNodes);
		m_meshDrawList.editorRevision = m_raytraceScene.contentRevision();
		m_meshDrawList.sceneRevision = m_sceneRevision;
	}

	glm::mat4 view = m_viewportNav.camera.getViewMatrix();
	//reverse-Z (rasterDepthProjection()); vulkan's clip space has y down, so the raster path flips it
	glm::mat4 projection = rasterDepthProjection();
	projection[1][1] *= -1;

	m_sceneData.view = view;
	m_sceneData.proj = projection;
	m_sceneData.viewproj = projection * view;

	//some default lighting parameters
	m_sceneData.ambientColor = glm::vec4(.1f);
	m_sceneData.sunlightColor = glm::vec4(1.f);
	m_sceneData.sunlightDirection = glm::vec4(0,1,0.5,1.f);
	
	//get clock again, compare with start clock
	auto end = std::chrono::system_clock::now();
	
	//convert to microseconds (integer), and then come back to miliseconds
	auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
	m_stats.sceneUpdateTime = elapsed.count() / 1000.f;
}

RasterKey VulkanEngine::rasterKey() const
{
	RasterKey key;
	key.viewproj = m_sceneData.viewproj;
	key.width = m_drawExtent.width;
	key.height = m_drawExtent.height;
	key.drawImages = m_drawImagesCreated;
	//every edit, cameras included (a camera edit can move the viewport through First-Person Edit), and every model change
	key.editorRevision = m_raytraceScene.revision();
	key.sceneRevision = m_sceneRevision;
	key.background = m_backgroundMode;
	key.backgroundColor = m_backgroundColor;
	key.environmentMaps = m_environmentMapsLoaded;
	key.environmentExposure = m_environmentIntensity * m_environmentMapScale;
	key.environmentRotation = m_environmentRotation;
	key.occlusionCulling = m_occlusionCulling;
	key.benchAssets = m_benchScene.revision();
	key.beamSections = m_beamSectionsRevision;
	return key;
}

namespace {

// the "environment" pass's push constants; must match env_background.comp
struct EnvironmentBackgroundPushConstants {
	glm::mat4 inverseViewProj;
	glm::vec2 drawExtent;
	float exposure;
	// 1 for the sky, 0 for the map
	uint32_t sky;
	// the map's rotation about +y, its cosine and sine (environmentRotation())
	glm::vec2 rotation;
};

// the editor's gradient, top and bottom, as display colors (gradient_color.comp decodes them)
constexpr glm::vec4 GRADIENT_TOP { 0.325f, 0.471f, 0.584f, 1.f };
constexpr glm::vec4 GRADIENT_BOTTOM { 0.035f, 0.125f, 0.247f, 1.f };

}

void VulkanEngine::drawBackground(VkCommandBuffer cmd)
{
	//the scene's background as the raster preview shows it: an environment choice with no map shows the gradient, as
	//the render then sees black
	BackgroundMode mode = m_backgroundMode;
	if (mode == BackgroundMode::Environment && m_environmentMap.image == VK_NULL_HANDLE) {
		mode = BackgroundMode::Gradient;
	}

	if (mode == BackgroundMode::Solid) {
		//the draw image is linear, as the color is
		VkClearColorValue clear {};
		clear.float32[0] = m_backgroundColor.r;
		clear.float32[1] = m_backgroundColor.g;
		clear.float32[2] = m_backgroundColor.b;
		clear.float32[3] = 1.f;
		const VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
		vkCmdClearColorImage(cmd, m_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
		return;
	}

	if (mode == BackgroundMode::Gradient) {
		ComputePushConstants params {};
		params.data1 = GRADIENT_TOP;
		params.data2 = GRADIENT_BOTTOM;
		recordDrawImageEffect(cmd, m_gradientPass, params);
		return;
	}

	//the sky or the map by view direction. The sky reads no map, but the binding is declared: it names the grey image.
	//Both images sit in SHADER_READ_ONLY_OPTIMAL for good: createImage() leaves uploads there, and a replaced map is
	//destroyed only after a device-wide wait, so a set written this frame can never outlive the view it names
	const bool sky = mode == BackgroundMode::Sky;
	const AllocatedImage& map = sky ? m_greyImage : m_environmentMap;

	const VkDescriptorSet set = getCurrentFrame().frameDescriptors.allocate(m_device, m_environmentPass.setLayout);
	DescriptorWriter writer;
	writer.writeImage(0, m_drawImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(1, map.imageView, m_defaultSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.updateSet(m_device, set);

	//m_sceneData is this frame's, set by updateScene() before recording began
	EnvironmentBackgroundPushConstants pushConstants {};
	pushConstants.inverseViewProj = glm::inverse(m_sceneData.viewproj);
	pushConstants.drawExtent = glm::vec2(m_drawExtent.width, m_drawExtent.height);
	//the map is stored divided by its scale, which comes back in here with the intensity
	pushConstants.exposure = m_environmentIntensity * m_environmentMapScale;
	pushConstants.sky = sky ? 1u : 0u;
	pushConstants.rotation = environmentRotation(m_environmentRotation);

	dispatchComputePassOver(cmd, m_environmentPass, set, &pushConstants, m_drawImage.imageExtent);
}

void VulkanEngine::recordDrawImageEffect(VkCommandBuffer cmd, const ComputePass& pass, const ComputePushConstants& params)
{
	//one set per frame from the frame's own allocator, which draw() resets when the slot comes
	//round again - the same way every other per-frame set here is handled
	const VkDescriptorSet set = getCurrentFrame().frameDescriptors.allocate(m_device, pass.setLayout);

	DescriptorWriter writer;
	writer.writeImage(0, m_drawImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.updateSet(m_device, set);

	//the whole image, not just m_drawExtent, so the effect looks the same at every render scale
	//as it always has
	dispatchComputePassOver(cmd, pass, set, &params, m_drawImage.imageExtent);
}

namespace {

//the raster pass's draw order: by material, then by surface, so a frame binds each as few times as it can and the
//draws of one surface with one material sit together (MeshDrawList::rebuild() sorts the same way)
bool drawsBefore(const RenderObject* a, const RenderObject* b)
{
	if (a->material != b->material) {
		return a->material < b->material;
	}
	if (a->indexBuffer != b->indexBuffer) {
		return a->indexBuffer < b->indexBuffer;
	}
	return a->firstIndex < b->firstIndex;
}

//whether two draws can be one instanced draw: the same triangles with the same material
bool sameSurface(const RenderObject& a, const RenderObject& b)
{
	return a.material == b.material && a.indexBuffer == b.indexBuffer && a.vertexBuffer == b.vertexBuffer
		&& a.firstIndex == b.firstIndex && a.indexCount == b.indexCount;
}

}

void VulkanEngine::drawGeometry(VkCommandBuffer cmd)
{
	//the occlusion queries this frame slot issued two frames ago have finished (draw() waited for its fence). A surface
	//whose box passed no samples is skipped this frame; it is queried again below, so it reappears two frames after it
	//comes into view. Results for an older draw list are dropped, as is everything when the culling is off
	FrameData& frame = getCurrentFrame();
	std::fill(m_meshDrawList.occluded.begin(), m_meshDrawList.occluded.end(), 0);
	if (m_occlusionCulling && !frame.occlusionEntries.empty() && frame.occlusionGeneration == m_meshDrawList.generation) {
		std::vector<uint64_t> samples(frame.occlusionEntries.size());
		const VkResult got = vkGetQueryPoolResults(m_device, frame.occlusionQueries, 0, (uint32_t)samples.size(), samples.size() * sizeof(uint64_t),
			samples.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
		if (got == VK_SUCCESS) {
			for (size_t i = 0; i < samples.size(); i++) {
				m_meshDrawList.occluded[frame.occlusionEntries[i]] = samples[i] == 0 ? 1 : 0;
			}
		}
	}

	//the mesh objects' surfaces in view, already in draw order
	std::vector<const RenderObject*> draws;
	std::vector<uint32_t> queries;
	const glm::vec3 eye = glm::vec3(glm::inverse(m_sceneData.view)[3]);
	const MeshDrawList::CullResult culled = m_meshDrawList.cull(m_sceneData.viewproj, eye, m_occlusionCulling, draws, queries);
	if (!m_occlusionCulling) {
		queries.clear();
	}

	//the raytracer's shapes, built every frame (drawRaytraceShapes()), after them
	const size_t shapesBegin = draws.size();
	for (const RenderObject& object : m_mainDrawContext.opaqueSurfaces) {
		if (is_visible(object, m_sceneData.viewproj)) {
			draws.push_back(&object);
		}
	}
	std::sort(draws.begin() + (ptrdiff_t)shapesBegin, draws.end(), drawsBefore);

	//then the transparent surfaces, which blend additively without writing depth, so their order does not matter
	for (const RenderObject& r : m_meshDrawList.transparent) {
		draws.push_back(&r);
	}
	for (const RenderObject& r : m_mainDrawContext.transparentSurfaces) {
		draws.push_back(&r);
	}

	//reset counters
	m_stats.drawcallCount = 0;
	m_stats.triangleCount = 0;
	m_stats.occludedCount = (int)culled.occluded;
	m_stats.occludedTriangles = culled.occludedTriangles;

	//this frame's queries, one per entry, reset outside the rendering they are counted in
	if (queries.size() > frame.occlusionCapacity) {
		if (frame.occlusionQueries != VK_NULL_HANDLE) {
			vkDestroyQueryPool(m_device, frame.occlusionQueries, nullptr);
		}
		frame.occlusionCapacity = std::max<uint32_t>((uint32_t)(queries.size() + queries.size() / 2), 256);
		VkQueryPoolCreateInfo poolInfo { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		poolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
		poolInfo.queryCount = frame.occlusionCapacity;
		checkVkResult(vkCreateQueryPool(m_device, &poolInfo, nullptr, &frame.occlusionQueries));
	}
	if (!queries.empty()) {
		vkCmdResetQueryPool(cmd, frame.occlusionQueries, 0, (uint32_t)queries.size());
	}
	frame.occlusionEntries = queries;
	frame.occlusionGeneration = m_meshDrawList.generation;
	//begin clock
	auto start = std::chrono::system_clock::now();

	//begin a render pass  connected to our draw image
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(m_drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(m_depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

	VkRenderingInfo renderInfo = vkinit::rendering_info(m_drawExtent, &colorAttachment, &depthAttachment);
	vkinit::VkFunctionLoader::get_instance().vkCmdBeginRenderingKHR(cmd, &renderInfo);

	//set dynamic viewport and scissor
	VkViewport viewport = {};
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = m_drawExtent.width;
	viewport.height = m_drawExtent.height;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;

	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor = {};
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent.width = m_drawExtent.width;
	scissor.extent.height = m_drawExtent.height;

	vkCmdSetScissor(cmd, 0, 1, &scissor);


	//allocate a new uniform buffer for the scene data
	AllocatedBuffer gpuSceneDataBuffer = createBuffer(sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	//add it to the deletion queue of this frame so it gets deleted once its been used
	getCurrentFrame().deletionQueue.push_function([=, this]() {
		destroyBuffer(gpuSceneDataBuffer);
	});

	//write the buffer
	GPUSceneData* sceneUniformData = (GPUSceneData*)gpuSceneDataBuffer.allocation->GetMappedData();
	*sceneUniformData = m_sceneData;
	vmaFlushAllocation(m_memAllocator, gpuSceneDataBuffer.allocation, 0, sizeof(GPUSceneData));

	//create a descriptor set that binds that buffer and update it
	VkDescriptorSet globalDescriptor = getCurrentFrame().frameDescriptors.allocate(m_device, m_gpuSceneDataDescriptorLayout);
	{
		DescriptorWriter writer;
		writer.writeBuffer(0, gpuSceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		writer.updateSet(m_device, globalDescriptor);
	}

	//every surface's world matrix, in draw order: a run of draws of one surface with one material is a single
	//instanced draw, and its instances' matrices are the run's slice of this buffer, which mesh.vert reads as
	//per-instance vertex attributes. One vkCmdDrawIndexed per (material, surface) rather than per object: a pbrt
	//scene places the same few hundred meshes tens of thousands of times
	if (draws.size() > frame.instanceCapacity) {
		if (frame.instanceBuffer.buffer != VK_NULL_HANDLE) {
			//the fence wait in draw() means this frame slot's last use of it is over
			destroyBuffer(frame.instanceBuffer);
		}
		frame.instanceCapacity = std::max<size_t>(draws.size() + draws.size() / 2, 1024);
		frame.instanceBuffer = createBuffer(frame.instanceCapacity * sizeof(glm::mat4), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	}
	if (!draws.empty()) {
		glm::mat4* instances = (glm::mat4*)frame.instanceBuffer.allocation->GetMappedData();
		for (size_t i = 0; i < draws.size(); i++) {
			instances[i] = draws[i]->transform;
		}
		vmaFlushAllocation(m_memAllocator, frame.instanceBuffer.allocation, 0, draws.size() * sizeof(glm::mat4));
		const VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers(cmd, 1, 1, &frame.instanceBuffer.buffer, &zero);
	}

	//defined outside of the draw function, this is the state we will try to skip
	MaterialPipeline* lastPipeline = nullptr;
	MaterialInstance* lastMaterial = nullptr;
	VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
	VkBuffer lastVertexBuffer = VK_NULL_HANDLE;

	//the draws [first, first + count), all of one surface with one material
	auto draw = [&](const RenderObject& r, uint32_t first, uint32_t count) {
		if (r.material != lastMaterial) {
			lastMaterial = r.material;
			//rebind pipeline and descriptors if the material changed
			if (r.material->pipeline != lastPipeline) {

				lastPipeline = r.material->pipeline;
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.material->pipeline->pipeline);
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,r.material->pipeline->layout, 0, 1,
					&globalDescriptor, 0, nullptr);

				VkViewport viewport = {};
				viewport.x = 0;
				viewport.y = 0;
				//the region the blit shows, not the window: the two disagreed after a resize and the
				//geometry was clipped and stretched against the imgui overlays
				viewport.width = (float)m_drawExtent.width;
				viewport.height = (float)m_drawExtent.height;
				viewport.minDepth = 0.f;
				viewport.maxDepth = 1.f;

				vkCmdSetViewport(cmd, 0, 1, &viewport);

				VkRect2D scissor = {};
				scissor.offset.x = 0;
				scissor.offset.y = 0;
				scissor.extent.width = m_drawExtent.width;
				scissor.extent.height = m_drawExtent.height;

				vkCmdSetScissor(cmd, 0, 1, &scissor);
			}

			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.material->pipeline->layout, 1, 1,
				&r.material->materialSet, 0, nullptr);
		}
		//rebind index and vertex buffers if needed
		if (r.indexBuffer != lastIndexBuffer) {
			lastIndexBuffer = r.indexBuffer;
			vkCmdBindIndexBuffer(cmd, r.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
		}
		if (r.vertexBuffer != lastVertexBuffer) {
			lastVertexBuffer = r.vertexBuffer;
			const VkDeviceSize zero = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &r.vertexBuffer, &zero);
		}

		//firstInstance offsets the per-instance attributes to this run's matrices
		vkCmdDrawIndexed(cmd, r.indexCount, count, r.firstIndex, 0, first);
		//stats
		m_stats.drawcallCount++;
		m_stats.triangleCount += (r.indexCount / 3) * count;
	};

	for (size_t first = 0; first < draws.size();) {
		const RenderObject& r = *draws[first];
		size_t end = first + 1;
		while (end < draws.size() && sameSurface(*draws[end], r)) {
			end++;
		}
		draw(r, (uint32_t)first, (uint32_t)(end - first));
		first = end;
	}

	//each queried surface's box against everything drawn, which the next use of this frame slot reads back
	if (!queries.empty()) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_occlusionBoxPipeline.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_occlusionBoxPipeline.layout, 0, 1, &globalDescriptor, 0, nullptr);
		for (uint32_t q = 0; q < (uint32_t)queries.size(); q++) {
			const auto& [lo, hi] = m_meshDrawList.boxes[queries[q]];
			const glm::vec4 box[2] = { glm::vec4(lo, 1.f), glm::vec4(hi, 1.f) };
			vkCmdPushConstants(cmd, m_occlusionBoxPipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(box), box);
			vkCmdBeginQuery(cmd, frame.occlusionQueries, q, 0);
			vkCmdDraw(cmd, 36, 1, 0, 0);
			vkCmdEndQuery(cmd, frame.occlusionQueries, q);
		}
	}

	vkinit::VkFunctionLoader::get_instance().vkCmdEndRenderingKHR(cmd);

	//the solved beams, in a rendering of their own into the weighted blend's two targets, tested against the depth drawn
	//above and hidden by it; draw() composites them over the draw image
	m_beamPreviewDrawn = !m_beamSections.empty();
	if (m_beamPreviewDrawn) {
		vkutil::transition_image(cmd, m_beamPreviewAccum.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		vkutil::transition_image(cmd, m_beamPreviewReveal.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		//nothing summed, everything revealed
		VkClearValue zero {};
		VkClearValue one {};
		one.color = { { 1.f, 1.f, 1.f, 1.f } };
		const VkRenderingAttachmentInfo beamAttachments[] = {
			vkinit::attachment_info(m_beamPreviewAccum.imageView, &zero, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
			vkinit::attachment_info(m_beamPreviewReveal.imageView, &one, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
		};
		VkRenderingAttachmentInfo beamDepth = vkinit::depth_attachment_info(m_depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
		beamDepth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		VkRenderingInfo beamInfo = vkinit::rendering_info(m_drawExtent, nullptr, &beamDepth);
		beamInfo.colorAttachmentCount = 2;
		beamInfo.pColorAttachments = beamAttachments;
		vkinit::VkFunctionLoader::get_instance().vkCmdBeginRenderingKHR(cmd, &beamInfo);
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		drawBeamPreview(cmd, globalDescriptor);
		vkinit::VkFunctionLoader::get_instance().vkCmdEndRenderingKHR(cmd);
		vkutil::transition_image(cmd, m_beamPreviewAccum.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, m_beamPreviewReveal.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	}

	// we delete the draw commands now that we processed them
	m_mainDrawContext.opaqueSurfaces.clear();
	m_mainDrawContext.transparentSurfaces.clear();

	auto end = std::chrono::system_clock::now();

	//convert to microseconds (integer), and then come back to miliseconds
	auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
	m_stats.meshDrawTime = elapsed.count() / 1000.f;
}

void VulkanEngine::drawImgui(VkCommandBuffer cmd, VkImageView targetImageView)
{
	VkClearValue clear {};
	clear.color = { { 0.f, 0.f, 0.f, 1.f } };
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, &clear, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(m_swapchainExtent, &colorAttachment, nullptr);

	vkinit::VkFunctionLoader::get_instance().vkCmdBeginRenderingKHR(cmd, &renderInfo);

	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

	vkinit::VkFunctionLoader::get_instance().vkCmdEndRenderingKHR(cmd);
}

void VulkanEngine::immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function)
{
	checkVkResult(vkResetFences(m_device, 1, &m_immFence));
	checkVkResult(vkResetCommandBuffer(m_immCommandBuffer, 0));

	VkCommandBuffer cmd = m_immCommandBuffer;

	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	checkVkResult(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

	function(cmd);

	checkVkResult(vkEndCommandBuffer(cmd));

	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
	VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, nullptr, nullptr);

	// submit command buffer to the queue and execute it.
	// m_renderFence will now block until the graphic commands finish execution
	checkVkResult(vkinit::VkFunctionLoader::get_instance().vkQueueSubmit2KHR(m_graphicsQueue, 1, &submit, m_immFence));
	checkVkResult(vkWaitForFences(m_device, 1, &m_immFence, true, 9999999999));
}

void VulkanEngine::run()
{
	//VKGUIDE_SELFTEST=1 runs the GPU self-tests once, prints them and returns with the result in
	//the exit code, for a script. Unset or 0, the tests run from the Windows menu
	const char* selfTest = getenv("VKGUIDE_SELFTEST");
	if (selfTest != nullptr && selfTest[0] != '\0' && strcmp(selfTest, "0") != 0) {
		m_selfTests.runAll(this);
		m_selfTests.print();
		m_exitCode = m_selfTests.allPassed() ? 0 : 1;
		return;
	}

	if (m_commandLine.batch() && !beginBatch()) {
		return;
	}

	bool bQuit = false;

	//main loop
	while (!bQuit && !glfwWindowShouldClose(m_window))
	{
		//begin clock
		auto start = std::chrono::system_clock::now();

		glfwPollEvents();

		if(glfwGetWindowAttrib(m_window, GLFW_VISIBLE) != GLFW_TRUE && !m_renderWhenHidden) {
			m_stop_rendering = true;
		}
		else {
			m_stop_rendering = false;
		}

		//do not draw if we are minimized
		if (m_stop_rendering) {
			//throttle the speed to avoid the endless spinning
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		if (m_resizeRequested) {
			resizeSwapchain();
		}

		//a workspace switch or reset loads a saved layout, which has to happen between frames
		m_workspaces.applyPending(m_capabilities);

		// imgui new frame
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		//the dock space every window docks into, before any window is built
		m_workspaces.beginFrame();
		m_displayRegistry.setDefaultDock(m_workspaces.centerDock());

		//the camera moves first, so the gizmo, the overlays, the panels and the raster pass all
		//see this frame's pose - a camera updated after the gizmo was drawn trails it by a frame
		updateViewportCamera();

		m_transformGizmo.beginFrame();

		m_displayRegistry.beginFrame(m_frameNumber);

		//the texture library loads what was added since the last frame (a model, a user texture, a light's image, a
		//camera's aperture), before the tracer packs the scene that names it
		syncSceneImages();
		m_textureLibrary.update(this, (uint32_t)std::clamp(m_raytracer.settings().maxTextureSize, 16, 16384));

		//before the imgui content below, so a raytrace that finished since the last frame has
		//its output on screen this frame rather than the next one
		m_raytracer.update(this, m_raytraceScene);

		if (m_batch) {
			stepBatch();
		}

		if (ImGui::BeginMainMenuBar()) {
			drawFileMenu();
			drawEditMenu();
			drawCameraMenu();
			drawViewMenu();
			if (ImGui::BeginMenu("Windows")) {
				m_workspaces.drawWindowsMenu(m_capabilities, [this]() { m_displayRegistry.drawWindowsMenu(); });
				ImGui::EndMenu();
			}
			forEachEnabledCapability(m_capabilities, [this](const CapabilityInfo& capability) {
				if (capability.menus != nullptr) {
					capability.menus(this);
				}
			});
			//Render at the bar's middle, the workspaces at its right end
			m_raytracer.drawMenuBarControls(this, m_raytraceScene);
			m_workspaces.drawTabs(m_capabilities);
			ImGui::EndMainMenuBar();
		}

		m_displayRegistry.drawWindows();
		//every panel, and last the Viewport with the overlays and the gizmo, so an "Edit Transform" click shows the
		//gizmo this frame
		m_workspaces.drawWindows(m_capabilities);
		drawSaveTemplatePopup(this);

		//make imgui calculate internal draw structures
		ImGui::Render();

		//between the imgui frame and the draw, since a native file dialog blocks in a modal loop
		runPendingFileAction();
		updateWindowTitle();
		//every edit of this frame's panels is in: each optic's stack follows its mount, and the bench's meshes are uploaded
		//before the draw records anything
		updateBench();

		draw();

		//get clock again, compare with start clock
		auto end = std::chrono::system_clock::now();
		
		//convert to microseconds (integer), and then come back to miliseconds
		auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
		m_stats.frameTime = elapsed.count() / 1000.f;

		//the Stats window's history, whether it is open or not
		FrameSample sample;
		sample.time = ImGui::GetTime();
		sample.frameMs = m_stats.frameTime;
		sample.updateMs = m_stats.sceneUpdateTime;
		sample.rasterRecordMs = m_stats.meshDrawTime;
		sample.rasterGpuMs = m_stats.rasterGpuTime;
		//a render's first frames have no timestamps back yet, and read 0
		const float raytraceMs = m_raytracer.gpu().lastFrameGpuMs();
		sample.raytraceGpuMs = m_raytracer.renderRunning() && m_raytracer.gpu().hasTimestamps() && raytraceMs > 0.f ? raytraceMs : NAN;
		sample.drawCalls = m_stats.drawcallCount;
		sample.triangles = m_stats.triangleCount;
		sample.beamSolveMs = m_stats.beamSolveTime;
		m_frameHistory.record(sample);
	}

	//imgui writes its file only every few seconds while something changed, so the last layout and workspace would be
	//lost on quit
	if (ImGui::GetIO().IniFilename != nullptr) {
		ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
	}
}

void VulkanEngine::registerWindows()
{
	//the Viewport is drawn last: its gizmo must see this frame's "Edit Transform" clicks in the panels
	m_workspaces.addWindow({ "Viewport", &m_showViewport, [this]() { drawViewportWindow(); }, {}, WindowGroup::Main, true });
	m_workspaces.addWindow({ "Scene", m_raytraceScene.visibilityFlag(), [this]() { m_raytraceScene.drawPanel(this); } });
	m_workspaces.addWindow({ "Materials", m_raytraceScene.materialsVisibilityFlag(), [this]() { m_raytraceScene.drawMaterialsWindow(this); } });
	m_workspaces.addWindow({ "Render Settings", m_raytracer.settingsVisibilityFlag(), [this]() { m_raytracer.drawSettingsPanel(this, m_raytraceScene); } });
	//the render's picture, and a detector's film and readings when the render target is one (docs/plans/completed/physoptics-detectors.md)
	m_workspaces.addWindow({ "Raytraced Output", m_raytracer.outputVisibilityFlag(), [this]() { m_raytracer.drawOutputWindow(this, m_raytraceScene); } });
	m_workspaces.addWindow({ "Raytracer Shaders", m_raytracer.kernelPanelVisibilityFlag(), [this]() { m_raytracer.drawKernelPanel(this); } });
	m_workspaces.addWindow({ "Import Report", &m_showImportReport, [this]() { drawImportReport(); } });
	m_workspaces.addWindow({ "Scene Capabilities", &m_showCapabilities, [this]() { drawCapabilitiesWindow(this, &m_showCapabilities); } });

	m_workspaces.addWindow({ "Stats", &m_showStatsWindow, [this]() { drawStatsWindow(this, &m_showStatsWindow); } });
	m_workspaces.addWindow({ "GPU self-tests", m_selfTests.visibilityFlag(), [this]() { m_selfTests.drawWindow(this); }, {}, WindowGroup::Debug });
	//the solved beams' segments and the preview's settings (docs/plans/completed/physoptics-gaussian-beam-solver.md)
	m_workspaces.addWindow({ "Beam", m_raytraceScene.beamWindowVisibilityFlag(), [this]() { m_raytraceScene.drawBeamWindow(this); }, CapabilityId::OpticalBench });
	//the bench's settings, under Optical bench (Optics -> Bench Settings...)
	m_workspaces.addWindow({ "Bench Settings", m_raytraceScene.benchSettingsVisibilityFlag(), [this]() {
		bool* open = m_raytraceScene.benchSettingsVisibilityFlag();
		if (!*open) {
			return;
		}
		if (ImGui::Begin("Bench Settings", open)) {
			m_raytraceScene.drawBenchSettings(this);
		}
		ImGui::End();
	}, CapabilityId::OpticalBench });
	m_workspaces.addWindow({ "Dear ImGui Demo", &m_showDemoWindow, [this]() {
		if (m_showDemoWindow) {
			ImGui::ShowDemoWindow(&m_showDemoWindow);
		}
	}, {}, WindowGroup::Debug });
	m_workspaces.addWindow({ "ImPlot Demo", &m_showPlotDemoWindow, [this]() {
		if (m_showPlotDemoWindow) {
			ImPlot::ShowDemoWindow(&m_showPlotDemoWindow);
		}
	}, {}, WindowGroup::Debug });
}

void VulkanEngine::drawViewportWindow()
{
	m_viewportVisible = false;
	m_viewportHovered = false;
	if (!m_showViewport) {
		return;
	}
	//the preview fills the window to its edges
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
	const bool shown = ImGui::Begin("Viewport", &m_showViewport, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	const ImVec2 size = ImGui::GetContentRegionAvail();
	if (!shown || size.x < 1.f || size.y < 1.f) {
		//collapsed, or a tab behind another: draw() skips the raster pass
		ImGui::End();
		return;
	}

	//the raster pass draws the window's size, at the render scale, into the corner of the window-sized draw image,
	//and the window shows that corner stretched over itself
	m_drawExtent.width = std::clamp((uint32_t)(size.x * m_renderScale), 1u, m_drawImage.imageExtent.width);
	m_drawExtent.height = std::clamp((uint32_t)(size.y * m_renderScale), 1u, m_drawImage.imageExtent.height);
	m_viewportVisible = true;

	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const ImVec2 uvMax { (float)m_drawExtent.width / (float)m_drawImage.imageExtent.width, (float)m_drawExtent.height / (float)m_drawImage.imageExtent.height };
	ImGui::Image(m_displayRegistry.textureId(VIEWPORT_TEXTURE_NAME), size, ImVec2(0.f, 0.f), uvMax);
	m_viewportHovered = ImGui::IsItemHovered();
	//F frames the selection, over the preview or with its window focused, unless a text field has the keyboard
	if ((m_viewportHovered || ImGui::IsWindowFocused()) && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
		frameInViewport(false);
	}

	ViewportView view;
	view.pos = pos;
	view.size = size;
	view.view = m_viewportNav.camera.getViewMatrix();
	view.projection = rasterProjection();
	view.viewProj = view.projection * view.view;
	view.drawList = ImGui::GetWindowDrawList();
	m_viewportView = view;

	//a first-person edit shows the flown camera's whole frame, outlined in yellow, and greys what the viewport sees
	//beyond it, as Blender's camera view and Unreal's piloted cameras do
	if (const std::optional<glm::vec2> frame = flownCameraFrame()) {
		const ImVec2 center { pos.x + size.x * 0.5f, pos.y + size.y * 0.5f };
		const ImVec2 lo { center.x - frame->x * size.x * 0.5f, center.y - frame->y * size.y * 0.5f };
		const ImVec2 hi { center.x + frame->x * size.x * 0.5f, center.y + frame->y * size.y * 0.5f };
		const ImVec2 end { pos.x + size.x, pos.y + size.y };
		const ImU32 grey = IM_COL32(24, 24, 24, 140);
		view.drawList->AddRectFilled(pos, ImVec2(end.x, lo.y), grey);
		view.drawList->AddRectFilled(ImVec2(pos.x, hi.y), end, grey);
		view.drawList->AddRectFilled(ImVec2(pos.x, lo.y), ImVec2(lo.x, hi.y), grey);
		view.drawList->AddRectFilled(ImVec2(hi.x, lo.y), ImVec2(end.x, hi.y), grey);
		view.drawList->AddRect(lo, hi, IM_COL32(255, 220, 80, 255), 0.f, 0, 1.5f);
	}

	updateFocusPick(view);
	drawCameraOverlays(view);
	drawFocusPlane(view);
	drawLightOverlays(view);
	forEachEnabledCapability(m_capabilities, [&](const CapabilityInfo& capability) {
		if (capability.viewportOverlays != nullptr) {
			capability.viewportOverlays(this, view);
		}
	});
	m_transformGizmo.draw(view, m_viewportHovered);

	ImGui::End();
}

void VulkanEngine::updateWindowTitle()
{
	//the template's name may come from its file, so it is looked up only when the scene changes
	const std::string key = m_scenePath.string() + "\n" + m_sceneTemplate;
	if (key == m_windowTitleKey) {
		return;
	}
	m_windowTitleKey = key;
	std::string title = m_scenePath.empty() ? "Untitled" : m_scenePath.filename().string();
	if (!m_sceneTemplate.empty()) {
		title += " - " + templateDisplayName(m_sceneTemplate, templatesDirectory());
	}
	title += " - VkGuide";
	glfwSetWindowTitle(m_window, title.c_str());
}

void VulkanEngine::enableCapability(CapabilityId id, bool offerRecommendations)
{
	if (m_capabilities.has(id)) {
		return;
	}
	m_capabilities.enabled.set((size_t)id);
	if (offerRecommendations && std::find(m_pendingRecommendations.begin(), m_pendingRecommendations.end(), id) == m_pendingRecommendations.end()) {
		m_pendingRecommendations.push_back(id);
	}
}

bool VulkanEngine::disableCapability(CapabilityId id)
{
	if (!m_capabilities.has(id)) {
		return true;
	}
	std::vector<ObjectRef> objects;
	capabilityInfo(id).dependentObjects(this, objects);
	if (!objects.empty()) {
		return false;
	}
	m_capabilities.enabled.reset((size_t)id);
	std::erase(m_pendingRecommendations, id);
	return true;
}

std::filesystem::path VulkanEngine::templatesDirectory() const
{
	return absoluteNormalized(m_rootPath + "assets/templates");
}

void VulkanEngine::requestNewScene(const SceneTemplate& entry)
{
	m_pendingTemplate = entry;
	m_pendingFileAction = FileAction::NewFromTemplate;
}

void VulkanEngine::newSceneFromTemplate(const SceneTemplate& entry)
{
	if (entry.path.empty()) {
		newScene();
		if (entry.populate != nullptr) {
			entry.populate(this);
		}
		m_sceneTemplate = entry.key;
		m_lastFileResult = IoResult::success(fmt::format("New scene from the {} template", entry.name));
		return;
	}
	//a template file opens untitled, so Save asks for a path rather than overwriting the template
	const IoResult opened = openScene(entry.path);
	if (m_scenePath != absoluteNormalized(entry.path)) {
		//the file did not read, and the current scene is untouched
		m_lastFileResult = opened;
		return;
	}
	m_scenePath.clear();
	m_sceneTemplate = entry.key;
	m_lastFileResult = opened.ok ? IoResult::success(fmt::format("New scene from the {} template", entry.name)) : opened;
}

IoResult VulkanEngine::saveTemplate(const std::string& key, const std::string& name, const std::string& description)
{
	SceneDescription scene = describeScene();
	scene.templateKey = key;
	scene.templateInfo = SceneTemplateInfo { name, description };
	std::error_code ec;
	std::filesystem::create_directories(templatesDirectory(), ec);
	const std::filesystem::path path = templatesDirectory() / (key + ".gltf");
	const IoResult result = saveSceneFile(scene, path);
	if (!result.ok) {
		return result;
	}
	return IoResult::success(fmt::format("Saved the template '{}' to {}", name, path.filename().string()));
}

void VulkanEngine::setViewportPose(const glm::vec3& position, const glm::vec3& target)
{
	m_viewportNav.lookAt(position, target);
}

void VulkanEngine::frameInViewport(bool all)
{
	std::optional<Aabb> box = all ? std::nullopt : m_raytraceScene.selectionBounds(this);
	if (!box) {
		box = m_raytraceScene.sceneBounds(this);
	}
	if (!box) {
		return;
	}
	//a first-person edit frames what the flown camera renders, not the wider preview around it
	if (const SceneCamera* flown = m_firstPersonCameraId != 0 ? m_raytraceScene.findCamera(m_firstPersonCameraId) : nullptr) {
		m_viewportNav.frame(*box, glm::radians(flown->vfovDegrees), renderAspect());
		return;
	}
	const float aspect = (float)std::max(m_drawExtent.width, 1u) / (float)std::max(m_drawExtent.height, 1u);
	m_viewportNav.frame(*box, glm::radians(rasterVerticalFov()), aspect);
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
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	//VKGUIDE_WINDOW=background opens the window without taking focus, for a scripted run while you
	//work in another app or on another desktop; =minimized also minimizes it to the Dock and keeps
	//rendering there. Unset, the window opens in front and focused, as it always has
	const char* windowMode = getenv("VKGUIDE_WINDOW");
	const bool minimized = windowMode != nullptr && strcmp(windowMode, "minimized") == 0;
	const bool background = minimized || (windowMode != nullptr && strcmp(windowMode, "background") == 0);
	if (background) {
		glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
		glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
	}

	//maximized, so the docked layout has the screen to divide; the swapchain follows whatever size that gives
	glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
	m_window = glfwCreateWindow(m_windowExtent.width, m_windowExtent.height, "VkGuide", nullptr, nullptr);
	if (minimized) {
		glfwIconifyWindow(m_window);
		m_renderWhenHidden = true;
	}
	int width = 0;
	int height = 0;
	glfwGetWindowSize(m_window, &width, &height);
	if (width > 0 && height > 0) {
		m_windowExtent = { (uint32_t)width, (uint32_t)height };
	}
	glfwSetWindowUserPointer(m_window, this);

	//the cursor's motion reaches the camera only while a drag holds it captured, so there is a single boundary between
	//driving the camera and using the ui. The keys a look flies with are read once a frame (updateViewportCamera())
	glfwSetCursorPosCallback(m_window, [](GLFWwindow* w, double x, double y) {
		auto* engine = static_cast<VulkanEngine*>(glfwGetWindowUserPointer(w));
		if (!engine->m_cameraCaptureActive) {
			return;
		}
		if (engine->m_firstMouse) {
			engine->m_lastMouseX = x;
			engine->m_lastMouseY = y;
			engine->m_firstMouse = false;
			return;
		}
		engine->m_viewportNav.mouseMoved(x - engine->m_lastMouseX, y - engine->m_lastMouseY);
		engine->m_lastMouseX = x;
		engine->m_lastMouseY = y;
	});
	glfwSetMouseButtonCallback(m_window, [](GLFWwindow* w, int button, int action, int mods) {
		auto* engine = static_cast<VulkanEngine*>(glfwGetWindowUserPointer(w));
		ViewportNavigation& navigation = engine->m_viewportNav;
		if (action == GLFW_PRESS) {
			//only a press over the raster preview, with no window in front of it, starts a drag - and a press on or
			//near an active gizmo handle is a gizmo drag, not the camera's. One drag at a time: a second button
			//pressed during one is ignored
			const ViewportDrag drag = ViewportNavigation::dragFor(button, mods);
			if (drag != ViewportDrag::None && navigation.drag() == ViewportDrag::None && engine->m_viewportHovered && !engine->m_transformGizmo.wantsMouse()) {
				navigation.beginDrag(drag, button);
				engine->setCameraCapture(true);
			}
		} else if (action == GLFW_RELEASE && button == navigation.dragButton()) {
			navigation.endDrag();
			engine->setCameraCapture(false);
		}
	});
	glfwSetScrollCallback(m_window, [](GLFWwindow* w, double, double yoffset) {
		auto* engine = static_cast<VulkanEngine*>(glfwGetWindowUserPointer(w));
		ViewportNavigation& navigation = engine->m_viewportNav;
		//while looking every scroll changes the speed; otherwise a trackpad's swipe is a gesture when they are on,
		//which updateViewportCamera() takes from the monitor, and everything else over the preview moves along the view
		if (navigation.drag() == ViewportDrag::Look) {
			navigation.scrolled(yoffset);
		} else if (engine->m_viewportHovered && !(navigation.prefs.trackpadGestures && scrollIsFromTrackpad())) {
			navigation.scrolled(yoffset);
		}
	});
	installTrackpadMonitor();
}

void VulkanEngine::setCameraCapture(bool active)
{
	if (active == m_cameraCaptureActive) {
		return;
	}

	if (active) {
		glfwGetCursorPos(m_window, &m_capturedCursorX, &m_capturedCursorY);
		//unbounded relative mouse-look, with no screen edge to run into
		glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		//the glfw backend still feeds imgui the disabled cursor's virtual position, which drifts
		//freely while looking around - without this it would wander over windows and hover them. The keys are the
		//camera's too, so W A S D never type into a text field that kept the focus
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
		//re-arm, so the first delta is not measured against wherever the free cursor last sat
		m_firstMouse = true;
	} else {
		glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
		//the virtual position has drifted, so put the visible cursor back where the user left it
		glfwSetCursorPos(m_window, m_capturedCursorX, m_capturedCursorY);
		ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard);
	}

	m_cameraCaptureActive = active;
}

float VulkanEngine::renderAspect() const
{
	const RenderSettings& settings = m_raytracer.settings();
	return settings.matchViewport
		? (float)std::max(m_drawExtent.width, 1u) / (float)std::max(m_drawExtent.height, 1u)
		: (float)std::max(settings.width, 1) / (float)std::max(settings.height, 1);
}

namespace {

// how much wider than the flown camera's frame the viewport sees on the axis where the frame fits tightest, so the
// yellow border stays inside the viewport on every side
constexpr float CAMERA_FRAME_MARGIN = 1.06f;

// the viewport's half extents on the image plane at distance 1, for a flown camera of vertical fov `vfovDegrees` and
// aspect `cameraAspect` seen in a viewport of aspect `viewportAspect`: the smallest that hold the camera's, times the
// margin
glm::vec2 flownViewportHalfExtents(float vfovDegrees, float cameraAspect, float viewportAspect)
{
	const float t = glm::tan(glm::radians(vfovDegrees) * 0.5f);
	const float halfHeight = CAMERA_FRAME_MARGIN * std::max(t, cameraAspect * t / viewportAspect);
	return glm::vec2(viewportAspect * halfHeight, halfHeight);
}

}

float VulkanEngine::rasterVerticalFov() const
{
	if (m_firstPersonCameraId != 0) {
		if (const SceneCamera* camera = m_raytraceScene.findCamera(m_firstPersonCameraId)) {
			//wider than the camera's own, so the whole frame it renders fits in a viewport of any shape
			const float viewportAspect = (float)std::max(m_drawExtent.width, 1u) / (float)std::max(m_drawExtent.height, 1u);
			const glm::vec2 half = flownViewportHalfExtents(camera->vfovDegrees, renderAspect(), viewportAspect);
			return glm::degrees(2.f * glm::atan(half.y));
		}
	}
	return m_viewportNav.settings.vfovDegrees;
}

std::optional<glm::vec2> VulkanEngine::flownCameraFrame() const
{
	const SceneCamera* camera = m_firstPersonCameraId != 0 ? m_raytraceScene.findCamera(m_firstPersonCameraId) : nullptr;
	if (camera == nullptr) {
		return std::nullopt;
	}
	const float t = glm::tan(glm::radians(camera->vfovDegrees) * 0.5f);
	const float cameraAspect = renderAspect();
	const float viewportAspect = (float)std::max(m_drawExtent.width, 1u) / (float)std::max(m_drawExtent.height, 1u);
	const glm::vec2 half = flownViewportHalfExtents(camera->vfovDegrees, cameraAspect, viewportAspect);
	return glm::vec2(cameraAspect * t / half.x, t / half.y);
}

glm::mat4 VulkanEngine::rasterProjection() const
{
	//OpenGL's convention, depth -1 to 1, which ImGuizmo and the overlays (ViewportView) take. The raster pass draws with
	//rasterDepthProjection() instead
	//the Viewport window's aspect, which m_drawExtent keeps at every render scale
	const ViewportCameraSettings& settings = m_viewportNav.settings;
	return glm::perspective(glm::radians(rasterVerticalFov()), (float)std::max(m_drawExtent.width, 1u) / (float)std::max(m_drawExtent.height, 1u), settings.nearClip, settings.farClip);
}

glm::mat4 VulkanEngine::rasterDepthProjection() const
{
	//reverse-Z for Vulkan's 0 to 1 depth: the near plane at depth 1, the far plane at 0, depth n (f - d) / (d (f - n))
	//for a point d ahead of the eye. A float depth then keeps about 1e-7 of the distance at any distance, whatever the
	//near plane, where forward-Z lost precision with the distance squared over the near plane. glm::perspective() cannot
	//build it: without GLM_FORCE_DEPTH_ZERO_TO_ONE it maps to OpenGL's -1 to 1, and with its near and far swapped
	//Vulkan clips everything beyond twice the near plane, which is how vkguide's reversed projection drew nothing here.
	//y is not flipped; updateScene() flips it for Vulkan's clip space
	const ViewportCameraSettings& settings = m_viewportNav.settings;
	const float aspect = (float)std::max(m_drawExtent.width, 1u) / (float)std::max(m_drawExtent.height, 1u);
	const float f = 1.f / std::tan(0.5f * glm::radians(rasterVerticalFov()));
	const float n = settings.nearClip;
	const float far = settings.farClip;
	glm::mat4 m(0.f);
	m[0][0] = f / aspect;
	m[1][1] = f;
	m[2][2] = n / (far - n);
	m[2][3] = -1.f;
	m[3][2] = far * n / (far - n);
	return m;
}

SceneCamera VulkanEngine::cameraFromViewport() const
{
	SceneCamera camera;
	camera.position = m_viewportNav.camera.position;
	camera.orientation = glm::normalize(glm::quat_cast(glm::mat3(m_viewportNav.camera.getRotationMatrix())));
	camera.vfovDegrees = m_viewportNav.settings.vfovDegrees;
	return camera;
}

void VulkanEngine::beginFirstPersonEdit(uint64_t cameraId)
{
	const SceneCamera* camera = m_raytraceScene.findCamera(cameraId);
	if (camera == nullptr) {
		return;
	}

	//the free camera is yaw/pitch only (no roll), so the scene camera's orientation is reduced
	//to the yaw and pitch of its forward vector. A pivot that follows the view goes where the camera focuses
	Camera& view = m_viewportNav.camera;
	view.setPose(camera->position, camera->forward());
	if (m_viewportNav.carriesPivot()) {
		view.pivot = view.position + view.forward() * camera->focusDistance;
	}
	m_firstPersonCameraId = cameraId;
}

void VulkanEngine::endFirstPersonEdit()
{
	//the viewport keeps the pose it is at; the camera keeps what was last written into it
	m_firstPersonCameraId = 0;
}

void VulkanEngine::updateViewportCamera()
{
	//real time, held to a tenth of a second so a slow frame (pbrt's landscape takes seconds) is not one long jump
	const auto now = std::chrono::steady_clock::now();
	const float dt = m_lastCameraUpdate == std::chrono::steady_clock::time_point {} ? 0.f : std::min(std::chrono::duration<float>(now - m_lastCameraUpdate).count(), 0.1f);
	m_lastCameraUpdate = now;

	ViewportFrameInput input;
	input.dt = dt;
	//a focus pick's click is the pick's, not the navigation's
	input.hovered = m_viewportHovered && m_focusPickCameraId == 0;
	if (m_viewportNav.drag() == ViewportDrag::Look) {
		auto held = [this](int key) { return glfwGetKey(m_window, key) == GLFW_PRESS ? 1.f : 0.f; };
		input.move = glm::vec3(held(GLFW_KEY_D) - held(GLFW_KEY_A), held(GLFW_KEY_E) - held(GLFW_KEY_Q), held(GLFW_KEY_W) - held(GLFW_KEY_S));
	}
	//taken every frame, so a gesture over another window is not saved up for when the cursor reaches the preview
	input.gestures = takeTrackpadGestures();
	input.vfovRadians = glm::radians(rasterVerticalFov());
	input.viewportHeight = std::max(m_viewportView.size.y, 1.f);
	input.selectionCenter = [this]() -> std::optional<glm::vec3> {
		if (const std::optional<Aabb> box = m_raytraceScene.selectionBounds(this)) {
			return 0.5f * (box->min + box->max);
		}
		return std::nullopt;
	};
	m_viewportNav.update(input);

	if (m_firstPersonCameraId == 0) {
		return;
	}
	if (m_raytraceScene.findCamera(m_firstPersonCameraId) == nullptr) {
		//deleted or the scene was replaced under the edit
		m_firstPersonCameraId = 0;
		return;
	}
	//the viewport's pose becomes the camera's; a resting viewport writes nothing
	const Camera& view = m_viewportNav.camera;
	m_raytraceScene.setCameraPose(m_firstPersonCameraId, view.position, glm::normalize(glm::quat_cast(glm::mat3(view.getRotationMatrix()))));
}

void VulkanEngine::beginFocusPick(uint64_t cameraId)
{
	m_focusPickCameraId = m_focusPickCameraId == cameraId ? 0 : cameraId;
}

void VulkanEngine::updateFocusPick(const ViewportView& view)
{
	if (m_focusPickCameraId == 0) {
		return;
	}
	SceneCamera* camera = m_raytraceScene.findCamera(m_focusPickCameraId);
	if (camera == nullptr || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		m_focusPickCameraId = 0;
		return;
	}
	if (!m_viewportHovered) {
		return;
	}
	ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	ImGui::SetTooltip("Click the surface to focus %s on (Esc cancels)", camera->name.c_str());
	if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		return;
	}
	m_focusPickCameraId = 0;
	//the ray under the cursor, from the viewport's own camera: the near and far points of its pixel, unprojected
	const ImVec2 mouse = ImGui::GetMousePos();
	const glm::vec2 ndc((mouse.x - view.pos.x) / view.size.x * 2.f - 1.f, 1.f - (mouse.y - view.pos.y) / view.size.y * 2.f);
	const glm::mat4 inverse = glm::inverse(view.viewProj);
	const glm::vec4 nearPoint = inverse * glm::vec4(ndc, -1.f, 1.f);
	const glm::vec4 farPoint = inverse * glm::vec4(ndc, 1.f, 1.f);
	const glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
	const glm::vec3 direction = glm::normalize(glm::vec3(farPoint) / farPoint.w - origin);
	const std::optional<float> distance = m_raytracer.pickDistance(this, m_raytraceScene, origin, direction);
	if (!distance.has_value()) {
		fmt::println("Focus on: the click found no surface");
		return;
	}
	//the plane of focus pivots about the point on the axis at the focus distance: the distance that puts the point on it,
	//which without a tilt is the point's depth along the axis
	const glm::vec3 point = origin + *distance * direction;
	const CameraFrame frame = buildCameraFrame(cameraSnapshot(*camera), 1, 1);
	const glm::vec3 normal = (frame.flags & CAMERA_FLAG_TILTED) != 0u ? glm::vec3(frame.focusPlane) : frame.backward;
	const float along = glm::dot(normal, point - camera->position) / glm::dot(normal, -frame.backward);
	if (!(along > 0.f)) {
		fmt::println("Focus on: the surface is behind the camera's plane of focus");
		return;
	}
	m_raytraceScene.setCameraFocusDistance(camera->id, std::max(along, 0.01f));
	m_focusFlashCameraId = camera->id;
	m_focusFlashUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
}

void VulkanEngine::drawFocusPlane(const ViewportView& view)
{
	if (m_focusFlashCameraId == 0 || std::chrono::steady_clock::now() > m_focusFlashUntil) {
		m_focusFlashCameraId = 0;
		return;
	}
	const SceneCamera* camera = m_raytraceScene.findCamera(m_focusFlashCameraId);
	if (camera == nullptr || !cameraTraits(camera->kind).lens) {
		return;
	}
	//the film's frame on the plane of focus, tilted as the render tilts it: the corners of the untilted frame, carried
	//along their chief rays onto the tilted plane
	const CameraFrame frame = buildCameraFrame(cameraSnapshot(*camera), (uint32_t)std::max(m_raytracer.settings().width, 1), (uint32_t)std::max(m_raytracer.settings().height, 1));
	ImVec2 corners[4];
	const glm::vec2 film[4] = { { 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f } };
	for (int i = 0; i < 4; i++) {
		//a ray reaches its point of focus at t = 1 from wherever on the lens it leaves
		const CameraRay ray = generateCameraRay(frame, film[i], glm::vec2(0.5f), nullptr);
		if (!view.project(ray.origin + ray.direction, corners[i])) {
			return;
		}
	}
	view.drawList->AddQuadFilled(corners[0], corners[1], corners[2], corners[3], IM_COL32(255, 220, 80, 60));
	view.drawList->AddQuad(corners[0], corners[1], corners[2], corners[3], IM_COL32(255, 220, 80, 200), 1.5f);
}

void VulkanEngine::drawCameraOverlays(const ViewportView& view)
{
	const std::vector<SceneCamera>& cameras = m_raytraceScene.cameras();
	if (cameras.empty()) {
		return;
	}
	auto line = [&](const glm::vec3& a, const glm::vec3& b, ImU32 color) { view.line(a, b, color); };

	//the frustum's aspect is the render's, so the outline shows what a render would frame
	const float aspect = renderAspect();

	for (const SceneCamera& camera : cameras) {
		//the flown camera is the viewport itself; its outline would fill the screen
		if (camera.id == m_firstPersonCameraId) {
			continue;
		}
		const bool isRenderCamera = camera.id == m_raytracer.renderCameraId();
		const ImU32 color = isRenderCamera ? IM_COL32(255, 220, 80, 255) : IM_COL32(200, 200, 200, 160);

		const float depth = 0.5f;
		const glm::mat4 transform = camera.transform();
		auto at = [&](float x, float y, float z) { return glm::vec3(transform * glm::vec4(x, y, z, 1.f)); };
		if (camera.kind == CameraKind::Spherical) {
			//it sees every way: three rings about its position, and a tick where its image's center looks
			const float radius = 0.2f;
			constexpr int SEGMENTS = 32;
			for (int ring = 0; ring < 3; ring++) {
				for (int i = 0; i < SEGMENTS; i++) {
					const float a0 = 2.f * glm::pi<float>() * (float)i / (float)SEGMENTS;
					const float a1 = 2.f * glm::pi<float>() * (float)(i + 1) / (float)SEGMENTS;
					auto point = [&](float a) {
						const float c = radius * std::cos(a);
						const float d = radius * std::sin(a);
						return ring == 0 ? at(c, d, 0.f) : ring == 1 ? at(c, 0.f, d) : at(0.f, c, d);
					};
					line(point(a0), point(a1), color);
				}
			}
			line(camera.position, at(0.f, 0.f, -2.f * radius), color);
			continue;
		}

		//a short pyramid along -z: the apex is the camera, the base a frame at `depth` sized by the fov, moved by the
		//shift. An orthographic camera's is a box as large as its view volume
		const bool orthographic = camera.kind == CameraKind::Orthographic;
		const float halfHeight = orthographic ? 0.5f * camera.orthoHeight : depth * glm::tan(glm::radians(camera.vfovDegrees) * 0.5f);
		const float halfWidth = halfHeight * aspect;
		const glm::vec2 offset = 2.f * halfHeight * camera.shift;
		auto corner = [&](float x, float y) { return at(x + offset.x, y + offset.y, -depth); };
		const glm::vec3 c0 = corner(-halfWidth, -halfHeight);
		const glm::vec3 c1 = corner(halfWidth, -halfHeight);
		const glm::vec3 c2 = corner(halfWidth, halfHeight);
		const glm::vec3 c3 = corner(-halfWidth, halfHeight);
		if (orthographic) {
			auto back = [&](float x, float y) { return at(x + offset.x, y + offset.y, 0.f); };
			const glm::vec3 b0 = back(-halfWidth, -halfHeight);
			const glm::vec3 b1 = back(halfWidth, -halfHeight);
			const glm::vec3 b2 = back(halfWidth, halfHeight);
			const glm::vec3 b3 = back(-halfWidth, halfHeight);
			line(b0, c0, color);
			line(b1, c1, color);
			line(b2, c2, color);
			line(b3, c3, color);
			line(b0, b1, color);
			line(b1, b2, color);
			line(b2, b3, color);
			line(b3, b0, color);
		} else {
			const glm::vec3 apex = camera.position;
			line(apex, c0, color);
			line(apex, c1, color);
			line(apex, c2, color);
			line(apex, c3, color);
		}
		line(c0, c1, color);
		line(c1, c2, color);
		line(c2, c3, color);
		line(c3, c0, color);
		//an "up" triangle on the top edge, so the roll is readable
		const glm::vec3 top = corner(0.f, halfHeight * 1.5f);
		line(c3, top, color);
		line(top, c2, color);
	}
}

void VulkanEngine::drawLightOverlays(const ViewportView& view)
{
	const std::vector<SceneLight>& lights = m_raytraceScene.lights();
	if (lights.empty()) {
		return;
	}
	auto line = [&](const glm::vec3& a, const glm::vec3& b, ImU32 color) { view.line(a, b, color); };

	for (const SceneLight& light : lights) {
		//the light's own colour, brightened so a dim or dark one still shows
		const glm::vec3 rgb = glm::max(exactRgb(light.color, SpectrumRole::Illuminant, {}), glm::vec3(0.f));
		const glm::vec3 tint = rgb / std::max({ rgb.r, rgb.g, rgb.b, 1e-3f });
		const ImU32 color = IM_COL32((int)(155 + 100 * tint.r), (int)(155 + 100 * tint.g), (int)(155 + 100 * tint.b), 230);
		const glm::mat4 transform = light.transform();
		auto at = [&](float x, float y, float z) { return glm::vec3(transform * glm::vec4(x, y, z, 1.f)); };

		//a small star at the position, which every kind has
		const float star = 0.12f;
		line(at(-star, 0.f, 0.f), at(star, 0.f, 0.f), color);
		line(at(0.f, -star, 0.f), at(0.f, star, 0.f), color);
		line(at(0.f, 0.f, -star), at(0.f, 0.f, star), color);

		if (light.kind == LightKind::Spot) {
			//the outer cone, cut off a short way down its axis
			const float depth = 0.6f;
			const float radius = depth * std::tan(glm::radians(std::clamp(light.outerConeDegrees, 0.f, 89.f)));
			glm::vec3 previous = at(radius, 0.f, -depth);
			for (int i = 1; i <= 16; i++) {
				const float angle = (float)i / 16.f * 2.f * glm::pi<float>();
				const glm::vec3 point = at(radius * std::cos(angle), radius * std::sin(angle), -depth);
				line(previous, point, color);
				if (i % 4 == 0) {
					line(light.position, point, color);
				}
				previous = point;
			}
		} else if (light.kind == LightKind::Directional) {
			//an arrow the way the light travels
			const float length = 0.8f;
			line(light.position, at(0.f, 0.f, -length), color);
			line(at(0.f, 0.f, -length), at(0.08f, 0.f, -length + 0.15f), color);
			line(at(0.f, 0.f, -length), at(-0.08f, 0.f, -length + 0.15f), color);
			line(at(0.f, 0.f, -length), at(0.f, 0.08f, -length + 0.15f), color);
			line(at(0.f, 0.f, -length), at(0.f, -0.08f, -length + 0.15f), color);
		}
	}
}

namespace {

constexpr const char* ENVIRONMENT_MAP_DISPLAY_NAME = "Environment Map";

// A glTF file's KHR_lights_punctual light as a scene light: placed and aimed by its node, its
// photometric intensity converted to the tracer's radiometric units, and tagged with the model so
// that removing the model removes it
SceneLight sceneLightOf(const GltfPunctualLight& source, const std::string& modelKey)
{
	SceneLight light;
	light.name = source.name;
	light.modelKey = modelKey;
	switch (source.type) {
	case GltfPunctualLight::Type::Point:
		light.kind = LightKind::Point;
		break;
	case GltfPunctualLight::Type::Spot:
		light.kind = LightKind::Spot;
		break;
	case GltfPunctualLight::Type::Directional:
		light.kind = LightKind::Directional;
		break;
	}
	light.position = glm::vec3(source.worldTransform[3]);
	//the node's rotation, with any scale it carries normalised away: a glTF light shines down its
	//node's -z, which is SceneLight's convention too
	const glm::mat3 basis(glm::normalize(glm::vec3(source.worldTransform[0])), glm::normalize(glm::vec3(source.worldTransform[1])), glm::normalize(glm::vec3(source.worldTransform[2])));
	light.orientation = glm::normalize(glm::quat_cast(basis));
	light.color = rgbSpectrum(glm::max(source.color, glm::vec3(0.f)));
	//candela (point, spot) or lux (directional) over 683 lm/W
	light.intensity = std::max(source.intensity, 0.f) * PHOTOMETRIC_TO_RADIOMETRIC;
	light.range = std::max(source.range, 0.f);
	light.outerConeDegrees = std::clamp(glm::degrees(source.outerConeAngle), 0.f, 90.f);
	light.innerConeDegrees = std::clamp(glm::degrees(source.innerConeAngle), 0.f, light.outerConeDegrees);
	return light;
}
constexpr const char* SCENE_FILE_EXTENSION = "gltf";

std::filesystem::path absoluteNormalized(const std::filesystem::path& path)
{
	std::error_code ec;
	std::filesystem::path absolute = std::filesystem::absolute(path, ec);
	return (ec ? path : absolute).lexically_normal();
}

}

void VulkanEngine::rebuildSceneDerivedData()
{
	m_raytraceMeshData = buildRaytraceMeshData(this);

	//the resolved nodes, so drawing the scene's mesh objects is a lookup rather than a tree walk
	//per object per frame. Raw pointers, valid exactly as long as m_models holds their model -
	//which is why nothing else may rebuild this
	m_meshNodes.clear();
	forEachMeshNode(this, [this](std::string_view modelKey, uint32_t nodeIndex, const MeshNode& node) {
		m_meshNodes[{ std::string(modelKey), nodeIndex }] = &node;
	});

	//the path tracer's base-colour textures, from the same models, so a triangle's material layer
	//and the array it indexes are always built together
	m_raytraceTextures.rebuild(this);
	//and the glTF materials' library entries, whose texture layers just moved
	m_raytraceScene.refreshModelMaterials([this](MaterialDef& material) {
		if (!material.origin.has_value()) {
			return false;
		}
		const auto model = m_models.find(material.origin->modelKey);
		if (model == m_models.end() || model->second == nullptr || material.origin->index >= model->second->materialsByIndex.size()) {
			return false;
		}
		const MaterialDef before = material;
		refreshModelMaterialDef(material, *model->second->materialsByIndex[material.origin->index], m_raytraceTextures);
		return !(before == material);
	});

	//the path tracer's BLASes, one per mesh not already built: the BVH work belongs to loading, not
	//to the first render
	m_raytracer.prepareGeometry(this);

	m_sceneRevision++;
}

void VulkanEngine::syncSceneImages()
{
	for (const SceneLight& light : m_raytraceScene.lights()) {
		if ((light.kind == LightKind::Projection || light.kind == LightKind::Goniometric) && !light.image.empty()) {
			const LightImageRequest request = lightImageRequest(light.image);
			m_textureLibrary.add(request.source, request.colorSpace, request.sampling, std::filesystem::path(light.image).filename().string());
		}
	}
	//an aperture image is read as a light's is: its luminance is what the render samples the lens by
	for (const SceneCamera& camera : m_raytraceScene.cameras()) {
		if (camera.apertureShape == ApertureShape::Image && !camera.apertureImage.empty()) {
			const LightImageRequest request = lightImageRequest(camera.apertureImage);
			m_textureLibrary.add(request.source, request.colorSpace, request.sampling, std::filesystem::path(camera.apertureImage).filename().string());
		}
	}
}

void VulkanEngine::resolveModelTextures(MaterialDef& material, const std::string& key, const LoadedModel& model, uint32_t index)
{
	//a pbrt model's references name its images by index: each becomes the library's entry for that file (or, for an
	//image the import decoded itself, for the model's copy of it), read as the importer said
	if (model.imported.has_value()) {
		for (auto& [name, binding] : material.textures) {
			if (binding.importImage < 0 || (size_t)binding.importImage >= model.imported->images.size()) {
				continue;
			}
			const ImportedImage& image = model.imported->images[(size_t)binding.importImage];
			TextureSource source;
			//an image the import decoded itself (its pixels are freed once uploaded, its size kept) has no file to read
			if (!image.rgba8.empty() || image.width != 0) {
				source.modelKey = key;
				source.modelImage = binding.importImage;
			} else {
				source.path = image.path;
				source.encoding = image.encoding == ImageEncoding::Linear ? TextureEncoding::Linear : image.encoding == ImageEncoding::Gamma ? TextureEncoding::Gamma : TextureEncoding::Srgb;
				source.gamma = image.gamma;
				source.invert = image.invert;
			}
			binding.texture = m_textureLibrary.add(source, binding.importColorSpace, binding.importSampling, image.path.filename().string());
			binding.derived = true;
			binding.importImage = -1;
		}
		return;
	}
	//a glTF material's four textures, read as glTF defines them: the base color (and its alpha as the coverage of a
	//MASK material), metallic and roughness from the blue and green of one texture, the normal map, the emission
	if (index >= model.materialsByIndex.size() || model.materialsByIndex[index] == nullptr) {
		return;
	}
	const GLTFMaterial& gltf = *model.materialsByIndex[index];
	auto bind = [&](const char* name, const GltfTextureRef& ref, TextureColorSpace colorSpace, TextureChannel channel) {
		if (ref.image < 0) {
			return;
		}
		TextureSource source;
		source.modelKey = key;
		source.modelImage = ref.image;
		TextureBinding binding;
		binding.texture = m_textureLibrary.add(source, colorSpace, ref.sampling);
		binding.channel = channel;
		binding.uvSet = std::min(ref.texCoord, 1u);
		binding.offset = ref.offset;
		binding.scale = ref.scale;
		binding.rotation = ref.rotation;
		binding.derived = true;
		material.textures[name] = binding;
	};
	bind("albedo", gltf.baseColorTexture, TextureColorSpace::Srgb, TextureChannel::Rgb);
	if (gltf.alphaMode == GltfAlphaMode::Mask) {
		bind("alpha", gltf.baseColorTexture, TextureColorSpace::Srgb, TextureChannel::Alpha);
	}
	bind("roughness", gltf.metalRoughTexture, TextureColorSpace::Linear, TextureChannel::Green);
	bind("metallic", gltf.metalRoughTexture, TextureColorSpace::Linear, TextureChannel::Blue);
	bind("normal", gltf.normalTexture, TextureColorSpace::Linear, TextureChannel::Rgb);
	bind("emission", gltf.emissiveTexture, TextureColorSpace::Srgb, TextureChannel::Rgb);
}

std::string VulkanEngine::addModel(std::shared_ptr<LoadedModel> model, const std::filesystem::path& path)
{
	model->sourcePath = absoluteNormalized(path);

	//keyed by file stem, made unique so the same file can be in the scene twice. A .pbrt.gz keeps its stem's stem
	std::filesystem::path stemPath = path.stem();
	if (stemPath.extension() == ".pbrt") {
		stemPath = stemPath.stem();
	}
	const std::string stem = stemPath.string().empty() ? "model" : stemPath.string();
	std::string key = stem;
	for (int suffix = 2; m_models.count(key) != 0; suffix++) {
		key = fmt::format("{}_{}", stem, suffix);
	}
	m_models[key] = std::move(model);

	rebuildSceneDerivedData();

	//one library entry per material, named as the file names it, which is what its objects are shaded with and what
	//the Materials window edits in place
	const LoadedModel& loaded = *m_models[key];
	std::vector<uint64_t> modelMaterialIds;
	for (uint32_t i = 0; i < loaded.materialsByIndex.size(); i++) {
		MaterialDef material = modelMaterialDef(loaded, i, m_raytraceTextures);
		resolveModelTextures(material, key, loaded, i);
		material.name = i < loaded.materialNames.size() ? loaded.materialNames[i] : fmt::format("material{}", i);
		material.origin = ModelMaterialOrigin { key, i };
		modelMaterialIds.push_back(m_raytraceScene.addMaterial(std::move(material)));
	}
	//an imported mix names its children by their index in the model; now that they have ids, it holds those
	for (const uint64_t id : modelMaterialIds) {
		MaterialDef* material = m_raytraceScene.findMaterial(id);
		for (auto& [name, value] : material->params) {
			MaterialLink* link = std::get_if<MaterialLink>(&value);
			if (link != nullptr && link->modelIndex >= 0) {
				link->id = (size_t)link->modelIndex < modelMaterialIds.size() ? modelMaterialIds[link->modelIndex] : 0;
				link->modelIndex = -1;
			}
		}
	}

	//a freshly imported model places one scene object per mesh node, at the transform it was
	//authored with, so importing looks exactly as it always did - the objects are what makes each
	//of those nodes movable and deletable afterwards
	m_raytraceScene.addMeshObjects(defaultMeshObjects(*m_raytraceMeshData, key));
	return key;
}

IoResult VulkanEngine::importModel(const std::filesystem::path& path)
{
	const std::string name = path.filename().string();
	const bool pbrt = name.ends_with(".pbrt") || name.ends_with(".pbrt.gz");
	return pbrt ? importPbrt(path) : importGltf(path);
}

IoResult VulkanEngine::importGltf(const std::filesystem::path& path)
{
	std::string error;
	auto loaded = loadGltf(this, path.string(), &error);
	if (!loaded.has_value()) {
		return IoResult::failure(error);
	}
	const std::string key = addModel(*loaded, path);

	//and one scene light per KHR_lights_punctual light, where its node puts it
	std::vector<SceneLight> lights;
	for (const GltfPunctualLight& light : m_models[key]->lights) {
		lights.push_back(sceneLightOf(light, key));
	}
	const size_t lightCount = lights.size();
	m_raytraceScene.addLights(std::move(lights));

	return IoResult::success(fmt::format("Imported '{}' as '{}'. Scene now has {} model(s), {} mesh object(s), {} triangles{}",
		path.filename().string(), key, m_models.size(), m_raytraceScene.meshObjects().size(), m_raytraceMeshData->triangleCount,
		lightCount > 0 ? fmt::format(", and the file's {} light(s)", lightCount) : ""));
}

IoResult VulkanEngine::importPbrt(const std::filesystem::path& path)
{
	const auto started = std::chrono::steady_clock::now();
	ImportedModel imported;
	std::string error;
	if (!importPbrtFile(path, imported, &error)) {
		return IoResult::failure(fmt::format("'{}' is not a scene pbrt can read: {}", path.filename().string(), error));
	}
	const std::string key = addModel(loadImportedModel(this, std::move(imported)), path);
	const ImportedModel& model = *m_models[key]->imported;

	//its lights and shapes belong to it, as a glTF file's lights do; its camera is the scene's
	std::vector<SceneLight> lights = model.lights;
	for (SceneLight& light : lights) {
		light.modelKey = key;
	}
	m_raytraceScene.addLights(std::move(lights));
	std::vector<SceneShape> shapes;
	for (const ImportedShape& imported : model.shapes) {
		SceneShape shape = imported.shape;
		shape.modelKey = key;
		const MaterialDef* material = m_raytraceScene.findModelMaterial(key, imported.material);
		shape.materialId = material != nullptr ? material->id : 0;
		shapes.push_back(std::move(shape));
	}
	m_raytraceScene.addShapes(std::move(shapes));
	m_modelCameras.erase(key);
	for (const SceneCamera& camera : model.cameras) {
		m_modelCameras[key] = m_raytraceScene.addCamera(camera);
	}
	if (model.environment.has_value()) {
		IoResult environment = installEnvironmentMap(model.environment->rgba.data(), model.environment->width, model.environment->height, model.environment->label, {});
		if (environment.ok) {
			m_environmentModelKey = key;
			m_environmentIntensity = model.environment->intensity;
			//the import turned the map into the renderer's frame already
			m_environmentRotation = 0.f;
		}
	} else if (m_environmentMap.image == VK_NULL_HANDLE) {
		//pbrt's world without an infinite light is black past its geometry, which the gradient renders
		m_backgroundMode = BackgroundMode::Gradient;
	}
	m_showImportReport = true;
	m_importReportModel = key;

	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	return IoResult::success(fmt::format("Imported pbrt scene '{}' as '{}' in {:.1f} s: {} mesh object(s), {} triangles, {} shape(s), {} light(s){}. {} line(s) in its import report",
		path.filename().string(), key, seconds, m_raytraceScene.meshObjects().size(), m_raytraceMeshData->triangleCount, model.shapes.size(), model.lights.size(),
		model.environment.has_value() ? ", an environment" : "", model.report.messages.size()));
}

void VulkanEngine::usePbrtSettings(const std::string& modelKey)
{
	const auto model = m_models.find(modelKey);
	if (model == m_models.end() || model->second == nullptr || !model->second->imported.has_value() || !model->second->imported->settings.has_value()) {
		return;
	}
	const ImportedSettings& settings = *model->second->imported->settings;
	RenderSettings render = settings.render;
	//what the file does not decide stays the scene's
	const RenderSettings current = m_raytracer.settings();
	render.matchViewport = false;
	render.samplesPerFrame = current.samplesPerFrame;
	render.restartOnChange = current.restartOnChange;
	render.errorBoundedOffsets = current.errorBoundedOffsets;
	render.watertightTriangles = current.watertightTriangles;
	render.outlierKappa = current.outlierKappa;
	render.lightGroups = current.lightGroups;
	m_raytracer.setSettings(render);
	KernelSelection kernels = m_raytracer.kernels();
	if (const IntegratorId integrator = findIntegrator(settings.integrator); integrator != IntegratorId::Count) {
		kernels.integrator = integrator;
	}
	if (!settings.sampler.empty()) {
		const std::span<const KernelVariant> samplers = kernelVariants(KernelSlot::GenerateSamples);
		for (uint32_t i = 0; i < (uint32_t)samplers.size(); i++) {
			if (settings.sampler == samplers[i].id) {
				kernels[KernelSlot::GenerateSamples] = i;
			}
		}
	}
	//pbrt filters every sample by importance and sums them with signed weights: kernel 00's filtered camera and a
	//film that keeps sums, which are a new scene's defaults
	const KernelSelection defaults = defaultKernelSelection();
	kernels[KernelSlot::GenerateCameraRays] = defaults[KernelSlot::GenerateCameraRays];
	kernels[KernelSlot::UpdateFilm] = defaults[KernelSlot::UpdateFilm];
	m_raytracer.setKernels(kernels);
	m_raytracer.setAccelSettings(this, m_raytracer.accelSettings());
	if (const auto camera = m_modelCameras.find(modelKey); camera != m_modelCameras.end() && m_raytraceScene.findCamera(camera->second) != nullptr) {
		m_raytracer.setRenderCamera(camera->second);
	}
	//pbrt's "ambientocclusion" integrator is the ambient occlusion debug view
	if (settings.ambientOcclusionDistance.has_value()) {
		m_raytracer.gpu().debugView = CrtDebugView::AmbientOcclusion;
		m_raytracer.gpu().aoDistance = std::isfinite(*settings.ambientOcclusionDistance) ? *settings.ambientOcclusionDistance : 0.f;
	}
	m_lastFileResult = IoResult::success(fmt::format("Render settings from '{}': {} x {}, {} samples, {}", modelKey, render.width, render.height, render.maxSamples,
		settings.sampler.empty() ? "the scene's sampler" : "the " + settings.sampler + " sampler"));
}

void VulkanEngine::removeModel(const std::string& name)
{
	auto it = m_models.find(name);
	if (it == m_models.end()) {
		return;
	}

	//frames in flight may still reference the model's buffers, images and descriptor pools, and
	//LoadedModel::clearAll() destroys them the moment the last reference goes. A wait is the
	//simple, obviously-correct way to retire them for a menu-driven action
	vkDeviceWaitIdle(m_device);
	m_textureLibrary.removeModel(name);
	m_models.erase(it);
	//the objects placing this model's nodes go with it: their geometry has just stopped existing.
	//So do the lights it brought, which belong to it the same way
	m_raytraceScene.removeMeshObjectsOf(name);
	m_raytraceScene.removeLightsOf(name);
	m_raytraceScene.removeShapesOf(name);
	m_modelCameras.erase(name);
	if (m_environmentModelKey == name) {
		clearEnvironmentMap();
	}
	//and its materials, except the ones a shape or an override still uses, which become the user's
	const std::vector<std::string> kept = m_raytraceScene.removeMaterialsOf(name);
	rebuildSceneDerivedData();
	if (!kept.empty()) {
		m_lastFileResult = IoResult::success(fmt::format("Removed '{}'. Its material(s) {} are still in use and are now user materials, without their textures", name, fmt::join(kept, ", ")));
	}
}

void VulkanEngine::clearModels()
{
	if (m_models.empty()) {
		return;
	}
	vkDeviceWaitIdle(m_device);
	for (const auto& [name, model] : m_models) {
		m_textureLibrary.removeModel(name);
		m_raytraceScene.removeLightsOf(name);
		m_raytraceScene.removeShapesOf(name);
		m_raytraceScene.removeMaterialsOf(name);
	}
	if (!m_environmentModelKey.empty()) {
		clearEnvironmentMap();
	}
	m_models.clear();
	m_modelCameras.clear();
	m_raytraceScene.replaceMeshObjects({});
	rebuildSceneDerivedData();
}

void VulkanEngine::newScene()
{
	m_raytracer.discardRender(this);
	clearModels();
	clearEnvironmentMap();
	m_firstPersonCameraId = 0;
	m_raytraceScene.replaceShapes({});
	m_raytraceScene.replaceLights({});
	m_raytraceScene.replaceBeamAxes({});
	m_raytraceScene.setBeamPreview({});
	m_raytracer.setGuideAxis(0);
	//the optics and the sensor stacks before the materials: each removes the slot materials it owns
	m_raytraceScene.replaceOptics({});
	m_raytraceScene.replaceSensorStacks({});
	m_raytraceScene.replaceMaterials({});
	m_raytraceScene.setBench(BenchSettings {});
	m_opticalLibrary.clearScene();
	setLengthUnit(LengthUnit::Meters);
	//the view stays where it is
	m_viewportNav.settings = ViewportCameraSettings {};
	//an empty scene still has somewhere to render from
	SceneCamera camera;
	camera.name = "render_camera";
	m_raytraceScene.replaceCameras({ camera });
	m_raytracer.setSettings(RenderSettings {});
	m_raytracer.setExportSettings(ExportSettings {});
	m_raytraceScene.setLightGroups({}, 0);
	m_raytracer.setKernels(defaultKernelSelection());
	m_raytracer.setAccelSettings(this, defaultAccelSettings());
	m_raytracer.setRenderDetector(0);
	m_environmentIntensity = 1.f;
	m_environmentRotation = 0.f;
	m_backgroundMode = BackgroundMode::Gradient;
	m_backgroundColor = glm::vec3(0.f);
	m_scenePath.clear();
	//the Render scene template, which turns on no capability
	m_capabilities = {};
	m_sceneTemplate = DEFAULT_TEMPLATE_KEY;
	m_pendingRecommendations.clear();
	updateBench();
	m_lastFileResult = IoResult::success("New empty scene");
}

IoResult VulkanEngine::saveScene(const std::filesystem::path& path)
{
	IoResult result = saveSceneFile(describeScene(), path);
	if (result.ok) {
		m_scenePath = absoluteNormalized(path);
	}
	return result;
}

SceneDescription VulkanEngine::describeScene()
{
	SceneDescription scene;
	//the models, and the index each one's key maps to, since a mesh object refers to its model by
	//position in this list rather than by a key the next load would re-derive
	std::map<std::string, int> modelIndices;
	for (const auto& [name, model] : m_models) {
		modelIndices[name] = (int)scene.modelPaths.size();
		scene.modelPaths.push_back(model->sourcePath);
	}

	//the library: every user material in full, referenced by its position in the file's list, and
	//each glTF material only as the parameters the user changed
	std::map<uint64_t, int> userIndices;
	for (const MaterialDef& material : m_raytraceScene.materials()) {
		//an optical element's slots are saved with the element
		if (material.opticOrigin.has_value()) {
			continue;
		}
		if (!material.origin.has_value()) {
			userIndices[material.id] = (int)scene.materials.size();
			scene.materials.push_back(material);
			continue;
		}
		const auto model = modelIndices.find(material.origin->modelKey);
		const bool userTextures = std::any_of(material.textures.begin(), material.textures.end(), [](const auto& entry) { return !entry.second.derived; });
		if ((material.edited.empty() && material.lightGroup == 0 && !userTextures) || model == modelIndices.end()) {
			continue;
		}
		SceneMaterialOverride edit;
		edit.model = model->second;
		edit.modelMaterial = material.origin->index;
		edit.lightGroup = material.lightGroup;
		for (const std::string& name : material.edited) {
			edit.params[name] = materialValue(material, name);
		}
		for (const auto& [name, binding] : material.textures) {
			if (!binding.derived) {
				edit.textures[name] = binding;
			}
		}
		scene.materialOverrides.push_back(std::move(edit));
	}
	//the textures the user gave materials, which name library entries no file holds: each becomes an index into the
	//scene's list of image files. A texture of a model's own image cannot be named by a file and is left out
	auto texturesToFile = [&](std::map<std::string, TextureBinding, std::less<>>& textures) {
		for (auto it = textures.begin(); it != textures.end();) {
			TextureBinding& binding = it->second;
			const TextureEntry* entry = binding.derived ? nullptr : m_textureLibrary.find(binding.texture);
			if (entry == nullptr || entry->source.isModelImage()) {
				it = binding.derived ? std::next(it) : textures.erase(it);
				continue;
			}
			SceneTextureRecord record { entry->source.path, entry->colorSpace, entry->sampling, entry->source.encoding, entry->source.gamma, entry->source.invert };
			const auto found = std::find_if(scene.textures.begin(), scene.textures.end(), [&](const SceneTextureRecord& r) {
				return r.path == record.path && r.colorSpace == record.colorSpace && r.sampling == record.sampling && r.encoding == record.encoding && r.gamma == record.gamma && r.invert == record.invert;
			});
			binding.fileTexture = (int)(found - scene.textures.begin());
			if (found == scene.textures.end()) {
				scene.textures.push_back(record);
			}
			++it;
		}
	};
	for (MaterialDef& material : scene.materials) {
		texturesToFile(material.textures);
	}
	for (SceneMaterialOverride& edit : scene.materialOverrides) {
		texturesToFile(edit.textures);
	}
	auto materialRef = [&](uint64_t id) {
		SceneMaterialRef ref;
		const MaterialDef* material = m_raytraceScene.findMaterial(id);
		if (material == nullptr) {
			return ref;
		}
		if (!material->origin.has_value()) {
			ref.user = userIndices[id];
		} else if (const auto model = modelIndices.find(material->origin->modelKey); model != modelIndices.end()) {
			ref.model = model->second;
			ref.modelMaterial = material->origin->index;
		}
		return ref;
	};

	//a mix's children are library ids, which no file holds: each becomes the reference a shape's material would be
	auto linksToReferences = [&](ParamSet& params) {
		for (auto& [name, value] : params) {
			if (MaterialLink* link = std::get_if<MaterialLink>(&value)) {
				const SceneMaterialRef ref = materialRef(link->id);
				*link = MaterialLink { .id = 0, .fileUser = ref.user, .fileModel = ref.model, .fileModelMaterial = ref.modelMaterial };
			}
		}
	};
	for (MaterialDef& material : scene.materials) {
		linksToReferences(material.params);
	}
	for (SceneMaterialOverride& edit : scene.materialOverrides) {
		linksToReferences(edit.params);
	}

	scene.hasMeshObjects = true;
	for (const SceneMeshObject& object : m_raytraceScene.meshObjects()) {
		const auto found = modelIndices.find(object.modelKey);
		if (found == modelIndices.end()) {
			//an object whose model is gone has nothing to be written against
			continue;
		}
		SceneMeshObjectRecord record;
		record.model = found->second;
		record.nodeIndex = object.nodeIndex;
		record.name = object.name;
		record.transform = object.transform;
		record.visible = object.visible;
		record.materialMode = object.materialMode;
		if (object.materialMode == MeshMaterialMode::Override) {
			record.material = materialRef(object.materialId);
		}
		scene.meshObjects.push_back(std::move(record));
	}

	scene.environmentMapPath = m_environmentMapPath;
	if (const auto found = modelIndices.find(m_environmentModelKey); !m_environmentModelKey.empty() && found != modelIndices.end()) {
		scene.environmentModel = found->second;
	}
	for (const SceneShape& shape : m_raytraceScene.shapes()) {
		SceneShapeRecord record { shape, materialRef(shape.materialId) };
		//a shape of a model that is not in the file becomes the user's own
		const auto found = modelIndices.find(shape.modelKey);
		record.model = shape.modelKey.empty() || found == modelIndices.end() ? -1 : found->second;
		if (record.model < 0) {
			record.shape.modelKey.clear();
		}
		scene.shapes.push_back(std::move(record));
	}
	scene.cameras = m_raytraceScene.cameras();
	scene.hasLights = true;
	for (const SceneLight& light : m_raytraceScene.lights()) {
		SceneLightRecord record;
		record.light = light;
		//a light of a model that is not in the file becomes the user's own
		const auto found = modelIndices.find(light.modelKey);
		record.model = light.modelKey.empty() || found == modelIndices.end() ? -1 : found->second;
		if (record.model < 0) {
			record.light.modelKey.clear();
		}
		scene.lights.push_back(std::move(record));
	}
	for (size_t i = 0; i < scene.cameras.size(); i++) {
		if (scene.cameras[i].id == m_raytracer.renderCameraId()) {
			scene.renderCamera = (int)i;
		}
	}
	//the detector render target, counted over the optics and then the sensor stacks' members, each stack's in order, as
	//the load makes them
	{
		std::vector<uint64_t> detectorOrder;
		for (const SceneOptic& optic : m_raytraceScene.optics()) {
			if (optic.stack == 0) {
				detectorOrder.push_back(optic.id);
			}
		}
		for (const SensorStack& stack : m_raytraceScene.sensorStacks()) {
			for (const SceneOptic& optic : m_raytraceScene.optics()) {
				if (optic.stack == stack.id) {
					detectorOrder.push_back(optic.id);
				}
			}
		}
		for (size_t i = 0; i < detectorOrder.size(); i++) {
			if (m_raytracer.renderDetectorId() != 0 && detectorOrder[i] == m_raytracer.renderDetectorId()) {
				scene.renderDetector = (int)i;
			}
		}
	}
	scene.render = m_raytracer.settings();
	scene.lightGroupNames = m_raytraceScene.lightGroupNames();
	scene.environmentGroup = m_raytraceScene.environmentGroup();
	scene.exportSettings = m_raytracer.exportSettings();
	scene.spectralDisplay = m_raytracer.gpu().spectralDisplay;
	//which wavefront kernel runs in each slot, and how the BVH they read is built
	scene.kernels = m_raytracer.kernels();
	scene.accel = m_raytracer.accelSettings();
	scene.environmentIntensity = m_environmentIntensity;
	scene.environmentRotation = m_environmentRotation;
	scene.backgroundMode = m_backgroundMode;
	scene.backgroundColor = m_backgroundColor;
	scene.capabilities = capabilityKeys(m_capabilities);
	scene.templateKey = m_sceneTemplate;
	//the optical elements, with only the slots that differ from the kind's defaults; the library's scene-owned part
	for (const SceneOptic& optic : m_raytraceScene.optics()) {
		//a stack's members are its stack's, saved below
		if (optic.stack != 0) {
			continue;
		}
		SceneOpticRecord record;
		record.optic = optic;
		record.optic.id = 0;
		for (size_t slot = 0; slot < opticTraits(optic.kind).slots.size(); slot++) {
			const MaterialDef* material = m_raytraceScene.findMaterial(optic.materials[slot]);
			const MaterialDef fallback = defaultSlotMaterial(optic, slot);
			if (material != nullptr && (material->type != fallback.type || material->params != fallback.params || material->lightGroup != fallback.lightGroup)) {
				record.slots[slot] = *material;
			}
		}
		scene.optics.push_back(std::move(record));
	}
	for (const SensorStack& stack : m_raytraceScene.sensorStacks()) {
		SceneSensorStackRecord record;
		record.stack = stack;
		record.stack.id = 0;
		for (size_t slot = 0; slot < opticTraits(stack.sensor.kind).slots.size(); slot++) {
			const MaterialDef* material = m_raytraceScene.findMaterial(stack.sensor.materials[slot]);
			const MaterialDef fallback = defaultSlotMaterial(stack.sensor, slot);
			if (material != nullptr && (material->type != fallback.type || material->params != fallback.params || material->lightGroup != fallback.lightGroup)) {
				record.slots[slot] = *material;
			}
		}
		scene.sensorStacks.push_back(std::move(record));
	}
	//the beam axes, naming the optics by their place in scene.optics and the cameras by theirs in scene.cameras. A visit
	//of an optic the file does not hold (a stack's member) goes as Remove takes it
	{
		std::map<uint64_t, uint64_t> opticIndex;
		for (const SceneOptic& optic : m_raytraceScene.optics()) {
			if (optic.stack == 0) {
				opticIndex.emplace(optic.id, opticIndex.size());
			}
		}
		std::map<uint64_t, uint64_t> cameraIndex;
		for (const SceneCamera& camera : m_raytraceScene.cameras()) {
			cameraIndex.emplace(camera.id, cameraIndex.size());
		}
		for (const BeamAxis& editorAxis : m_raytraceScene.beamAxes()) {
			if (editorAxis.id == m_raytracer.guideAxisId()) {
				scene.guideAxis = (int)scene.beamAxes.size();
			}
			BeamAxis axis = editorAxis;
			axis.id = 0;
			const std::map<uint64_t, uint64_t>& sources = axis.source.kind == BeamSourceKind::Camera ? cameraIndex : opticIndex;
			if (const auto found = sources.find(axis.source.id); axis.source.kind != BeamSourceKind::None && found != sources.end()) {
				axis.source.id = found->second;
			} else {
				axis.source = {};
			}
			std::vector<uint32_t> unsaved;
			for (const BeamAxisNode& node : axis.nodes) {
				if (!opticIndex.contains(node.optic)) {
					unsaved.push_back(node.id);
				}
			}
			for (uint32_t id : unsaved) {
				removeBeamNode(axis, id);
			}
			for (BeamAxisNode& node : axis.nodes) {
				node.optic = opticIndex.at(node.optic);
			}
			renumberBeamNodes(axis);
			scene.beamAxes.push_back({ std::move(axis) });
		}
	}
	scene.glassCatalogs = m_opticalLibrary.loadedCatalogPaths();
	scene.userGlasses = m_opticalLibrary.userGlasses();
	scene.userCoatings = m_opticalLibrary.userCoatings();
	scene.lengthUnit = m_lengthUnit;
	scene.bench = m_raytraceScene.bench();
	const Camera& view = m_viewportNav.camera;
	scene.viewport = SceneViewportRecord { m_viewportNav.settings, view.position, glm::normalize(glm::quat_cast(glm::mat3(view.getRotationMatrix()))), view.pivot,
		m_raytraceScene.beamPreview() };
	return scene;
}

IoResult VulkanEngine::openScene(const std::filesystem::path& path, bool* loaded)
{
	SceneDescription scene;
	IoResult parsed = loadSceneFile(path, scene);
	if (loaded != nullptr) {
		*loaded = parsed.ok;
	}
	if (!parsed.ok) {
		//the current scene is untouched
		return parsed;
	}

	//only now that the file itself is known good does the current scene go. A model or map the
	//file refers to may still be missing; the rest of the scene loads regardless and the
	//problems are reported together
	m_raytracer.discardRender(this);
	clearModels();
	clearEnvironmentMap();
	//the previous scene's lights go too: a file older than lights has only what its models bring.
	//So do its user materials; each import below brings its model's
	m_raytraceScene.replaceLights({});
	m_raytraceScene.replaceBeamAxes({});
	m_raytraceScene.replaceOptics({});
	m_raytraceScene.replaceSensorStacks({});
	m_raytraceScene.replaceMaterials({});
	std::vector<std::string> problems;
	//the optical library's scene-owned part before anything names its glasses or coatings
	m_opticalLibrary.clearScene();
	for (const std::filesystem::path& catalog : scene.glassCatalogs) {
		std::vector<std::string> warnings;
		std::string error;
		if (!m_opticalLibrary.loadCatalog(catalog, warnings, &error)) {
			problems.push_back(fmt::format("glass catalog {}: {}", catalog.filename().string(), error));
		}
	}
	for (const OpticGlass& glass : scene.userGlasses) {
		m_opticalLibrary.setUserGlass(glass);
	}
	for (const OpticCoating& coating : scene.userCoatings) {
		m_opticalLibrary.setUserCoating(coating);
	}
	//the key each model actually got, by its position in the file's list: importGltf() derives a
	//key from the file stem and uniquifies it, so the same file twice is two different keys
	std::vector<std::string> modelKeys;
	for (const std::filesystem::path& modelPath : scene.modelPaths) {
		const size_t before = m_models.size();
		IoResult imported = importModel(modelPath);
		if (!imported.ok) {
			problems.push_back(imported.message);
			modelKeys.push_back({});
			continue;
		}
		//the key the import just added, found by what is new rather than by re-deriving the rule
		std::string key;
		if (m_models.size() > before) {
			for (const auto& [name, model] : m_models) {
				if (model != nullptr && model->sourcePath == absoluteNormalized(modelPath) && std::find(modelKeys.begin(), modelKeys.end(), name) == modelKeys.end()) {
					key = name;
					break;
				}
			}
		}
		modelKeys.push_back(std::move(key));
	}

	//the image files the materials' textures read, into the texture library, and each reference to one as its id
	std::vector<uint64_t> textureIds;
	for (const SceneTextureRecord& record : scene.textures) {
		TextureSource source;
		source.path = record.path;
		source.encoding = record.encoding;
		source.gamma = record.gamma;
		source.invert = record.invert;
		textureIds.push_back(m_textureLibrary.add(source, record.colorSpace, record.sampling));
	}
	auto texturesFromFile = [&](std::map<std::string, TextureBinding, std::less<>>& textures) {
		for (auto& [name, binding] : textures) {
			binding.texture = binding.fileTexture >= 0 && binding.fileTexture < (int)textureIds.size() ? textureIds[(size_t)binding.fileTexture] : 0;
			binding.fileTexture = -1;
		}
	};

	//the library: the edits to the glTF materials the imports just made, then the user materials
	for (SceneMaterialOverride& edit : scene.materialOverrides) {
		const std::string& key = edit.model < (int)modelKeys.size() ? modelKeys[edit.model] : std::string();
		MaterialDef* material = key.empty() ? nullptr : m_raytraceScene.findModelMaterial(key, edit.modelMaterial);
		if (material == nullptr) {
			continue;
		}
		for (const auto& [name, value] : edit.params) {
			material->params[name] = value;
			material->edited.insert(name);
		}
		texturesFromFile(edit.textures);
		for (const auto& [name, binding] : edit.textures) {
			material->textures[name] = binding;
		}
		material->lightGroup = edit.lightGroup;
	}
	std::vector<uint64_t> userIds;
	for (MaterialDef& material : scene.materials) {
		texturesFromFile(material.textures);
		userIds.push_back(m_raytraceScene.addMaterial(std::move(material)));
	}
	//a reference the file holds, as a library id; an object whose material went missing gets a new
	//one of its own
	auto resolveMaterial = [&](const SceneMaterialRef& ref, const std::string& ownerName) -> uint64_t {
		if (ref.user >= 0 && ref.user < (int)userIds.size()) {
			return userIds[ref.user];
		}
		if (ref.model >= 0 && ref.model < (int)modelKeys.size() && !modelKeys[ref.model].empty()) {
			if (const MaterialDef* material = m_raytraceScene.findModelMaterial(modelKeys[ref.model], ref.modelMaterial)) {
				return material->id;
			}
		}
		MaterialDef material = makeMaterial(MaterialType::Lambertian);
		material.name = ownerName;
		return m_raytraceScene.addMaterial(std::move(material));
	};

	//a mix's children, from the references the file holds to the ids the library just gave: in the user materials, and in
	//the edits to a model's materials
	std::vector<uint64_t> linkOwners = userIds;
	for (const SceneMaterialOverride& edit : scene.materialOverrides) {
		const std::string& modelKey = edit.model < (int)modelKeys.size() ? modelKeys[edit.model] : std::string();
		if (const MaterialDef* edited = modelKey.empty() ? nullptr : m_raytraceScene.findModelMaterial(modelKey, edit.modelMaterial)) {
			linkOwners.push_back(edited->id);
		}
	}
	for (const uint64_t userId : linkOwners) {
		MaterialDef* material = m_raytraceScene.findMaterial(userId);
		if (material == nullptr) {
			continue;
		}
		for (auto& [name, value] : material->params) {
			MaterialLink* link = std::get_if<MaterialLink>(&value);
			if (link == nullptr || (link->fileUser < 0 && link->fileModel < 0)) {
				continue;
			}
			uint64_t id = 0;
			if (link->fileUser >= 0 && link->fileUser < (int)userIds.size()) {
				id = userIds[link->fileUser];
			} else if (link->fileModel >= 0 && link->fileModel < (int)modelKeys.size() && !modelKeys[link->fileModel].empty()) {
				if (const MaterialDef* target = m_raytraceScene.findModelMaterial(modelKeys[link->fileModel], link->fileModelMaterial)) {
					id = target->id;
				}
			}
			*link = MaterialLink { .id = id };
		}
	}

	//the placed objects the file recorded replace the defaults every import just added. A
	//version-4 file has none, and its models keep those defaults - every node at its authored
	//transform, which is what that file meant
	if (scene.hasMeshObjects) {
		std::vector<SceneMeshObject> objects;
		for (const SceneMeshObjectRecord& record : scene.meshObjects) {
			if (record.model < 0 || record.model >= (int)modelKeys.size() || modelKeys[record.model].empty()) {
				//its model failed to load; the problem is already reported above
				continue;
			}
			SceneMeshObject object;
			object.name = record.name;
			object.modelKey = modelKeys[record.model];
			object.nodeIndex = record.nodeIndex;
			object.transform = record.transform;
			object.visible = record.visible;
			object.materialMode = record.materialMode;
			if (record.materialMode == MeshMaterialMode::Override) {
				object.materialId = resolveMaterial(record.material, record.name);
			}
			objects.push_back(std::move(object));
		}
		m_raytraceScene.replaceMeshObjects(std::move(objects));
	}
	//the environment the file names: a map by path, or the one a model's import just recreated. A model's environment
	//the file does not name was removed before the save, so it goes again
	const std::string environmentModel = scene.environmentModel >= 0 && scene.environmentModel < (int)modelKeys.size() ? modelKeys[scene.environmentModel] : std::string();
	if (!m_environmentModelKey.empty() && m_environmentModelKey != environmentModel) {
		clearEnvironmentMap();
	}
	if (!scene.environmentMapPath.empty()) {
		IoResult mapLoaded = loadEnvironmentMap(scene.environmentMapPath);
		if (!mapLoaded.ok) {
			problems.push_back(mapLoaded.message);
		}
	}
	//the file's lights replace what the imports just recreated from the models, exactly as its mesh
	//objects do. An older file has none, and keeps those
	if (scene.hasLights) {
		std::vector<SceneLight> lights;
		for (SceneLightRecord& record : scene.lights) {
			SceneLight light = std::move(record.light);
			light.modelKey = record.model >= 0 && record.model < (int)modelKeys.size() ? modelKeys[record.model] : std::string();
			lights.push_back(std::move(light));
		}
		m_raytraceScene.replaceLights(std::move(lights));
	}
	m_firstPersonCameraId = 0;
	std::vector<SceneShape> shapes;
	for (SceneShapeRecord& record : scene.shapes) {
		record.shape.materialId = resolveMaterial(record.material, record.shape.name);
		record.shape.modelKey = record.model >= 0 && record.model < (int)modelKeys.size() ? modelKeys[record.model] : std::string();
		shapes.push_back(std::move(record.shape));
	}
	m_raytraceScene.replaceShapes(std::move(shapes));
	//the bench, then the optics, each with its default slots, then the slots the file changed
	m_raytraceScene.setBench(scene.bench);
	{
		std::vector<SceneOptic> optics;
		for (const SceneOpticRecord& record : scene.optics) {
			optics.push_back(record.optic);
		}
		m_raytraceScene.replaceOptics(std::move(optics));
		std::vector<uint64_t> ids;
		for (size_t i = 0; i < scene.optics.size() && i < m_raytraceScene.optics().size(); i++) {
			ids.push_back(m_raytraceScene.optics()[i].id);
		}
		for (size_t i = 0; i < ids.size(); i++) {
			for (size_t slot = 0; slot < MAX_OPTIC_MATERIAL_SLOTS; slot++) {
				if (scene.optics[i].slots[slot]) {
					m_raytraceScene.setOpticSlotMaterial(ids[i], slot, *scene.optics[i].slots[slot]);
				}
			}
		}
		//a mounted optic the file placed by its body stands in its mount there, in the file's order. Only a turn of the body
		//is left to report: the stack keeps its origin where the file put it
		for (size_t i = 0; i < ids.size(); i++) {
			if (!scene.optics[i].standOnBench) {
				continue;
			}
			const std::optional<double> turned = m_raytraceScene.standOpticOnBench(this, ids[i]);
			if (!turned) {
				problems.push_back(fmt::format("optic \"{}\" has no mount model to stand in; it stays where the file put it", scene.optics[i].optic.name));
			} else if (*turned > 1e-6) {
				problems.push_back(fmt::format("optic \"{}\" stands in its mount turned {:.6g} deg from the orientation the file gave it", scene.optics[i].optic.name, *turned * 180.0 / 3.14159265358979323846));
			}
		}
		//the sensor stacks, which add their members after the optics
		std::vector<SensorStack> stacks;
		for (const SceneSensorStackRecord& record : scene.sensorStacks) {
			stacks.push_back(record.stack);
		}
		m_raytraceScene.replaceSensorStacks(std::move(stacks));
		for (size_t i = 0; i < scene.sensorStacks.size() && i < m_raytraceScene.sensorStacks().size(); i++) {
			const uint64_t id = m_raytraceScene.sensorStacks()[i].id;
			for (size_t slot = 0; slot < MAX_OPTIC_MATERIAL_SLOTS; slot++) {
				if (scene.sensorStacks[i].slots[slot]) {
					m_raytraceScene.setSensorStackSlotMaterial(id, slot, *scene.sensorStacks[i].slots[slot]);
				}
			}
		}
	}
	setLengthUnit(scene.lengthUnit);
	if (scene.viewport) {
		m_viewportNav.settings = scene.viewport->settings;
		m_viewportNav.lookAt(scene.viewport->position, scene.viewport->position + scene.viewport->orientation * glm::vec3(0.f, 0.f, -1.f));
		m_viewportNav.camera.pivot = scene.viewport->pivot;
		m_raytraceScene.setBeamPreview(scene.viewport->beamPreview);
	} else {
		m_viewportNav.settings = ViewportCameraSettings {};
		m_raytraceScene.setBeamPreview({});
	}
	m_raytraceScene.replaceCameras(std::move(scene.cameras));
	//the beam axes, once the optics and cameras they name have ids. optics() starts with the file's optics in its order;
	//the stacks' members come after them
	{
		std::vector<BeamAxis> axes;
		for (SceneBeamAxisRecord& record : scene.beamAxes) {
			BeamAxis axis = std::move(record.axis);
			const std::vector<SceneOptic>& optics = m_raytraceScene.optics();
			const std::vector<SceneCamera>& cameras = m_raytraceScene.cameras();
			if (axis.source.kind == BeamSourceKind::Laser) {
				axis.source.id = axis.source.id < optics.size() ? optics[axis.source.id].id : 0;
			} else if (axis.source.kind == BeamSourceKind::Camera) {
				axis.source.id = axis.source.id < cameras.size() ? cameras[axis.source.id].id : 0;
			}
			if (axis.source.kind != BeamSourceKind::None && axis.source.id == 0) {
				axis.source = {};
			}
			for (BeamAxisNode& node : axis.nodes) {
				node.optic = node.optic < optics.size() ? optics[node.optic].id : 0;
			}
			axes.push_back(std::move(axis));
		}
		m_raytraceScene.replaceBeamAxes(std::move(axes));
		const std::vector<BeamAxis>& loaded = m_raytraceScene.beamAxes();
		m_raytracer.setGuideAxis(scene.guideAxis >= 0 && scene.guideAxis < (int)loaded.size() ? loaded[(size_t)scene.guideAxis].id : 0);
		for (const std::string& warning : scene.beamAxisWarnings) {
			problems.push_back(warning);
		}
	}
	//the light groups last of the objects that name one: a light or material in a group the file does not list moves to group 0
	m_raytraceScene.setLightGroups(scene.lightGroupNames, scene.environmentGroup);
	m_raytracer.setExportSettings(scene.exportSettings);
	m_raytracer.gpu().spectralDisplay = scene.spectralDisplay;
	m_raytracer.setSettings(scene.render);
	//the kernel selection first: it is what decides the BLAS node layout, so setAccelSettings()
	//below sees the layout the file's traversal actually needs
	m_raytracer.setKernels(scene.kernels);
	m_raytracer.setAccelSettings(this, scene.accel);
	for (const std::string& warning : scene.kernelWarnings) {
		problems.push_back(warning);
	}
	if (scene.renderCamera >= 0 && scene.renderCamera < (int)m_raytraceScene.cameras().size()) {
		m_raytracer.setRenderCamera(m_raytraceScene.cameras()[scene.renderCamera].id);
	}
	//a detector render target, which the optics above made
	if (scene.renderDetector >= 0 && scene.renderDetector < (int)m_raytraceScene.optics().size() && isDetectorKind(m_raytraceScene.optics()[scene.renderDetector].kind)) {
		m_raytracer.setRenderDetector(m_raytraceScene.optics()[scene.renderDetector].id);
	} else {
		m_raytracer.setRenderDetector(0);
	}
	m_environmentIntensity = scene.environmentIntensity;
	m_environmentRotation = scene.environmentRotation;
	//after the map loaded, which chose it: the file's choice stands, an environment whose map failed to load included
	m_backgroundMode = scene.backgroundMode;
	m_backgroundColor = scene.backgroundColor;
	m_scenePath = absoluteNormalized(path);

	//the capabilities the file names, an unknown one kept for the save, and the template it started from. Its objects
	//are all loaded, so a capability a file lacks but whose objects it holds (one written before capabilities) is
	//turned on here, and said so; nothing a file opens with offers its recommendations, which are the file's to keep
	m_capabilities = capabilitiesFromKeys(scene.capabilities, problems);
	m_sceneTemplate = scene.templateKey;
	m_pendingRecommendations.clear();
	//the optics on their posts before anything renders: a batch render starts on the next frame
	updateBench();
	std::string enabled;
	for (const CapabilityInfo& capability : capabilities()) {
		std::vector<ObjectRef> objects;
		capability.dependentObjects(this, objects);
		if (!m_capabilities.has(capability.id) && !objects.empty()) {
			enableCapability(capability.id, false);
			enabled += fmt::format("; turned on {}, which its {} object(s) need", capability.name, objects.size());
		}
	}

	if (!problems.empty()) {
		return IoResult::failure(fmt::format("Opened '{}' with {} problem(s): {}{}", path.filename().string(), problems.size(), fmt::join(problems, "; "), enabled));
	}
	return IoResult::success(fmt::format("Opened '{}': {} model(s), {} shape(s){}{}", path.filename().string(), m_models.size(), m_raytraceScene.shapes().size(),
		m_environmentMapPath.empty() ? "" : ", environment map", enabled));
}

IoResult VulkanEngine::loadEnvironmentMap(const std::filesystem::path& path)
{
	const std::string pathString = path.string();

	//a Radiance .hdr through stb_image, or an OpenEXR file through tinyexr (src/image_io.h); either way float rgba
	int width = 0;
	int height = 0;
	float* pixels = nullptr;
	std::vector<float> exrPixels;
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	if (extension == ".exr") {
		Image image;
		const ImageIoStatus status = loadImage(path, image);
		if (!status.ok) {
			return IoResult::failure(status.message);
		}
		width = (int)image.width;
		height = (int)image.height;
		exrPixels.resize(image.pixelCount() * 4, 1.f);
		for (size_t i = 0; i < image.pixelCount(); i++) {
			exrPixels[i * 4 + 0] = image.rgb[i * 3 + 0];
			exrPixels[i * 4 + 1] = image.rgb[i * 3 + 1];
			exrPixels[i * 4 + 2] = image.rgb[i * 3 + 2];
		}
		pixels = exrPixels.data();
	} else {
		if (!stbi_is_hdr(pathString.c_str())) {
			//stbi_is_hdr is also false for a file that is not there, so tell the two apart
			std::error_code ec;
			if (!std::filesystem::exists(path, ec)) {
				return IoResult::failure(fmt::format("No file at '{}'", pathString));
			}
			return IoResult::failure(fmt::format("'{}' is not a Radiance .hdr or OpenEXR .exr image", pathString));
		}
		int channels = 0;
		pixels = stbi_loadf(pathString.c_str(), &width, &height, &channels, 4);
		if (pixels == nullptr) {
			return IoResult::failure(fmt::format("Failed to decode '{}': {}", pathString, stbi_failure_reason()));
		}
	}

	IoResult result = installEnvironmentMap(pixels, (uint32_t)width, (uint32_t)height, path.filename().string(), absoluteNormalized(path));
	if (exrPixels.empty()) {
		stbi_image_free(pixels);
	}
	return result;
}

IoResult VulkanEngine::installEnvironmentMap(const float* pixels, uint32_t width, uint32_t height, const std::string& label, const std::filesystem::path& path)
{
	if (pixels == nullptr || width == 0 || height == 0) {
		return IoResult::failure(fmt::format("'{}' has no pixels", label));
	}
	//the path tracer's sampling tables, from the full-precision pixels before they are halved: a
	//piecewise-constant distribution over the texels, at most 2048 cells wide, and its MIS-compensated
	//copy (light_sampling.h). A 4k map's tables are ~33 MB and take a fraction of a second
	auto distribution = std::make_shared<EnvironmentDistribution>(buildEnvironmentDistribution(pixels, width, height, 2048));

	//half float: the usual precision for environment lighting, half the memory of rgba32f (an 8k
	//map is 256 MB rather than 512), and linearly filterable everywhere - 32-bit float filtering
	//is optional in vulkan. A map brighter than half float's 65504 is stored divided by a power of
	//two, which the shaders multiply back in with the intensity (packEnvironmentTexels())
	const size_t pixelCount = (size_t)width * (size_t)height;
	const float peak = environmentPeak(pixels, pixelCount);
	const float storageScale = environmentStorageScale(peak);
	std::vector<uint32_t> halves = packEnvironmentTexels(pixels, pixelCount, storageScale);

	//only now that the new data is known good does the old resource go. Its preview window stays as it was: loading a
	//map opens none (Scene -> Background -> Preview does)
	const bool previewShown = m_displayRegistry.isVisible(ENVIRONMENT_MAP_DISPLAY_NAME);
	destroyEnvironmentMap();

	const VkExtent3D extent { width, height, 1 };
	m_environmentMap = createImage(halves.data(), extent, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
	m_environmentMapsLoaded++;
	m_environmentMapExtent = { extent.width, extent.height };
	m_environmentMapScale = storageScale;
	m_environmentMapPath = path;
	//a map from a file is the scene's own; importPbrt() marks one a model brought after this returns
	m_environmentModelKey.clear();
	m_environmentDistribution = std::move(distribution);

	//the map as an image, values above 1 clipped, in a window that opens on request
	m_displayRegistry.registerImage(ENVIRONMENT_MAP_DISPLAY_NAME, m_environmentMap.imageView, m_environmentMapExtent);
	m_displayRegistry.setVisible(ENVIRONMENT_MAP_DISPLAY_NAME, previewShown);
	//a map just loaded is what the user wants to see: the viewport and the render switch to it
	m_backgroundMode = BackgroundMode::Environment;

	std::string message = fmt::format("Loaded '{}': {} x {}, rgba16f", label, width, height);
	if (storageScale > 1.f) {
		message += fmt::format(", stored at 1/{:g} so its peak of {:g} fits in half float", storageScale, peak);
	}
	return IoResult::success(message);
}

void VulkanEngine::clearEnvironmentMap()
{
	destroyEnvironmentMap();
	m_environmentMapPath.clear();
	m_environmentModelKey.clear();
	if (m_backgroundMode == BackgroundMode::Environment) {
		m_backgroundMode = BackgroundMode::Gradient;
	}
}

void VulkanEngine::showEnvironmentPreview()
{
	if (m_displayRegistry.isRegistered(ENVIRONMENT_MAP_DISPLAY_NAME)) {
		m_displayRegistry.setVisible(ENVIRONMENT_MAP_DISPLAY_NAME, true);
		ImGui::SetWindowFocus(ENVIRONMENT_MAP_DISPLAY_NAME);
	}
}

void VulkanEngine::destroyEnvironmentMap()
{
	if (m_environmentMap.image == VK_NULL_HANDLE) {
		return;
	}

	//a GPU render in flight samples this image every frame, so it ends here - the snapshot
	//model's one concession to a resource it cannot hold a reference to
	m_raytracer.cancelRender();

	//unregister first so no new frame references the view, then wait out the ones that already do
	m_displayRegistry.unregisterImage(ENVIRONMENT_MAP_DISPLAY_NAME);
	vkDeviceWaitIdle(m_device);
	destroyImage(m_environmentMap);
	m_environmentMap = {};
	m_environmentMapsLoaded++;
	m_environmentMapExtent = { 0, 0 };
	m_environmentMapScale = 1.f;
	m_environmentDistribution.reset();
}

void VulkanEngine::drawImportReport()
{
	if (!m_showImportReport) {
		return;
	}
	ImGui::SetNextWindowSize(ImVec2(760.f, 420.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Import Report", &m_showImportReport)) {
		ImGui::End();
		return;
	}
	//the models an importer other than glTF's brought, each with what it substituted and left out
	std::vector<std::string> keys;
	for (const auto& [key, model] : m_models) {
		if (model != nullptr && model->imported.has_value()) {
			keys.push_back(key);
		}
	}
	if (keys.empty()) {
		ImGui::TextDisabled("No pbrt scene is imported. File -> Import pbrt Scene... brings one in.");
		ImGui::End();
		return;
	}
	if (std::find(keys.begin(), keys.end(), m_importReportModel) == keys.end()) {
		m_importReportModel = keys.front();
	}
	if (ImGui::BeginCombo("model", m_importReportModel.c_str())) {
		for (const std::string& key : keys) {
			if (ImGui::Selectable(key.c_str(), key == m_importReportModel)) {
				m_importReportModel = key;
			}
		}
		ImGui::EndCombo();
	}
	const LoadedModel& model = *m_models[m_importReportModel];
	const ImportedModel& imported = *model.imported;
	if (imported.settings.has_value()) {
		const RenderSettings& r = imported.settings->render;
		ImGui::SameLine();
		if (ImGui::Button("Use pbrt settings")) {
			usePbrtSettings(m_importReportModel);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Render from the file's camera at %d x %d with %d samples, its sampler, pixel filter and ray depth %d%s",
				r.width, r.height, r.maxSamples, r.rayDepth, r.clampSamples ? fmt::format(", clamping samples at {:g}", r.clampValue).c_str() : "");
		}
	}
	ImGui::TextWrapped("%s", model.sourcePath.string().c_str());
	ImGui::TextDisabled("Every substitution and everything the import left out, once each with how often and where it first happened. "
		"The plan in docs/plans/ adds what is missing.");
	if (ImGui::BeginTable("report", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("count", ImGuiTableColumnFlags_WidthFixed, 50.f);
		ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("first at", ImGuiTableColumnFlags_WidthFixed, 160.f);
		ImGui::TableSetupColumn("plan", ImGuiTableColumnFlags_WidthFixed, 200.f);
		ImGui::TableHeadersRow();
		for (const ImportMessage& message : imported.report.messages) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%zu", message.count);
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", message.text.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(message.where.c_str());
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", message.plan.c_str());
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

void VulkanEngine::drawFileMenu()
{
	if (ImGui::BeginMenu("File")) {
		if (ImGui::MenuItem("New Scene")) {
			m_pendingFileAction = FileAction::NewScene;
		}
		ImGui::SetItemTooltip("One camera and the default settings: the Render scene template");
		drawTemplateMenu(this);
		if (ImGui::MenuItem("Open Scene...")) {
			m_pendingFileAction = FileAction::OpenScene;
		}
		if (ImGui::MenuItem("Save Scene")) {
			m_pendingFileAction = FileAction::SaveScene;
		}
		if (ImGui::MenuItem("Save Scene As...")) {
			m_pendingFileAction = FileAction::SaveSceneAs;
		}
		if (ImGui::MenuItem("Save as Template...")) {
			openSaveTemplatePopup();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Import glTF Model...")) {
			m_pendingFileAction = FileAction::ImportGltf;
		}
		if (ImGui::MenuItem("Import pbrt Scene...")) {
			m_pendingFileAction = FileAction::ImportPbrt;
		}
		if (ImGui::MenuItem("Set Environment Map...")) {
			m_pendingFileAction = FileAction::SetEnvironmentMap;
		}
		if (ImGui::MenuItem("Clear Environment Map", nullptr, false, m_environmentMap.image != VK_NULL_HANDLE)) {
			m_pendingFileAction = FileAction::ClearEnvironmentMap;
		}
		ImGui::Separator();
		forEachEnabledCapability(m_capabilities, [this](const CapabilityInfo& capability) {
			if (capability.fileMenu != nullptr) {
				capability.fileMenu(this);
			}
		});
		if (ImGui::MenuItem("Export Render...", nullptr, false, m_raytracer.hasRender())) {
			m_pendingFileAction = FileAction::ExportRender;
		}
		ImGui::EndMenu();
	}
}

void VulkanEngine::drawEditMenu()
{
	if (ImGui::BeginMenu("Edit")) {
		if (ImGui::MenuItem("Scene Capabilities...")) {
			m_showCapabilities = true;
			ImGui::SetWindowFocus("Scene Capabilities");
		}
		ImGui::SetItemTooltip("What this scene uses beyond a plain render, such as the optical bench");
		//every panel's lengths, saved with the scene; the scene itself is meters whatever this says
		if (ImGui::BeginMenu("Length Unit")) {
			for (const LengthUnit unit : { LengthUnit::Meters, LengthUnit::Millimeters }) {
				if (ImGui::MenuItem(lengthUnitLabel(unit), nullptr, m_lengthUnit == unit)) {
					setLengthUnit(unit);
				}
			}
			ImGui::EndMenu();
		}
		ImGui::EndMenu();
	}
}

void VulkanEngine::drawCameraMenu()
{
	if (ImGui::BeginMenu("Camera")) {
		const bool selected = m_raytraceScene.selectionBounds(this).has_value();
		if (ImGui::MenuItem("Frame Selected", "F", false, selected)) {
			frameInViewport(false);
		}
		if (ImGui::MenuItem("Frame All")) {
			frameInViewport(true);
		}
		ImGui::Separator();
		m_viewportNav.drawMenuSettings(m_firstPersonCameraId != 0);
		ImGui::EndMenu();
	}
}

void VulkanEngine::drawViewMenu()
{
	if (ImGui::BeginMenu("View")) {
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.f);
		ImGui::SliderFloat("Render Scale", &m_renderScale, 0.3f, 1.f, "%.2f");
		ImGui::SetItemTooltip("The raster preview's resolution as a share of the Viewport window's. Lower is faster and blurrier");
		if (ImGui::BeginMenu("Pause Raster Preview")) {
			const char* modes[] = { "Always", "While Rendering", "Never" };
			for (int i = 0; i < IM_ARRAYSIZE(modes); i++) {
				if (ImGui::MenuItem(modes[i], nullptr, (int)m_rasterPauseMode == i)) {
					m_rasterPauseMode = (RasterPauseMode)i;
				}
			}
			ImGui::EndMenu();
		}
		ImGui::SetItemTooltip("Keep the last picture instead of drawing an identical one, once the camera, the scene and the background have been still for %u frames", FRAME_OVERLAP + 1);
		ImGui::MenuItem("Occlusion Culling", nullptr, &m_occlusionCulling);
		ImGui::SetItemTooltip("Skip a surface of %u or more triangles whose box was hidden two frames ago. A surface that comes out from behind another appears two frames late", MeshDrawList::OCCLUSION_MIN_TRIANGLES);
		ImGui::EndMenu();
	}
}

void VulkanEngine::setLengthUnit(LengthUnit unit)
{
	m_lengthUnit = unit;
	setCurrentLengthUnit(unit);
}

BenchContext VulkanEngine::benchContext()
{
	BenchContext context;
	context.settings = m_raytraceScene.bench();
	context.models = [this](std::string_view id) { return m_mountLibrary.model(id); };
	context.build = opticBuildContext();
	return context;
}

void VulkanEngine::updateBench()
{
	m_raytraceScene.resolveBench(this);
	if (m_benchScene.update(this)) {
		//the breadboard or a mount model is new to the tracer: its BLAS is built now, the scene's are kept
		m_raytraceMeshData = buildRaytraceMeshData(this);
		m_raytracer.prepareGeometry(this);
		m_sceneRevision++;
	}
	//the beams for the poses just resolved (docs/plans/completed/physoptics-gaussian-beam-solver.md), before anything draws them
	const auto started = std::chrono::steady_clock::now();
	m_raytraceScene.solveBeams(this);
	m_stats.beamSolveTime = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
	//the preview's sections, when the beams or how they are shown changed
	const std::pair<uint64_t, uint64_t> beamsFor { m_raytraceScene.beamSolver().revision(), m_raytraceScene.beamPreviewRevision() };
	if (beamsFor != m_beamSectionsFor) {
		m_beamSectionsFor = beamsFor;
		m_beamSections = buildBeamSections(m_raytraceScene.beamSolver().solution(), m_raytraceScene.beamPreview());
		m_beamSectionsRevision++;
	}
}

OpticBuildContext VulkanEngine::opticBuildContext() const
{
	OpticBuildContext context;
	context.library = &m_opticalLibrary;
	context.designWavelength = designWavelength(m_raytracer.settings().spectral, m_raytraceScene.optics());
	context.surrounding = surroundingGlass(m_raytracer.settings().optics.surrounding);
	return context;
}

namespace {

// --solve-beams: the solver's solution as JSON, every optic named by its name (docs/plans/completed/physoptics-gaussian-beam-solver.md)
std::string beamSolutionJson(const RaytraceSceneEditor& editor)
{
	const BeamSolver& solver = editor.beamSolver();
	const BeamSolution& solution = solver.solution();
	const auto name = [&](uint64_t id) -> std::string {
		if (id == 0) {
			return "null";
		}
		const SceneOptic* optic = editor.findOptic(id);
		return optic != nullptr ? fmt::format("\"{}\"", optic->name) : std::string("null");
	};
	const auto vec3 = [](const glm::dvec3& v) { return fmt::format("[{:.12g},{:.12g},{:.12g}]", v.x, v.y, v.z); };
	const auto vec2 = [](const glm::dvec2& v) { return fmt::format("[{:.12g},{:.12g}]", v.x, v.y); };
	std::string json = fmt::format(R"({{"solveMs":{:.6g},"trees":[)", solver.lastSolveMs());
	for (size_t t = 0; t < solution.trees.size(); t++) {
		const BeamTree& tree = solution.trees[t];
		std::string source = name(tree.source);
		if (tree.sourceKind == BeamSourceKind::Camera) {
			const SceneCamera* camera = editor.findCamera(tree.source);
			source = camera != nullptr ? fmt::format("\"{}\"", camera->name) : "null";
		}
		const BeamPowerBudget& b = tree.budget;
		json += fmt::format(
			R"({}{{"source":{},"kind":"{}","wavelength":{:.12g},"power":{:.12g},"threshold":{:.6g},"truncated":{},"budget":{{"detected":{:.12g},"absorbed":{:.12g},"glass":{:.12g},"clipped":{:.12g},"scattered":{:.12g},"escaped":{:.12g},"backReflected":{:.12g},"belowThreshold":{:.12g},"truncated":{:.12g},"total":{:.15g}}},"segments":[)",
			t == 0 ? "" : ",", source, tree.sourceKind == BeamSourceKind::Camera ? "camera" : "laser", tree.wavelength, tree.power, tree.threshold, tree.truncated, b.detected,
			b.absorbed, b.glass, b.clipped, b.scattered, b.escaped, b.backReflected, b.belowThreshold, b.truncated, b.total());
		for (uint32_t i = 0; i < tree.segments.size(); i++) {
			const BeamSegment& s = tree.segments[i];
			const BeamCrossSection start = tree.crossSection(i, 0.0);
			const BeamCrossSection end = tree.crossSection(i, s.length);
			const BeamProfile profile = tree.profile(i);
			const SceneOptic* from = editor.findOptic(s.start.fromOptic);
			const std::string port = from != nullptr && s.start.port != NO_PORT ? fmt::format("\"{}\"", opticPortName(from->kind, s.start.port)) : std::string("null");
			json += fmt::format(
				R"({}{{"parent":{},"start":{},"direction":{},"length":{:.12g},"end":"{}","optic":{},"surface":{},"power":{:.12g},"endPower":{:.12g},"clipped":{:.9g},"from":{},"port":{},"ghost":{},"inside":{},"index":{:.12g},"distance":{:.12g},"radiiStart":{},"radiiEnd":{},"angleStart":{:.9g},"waists":[{{"w0":{:.12g},"at":{:.12g},"zR":{:.12g}}},{{"w0":{:.12g},"at":{:.12g},"zR":{:.12g}}}],"profileAngle":{:.9g},"twisted":{}}})",
				i == 0 ? "" : ",", s.parent == NO_BEAM_SEGMENT ? -1 : (int)s.parent, vec3(s.start.origin), vec3(s.start.direction), s.length, beamEndLabel(s.end),
				name(s.endOptic), s.endSurface, s.start.power, s.endPower, s.endClipped, name(s.start.fromOptic), port, s.start.ghost, name(s.start.inside), s.start.index,
				s.start.distance, vec2(start.radii), vec2(end.radii), start.angle, profile.axes[0].waist, profile.axes[0].waistAt, profile.axes[0].rayleighRange,
				profile.axes[1].waist, profile.axes[1].waistAt, profile.axes[1].rayleighRange, profile.angle, profile.twisted);
		}
		json += R"(],"readings":[)";
		for (size_t r = 0; r < tree.readings.size(); r++) {
			const BeamDetectorReading& reading = tree.readings[r];
			json += fmt::format(R"({}{{"optic":{},"segment":{},"power":{:.12g},"centroid":{},"radii":{},"angle":{:.9g}}})", r == 0 ? "" : ",", name(reading.optic), reading.segment,
				reading.power, vec2(reading.centroid), vec2(reading.radii), reading.angle);
		}
		json += R"(],"warnings":[)";
		for (size_t w = 0; w < tree.warnings.size(); w++) {
			const BeamWarning& warning = tree.warnings[w];
			json += fmt::format(R"({}{{"kind":"{}","segment":{},"optic":{},"power":{:.9g},"value":{:.9g}}})", w == 0 ? "" : ",",
				warning.kind == BeamWarning::BackReflection ? "backReflection" : "ghostFocus", warning.segment, name(warning.optic), warning.power, warning.value);
		}
		json += "]}";
	}
	json += R"(],"axes":[)";
	for (size_t a = 0; a < editor.beamAxes().size(); a++) {
		const BeamAxis& axis = editor.beamAxes()[a];
		const auto tree = solution.axisTrees.find(axis.id);
		json += fmt::format(R"({}{{"name":"{}","tree":{},"nodes":[)", a == 0 ? "" : ",", axis.name, tree != solution.axisTrees.end() ? (int)tree->second : -1);
		const auto reports = solution.reports.find(axis.id);
		bool first = true;
		if (reports != solution.reports.end()) {
			for (const BeamNodeReport& r : reports->second) {
				const BeamAxisNode* node = findBeamNode(axis, r.node);
				const BeamAxisNode* parent = node != nullptr ? findBeamNode(axis, node->parent) : nullptr;
				const SceneOptic* parentOptic = parent != nullptr ? editor.findOptic(parent->optic) : nullptr;
				const std::string port = parentOptic != nullptr && node->port != NO_PORT ? fmt::format("\"{}\"", opticPortName(parentOptic->kind, node->port)) : std::string("null");
				json += fmt::format(
					R"({}{{"node":{},"optic":{},"parent":{},"port":{},"status":"{}","power":{:.12g},"lostFraction":{:.9g},"missDistance":{:.12g},"steer":{},"blocker":"{}","segment":{},"from":{},"to":{}}})",
					first ? "" : ",", r.node, node != nullptr ? name(node->optic) : "null", node != nullptr && node->parent < BEAM_DANGLING_NODE ? (int)node->parent : -1, port,
					beamNodeStatusLabel(r.status), r.power, r.lostFraction, r.missDistance, vec3(r.steer), r.blocker, r.segment == NO_BEAM_SEGMENT ? -1 : (int)r.segment,
					vec3(r.from), vec3(r.to));
				first = false;
			}
		}
		json += "]}";
	}
	json += "]}\n";
	return json;
}

} // namespace

bool VulkanEngine::beginBatch()
{
	BatchRun run;
	run.samples = m_commandLine.samples;
	run.output = m_commandLine.output;
	run.timeoutSeconds = m_commandLine.timeoutSeconds;
	std::filesystem::path scenePath = m_commandLine.renderScene;
	if (scenePath.is_relative() && !std::filesystem::exists(scenePath)) {
		scenePath = absoluteNormalized(m_rootPath + "assets/scenes") / scenePath;
	}
	//a pbrt file is a new scene of that one model, rendered the way the file asks
	const std::string sceneName = scenePath.filename().string();
	const bool pbrt = sceneName.ends_with(".pbrt") || sceneName.ends_with(".pbrt.gz");
	if (pbrt) {
		newScene();
	}
	bool loaded = false;
	const IoResult opened = pbrt ? importPbrt(scenePath) : openScene(scenePath, &loaded);
	fmt::println("render: {}", opened.message);
	//--save-scene: the scene as the editor holds it, written back, with no render; a file that opened with problems
	//saves as the load repaired it
	if (!m_commandLine.saveScene.empty() && (opened.ok || loaded)) {
		const IoResult saved = saveScene(m_commandLine.saveScene);
		fmt::println("render: {}", saved.message);
		m_exitCode = saved.ok ? 0 : 2;
		return false;
	}
	if (!opened.ok) {
		m_exitCode = 2;
		return false;
	}
	//--solve-beams: the beam solver's solution for the scene as it opened, with no render
	if (!m_commandLine.solveBeams.empty()) {
		updateBench();
		if (m_commandLine.followBeams) {
			std::vector<uint64_t> empty;
			for (const BeamAxis& axis : m_raytraceScene.beamAxes()) {
				if (beamNodeAt(axis, BEAM_SOURCE_NODE, NO_PORT) == nullptr) {
					empty.push_back(axis.id);
				}
			}
			for (uint64_t id : empty) {
				m_raytraceScene.followBeamFrom(id, BEAM_SOURCE_NODE);
			}
			updateBench();
		}
		std::ofstream file(m_commandLine.solveBeams, std::ios::binary);
		file << beamSolutionJson(m_raytraceScene);
		fmt::println("render: beam solution written to {} ({:.3f} ms)", m_commandLine.solveBeams.string(), m_raytraceScene.beamSolver().lastSolveMs());
		m_exitCode = file ? 0 : 2;
		return false;
	}
	if (pbrt) {
		std::string key;
		for (const auto& [name, model] : m_models) {
			key = name;
		}
		usePbrtSettings(key);
		//what pbrt multiplies its image by and the EXR leaves out, for tools/verify/pbrt_compare.py
		fmt::println("render: pbrt imaging ratio {:.9g}", m_models[key]->imported->settings.has_value() ? m_models[key]->imported->settings->imagingRatio : 1.f);
		for (const ImportMessage& message : m_models[key]->imported->report.messages) {
			fmt::println("render: import: {}{} ({}){}", message.text, message.count > 1 ? fmt::format(" x{}", message.count) : "", message.where,
				message.plan.empty() ? "" : " -> " + message.plan);
		}
	}

	//the scene file's settings, with the sample target, a fixed seed and no restarts imposed, and what the
	//command line asked to keep
	RenderSettings settings = m_raytracer.settings();
	if (run.samples > 0) {
		settings.maxSamples = (int)run.samples;
	}
	run.samples = (uint32_t)std::max(settings.maxSamples, 1);
	settings.unlimitedSamples = false;
	settings.useFixedSeed = true;
	settings.seed = m_commandLine.seed;
	settings.restartOnChange = false;
	//--detectors asks for every detector's reading, so the render records them all
	if (!m_commandLine.detectors.empty()) {
		settings.renderAllSensors = true;
	}
	settings.matchViewport = false;
	if (m_commandLine.size.has_value()) {
		settings.width = m_commandLine.size->first;
		settings.height = m_commandLine.size->second;
	}
	for (const std::string& id : m_commandLine.aov) {
		settings.aov.set(aovOutputFromId(id), true);
	}
	settings.aov.captureThroughSpecular = settings.aov.captureThroughSpecular || m_commandLine.captureThroughSpecular;
	settings.lightGroups = settings.lightGroups || m_commandLine.lightGroups;
	//the spectral options, each in place of the scene's own
	{
		SpectralSettings& spectral = settings.spectral;
		if (!m_commandLine.spectral.empty()) {
			bool found = false;
			for (uint32_t r = 0; r < (uint32_t)SpectralRepresentation::Count; r++) {
				if (m_commandLine.spectral == spectralRepresentationKey((SpectralRepresentation)r)) {
					spectral.representation = (SpectralRepresentation)r;
					found = true;
				}
			}
			if (!found) {
				fmt::println("render: '{}' is not a spectral mode; see --help", m_commandLine.spectral);
				m_exitCode = 2;
				return false;
			}
		}
		if (!m_commandLine.sampling.empty()) {
			bool found = false;
			for (uint32_t s = 0; s < (uint32_t)WavelengthSampling::Count; s++) {
				if (m_commandLine.sampling == wavelengthSamplingKey((WavelengthSampling)s)) {
					spectral.sampling = (WavelengthSampling)s;
					found = true;
				}
			}
			if (!found) {
				fmt::println("render: '{}' is not a wavelength sampling; see --help", m_commandLine.sampling);
				m_exitCode = 2;
				return false;
			}
		}
		if (!m_commandLine.colorSpace.empty()) {
			const std::optional<ColorSpaceId> space = findColorSpace(m_commandLine.colorSpace);
			if (!space.has_value()) {
				fmt::println("render: '{}' is not a color space; see --help", m_commandLine.colorSpace);
				m_exitCode = 2;
				return false;
			}
			spectral.colorSpace = *space;
		}
		if (m_commandLine.lanes != 0) {
			spectral.lanes = m_commandLine.lanes;
		}
		if (m_commandLine.spectralImages != 0) {
			spectral.imageBuckets = m_commandLine.spectralImages;
		}
		if (m_commandLine.wavelength > 0.f) {
			spectral.monochromaticWavelength = m_commandLine.wavelength;
		}
		if (m_commandLine.designWavelength > 0.f) {
			spectral.designWavelength = m_commandLine.designWavelength;
		}
		if (m_commandLine.range.has_value()) {
			spectral.lambdaMin = m_commandLine.range->first;
			spectral.lambdaMax = m_commandLine.range->second;
		}
	}
	m_raytracer.setSettings(settings);
	//the display-side controls: a debug view, the ambient occlusion distance, and Relight with the scales given
	if (!m_commandLine.debugView.empty()) {
		const CrtDebugView view = crtDebugViewFromId(m_commandLine.debugView);
		if (view == CrtDebugView::Count) {
			fmt::println("render: '{}' is not a debug view; see --help", m_commandLine.debugView);
			m_exitCode = 2;
			return false;
		}
		m_raytracer.gpu().debugView = view;
	}
	m_raytracer.gpu().aoDistance = m_commandLine.aoDistance;
	if (!m_commandLine.relight.empty()) {
		m_raytracer.gpu().relight = true;
		if (SceneCamera* camera = m_raytraceScene.findCamera(m_raytracer.renderCameraId())) {
			for (size_t g = 0; g < m_commandLine.relight.size() && g < camera->relight.size(); g++) {
				camera->relight[g].scale = m_commandLine.relight[g];
			}
		}
	}
	//the batch writes the file itself, once, and never saves a second copy to the scene's own path
	ExportSettings exportSettings = m_raytracer.exportSettings();
	exportSettings.saveWhenFinished = false;
	exportSettings.halfFloat = exportSettings.halfFloat || m_commandLine.halfFloat;
	m_raytracer.setExportSettings(exportSettings);
	run.started = std::chrono::steady_clock::now();
	m_batch = run;
	return true;
}

void VulkanEngine::stepBatch()
{
	BatchRun& run = *m_batch;
	auto finish = [&](int code, const std::string& message) {
		fmt::println("render: {}", message);
		m_exitCode = code;
		glfwSetWindowShouldClose(m_window, GLFW_TRUE);
		m_batch.reset();
	};
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - run.started).count();
	if (elapsed > run.timeoutSeconds) {
		finish(3, fmt::format("timed out after {:.0f} s at {} of {} samples", elapsed, m_raytracer.samplesAccumulated(), run.samples));
		return;
	}
	switch (run.phase) {
	case BatchRun::Phase::Start:
		m_raytracer.render(this, m_raytraceScene);
		if (!m_raytracer.renderRunning()) {
			finish(2, "the render did not start: " + (m_raytracer.blockedReason().empty() ? std::string("no scene camera") : m_raytracer.blockedReason()));
			return;
		}
		run.phase = BatchRun::Phase::Rendering;
		break;
	case BatchRun::Phase::Rendering:
		if (!m_raytracer.renderRunning()) {
			//the film's last frame and the counters it fed arrive FRAME_OVERLAP frames after it ends
			run.phase = BatchRun::Phase::Settling;
			run.settleFrame = m_frameNumber + 4;
		}
		break;
	case BatchRun::Phase::Settling:
		if (m_frameNumber < run.settleFrame) {
			break;
		}
		IoResult saved = m_raytracer.exportRender(this, m_raytraceScene, run.output);
		//the detectors' readings and films beside it (docs/plans/completed/physoptics-detectors.md)
		if (saved.ok && !m_commandLine.detectors.empty()) {
			const IoResult detectors = writeDetectorReadings(this, m_commandLine.detectors);
			saved = detectors.ok ? IoResult::success(saved.message + "; " + detectors.message) : detectors;
		}
		const std::string summary = fmt::format("{} samples, {} dropped, {:.0f} ms of GPU time, {:.1f} s wall", m_raytracer.samplesAccumulated(), m_raytracer.droppedSamples(), m_raytracer.totalGpuMs(), elapsed);
		finish(saved.ok ? 0 : 2, saved.message + "; " + summary);
		break;
	}
}

void VulkanEngine::requestOpenFile(std::string title, std::vector<std::string> extensions, std::function<void(const std::filesystem::path&)> chosen)
{
	m_pendingFileRequest = FileRequest { std::move(title), std::move(extensions), std::move(chosen) };
	m_pendingFileAction = FileAction::OpenFileFor;
}

void VulkanEngine::runPendingFileAction()
{
	const FileAction action = m_pendingFileAction;
	m_pendingFileAction = FileAction::None;
	if (action == FileAction::None) {
		return;
	}

	const std::filesystem::path assetsDirectory = absoluteNormalized(m_rootPath + "assets");
	const std::filesystem::path sceneDirectory = m_scenePath.empty() ? assetsDirectory / "scenes" : m_scenePath.parent_path();

	switch (action) {
	case FileAction::None:
		break;
	case FileAction::NewScene:
		newScene();
		break;
	case FileAction::OpenScene:
		if (auto chosen = showOpenFileDialog("Open Scene", { SCENE_FILE_EXTENSION }, sceneDirectory)) {
			m_lastFileResult = openScene(*chosen);
		}
		break;
	case FileAction::SaveScene:
		if (!m_scenePath.empty()) {
			m_lastFileResult = saveScene(m_scenePath);
			break;
		}
		[[fallthrough]];
	case FileAction::SaveSceneAs:
		if (auto chosen = showSaveFileDialog("Save Scene", { SCENE_FILE_EXTENSION }, sceneDirectory, m_scenePath.empty() ? "scene.gltf" : m_scenePath.filename().string())) {
			m_lastFileResult = saveScene(*chosen);
		}
		break;
	case FileAction::ImportGltf:
		if (auto chosen = showOpenFileDialog("Import glTF Model", { "gltf", "glb" }, assetsDirectory)) {
			m_lastFileResult = importGltf(*chosen);
		}
		break;
	case FileAction::ImportPbrt:
		if (auto chosen = showOpenFileDialog("Import pbrt Scene", { "pbrt", "gz" }, assetsDirectory)) {
			m_lastFileResult = importPbrt(*chosen);
		}
		break;
	case FileAction::SetEnvironmentMap:
		if (auto chosen = showOpenFileDialog("Set Environment Map", { "hdr", "exr" }, assetsDirectory)) {
			m_lastFileResult = loadEnvironmentMap(*chosen);
		}
		break;
	case FileAction::ClearEnvironmentMap:
		clearEnvironmentMap();
		m_lastFileResult = IoResult::success("Environment map cleared");
		break;
	case FileAction::SaveReference:
		if (auto chosen = showSaveFileDialog("Save Reference", { "exr", "pfm" }, sceneDirectory, "reference.exr")) {
			m_lastFileResult = m_raytracer.saveReference(this, *chosen);
			m_raytracer.setReferenceIo(m_lastFileResult);
		}
		break;
	case FileAction::LoadReference:
		if (auto chosen = showOpenFileDialog("Load Reference", { "exr", "pfm" }, sceneDirectory)) {
			m_lastFileResult = m_raytracer.loadReference(this, *chosen);
			m_raytracer.setReferenceIo(m_lastFileResult);
		}
		break;
	case FileAction::ExportRender:
		if (auto chosen = showSaveFileDialog("Export Render", { "exr", "pfm", "hdr", "png" }, sceneDirectory, "render.exr")) {
			m_lastFileResult = m_raytracer.exportRender(this, m_raytraceScene, *chosen);
		}
		break;
	case FileAction::NewFromTemplate:
		newSceneFromTemplate(m_pendingTemplate);
		break;
	case FileAction::LoadGlassCatalog:
		if (auto chosen = showOpenFileDialog("Load Glass Catalog", { "agf", "AGF" }, assetsDirectory / "glass")) {
			std::vector<std::string> warnings;
			std::string error;
			if (m_opticalLibrary.loadCatalog(*chosen, warnings, &error)) {
				m_lastFileResult = IoResult::success(fmt::format("Loaded the glass catalog '{}'{}", chosen->filename().string(), warnings.empty() ? "" : fmt::format(" ({} warnings)", warnings.size())));
				for (const std::string& warning : warnings) {
					fmt::println("glass catalog: {}", warning);
				}
			} else {
				m_lastFileResult = IoResult::failure(fmt::format("The glass catalog '{}' did not load: {}", chosen->filename().string(), error));
			}
		}
		break;
	case FileAction::OpenFileFor: {
		FileRequest request = std::move(m_pendingFileRequest);
		m_pendingFileRequest = {};
		if (auto chosen = showOpenFileDialog(request.title, request.extensions, sceneDirectory)) {
			if (request.chosen) {
				request.chosen(*chosen);
			}
		}
		break;
	}
	case FileAction::ImportCoatingReflectance:
	case FileAction::ImportCoatingTransmittance:
		if (auto chosen = showOpenFileDialog("Import Coating Curve", { "csv", "txt" }, sceneDirectory)) {
			std::ifstream file(*chosen);
			std::stringstream text;
			text << file.rdbuf();
			std::string error;
			std::optional<OpticCoating> coating = readCoatingCsv(text.str(), action == FileAction::ImportCoatingTransmittance, 0.0, &error);
			if (coating) {
				coating->key = m_opticalLibrary.uniqueCoatingKey(chosen->stem().string());
				coating->name = chosen->stem().string();
				coating->source = fmt::format("Measured, from {} (normal incidence)", chosen->filename().string());
				m_opticalLibrary.setUserCoating(*coating);
				m_lastFileResult = IoResult::success(fmt::format("Imported the coating '{}'", coating->key));
			} else {
				m_lastFileResult = IoResult::failure(fmt::format("'{}' is not a coating curve: {}", chosen->filename().string(), error));
			}
		}
		break;
	}

	if (!m_lastFileResult.ok) {
		fmt::println("{}", m_lastFileResult.message);
	}
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

	glfwCreateWindowSurface(m_instance, m_window, m_allocator, &m_surface);

	VkPhysicalDeviceSynchronization2FeaturesKHR sync2_features{};
	sync2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
	sync2_features.synchronization2 = true;

	//vulkan 1.2 features
	VkPhysicalDeviceVulkan12Features features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	features.bufferDeviceAddress = true;
	features.descriptorIndexing = true;
	//the texture library's set (src/texture_library.h): an array of images indexed per invocation, written after it is
	//bound, partly filled and sized when it is allocated
	features.runtimeDescriptorArray = true;
	features.shaderSampledImageArrayNonUniformIndexing = true;
	features.descriptorBindingPartiallyBound = true;
	features.descriptorBindingSampledImageUpdateAfterBind = true;
	features.descriptorBindingVariableDescriptorCount = true;

	//use vkbootstrap to select a gpu. 
	//We want a gpu that can write to the GLFW surface and supports vulkan 1.3 with the correct features
	//the texture library's samplers are anisotropic, for the anisotropic texture filter
	VkPhysicalDeviceFeatures coreFeatures {};
	coreFeatures.samplerAnisotropy = VK_TRUE;
	//the beam preview's weighted blend sums into one target and multiplies into the other
	coreFeatures.independentBlend = VK_TRUE;

	vkb::PhysicalDeviceSelector selector{ vkb_inst };
	auto physdev_ret = selector
		.set_required_features(coreFeatures)
		.set_minimum_version(1, 2)
		.add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)
		.add_required_extension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)
		.add_required_extension(VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME)
		//float atomics on buffers: kernels 03, 04 and 08 add to the light groups' sums with them
		//(shaders/rt/include/crt_light_groups.glsl). Image float atomics are missing on this GPU
		.add_required_extension(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME)
		.set_required_features_12(features)
		.set_surface(m_surface)
		.select();
	vkbErr(physdev_ret);

	VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering_features{};
	dynamic_rendering_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
	dynamic_rendering_features.dynamicRendering = VK_TRUE;

	VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomic_float_features{};
	atomic_float_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
	atomic_float_features.shaderBufferFloat32Atomics = VK_TRUE;
	atomic_float_features.shaderBufferFloat32AtomicAdd = VK_TRUE;

	//create the final vulkan device
	vkb::PhysicalDevice physicalDevice = physdev_ret.value();
	vkb::DeviceBuilder deviceBuilder{ physicalDevice };
	auto dev_ret = deviceBuilder
		//each feature struct gets its own add_pNext: vk-bootstrap rewrites every struct's pNext as it
		//joins them, so a struct hung off another's pNext by hand is dropped
		.add_pNext(&dynamic_rendering_features)
		.add_pNext(&sync2_features)
		.add_pNext(&atomic_float_features)
		.build();
	vkbErr(dev_ret);
	vkb::Device vkbDevice = dev_ret.value();

	// Get the VkDevice handle used in the rest of a vulkan application
	m_device = vkbDevice.device;
	m_chosenGPU = physicalDevice.physical_device;

	vkGetPhysicalDeviceProperties(m_chosenGPU, &m_gpuProperties);
	VkPhysicalDeviceMaintenance3Properties maintenance3 { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES };
	VkPhysicalDeviceProperties2 properties2 { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &maintenance3 };
	vkGetPhysicalDeviceProperties2(m_chosenGPU, &properties2);
	m_maxAllocationBytes = maintenance3.maxMemoryAllocationSize;
	VkPhysicalDeviceMemoryProperties memoryProperties;
	vkGetPhysicalDeviceMemoryProperties(m_chosenGPU, &memoryProperties);
	for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; i++) {
		if ((memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
			m_deviceLocalBytes = std::max(m_deviceLocalBytes, memoryProperties.memoryHeaps[i].size);
		}
	}
	const VkPhysicalDeviceLimits& limits = m_gpuProperties.limits;
	fmt::println("GPU: {} - maxPushConstantsSize {} bytes, maxComputeWorkGroupInvocations {}, maxComputeWorkGroupSize {}x{}x{}, largest allocation {} MB, device-local {} MB",
		m_gpuProperties.deviceName, limits.maxPushConstantsSize, limits.maxComputeWorkGroupInvocations,
		limits.maxComputeWorkGroupSize[0], limits.maxComputeWorkGroupSize[1], limits.maxComputeWorkGroupSize[2],
		m_maxAllocationBytes >> 20, m_deviceLocalBytes >> 20);

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

	m_swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;
	auto swapchain_ret = swapchainBuilder
		.set_desired_format(VkSurfaceFormatKHR{ .format = m_swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
		.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
		.set_desired_extent(width, height)
		.add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
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

void VulkanEngine::resizeSwapchain()
{
	vkDeviceWaitIdle(m_device);

	destroySwapchain();

	int w, h;
	glfwGetWindowSize(m_window, &w, &h);
	m_windowExtent.width = w;
	m_windowExtent.height = h;

	createSwapchain(m_windowExtent.width, m_windowExtent.height);

	//the draw images follow the window, the largest the Viewport window can be, and the viewport shows the new image.
	//The device is idle, so the old views can go now
	m_displayRegistry.unregisterImage(VIEWPORT_TEXTURE_NAME);
	destroyDrawImages();
	createDrawImages(m_windowExtent);
	m_displayRegistry.registerTexture(VIEWPORT_TEXTURE_NAME, m_drawImage.imageView, { m_drawImage.imageExtent.width, m_drawImage.imageExtent.height });

	m_resizeRequested = false;
}

AllocatedBuffer VulkanEngine::createBuffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
	// allocate buffer
	VkBufferCreateInfo bufferInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bufferInfo.pNext = nullptr;
	bufferInfo.size = allocSize;

	bufferInfo.usage = usage;

	VmaAllocationCreateInfo vmaallocInfo = {};
	vmaallocInfo.usage = memoryUsage;
	vmaallocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	AllocatedBuffer newBuffer;

	// allocate the buffer
	checkVkResult(vmaCreateBuffer(m_memAllocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info));

	return newBuffer;
}

void VulkanEngine::destroyBuffer(const AllocatedBuffer& buffer)
{
	vmaDestroyBuffer(m_memAllocator, buffer.buffer, buffer.allocation);
}

AllocatedImage VulkanEngine::createImage(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped/* = false*/, bool colorView/* = false*/)
{
	AllocatedImage newImage;
	newImage.imageFormat = format;
	newImage.imageExtent = size;
	colorView = colorView && format == VK_FORMAT_R8G8B8A8_UNORM;

	VkImageCreateInfo img_info = vkinit::image_create_info(format, usage, size);
	if (colorView) {
		img_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	}
	if (mipmapped) {
		img_info.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(size.width, size.height)))) + 1;
	}

	// always allocate images on dedicated GPU memory
	VmaAllocationCreateInfo allocinfo = {};
	allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);


	// allocate and create the image
	checkVkResult(vmaCreateImage(m_memAllocator, &img_info, &allocinfo, &newImage.image, &newImage.allocation, nullptr));

	// if the format is a depth format, we will need to have it use the correct
	// aspect flag
	VkImageAspectFlags aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;
	if (format == VK_FORMAT_D32_SFLOAT) {
		aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
	}

	// build a image-view for the image
	VkImageViewCreateInfo view_info = vkinit::imageview_create_info(format, newImage.image, aspectFlag);
	view_info.subresourceRange.levelCount = img_info.mipLevels;

	checkVkResult(vkCreateImageView(m_device, &view_info, nullptr, &newImage.imageView));
	if (colorView) {
		view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
		checkVkResult(vkCreateImageView(m_device, &view_info, nullptr, &newImage.colorView));
	}

	return newImage;
}

AllocatedImage VulkanEngine::createImageArray(VkExtent3D size, uint32_t layers, VkFormat format, VkImageUsageFlags usage)
{
	AllocatedImage newImage;
	newImage.imageFormat = format;
	newImage.imageExtent = size;

	VkImageCreateInfo img_info = vkinit::image_create_info(format, usage, size);
	img_info.arrayLayers = layers;

	VmaAllocationCreateInfo allocinfo = {};
	allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	checkVkResult(vmaCreateImage(m_memAllocator, &img_info, &allocinfo, &newImage.image, &newImage.allocation, nullptr));

	VkImageViewCreateInfo view_info = vkinit::imageview_create_info(format, newImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	view_info.subresourceRange.layerCount = layers;
	checkVkResult(vkCreateImageView(m_device, &view_info, nullptr, &newImage.imageView));
	return newImage;
}

//bytes per texel for the formats uploaded through createImage(void* data, ...). Aborts on anything
//else rather than guessing a size and silently under- or over-reading the caller's buffer
static size_t bytes_per_pixel(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
		return 4;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		return 8;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		return 16;
	default:
		fmt::println("createImage: no upload size known for VkFormat {}", (int)format);
		abort();
	}
}

AllocatedImage VulkanEngine::createImage(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped/* = false*/, bool colorView/* = false*/)
{
	size_t data_size = (size_t)size.depth * size.width * size.height * bytes_per_pixel(format);
	AllocatedBuffer uploadbuffer = createBuffer(data_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

	memcpy(uploadbuffer.info.pMappedData, data, data_size);

	vmaFlushAllocation(m_memAllocator, uploadbuffer.allocation, 0, data_size);//flush vma on MoltenVK

	AllocatedImage new_image = createImage(size, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmapped, colorView);

	immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, new_image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkBufferImageCopy copyRegion = {};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = 0;
		copyRegion.bufferImageHeight = 0;

		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageExtent = size;

		// copy the buffer into the image
		vkCmdCopyBufferToImage(cmd, uploadbuffer.buffer, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

		vkutil::transition_image(cmd, new_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	destroyBuffer(uploadbuffer);

	return new_image;
}

void VulkanEngine::destroyImage(const AllocatedImage& img)
{
	vkDestroyImageView(m_device, img.imageView, nullptr);
	if (img.colorView != VK_NULL_HANDLE) {
		vkDestroyImageView(m_device, img.colorView, nullptr);
	}
	vmaDestroyImage(m_memAllocator, img.image, img.allocation);
}


GPUMeshBuffers VulkanEngine::uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices)
{
	const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
	const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

	GPUMeshBuffers newSurface;

	//create vertex buffer
	//a vertex buffer the pipeline reads through its vertex input, not by device address (see mesh.vert)
	newSurface.vertexBuffer = createBuffer(vertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	//create index buffer
	newSurface.indexBuffer = createBuffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	AllocatedBuffer staging = createBuffer(vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

	void* data = staging.allocation->GetMappedData();

	// copy vertex buffer
	memcpy(data, vertices.data(), vertexBufferSize);
	// copy index buffer
	memcpy((char*)data + vertexBufferSize, indices.data(), indexBufferSize);

	immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy vertexCopy{ 0 };
		vertexCopy.dstOffset = 0;
		vertexCopy.srcOffset = 0;
		vertexCopy.size = vertexBufferSize;

		vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

		VkBufferCopy indexCopy{ 0 };
		indexCopy.dstOffset = 0;
		indexCopy.srcOffset = vertexBufferSize;
		indexCopy.size = indexBufferSize;

		vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
	});

	destroyBuffer(staging);

	return newSurface;
}

void VulkanEngine::initSwapchain()
{
	createSwapchain(m_windowExtent.width, m_windowExtent.height);
	createDrawImages(m_windowExtent);

	//add to deletion queues: whatever draw images the members hold at shutdown
	m_mainDeletionQueue.push_function([this]() {
		destroyDrawImages();
	});
}

void VulkanEngine::createDrawImages(VkExtent2D extent)
{
	m_drawImagesCreated++;
	//the draw image is the window's size in points (the swapchain is the framebuffer, 2x that on a retina display),
	//the largest the Viewport window can be. The raster pass draws m_drawExtent of it, the Viewport window's size, and
	//the window shows that part, so the raster viewport, the projection's aspect and the window always agree
	VkExtent3D drawImageExtent = {
		extent.width,
		extent.height,
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
	//the Viewport window samples this image from imgui
	drawImageUsages |= VK_IMAGE_USAGE_SAMPLED_BIT;

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

	m_depthImage.imageFormat = VK_FORMAT_D32_SFLOAT;
	m_depthImage.imageExtent = drawImageExtent;

	VkImageUsageFlags depthImageUsages{};
	depthImageUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

	VkImageCreateInfo dimg_info = vkinit::image_create_info(m_depthImage.imageFormat, depthImageUsages, drawImageExtent);
	
	//allocate and create the image
	vmaCreateImage(m_memAllocator, &dimg_info, &rimg_allocinfo, &m_depthImage.image, &m_depthImage.allocation, nullptr);

	//build a image-view for the draw image to use for rendering
	VkImageViewCreateInfo dview_info = vkinit::imageview_create_info(m_depthImage.imageFormat, m_depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);

	checkVkResult(vkCreateImageView(m_device, &dview_info, nullptr, &m_depthImage.imageView));

	//the beam preview's weighted blend: drawn into as color attachments, read by its composite as storage images
	const VkImageUsageFlags beamUsages = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	m_beamPreviewAccum = createImage(drawImageExtent, VK_FORMAT_R16G16B16A16_SFLOAT, beamUsages);
	m_beamPreviewReveal = createImage(drawImageExtent, VK_FORMAT_R16G16B16A16_SFLOAT, beamUsages);
}

void VulkanEngine::destroyDrawImages()
{
	vkDestroyImageView(m_device, m_drawImage.imageView, nullptr);
	vmaDestroyImage(m_memAllocator, m_drawImage.image, m_drawImage.allocation);

	vkDestroyImageView(m_device, m_depthImage.imageView, nullptr);
	vmaDestroyImage(m_memAllocator, m_depthImage.image, m_depthImage.allocation);
	destroyImage(m_beamPreviewAccum);
	destroyImage(m_beamPreviewReveal);
	m_drawImage = {};
	m_depthImage = {};
	m_beamPreviewAccum = {};
	m_beamPreviewReveal = {};
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

	//[IMGUI] create a command pool for the immediate mode commands
	checkVkResult(vkCreateCommandPool(m_device, &commandPoolInfo, nullptr, &m_immCommandPool));

	//[IMGUI] allocate the command buffer for immediate submits
	VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(m_immCommandPool, 1);
	checkVkResult(vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &m_immCommandBuffer));
	m_mainDeletionQueue.push_function([this]() {
	vkDestroyCommandPool(m_device, m_immCommandPool, nullptr);
	});
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

	//two timestamps per frame slot around the raster pass, when the device can time graphics and compute work
	if (m_gpuProperties.limits.timestampComputeAndGraphics == VK_TRUE && m_gpuProperties.limits.timestampPeriod > 0.f) {
		m_timestampPeriodNs = m_gpuProperties.limits.timestampPeriod;
		VkQueryPoolCreateInfo queryInfo { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queryInfo.queryCount = 2;
		for (int i = 0; i < FRAME_OVERLAP; i++) {
			checkVkResult(vkCreateQueryPool(m_device, &queryInfo, nullptr, &m_frames[i].rasterTimestamps));
		}
	}

	//[IMGUI] create fence for immediate mode commands
	checkVkResult(vkCreateFence(m_device, &fenceCreateInfo, nullptr, &m_immFence));
	m_mainDeletionQueue.push_function([this]() { vkDestroyFence(m_device, m_immFence, nullptr); });

}

void VulkanEngine::initDescriptors()
{
	//create a descriptor pool that will hold 10 sets with 1 image each
	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes =
	{
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 }
	};

	m_globalDescriptorAllocator.init(m_device, 10, sizes);

	//the background effects' sets are allocated per frame from the frame allocators below; each
	//effect's layout lives in its ComputePass (initComputePipelines())

	for (int i = 0; i < FRAME_OVERLAP; i++) {
		// create a descriptor pool
		std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frame_sizes = { 
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 },
		};

		m_frames[i].frameDescriptors = DescriptorAllocatorGrowable{};
		m_frames[i].frameDescriptors.init(m_device, 1000, frame_sizes);

		//the preview shapes' long-lived material sets, one allocator per frame slot
		std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> shape_sizes = {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
		};
		m_previewShapeSlots[i].descriptors.init(m_device, 64, shape_sizes);
	
		m_mainDeletionQueue.push_function([&, i]() {
			m_frames[i].frameDescriptors.destroyPools(m_device);
			m_previewShapeSlots[i].descriptors.destroyPools(m_device);
			if (m_previewShapeSlots[i].capacity > 0) {
				destroyBuffer(m_previewShapeSlots[i].materialBuffer);
			}
		});
	}

	//make the descriptor set layout for our triangle raster draw
	{
		DescriptorLayoutBuilder builder;
		builder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		m_gpuSceneDataDescriptorLayout = builder.build(m_device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	}

	//make the descriptor layout for a single texture image
	{
		DescriptorLayoutBuilder builder;
		builder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		m_singleImageDescriptorLayout = builder.build(m_device, VK_SHADER_STAGE_FRAGMENT_BIT);
	}

	//make sure both the descriptor allocator and the new layout get cleaned up properly
	m_mainDeletionQueue.push_function([&]() {
		m_globalDescriptorAllocator.destroyPools(m_device);
		vkDestroyDescriptorSetLayout(m_device, m_gpuSceneDataDescriptorLayout, nullptr);
		vkDestroyDescriptorSetLayout(m_device, m_singleImageDescriptorLayout, nullptr);
	});
}

void VulkanEngine::initPipeline()
{
	initComputePipelines();
	initMeshPipeline();
	initOcclusionPipeline();
	initBeamPreviewPipeline();
	m_metalRoughMaterial.buildPipelines(this);

	m_tonemapPass.init(this);
	m_mainDeletionQueue.push_function([this]() {
		m_tonemapPass.destroy(m_device);
	});
}

void VulkanEngine::initComputePipelines()
{
	const std::string shaderDir = m_rootPath + "shaders/";

	//the gradient and the encode share one shape: the draw image at binding 0 plus ComputePushConstants
	auto buildDrawImageEffect = [&](const char* shaderFile) {
		return ComputePassBuilder(shaderDir + shaderFile)
			.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
			.setPushConstants<ComputePushConstants>()
			.build(m_device);
	};

	m_gradientPass = buildDrawImageEffect("gradient_color.comp.spv");
	//the sky or the environment map seen through the raster camera: the draw image as a storage image and the map
	//through a sampler
	m_environmentPass = ComputePassBuilder(shaderDir + "env_background.comp.spv")
		.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
		.setPushConstants<EnvironmentBackgroundPushConstants>()
		.build(m_device);
	//the raster preview's last pass: its linear draw image sRGB-encoded for the UNORM swapchain the Viewport is shown on
	m_viewportEncodePass = buildDrawImageEffect("viewport_encode.comp.spv");

	m_mainDeletionQueue.push_function([this]() {
		m_gradientPass.destroy(m_device);
		m_environmentPass.destroy(m_device);
		m_viewportEncodePass.destroy(m_device);
	});
}


void VulkanEngine::initMeshPipeline()
{
	VkShaderModule triangleFragShader;
	std::string fragPath = m_rootPath + "shaders/tex_image.frag.spv";
	if (!vkutil::load_shader_module(fragPath, m_device, &triangleFragShader)) {
		fmt::println("Error when building the triangle fragment shader module");
	}
	else {
		fmt::println("Triangle fragment shader succesfully loaded");
	}

	VkShaderModule triangleVertexShader;
	std::string vertPath = m_rootPath + "shaders/colored_triangle_mesh.vert.spv";
	if (!vkutil::load_shader_module(vertPath, m_device, &triangleVertexShader)) {
		fmt::println("Error when building the triangle vertex shader module");
	}
	else {
		fmt::println("Triangle vertex shader succesfully loaded");
	}

	VkPushConstantRange bufferRange{};
	bufferRange.offset = 0;
	bufferRange.size = sizeof(GPUDrawPushConstants);
	bufferRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	VkPipelineLayoutCreateInfo pipeline_layout_info = vkinit::pipelineLayoutCreateInfo();
	pipeline_layout_info.pPushConstantRanges = &bufferRange;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pSetLayouts = &m_singleImageDescriptorLayout;
	pipeline_layout_info.setLayoutCount = 1;

	checkVkResult(vkCreatePipelineLayout(m_device, &pipeline_layout_info, nullptr, &m_meshPipelineLayout));

	PipelineBuilder pipelineBuilder;

	//use the triangle layout we created
	pipelineBuilder.m_pipelineLayout = m_meshPipelineLayout;
	//connecting the vertex and pixel shaders to the pipeline
	pipelineBuilder.setShaders(triangleVertexShader, triangleFragShader);
	//it will draw triangles
	pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	//filled triangles
	pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
	//no backface culling
	pipelineBuilder.setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	//no multisampling
	pipelineBuilder.setMultisamplingNone();
	//no blending
	pipelineBuilder.disableBlending();

	pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);

	//connect the image format we will draw into, from draw image
	pipelineBuilder.setColorAttachmentFormat(m_drawImage.imageFormat);
	pipelineBuilder.setDepthFormat(m_depthImage.imageFormat);

	//finally build the pipeline
	m_meshPipeline = pipelineBuilder.buildPipeline(m_device);

	//clean structures
	vkDestroyShaderModule(m_device, triangleFragShader, nullptr);
	vkDestroyShaderModule(m_device, triangleVertexShader, nullptr);

	m_mainDeletionQueue.push_function([&]() {
		vkDestroyPipelineLayout(m_device, m_meshPipelineLayout, nullptr);
		vkDestroyPipeline(m_device, m_meshPipeline, nullptr);
	});
}

void VulkanEngine::initOcclusionPipeline()
{
	VkShaderModule vertexShader;
	VkShaderModule fragmentShader;
	if (!vkutil::load_shader_module(m_rootPath + "shaders/occlusion_box.vert.spv", m_device, &vertexShader)
		|| !vkutil::load_shader_module(m_rootPath + "shaders/occlusion_box.frag.spv", m_device, &fragmentShader)) {
		fmt::println("Error when building the occlusion box shader modules");
	}

	//set 0 for the view-projection, the box's two corners in push constants
	VkPushConstantRange boxRange {};
	boxRange.size = 2 * sizeof(glm::vec4);
	boxRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipelineLayoutCreateInfo();
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &m_gpuSceneDataDescriptorLayout;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &boxRange;
	checkVkResult(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_occlusionBoxPipeline.layout));

	//tested against the depth the surfaces left, writing neither depth nor color
	PipelineBuilder pipelineBuilder;
	pipelineBuilder.m_pipelineLayout = m_occlusionBoxPipeline.layout;
	pipelineBuilder.setShaders(vertexShader, fragmentShader);
	pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.setMultisamplingNone();
	pipelineBuilder.disableBlending();
	pipelineBuilder.m_colorBlendAttachment.colorWriteMask = 0;
	pipelineBuilder.enableDepthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
	pipelineBuilder.setColorAttachmentFormat(m_drawImage.imageFormat);
	pipelineBuilder.setDepthFormat(m_depthImage.imageFormat);
	m_occlusionBoxPipeline.pipeline = pipelineBuilder.buildPipeline(m_device);

	vkDestroyShaderModule(m_device, vertexShader, nullptr);
	vkDestroyShaderModule(m_device, fragmentShader, nullptr);

	m_mainDeletionQueue.push_function([this]() {
		vkDestroyPipelineLayout(m_device, m_occlusionBoxPipeline.layout, nullptr);
		vkDestroyPipeline(m_device, m_occlusionBoxPipeline.pipeline, nullptr);
	});
}

void VulkanEngine::initBeamPreviewPipeline()
{
	VkShaderModule vertexShader;
	VkShaderModule fragmentShader;
	if (!vkutil::load_shader_module(m_rootPath + "shaders/optics/beam_preview.vert.spv", m_device, &vertexShader)
		|| !vkutil::load_shader_module(m_rootPath + "shaders/optics/beam_preview.frag.spv", m_device, &fragmentShader)) {
		fmt::println("Error when building the beam preview's shader modules");
	}

	//set 0 for the view-projection, set 1 the sections, the eye, the beam's and the screen's numbers in push constants
	{
		DescriptorLayoutBuilder builder;
		builder.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		m_beamPreviewSetLayout = builder.build(m_device, VK_SHADER_STAGE_VERTEX_BIT);
	}
	const VkDescriptorSetLayout layouts[] = { m_gpuSceneDataDescriptorLayout, m_beamPreviewSetLayout };
	VkPushConstantRange pushRange {};
	pushRange.size = 3 * sizeof(glm::vec4);
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	VkPipelineLayoutCreateInfo layoutInfo = vkinit::pipelineLayoutCreateInfo();
	layoutInfo.setLayoutCount = 2;
	layoutInfo.pSetLayouts = layouts;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushRange;
	checkVkResult(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_beamPreviewPipeline.layout));

	//weighted blended order-independent transparency (McGuire and Bavoil 2013): each fragment's weighted color and weight
	//summed into one target, and the revealage multiplied by 1 - alpha in the other, so the order the beams are drawn in
	//does not matter. Tested against the raster pass's depth (reverse-Z) and writing none, so optics hide a beam behind
	//them. A tube ends exactly on the face it meets, which the optics draw biased toward the camera (1 step, slope 0.25), so
	//the tubes are biased a little further, or their ends would vanish into the face
	VkPipelineColorBlendAttachmentState accumBlend {};
	accumBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	accumBlend.blendEnable = VK_TRUE;
	accumBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	accumBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	accumBlend.colorBlendOp = VK_BLEND_OP_ADD;
	accumBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	accumBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	accumBlend.alphaBlendOp = VK_BLEND_OP_ADD;
	VkPipelineColorBlendAttachmentState revealBlend = accumBlend;
	revealBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
	revealBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	revealBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	revealBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	const VkFormat formats[] = { m_beamPreviewAccum.imageFormat, m_beamPreviewReveal.imageFormat };
	const VkPipelineColorBlendAttachmentState blends[] = { accumBlend, revealBlend };

	PipelineBuilder pipelineBuilder;
	pipelineBuilder.m_pipelineLayout = m_beamPreviewPipeline.layout;
	pipelineBuilder.setShaders(vertexShader, fragmentShader);
	pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.setDepthBias(2.f, 0.5f);
	pipelineBuilder.setMultisamplingNone();
	pipelineBuilder.setColorAttachments(formats, blends);
	pipelineBuilder.enableDepthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
	pipelineBuilder.setDepthFormat(m_depthImage.imageFormat);
	m_beamPreviewPipeline.pipeline = pipelineBuilder.buildPipeline(m_device);

	m_beamPreviewComposite = ComputePassBuilder(m_rootPath + "shaders/optics/beam_preview_composite.comp.spv")
		.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.build(m_device);

	vkDestroyShaderModule(m_device, vertexShader, nullptr);
	vkDestroyShaderModule(m_device, fragmentShader, nullptr);

	m_mainDeletionQueue.push_function([this]() {
		vkDestroyPipelineLayout(m_device, m_beamPreviewPipeline.layout, nullptr);
		vkDestroyPipeline(m_device, m_beamPreviewPipeline.pipeline, nullptr);
		m_beamPreviewComposite.destroy(m_device);
		vkDestroyDescriptorSetLayout(m_device, m_beamPreviewSetLayout, nullptr);
	});
}

void VulkanEngine::drawBeamPreview(VkCommandBuffer cmd, VkDescriptorSet globalDescriptor)
{
	if (m_beamSections.empty()) {
		return;
	}
	//this frame slot's copy, rewritten only when the sections changed since it was last written; the fence wait in draw()
	//means the slot's last use of it is over
	FrameData& frame = getCurrentFrame();
	const size_t bytes = m_beamSections.size() * sizeof(BeamSection);
	if (m_beamSections.size() > frame.beamSectionsCapacity) {
		if (frame.beamSections.buffer != VK_NULL_HANDLE) {
			destroyBuffer(frame.beamSections);
		}
		frame.beamSectionsCapacity = std::max<size_t>(m_beamSections.size() + m_beamSections.size() / 2, 256);
		frame.beamSections = createBuffer(frame.beamSectionsCapacity * sizeof(BeamSection), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		frame.beamSectionsRevision = 0;
	}
	if (frame.beamSectionsRevision != m_beamSectionsRevision) {
		std::memcpy(frame.beamSections.allocation->GetMappedData(), m_beamSections.data(), bytes);
		vmaFlushAllocation(m_memAllocator, frame.beamSections.allocation, 0, bytes);
		frame.beamSectionsRevision = m_beamSectionsRevision;
	}
	const VkDescriptorSet sections = frame.frameDescriptors.allocate(m_device, m_beamPreviewSetLayout);
	{
		DescriptorWriter writer;
		writer.writeBuffer(0, frame.beamSections.buffer, bytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.updateSet(m_device, sections);
	}
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_beamPreviewPipeline.pipeline);
	const VkDescriptorSet sets[] = { globalDescriptor, sections };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_beamPreviewPipeline.layout, 0, 2, sets, 0, nullptr);
	//shaders/optics/beam_common.glsl's: the eye, and dashes 4 mm long; the radius drawn, and every ring at least 1.5 pixels
	//in radius; the viewport's height and focal scale, and the blend's weights 1 at the orbit pivot's distance, where the
	//eye is most likely looking
	const BeamPreviewSettings& settings = m_raytraceScene.beamPreview();
	const glm::vec3 eyePosition = glm::vec3(glm::inverse(m_sceneData.view)[3]);
	glm::vec4 push[3];
	push[0] = glm::vec4(eyePosition, 0.008f);
	push[1] = glm::vec4(settings.radiusScale, 1.5f, 0.f, 0.f);
	const float focal = std::abs(rasterDepthProjection()[1][1]);
	push[2] = glm::vec4((float)m_drawExtent.height, focal, std::max(glm::distance(eyePosition, m_viewportNav.camera.pivot), 1e-3f), 0.f);
	vkCmdPushConstants(cmd, m_beamPreviewPipeline.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
	//BEAM_RING_VERTICES steps of two triangles a section
	vkCmdDraw(cmd, 48 * 6, (uint32_t)m_beamSections.size(), 0, 0);
	m_stats.drawcallCount++;
}

void VulkanEngine::initIMGUI()
{
	// 1: create descriptor pool for IMGUI
	//	the size of the pool is very oversize, but it's copied from imgui demo
	//	itself.
	VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 1000;
	pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
	pool_info.pPoolSizes = pool_sizes;

	VkDescriptorPool imguiPool;
	checkVkResult(vkCreateDescriptorPool(m_device, &pool_info, nullptr, &imguiPool));

	// 2: initialize imgui library

	// this initializes the core structures of imgui, and ImPlot's beside them (the Stats window's plots)
	ImGui::CreateContext();
	ImPlot::CreateContext();
	m_mainDeletionQueue.push_function([]() { ImPlot::DestroyContext(); });

	//every window docks into the main viewport's dock space (workspace.h), and moves only by its title bar or tab, so
	//a drag in the Viewport window is the gizmo's or the camera's, never the window's
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;

	//the workspaces' own entries in imgui.ini, then the file itself, read now rather than at the first frame so a file
	//from before the dock space can be discarded before any window takes its old position
	m_workspaces.init();
	m_viewportNav.registerSettingsHandler();
	if (ImGui::GetIO().IniFilename != nullptr) {
		ImGui::LoadIniSettingsFromDisk(ImGui::GetIO().IniFilename);
	}
	m_workspaces.afterIniLoaded();

	// this initializes imgui for GLFW
	ImGui_ImplGlfw_InitForVulkan(m_window, true);

	// this initializes imgui for Vulkan
	ImGui_ImplVulkan_InitInfo init_info = {};
	//must match the instance's apiVersion, so the backend loads the KHR dynamic rendering entry points rather than the core 1.3 ones
	init_info.ApiVersion = VK_API_VERSION_1_2;
	init_info.Instance = m_instance;
	init_info.PhysicalDevice = m_chosenGPU;
	init_info.Device = m_device;
	init_info.QueueFamily = m_graphicsQueueFamily;
	init_info.Queue = m_graphicsQueue;
	init_info.DescriptorPool = imguiPool;
	init_info.MinImageCount = 3;
	init_info.ImageCount = 3;
	init_info.UseDynamicRendering = true;
	init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	//dynamic rendering parameters for imgui to use
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &m_swapchainImageFormat;

	ImGui_ImplVulkan_Init(&init_info);

	//the font atlas uploads itself on demand now (ImGuiBackendFlags_RendererHasTextures), so there is no CreateFontsTexture call

	m_displayRegistry.init(FRAME_OVERLAP);

	// add the destroy the imgui created structures
	m_mainDeletionQueue.push_function([this, imguiPool]() {
		ImGui_ImplVulkan_Shutdown();
		vkDestroyDescriptorPool(m_device, imguiPool, nullptr);
	});

	//the queue is flushed in reverse, so this runs before the shutdown above - which it has to,
	//since releasing a display image goes back through the vulkan backend
	m_mainDeletionQueue.push_function([this]() {
		m_displayRegistry.destroyAll();
	});
}

//a shape's unit primitive as a drawable mesh: one surface, its bounds the mesh's own, and no
//material - drawRaytraceShapes() supplies a per-shape one rather than going through
//MeshNode::Draw(), which is what would otherwise read it
static MeshAsset makeShapePreviewMesh(VulkanEngine* engine, ShapeKind kind)
{
	ShapeMeshData data = buildShapeMesh(kind);

	glm::vec3 minPos = data.vertices[0].position;
	glm::vec3 maxPos = minPos;
	for (const Vertex& vertex : data.vertices) {
		minPos = glm::min(minPos, vertex.position);
		maxPos = glm::max(maxPos, vertex.position);
	}

	MeshAsset mesh;
	mesh.name = fmt::format("raytracer_{}", shapeTraits(kind).name);

	GeoSurface surface;
	surface.startIndex = 0;
	surface.count = (uint32_t)data.indices.size();
	surface.bounds.origin = (maxPos + minPos) * 0.5f;
	surface.bounds.extents = (maxPos - minPos) * 0.5f;
	surface.bounds.sphereRadius = glm::length(surface.bounds.extents);
	mesh.surfaces.push_back(surface);

	mesh.meshBuffers = engine->uploadMesh(data.indices, data.vertices);
	mesh.cpuIndices = std::move(data.indices);
	mesh.cpuVertices = std::move(data.vertices);

	return mesh;
}

void VulkanEngine::drawRaytraceShapes()
{
	const std::vector<SceneShape>& shapes = m_raytraceScene.shapes();
	const std::vector<SceneOptic>& optics = m_raytraceScene.optics();

	if (shapes.empty() && optics.empty()) {
		return;
	}
	//the slot's materials: one per shape, then one per optic
	const size_t entries = shapes.size() + optics.size();

	PreviewShapeSlot& slot = m_previewShapeSlots[m_frameNumber % FRAME_OVERLAP];
	const size_t stride = sizeof(GLTFMetallic_Roughness::MaterialConstants);

	//the slot's last frame has been waited on, so its buffer and sets are free to rewrite - and
	//they only are rewritten when something about the shapes changed
	if (slot.revision != m_raytraceScene.revision() || slot.capacity < entries) {
		if (slot.capacity < entries) {
			if (slot.capacity > 0) {
				destroyBuffer(slot.materialBuffer);
			}
			//one buffer for the whole set, indexed per shape by offset - the same layout
			//loadGltf() uses for a file's materials
			slot.capacity = entries;
			slot.materialBuffer = createBuffer(stride * slot.capacity, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		}

		GLTFMetallic_Roughness::MaterialConstants* constants = (GLTFMetallic_Roughness::MaterialConstants*)slot.materialBuffer.info.pMappedData;

		slot.descriptors.clearPools(m_device);
		slot.materials.clear();
		slot.materials.resize(entries);

		for (size_t i = 0; i < shapes.size(); i++) {
			const MaterialDef* material = m_raytraceScene.findMaterial(shapes[i].materialId);
			constants[i].colorFactors = glm::vec4(material != nullptr ? materialPreviewColor(*material) : glm::vec3(0.7f), 1.f);
			constants[i].metalRoughFactors = glm::vec4(1.f, 0.5f, 0.f, 0.f);

			GLTFMetallic_Roughness::MaterialResources resources;
			resources.colorImage = m_whiteImage;
			resources.colorSampler = m_defaultSamplerLinear;
			resources.metalRoughImage = m_whiteImage;
			resources.metalRoughSampler = m_defaultSamplerLinear;
			resources.dataBuffer = slot.materialBuffer.buffer;
			resources.dataBufferOffset = (uint32_t)(i * stride);

			slot.materials[i] = m_metalRoughMaterial.writeMaterial(m_device, MaterialPass::MainColor, resources, slot.descriptors);
		}

		//an optic in a flat color by what it is: glass a dim tint, drawn additively without depth (the transparent
		//pass); a mirror light gray; an iris's blades nearly black
		for (size_t i = 0; i < optics.size(); i++) {
			const size_t entry = shapes.size() + i;
			const OpticKind kind = optics[i].kind;
			const bool mirror = kind == OpticKind::PlanarMirror || kind == OpticKind::ConcaveMirror || kind == OpticKind::OffAxisParabola;
			const bool blades = kind == OpticKind::Iris;
			const glm::vec3 color = mirror ? glm::vec3(0.75f) : (blades ? glm::vec3(0.04f) : 0.22f * glm::vec3(0.75f, 0.85f, 1.f));
			constants[entry].colorFactors = glm::vec4(color, 1.f);
			constants[entry].metalRoughFactors = glm::vec4(mirror ? 1.f : 0.f, 0.3f, 0.f, 0.f);
			GLTFMetallic_Roughness::MaterialResources resources;
			resources.colorImage = m_whiteImage;
			resources.colorSampler = m_defaultSamplerLinear;
			resources.metalRoughImage = m_whiteImage;
			resources.metalRoughSampler = m_defaultSamplerLinear;
			resources.dataBuffer = slot.materialBuffer.buffer;
			resources.dataBufferOffset = (uint32_t)(entry * stride);
			slot.materials[entry] = m_metalRoughMaterial.writeMaterial(m_device, mirror || blades ? MaterialPass::MainColor : MaterialPass::Transparent, resources, slot.descriptors);
			//toward the camera by a few depth steps: an optic's faces lie in its mount's where they touch
			slot.materials[entry].pipeline = mirror || blades ? &m_metalRoughMaterial.opaqueBiasedPipeline : &m_metalRoughMaterial.transparentBiasedPipeline;
		}

		vmaFlushAllocation(m_memAllocator, slot.materialBuffer.allocation, 0, stride * entries);//flush vma on MoltenVK
		slot.revision = m_raytraceScene.revision();
	}
	drawOpticPreviews(slot, shapes.size());

	for (size_t i = 0; i < shapes.size(); i++) {
		const SceneShape& shape = shapes[i];
		const MeshAsset& mesh = *m_shapeMeshes[(size_t)shape.kind];
		const GeoSurface& surface = mesh.surfaces[0];

		RenderObject obj;
		obj.indexCount = surface.count;
		obj.firstIndex = surface.startIndex;
		obj.indexBuffer = mesh.meshBuffers.indexBuffer.buffer;
		obj.material = &slot.materials[i];
		obj.bounds = surface.bounds;
		obj.transform = shape.objectToWorld();
		obj.vertexBuffer = mesh.meshBuffers.vertexBuffer.buffer;

		m_mainDrawContext.opaqueSurfaces.push_back(obj);
	}
}

void VulkanEngine::drawOpticPreviews(PreviewShapeSlot& slot, size_t firstMaterial)
{
	const std::vector<SceneOptic>& optics = m_raytraceScene.optics();
	const OpticBuildContext context = opticBuildContext();
	static const char* const BLANKS[] = { "PF05-03", "PF10-03", "PF20-03" };
	const std::vector<OpticStack>& stacks = m_raytraceScene.benchStacks();
	//the blanks' surfaces drawn with the biased pipelines, as the tessellated optics are: copies of their materials, kept
	//until this frame's draw has recorded them
	m_opticBiasedMaterials.clear();
	for (size_t i = 0; i < optics.size(); i++) {
		const SceneOptic& optic = optics[i];
		const glm::mat4 transform(optic.bodyToWorld());
		//a mounted iris's blades are its housing model's, which the bench draws turned to the opening
		if (optic.kind == OpticKind::Iris && i < stacks.size() && !stacks[i].parts.empty()) {
			continue;
		}
		//a probe plane is no geometry: the Optical bench overlays outline it
		if (optic.kind == OpticKind::ProbePlane) {
			continue;
		}
		//a planar mirror at one of Thorlabs' sizes is its blank's model, which shares its frame (the front's center)
		if (opticTraits(optic.kind).previewFromModel && optic.sizePreset >= 0 && optic.sizePreset < 3) {
			if (const std::shared_ptr<LoadedModel> model = m_mountLibrary.meshes(BLANKS[optic.sizePreset])) {
				const size_t firstOpaque = m_mainDrawContext.opaqueSurfaces.size();
				const size_t firstTransparent = m_mainDrawContext.transparentSurfaces.size();
				for (const auto& node : model->topNodes) {
					node->Draw(transform, m_mainDrawContext);
				}
				auto bias = [this](RenderObject& object) {
					MaterialInstance& biased = m_opticBiasedMaterials.emplace_back(*object.material);
					biased.pipeline = biased.passType == MaterialPass::Transparent ? &m_metalRoughMaterial.transparentBiasedPipeline : &m_metalRoughMaterial.opaqueBiasedPipeline;
					object.material = &biased;
				};
				for (size_t s = firstOpaque; s < m_mainDrawContext.opaqueSurfaces.size(); s++) {
					bias(m_mainDrawContext.opaqueSurfaces[s]);
				}
				for (size_t s = firstTransparent; s < m_mainDrawContext.transparentSurfaces.size(); s++) {
					bias(m_mainDrawContext.transparentSurfaces[s]);
				}
				continue;
			}
		}
		const std::string key = fmt::format("{}|{}|{}|{}", opticBodyKey(optic), context.designWavelength, context.surrounding, m_opticalLibrary.revision());
		OpticPreviewMesh& entry = m_opticPreviewMeshes[key];
		if (entry.mesh == nullptr) {
			OpticReadout readout;
			const OpticMesh tessellated = tessellateOptic(buildOptic(optic, context, readout));
			if (tessellated.indices.empty()) {
				m_opticPreviewMeshes.erase(key);
				continue;
			}
			std::vector<Vertex> vertices;
			vertices.reserve(tessellated.vertices.size());
			for (const OpticMeshVertex& v : tessellated.vertices) {
				vertices.push_back(Vertex { v.position, 0.f, v.normal, 0.f, glm::vec4(1.f) });
			}
			std::vector<uint32_t> indices = tessellated.indices;
			auto mesh = std::make_shared<MeshAsset>();
			mesh->name = fmt::format("optic_{}", opticTraits(optic.kind).name);
			glm::vec3 lo = vertices[0].position;
			glm::vec3 hi = lo;
			for (const Vertex& v : vertices) {
				lo = glm::min(lo, v.position);
				hi = glm::max(hi, v.position);
			}
			GeoSurface surface;
			surface.startIndex = 0;
			surface.count = (uint32_t)indices.size();
			surface.bounds.origin = 0.5f * (lo + hi);
			surface.bounds.extents = 0.5f * (hi - lo);
			surface.bounds.sphereRadius = glm::length(surface.bounds.extents);
			mesh->surfaces.push_back(surface);
			mesh->meshBuffers = uploadMesh(indices, vertices);
			entry.mesh = std::move(mesh);
		}
		entry.lastFrame = m_frameNumber;
		const GeoSurface& surface = entry.mesh->surfaces[0];
		RenderObject object;
		object.indexCount = surface.count;
		object.firstIndex = surface.startIndex;
		object.indexBuffer = entry.mesh->meshBuffers.indexBuffer.buffer;
		object.material = &slot.materials[firstMaterial + i];
		object.bounds = surface.bounds;
		object.transform = transform;
		object.vertexBuffer = entry.mesh->meshBuffers.vertexBuffer.buffer;
		if (object.material->passType == MaterialPass::Transparent) {
			m_mainDrawContext.transparentSurfaces.push_back(object);
		} else {
			m_mainDrawContext.opaqueSurfaces.push_back(object);
		}
	}
	//a body no optic drew this frame is retired once the frames that may still draw it are done
	for (auto it = m_opticPreviewMeshes.begin(); it != m_opticPreviewMeshes.end();) {
		if (it->second.lastFrame != m_frameNumber) {
			const GPUMeshBuffers buffers = it->second.mesh->meshBuffers;
			getCurrentFrame().deletionQueue.push_function([this, buffers]() {
				destroyBuffer(buffers.indexBuffer);
				destroyBuffer(buffers.vertexBuffer);
			});
			it = m_opticPreviewMeshes.erase(it);
		} else {
			++it;
		}
	}
}

void VulkanEngine::initDefaultData()
{
	//3 default textures, white, grey, black. 1 pixel each
	uint32_t white = glm::packUnorm4x8(glm::vec4(1, 1, 1, 1));
	m_whiteImage = createImage((void*)&white, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	uint32_t grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1));
	m_greyImage = createImage((void*)&grey, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	uint32_t black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 1));
	m_blackImage = createImage((void*)&black, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	//checkerboard image
	uint32_t magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
	std::array<uint32_t, 16 *16 > pixels; //for 16x16 checkerboard texture
	for (int x = 0; x < 16; x++) {
		for (int y = 0; y < 16; y++) {
			pixels[y*16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
		}
	}
	m_errorCheckerboardImage = createImage(pixels.data(), VkExtent3D{16, 16, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

	VkSamplerCreateInfo sampl = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};

	sampl.magFilter = VK_FILTER_NEAREST;
	sampl.minFilter = VK_FILTER_NEAREST;

	vkCreateSampler(m_device, &sampl, nullptr, &m_defaultSamplerNearest);

	sampl.magFilter = VK_FILTER_LINEAR;
	sampl.minFilter = VK_FILTER_LINEAR;
	vkCreateSampler(m_device, &sampl, nullptr, &m_defaultSamplerLinear);

	m_mainDeletionQueue.push_function([&](){
		vkDestroySampler(m_device,m_defaultSamplerNearest,nullptr);
		vkDestroySampler(m_device,m_defaultSamplerLinear,nullptr);

		destroyImage(m_whiteImage);
		destroyImage(m_greyImage);
		destroyImage(m_blackImage);
		destroyImage(m_errorCheckerboardImage);
	});
	
	GLTFMetallic_Roughness::MaterialResources materialResources;
	//default the material textures
	materialResources.colorImage = m_whiteImage;
	materialResources.colorSampler = m_defaultSamplerLinear;
	materialResources.metalRoughImage = m_whiteImage;
	materialResources.metalRoughSampler = m_defaultSamplerLinear;

	//set the uniform buffer for the material data
	AllocatedBuffer materialConstants = createBuffer(sizeof(GLTFMetallic_Roughness::MaterialConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	//write the buffer
	GLTFMetallic_Roughness::MaterialConstants* sceneUniformData = (GLTFMetallic_Roughness::MaterialConstants*)materialConstants.allocation->GetMappedData();
	sceneUniformData->colorFactors = glm::vec4{1,1,1,1};
	sceneUniformData->metalRoughFactors = glm::vec4{1,0.5,0,0};
	vmaFlushAllocation(m_memAllocator, materialConstants.allocation, 0, sizeof(GLTFMetallic_Roughness::MaterialConstants));

	m_mainDeletionQueue.push_function([=, this]() {
		destroyBuffer(materialConstants);
	});

	materialResources.dataBuffer = materialConstants.buffer;
	materialResources.dataBufferOffset = 0;

	m_defaultData = m_metalRoughMaterial.writeMaterial(m_device,MaterialPass::MainColor,materialResources, m_globalDescriptorAllocator);

	for (const ShapeKind kind : SHAPE_KINDS) {
		std::shared_ptr<MeshAsset> mesh = std::make_shared<MeshAsset>(makeShapePreviewMesh(this, kind));
		m_shapeMeshes[(size_t)kind] = mesh;
		m_mainDeletionQueue.push_function([this, mesh]() {
			destroyBuffer(mesh->meshBuffers.indexBuffer);
			destroyBuffer(mesh->meshBuffers.vertexBuffer);
		});
	}

}

void VulkanEngine::cleanup()
{	
	if (m_isInitialized) {
		//make sure the gpu has stopped doing its things
		vkDeviceWaitIdle(m_device);

			//stops any in-flight render and releases its output image and the GPU tracer's resources
		//while the display registry it is registered with is still alive
		m_raytracer.shutdown(this);

		m_models.clear();
		m_benchScene.clear();
		m_mountLibrary.clear();
		for (const auto& [key, preview] : m_opticPreviewMeshes) {
			destroyBuffer(preview.mesh->meshBuffers.indexBuffer);
			destroyBuffer(preview.mesh->meshBuffers.vertexBuffer);
		}
		m_opticPreviewMeshes.clear();
		m_raytraceMeshData.reset();
		m_meshNodes.clear();
		//after the tracer, whose descriptor sets name the array's view
		m_raytraceTextures.destroy(this);
		m_textureLibrary.destroy(this);
		destroyEnvironmentMap();

		m_metalRoughMaterial.clearResources(m_device);

		for (int i = 0; i < FRAME_OVERLAP; i++) {
			vkDestroyCommandPool(m_device, m_frames[i].commandPool, nullptr);

			//destroy sync objects
			vkDestroyFence(m_device, m_frames[i].renderFence, nullptr);
			vkDestroySemaphore(m_device, m_frames[i].renderSemaphore, nullptr);
			vkDestroySemaphore(m_device ,m_frames[i].swapchainSemaphore, nullptr);

			m_frames[i].deletionQueue.flush();
			if (m_frames[i].instanceBuffer.buffer != VK_NULL_HANDLE) {
				destroyBuffer(m_frames[i].instanceBuffer);
			}
			if (m_frames[i].beamSections.buffer != VK_NULL_HANDLE) {
				destroyBuffer(m_frames[i].beamSections);
			}
			if (m_frames[i].occlusionQueries != VK_NULL_HANDLE) {
				vkDestroyQueryPool(m_device, m_frames[i].occlusionQueries, nullptr);
			}
			if (m_frames[i].rasterTimestamps != VK_NULL_HANDLE) {
				vkDestroyQueryPool(m_device, m_frames[i].rasterTimestamps, nullptr);
			}
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

void MeshNode::drawSurfaces(const glm::mat4& worldMatrix, DrawContext& ctx) const
{
	if (mesh == nullptr) {
		return;
	}

	for (auto& s : mesh->surfaces) {
		RenderObject def;
		def.indexCount = s.count;
		def.firstIndex = s.startIndex;
		def.indexBuffer = mesh->meshBuffers.indexBuffer.buffer;
		def.material = &s.material->data;
		def.bounds = s.bounds;
		def.transform = worldMatrix;
		def.vertexBuffer = mesh->meshBuffers.vertexBuffer.buffer;

		if (s.material->data.passType == MaterialPass::Transparent) {
			ctx.transparentSurfaces.push_back(def);
		} else {
			ctx.opaqueSurfaces.push_back(def);
		}
	}
}

void MeshNode::Draw(const glm::mat4& topMatrix, DrawContext& ctx)
{
	drawSurfaces(topMatrix * worldTransform, ctx);

	// recurse down
	Node::Draw(topMatrix, ctx);
}

void GLTFMetallic_Roughness::buildPipelines(VulkanEngine* engine)
{
	VkShaderModule meshFragShader;
	std::string fragPath = engine->m_rootPath + "shaders/mesh.frag.spv";
	if (!vkutil::load_shader_module(fragPath, engine->m_device, &meshFragShader)) {
		fmt::println("Error when building the triangle fragment shader module");
	}

	VkShaderModule meshVertexShader;
	std::string vertPath = engine->m_rootPath + "shaders/mesh.vert.spv";
	if (!vkutil::load_shader_module(vertPath, engine->m_device, &meshVertexShader)) {
		fmt::println("Error when building the triangle vertex shader module");
	}

	DescriptorLayoutBuilder layoutBuilder;
	layoutBuilder.addBinding(0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	layoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	layoutBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

	materialLayout = layoutBuilder.build(engine->m_device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	VkDescriptorSetLayout layouts[] = { engine->m_gpuSceneDataDescriptorLayout, materialLayout };

	VkPipelineLayoutCreateInfo mesh_layout_info = vkinit::pipelineLayoutCreateInfo();
	mesh_layout_info.setLayoutCount = 2;
	mesh_layout_info.pSetLayouts = layouts;

	VkPipelineLayout newLayout;
	checkVkResult(vkCreatePipelineLayout(engine->m_device, &mesh_layout_info, nullptr, &newLayout));

	opaquePipeline.layout = newLayout;
	transparentPipeline.layout = newLayout;

	// build the stage-create-info for both vertex and fragment stages. This lets
	// the pipeline know the shader modules per stage
	PipelineBuilder pipelineBuilder;
	pipelineBuilder.setShaders(meshVertexShader, meshFragShader);
	//binding 0 is the mesh's vertices, binding 1 the instances' world matrices (VulkanEngine::drawGeometry()),
	//as mesh.vert declares them
	const VkVertexInputBindingDescription bindings[] = {
		{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX },
		{ 1, sizeof(glm::mat4), VK_VERTEX_INPUT_RATE_INSTANCE },
	};
	const VkVertexInputAttributeDescription attributes[] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position) },
		{ 1, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, uv_x) },
		{ 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal) },
		{ 3, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, uv_y) },
		{ 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color) },
		{ 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
		{ 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
		{ 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 },
		{ 8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48 },
	};
	pipelineBuilder.setVertexInput(bindings, attributes);
	pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.setMultisamplingNone();
	pipelineBuilder.disableBlending();
	pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);

	//render format
	pipelineBuilder.setColorAttachmentFormat(engine->m_drawImage.imageFormat);
	pipelineBuilder.setDepthFormat(engine->m_depthImage.imageFormat);

	// use the triangle layout we created
	pipelineBuilder.m_pipelineLayout = newLayout;

	// finally build the pipeline
	opaquePipeline.pipeline = pipelineBuilder.buildPipeline(engine->m_device);

	// create the transparent variant
	pipelineBuilder.enableBlendingAdditive();

	pipelineBuilder.enableDepthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);

	transparentPipeline.pipeline = pipelineBuilder.buildPipeline(engine->m_device);

	//the biased pair: just enough to win a tie with a coplanar face, toward the camera, which under reverse-Z is a larger
	//depth. A float depth steps by about 1.2e-7 of itself, so the constant term is about 1e-7 of the distance, and the
	//slope term grows with how steeply a face recedes: too large a slope term let glass seen nearly edge on draw over
	//the lip of its mount from across the table
	opaqueBiasedPipeline.layout = newLayout;
	transparentBiasedPipeline.layout = newLayout;
	pipelineBuilder.setDepthBias(1.f, 0.25f);
	transparentBiasedPipeline.pipeline = pipelineBuilder.buildPipeline(engine->m_device);
	pipelineBuilder.disableBlending();
	pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
	opaqueBiasedPipeline.pipeline = pipelineBuilder.buildPipeline(engine->m_device);
	
	vkDestroyShaderModule(engine->m_device, meshFragShader, nullptr);
	vkDestroyShaderModule(engine->m_device, meshVertexShader, nullptr);
}

void GLTFMetallic_Roughness::clearResources(VkDevice device)
{
	vkDestroyDescriptorSetLayout(device,materialLayout,nullptr);
	vkDestroyPipelineLayout(device,transparentPipeline.layout,nullptr);

	vkDestroyPipeline(device, transparentPipeline.pipeline, nullptr);
	vkDestroyPipeline(device, opaquePipeline.pipeline, nullptr);
	vkDestroyPipeline(device, transparentBiasedPipeline.pipeline, nullptr);
	vkDestroyPipeline(device, opaqueBiasedPipeline.pipeline, nullptr);
}

MaterialInstance GLTFMetallic_Roughness::writeMaterial(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator)
{
	MaterialInstance matData;
	matData.passType = pass;
	if (pass == MaterialPass::Transparent) {
		matData.pipeline = &transparentPipeline;
	}
	else {
		matData.pipeline = &opaquePipeline;
	}

	matData.materialSet = descriptorAllocator.allocate(device, materialLayout);


	writer.clear();
	writer.writeBuffer(0, resources.dataBuffer, sizeof(MaterialConstants), resources.dataBufferOffset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	//a color texture through its sRGB view, decoded to the linear values the factors are in
	const VkImageView color = resources.colorImage.colorView != VK_NULL_HANDLE ? resources.colorImage.colorView : resources.colorImage.imageView;
	writer.writeImage(1, color, resources.colorSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	writer.writeImage(2, resources.metalRoughImage.imageView, resources.metalRoughSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

	writer.updateSet(device, matData.materialSet);

	return matData;
}

bool is_visible(const RenderObject& obj, const glm::mat4& viewproj) {
	std::array<glm::vec3, 8> corners {
		glm::vec3 { 1, 1, 1 },
		glm::vec3 { 1, 1, -1 },
		glm::vec3 { 1, -1, 1 },
		glm::vec3 { 1, -1, -1 },
		glm::vec3 { -1, 1, 1 },
		glm::vec3 { -1, 1, -1 },
		glm::vec3 { -1, -1, 1 },
		glm::vec3 { -1, -1, -1 },
	};

	glm::mat4 matrix = viewproj * obj.transform;

	glm::vec3 min = { 1.5, 1.5, 1.5 };
	glm::vec3 max = { -1.5, -1.5, -1.5 };

	for (int c = 0; c < 8; c++) {
		// project each corner into clip space
		glm::vec4 v = matrix * glm::vec4(obj.bounds.origin + (corners[c] * obj.bounds.extents), 1.f);

		// perspective correction
		v.x = v.x / v.w;
		v.y = v.y / v.w;
		v.z = v.z / v.w;

		min = glm::min(glm::vec3 { v.x, v.y, v.z }, min);
		max = glm::max(glm::vec3 { v.x, v.y, v.z }, max);
	}

	// check the clip space box is within the view
	if (min.z > 1.f || max.z < 0.f || min.x > 1.f || max.x < -1.f || min.y > 1.f || max.y < -1.f) {
		return false;
	} else {
		return true;
	}
}


