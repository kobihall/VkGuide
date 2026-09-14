#include "vk_engine.h"

#include <rt_scene.h>
#include <scene_io.h>
#include <file_dialog.h>
#include <vk_ui.h>

#include <fmt/ranges.h>

#include <stb_image.h>
#include <glm/gtc/packing.hpp>

#include <stdio.h>			// printf, fprintf
#include <stdlib.h>			// abort, getenv
#include <iostream>
#include <vector>
#include <thread>

#include <vk_types.h>
#include <vk_initializers.h>
#include <vk_images.h>
#include <vk_pipelines.h>
#include <rt_material.h>

#include <VkBootstrap.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>

constexpr bool b_UseValidationLayers = false;

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

	//debug view of the main draw image. Hidden by default, shown from the "Windows" menu.
	//note this mirrors the whole draw image, so at a render scale below 1 the unused border
	//still holds the previous frame's pixels
	m_displayRegistry.registerImage("Scene Mirror", m_drawImage.imageView, { m_drawImage.imageExtent.width, m_drawImage.imageExtent.height });
	m_displayRegistry.setVisible("Scene Mirror", false);

	m_mainCamera.velocity = glm::vec3(0.f);
	m_mainCamera.position = glm::vec3(0, 0, 5);

	m_mainCamera.pitch = 0;
	m_mainCamera.yaw = 0;

	//the default scene: the tutorial's structure model plus the editor's seeded spheres. Not a
	//file - "Save Scene" writes one wherever the user chooses. A missing asset is reported
	//rather than fatal; the app is usable with an empty scene
	m_lastFileResult = importGltf(m_rootPath + "assets/structure.glb");
	if (!m_lastFileResult.ok) {
		fmt::println("{}", m_lastFileResult.message);
	}
	
	//everything went fine
	m_isInitialized = true;
}

void VulkanEngine::draw()
{
	// wait until the gpu has finished rendering the last frame. Timeout of 1
	// second
	checkVkResult(vkWaitForFences(m_device, 1, &getCurrentFrame().renderFence, true, 1000000000));

	getCurrentFrame().deletionQueue.flush();
	getCurrentFrame().frameDescriptors.clearPools(m_device);

	// after the flush and the pool reset, not before: updateScene() allocates this frame slot's
	// sphere-material buffer and descriptor sets, and both would be reclaimed out from under it
	updateScene();

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

	m_drawExtent.width = std::min(m_drawImage.imageExtent.width, m_swapchainExtent.width)*m_renderScale;
	m_drawExtent.height = std::min(m_drawImage.imageExtent.height, m_swapchainExtent.height)*m_renderScale;

	//start the command buffer recording
	checkVkResult(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

	// transition our main draw image into general layout so we can write into it
	// we will overwrite it all so we don't care about what was the older layout
	vkutil::transition_image(cmd, m_drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	//also set depth image layout. ideally only once, but we do every frame for now
	vkutil::transition_image(cmd, m_depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

	// flash the background color
	drawBackground(cmd);

	vkutil::transition_image(cmd, m_drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	drawGeometry(cmd);

	// transition the draw image and the swapchain image into their correct transfer layouts
	vkutil::transition_image(cmd, m_drawImage.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkutil::transition_image(cmd, m_swapchainImages[swapchainImageIndex],VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	// execute a copy from the draw image into the swapchain
	vkutil::copy_image_to_image(cmd, m_drawImage.image, m_swapchainImages[swapchainImageIndex], m_drawExtent, m_swapchainExtent);

	// the "Scene Mirror" display window samples the draw image, so it has to be readable before
	// drawImgui() below. Every image registered with m_displayRegistry needs this same transition.
	vkutil::transition_image(cmd, m_drawImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// set swapchain image layout to Attachment Optimal so we can draw it
	vkutil::transition_image(cmd, m_swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

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

	m_mainCamera.update();

	m_mainDrawContext.opaqueSurfaces.clear();

	for (auto& [name, model] : m_models) {
		model->Draw(glm::mat4{ 1.f }, m_mainDrawContext);
	}

	drawRaytraceSpheres();

	glm::mat4 view = m_mainCamera.getViewMatrix();
	//vulkan's clip space has y down, so the raster path flips what rasterProjection() returns
	glm::mat4 projection = rasterProjection();
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

void VulkanEngine::drawBackground(VkCommandBuffer cmd)
{
	ComputeEffect& effect = m_backgroundEffects[m_currentBackgroundEffect];

	// bind the gradient drawing compute pipeline
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);

	// bind the descriptor set containing the draw image for the compute pipeline
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipelineLayout, 0, 1, &m_drawImageDescriptors, 0, nullptr);

	vkCmdPushConstants(cmd, m_computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &effect.data);

	// execute the compute pipeline dispatch. We are using 16x16 workgroup size so we need to divide by it
	vkCmdDispatch(cmd, std::ceil(m_drawImage.imageExtent.width / 16.0), std::ceil(m_drawImage.imageExtent.height / 16.0), 1);
}

void VulkanEngine::drawGeometry(VkCommandBuffer cmd)
{
	std::vector<uint32_t> opaque_draws;
	opaque_draws.reserve(m_mainDrawContext.opaqueSurfaces.size());

	for (uint32_t i = 0; i < m_mainDrawContext.opaqueSurfaces.size(); i++) {
		if (is_visible(m_mainDrawContext.opaqueSurfaces[i], m_sceneData.viewproj)) {
			opaque_draws.push_back(i);
		}
	}

	// sort the opaque surfaces by material and mesh
	std::sort(opaque_draws.begin(), opaque_draws.end(), [&](const auto& iA, const auto& iB) {
		const RenderObject& A = m_mainDrawContext.opaqueSurfaces[iA];
		const RenderObject& B = m_mainDrawContext.opaqueSurfaces[iB];
		if (A.material == B.material) {
			return A.indexBuffer < B.indexBuffer;
		}
		else {
			return A.material < B.material;
		}
	});

	//reset counters
	m_stats.drawcallCount = 0;
	m_stats.triangleCount = 0;
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

	//defined outside of the draw function, this is the state we will try to skip
	MaterialPipeline* lastPipeline = nullptr;
	MaterialInstance* lastMaterial = nullptr;
	VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

	auto draw = [&](const RenderObject& r) {
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
				viewport.width = (float)m_windowExtent.width;
				viewport.height = (float)m_windowExtent.height;
				viewport.minDepth = 0.f;
				viewport.maxDepth = 1.f;

				vkCmdSetViewport(cmd, 0, 1, &viewport);

				VkRect2D scissor = {};
				scissor.offset.x = 0;
				scissor.offset.y = 0;
				scissor.extent.width = m_windowExtent.width;
				scissor.extent.height = m_windowExtent.height;

				vkCmdSetScissor(cmd, 0, 1, &scissor);
			}

			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.material->pipeline->layout, 1, 1,
				&r.material->materialSet, 0, nullptr);
		}
		//rebind index buffer if needed
		if (r.indexBuffer != lastIndexBuffer) {
			lastIndexBuffer = r.indexBuffer;
			vkCmdBindIndexBuffer(cmd, r.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
		}
		// calculate final mesh matrix
		GPUDrawPushConstants push_constants;
		push_constants.worldMatrix = r.transform;
		push_constants.vertexBuffer = r.vertexBufferAddress;

		vkCmdPushConstants(cmd, r.material->pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &push_constants);

		vkCmdDrawIndexed(cmd, r.indexCount, 1, r.firstIndex, 0, 0);
		//stats
		m_stats.drawcallCount++;
		m_stats.triangleCount += r.indexCount / 3;
	};

	for (auto& r : opaque_draws) {
		draw(m_mainDrawContext.opaqueSurfaces[r]);
	}
	for (auto& r : m_mainDrawContext.transparentSurfaces) {
		draw(r);
	}

	vkinit::VkFunctionLoader::get_instance().vkCmdEndRenderingKHR(cmd);

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
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
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
	bool bQuit = false;

	//main loop
	while (!bQuit && !glfwWindowShouldClose(m_window))
	{
		//begin clock
		auto start = std::chrono::system_clock::now();

		glfwPollEvents();

		if(glfwGetWindowAttrib(m_window, GLFW_VISIBLE) != GLFW_TRUE) {
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

		// imgui new frame
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		//first, so the window ImGuizmo draws into sits behind every panel built after it
		m_transformGizmo.beginFrame();

		m_displayRegistry.beginFrame(m_frameNumber);



		//before the imgui content below, so a raytrace that finished since the last frame has
		//its output registered in time to be drawn this frame rather than the next one
		m_raytraceJob.update(this);

		if (ImGui::BeginMainMenuBar()) {
			drawFileMenu();
			if (ImGui::BeginMenu("Windows")) {
				ImGui::MenuItem("background", nullptr, &m_showBackgroundWindow);
				ImGui::MenuItem("Stats", nullptr, &m_showStatsWindow);
				ImGui::MenuItem("Scene", nullptr, m_raytraceScene.visibilityFlag());
				ImGui::MenuItem("Raytrace Render", nullptr, m_raytraceJob.visibilityFlag());
				ImGui::MenuItem("ImGui Demo", nullptr, &m_showDemoWindow);
				ImGui::Separator();
				m_displayRegistry.drawWindowsMenu();
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}


		if (m_showBackgroundWindow) {
			if (ImGui::Begin("background", &m_showBackgroundWindow)) {
				ImGui::SliderFloat("Render Scale",&m_renderScale, 0.3f, 1.f);
				
				ComputeEffect& selected = m_backgroundEffects[m_currentBackgroundEffect];
			
				ImGui::Text("Selected effect: %s", selected.name);
			
				ImGui::SliderInt("Effect Index", &m_currentBackgroundEffect,0, m_backgroundEffects.size() - 1);
			
				ImGui::InputFloat4("data1",(float*)& selected.data.data1);
				ImGui::InputFloat4("data2",(float*)& selected.data.data2);
				ImGui::InputFloat4("data3",(float*)& selected.data.data3);
				ImGui::InputFloat4("data4",(float*)& selected.data.data4);
			}
			ImGui::End();
		}

		if (m_showStatsWindow) {
			if (ImGui::Begin("Stats", &m_showStatsWindow)) {
				ImGui::Text("frametime %f ms", m_stats.frameTime);
				ImGui::Text("draw time %f ms", m_stats.meshDrawTime);
				ImGui::Text("update time %f ms", m_stats.sceneUpdateTime);
				ImGui::Text("triangles %i", m_stats.triangleCount);
				ImGui::Text("draws %i", m_stats.drawcallCount);
			}
			ImGui::End();
		}

		m_raytraceScene.drawPanel(this);
		m_raytraceJob.drawControlPanel(this, m_raytraceScene);

		//some imgui UI to test
		if (m_showDemoWindow) {
			ImGui::ShowDemoWindow(&m_showDemoWindow);
		}

		m_displayRegistry.drawWindows();

		//after the panels, so an "Edit Transform" click shows the gizmo this frame. ImGuizmo
		//maps clip space to the screen itself, y up, so it gets the un-flipped projection
		m_transformGizmo.draw(m_mainCamera.getViewMatrix(), rasterProjection());

		//make imgui calculate internal draw structures
		ImGui::Render();

		//between the imgui frame and the draw, since a native file dialog blocks in a modal loop
		runPendingFileAction();

		draw();

		//get clock again, compare with start clock
		auto end = std::chrono::system_clock::now();
		
		//convert to microseconds (integer), and then come back to miliseconds
		auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
		m_stats.frameTime = elapsed.count() / 1000.f;
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
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	m_window = glfwCreateWindow(m_windowExtent.width, m_windowExtent.height, "Vulkan", nullptr, nullptr);
	glfwSetWindowUserPointer(m_window, this);

	//both of these are gated on the one capture flag rather than on separate heuristics, so there
	//is a single boundary between driving the camera and using the ui
	glfwSetKeyCallback(m_window, [](GLFWwindow* w, int key, int, int action, int) {
		auto* engine = static_cast<VulkanEngine*>(glfwGetWindowUserPointer(w));
		if (!engine->m_cameraCaptureActive) {
			return;
		}
		engine->m_mainCamera.processKeyEvent(key, action);
	});
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
		engine->m_mainCamera.processMouseMotion(x - engine->m_lastMouseX, y - engine->m_lastMouseY);
		engine->m_lastMouseX = x;
		engine->m_lastMouseY = y;
	});
	glfwSetMouseButtonCallback(m_window, [](GLFWwindow* w, int button, int action, int) {
		if (button != GLFW_MOUSE_BUTTON_RIGHT) {
			return;
		}
		auto* engine = static_cast<VulkanEngine*>(glfwGetWindowUserPointer(w));
		if (action == GLFW_PRESS) {
			//the ui has first claim on the cursor, so only a press over the raster background looks
			//around - and a press on or near an active gizmo handle is a gizmo drag, not a look
			if (!ImGui::GetIO().WantCaptureMouse && !engine->m_transformGizmo.wantsMouse()) {
				engine->setCameraCapture(true);
			}
		} else if (action == GLFW_RELEASE) {
			engine->setCameraCapture(false);
		}
	});
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
		//freely while looking around - without this it would wander over windows and hover them
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
		//re-arm, so the first delta is not measured against wherever the free cursor last sat
		m_firstMouse = true;
	} else {
		glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
		//the virtual position has drifted, so put the visible cursor back where the user left it
		glfwSetCursorPos(m_window, m_capturedCursorX, m_capturedCursorY);
		ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
		//a key released after this point never reaches the camera, so held movement would stick
		m_mainCamera.velocity = glm::vec3(0.f);
	}

	m_cameraCaptureActive = active;
}

glm::mat4 VulkanEngine::rasterProjection() const
{
	//forward-z, not vkguide's reversed near/far - see CLAUDE.md's MoltenVK notes
	return glm::perspective(glm::radians(CAMERA_VERTICAL_FOV_DEGREES), (float)m_windowExtent.width / (float)m_windowExtent.height, 0.1f, 10000.f);
}

namespace {

constexpr const char* ENVIRONMENT_MAP_DISPLAY_NAME = "Environment Map";
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
	m_sceneRevision++;
}

IoResult VulkanEngine::importGltf(const std::filesystem::path& path)
{
	std::string error;
	auto loaded = loadGltf(this, path.string(), &error);
	if (!loaded.has_value()) {
		return IoResult::failure(error);
	}
	(*loaded)->sourcePath = absoluteNormalized(path);

	//keyed by file stem, made unique so the same file can be in the scene twice
	const std::string stem = path.stem().string().empty() ? "model" : path.stem().string();
	std::string key = stem;
	for (int suffix = 2; m_models.count(key) != 0; suffix++) {
		key = fmt::format("{}_{}", stem, suffix);
	}
	m_models[key] = *loaded;

	rebuildSceneDerivedData();

	return IoResult::success(fmt::format("Imported '{}' as '{}'. Scene now has {} model(s), {} surface instance(s), {} triangles",
		path.filename().string(), key, m_models.size(), m_raytraceMeshData->instances.size(), m_raytraceMeshData->triangleCount));
}

void VulkanEngine::removeGltf(const std::string& name)
{
	auto it = m_models.find(name);
	if (it == m_models.end()) {
		return;
	}

	//frames in flight may still reference the model's buffers, images and descriptor pools, and
	//LoadedGLTF::clearAll() destroys them the moment the last reference goes. A wait is the
	//simple, obviously-correct way to retire them for a menu-driven action
	vkDeviceWaitIdle(m_device);
	m_models.erase(it);
	rebuildSceneDerivedData();
}

void VulkanEngine::clearModels()
{
	if (m_models.empty()) {
		return;
	}
	vkDeviceWaitIdle(m_device);
	m_models.clear();
	rebuildSceneDerivedData();
}

void VulkanEngine::newScene()
{
	clearModels();
	clearEnvironmentMap();
	m_raytraceScene.replaceSpheres({});
	m_scenePath.clear();
	m_lastFileResult = IoResult::success("New empty scene");
}

IoResult VulkanEngine::saveScene(const std::filesystem::path& path)
{
	SceneDescription scene;
	for (const auto& [name, model] : m_models) {
		scene.modelPaths.push_back(model->sourcePath);
	}
	scene.environmentMapPath = m_environmentMapPath;
	scene.spheres = m_raytraceScene.spheres();

	IoResult result = saveSceneFile(scene, path);
	if (result.ok) {
		m_scenePath = absoluteNormalized(path);
	}
	return result;
}

IoResult VulkanEngine::openScene(const std::filesystem::path& path)
{
	SceneDescription scene;
	IoResult parsed = loadSceneFile(path, scene);
	if (!parsed.ok) {
		//the current scene is untouched
		return parsed;
	}

	//only now that the file itself is known good does the current scene go. A model or map the
	//file refers to may still be missing; the rest of the scene loads regardless and the
	//problems are reported together
	clearModels();
	clearEnvironmentMap();
	std::vector<std::string> problems;
	for (const std::filesystem::path& modelPath : scene.modelPaths) {
		IoResult imported = importGltf(modelPath);
		if (!imported.ok) {
			problems.push_back(imported.message);
		}
	}
	if (!scene.environmentMapPath.empty()) {
		IoResult mapLoaded = loadEnvironmentMap(scene.environmentMapPath);
		if (!mapLoaded.ok) {
			problems.push_back(mapLoaded.message);
		}
	}
	m_raytraceScene.replaceSpheres(std::move(scene.spheres));
	m_scenePath = absoluteNormalized(path);

	if (!problems.empty()) {
		return IoResult::failure(fmt::format("Opened '{}' with {} problem(s): {}", path.filename().string(), problems.size(), fmt::join(problems, "; ")));
	}
	return IoResult::success(fmt::format("Opened '{}': {} model(s), {} sphere(s){}", path.filename().string(), m_models.size(), m_raytraceScene.spheres().size(),
		m_environmentMapPath.empty() ? "" : ", environment map"));
}

IoResult VulkanEngine::loadEnvironmentMap(const std::filesystem::path& path)
{
	const std::string pathString = path.string();

	if (!stbi_is_hdr(pathString.c_str())) {
		//stbi_is_hdr is also false for a file that is not there, so tell the two apart
		std::error_code ec;
		if (!std::filesystem::exists(path, ec)) {
			return IoResult::failure(fmt::format("No file at '{}'", pathString));
		}
		return IoResult::failure(fmt::format("'{}' is not a Radiance .hdr image (only .hdr is supported)", pathString));
	}

	int width = 0;
	int height = 0;
	int channels = 0;
	float* pixels = stbi_loadf(pathString.c_str(), &width, &height, &channels, 4);
	if (pixels == nullptr) {
		return IoResult::failure(fmt::format("Failed to decode '{}': {}", pathString, stbi_failure_reason()));
	}

	//half float: the usual precision for environment lighting, half the memory of rgba32f (an 8k
	//map is 256 MB rather than 512), and linearly filterable everywhere - 32-bit float filtering
	//is optional in vulkan
	const size_t pixelCount = (size_t)width * (size_t)height;
	std::vector<uint32_t> halves(pixelCount * 2);
	for (size_t i = 0; i < pixelCount; i++) {
		halves[i * 2 + 0] = glm::packHalf2x16(glm::vec2(pixels[i * 4 + 0], pixels[i * 4 + 1]));
		halves[i * 2 + 1] = glm::packHalf2x16(glm::vec2(pixels[i * 4 + 2], pixels[i * 4 + 3]));
	}
	stbi_image_free(pixels);

	//only now that the new data is known good does the old resource go
	destroyEnvironmentMap();

	const VkExtent3D extent { (uint32_t)width, (uint32_t)height, 1 };
	m_environmentMap = createImage(halves.data(), extent, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
	m_environmentMapExtent = { extent.width, extent.height };
	m_environmentMapPath = absoluteNormalized(path);

	//a preview window, so a loaded map can be seen even though nothing renders with it yet.
	//Values above 1 show clipped
	m_displayRegistry.registerImage(ENVIRONMENT_MAP_DISPLAY_NAME, m_environmentMap.imageView, m_environmentMapExtent);

	return IoResult::success(fmt::format("Loaded '{}': {} x {}, rgba16f", path.filename().string(), width, height));
}

void VulkanEngine::clearEnvironmentMap()
{
	destroyEnvironmentMap();
	m_environmentMapPath.clear();
}

void VulkanEngine::destroyEnvironmentMap()
{
	if (m_environmentMap.image == VK_NULL_HANDLE) {
		return;
	}

	//unregister first so no new frame references the view, then wait out the ones that already do
	m_displayRegistry.unregisterImage(ENVIRONMENT_MAP_DISPLAY_NAME);
	vkDeviceWaitIdle(m_device);
	destroyImage(m_environmentMap);
	m_environmentMap = {};
	m_environmentMapExtent = { 0, 0 };
}

void VulkanEngine::drawFileMenu()
{
	if (ImGui::BeginMenu("File")) {
		if (ImGui::MenuItem("New Scene")) {
			m_pendingFileAction = FileAction::NewScene;
		}
		if (ImGui::MenuItem("Open Scene...")) {
			m_pendingFileAction = FileAction::OpenScene;
		}
		if (ImGui::MenuItem("Save Scene")) {
			m_pendingFileAction = FileAction::SaveScene;
		}
		if (ImGui::MenuItem("Save Scene As...")) {
			m_pendingFileAction = FileAction::SaveSceneAs;
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Import glTF Model...")) {
			m_pendingFileAction = FileAction::ImportGltf;
		}
		if (ImGui::MenuItem("Set Environment Map...")) {
			m_pendingFileAction = FileAction::SetEnvironmentMap;
		}
		if (ImGui::MenuItem("Clear Environment Map", nullptr, false, m_environmentMap.image != VK_NULL_HANDLE)) {
			m_pendingFileAction = FileAction::ClearEnvironmentMap;
		}
		ImGui::EndMenu();
	}
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
	case FileAction::SetEnvironmentMap:
		if (auto chosen = showOpenFileDialog("Set Environment Map", { "hdr" }, assetsDirectory)) {
			m_lastFileResult = loadEnvironmentMap(*chosen);
		}
		break;
	case FileAction::ClearEnvironmentMap:
		clearEnvironmentMap();
		m_lastFileResult = IoResult::success("Environment map cleared");
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

	m_swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;
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

void VulkanEngine::resizeSwapchain()
{
	vkDeviceWaitIdle(m_device);

	destroySwapchain();

	int w, h;
	glfwGetWindowSize(m_window, &w, &h);
	m_windowExtent.width = w;
	m_windowExtent.height = h;

	createSwapchain(m_windowExtent.width, m_windowExtent.height);

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

AllocatedImage VulkanEngine::createImage(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped/* = false*/)
{
	AllocatedImage newImage;
	newImage.imageFormat = format;
	newImage.imageExtent = size;

	VkImageCreateInfo img_info = vkinit::image_create_info(format, usage, size);
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

AllocatedImage VulkanEngine::createImage(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped/* = false*/)
{
	size_t data_size = (size_t)size.depth * size.width * size.height * bytes_per_pixel(format);
	AllocatedBuffer uploadbuffer = createBuffer(data_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

	memcpy(uploadbuffer.info.pMappedData, data, data_size);

	vmaFlushAllocation(m_memAllocator, uploadbuffer.allocation, 0, data_size);//flush vma on MoltenVK

	AllocatedImage new_image = createImage(size, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmapped);

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
	vmaDestroyImage(m_memAllocator, img.image, img.allocation);
}


GPUMeshBuffers VulkanEngine::uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices)
{
	const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
	const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

	GPUMeshBuffers newSurface;

	//create vertex buffer
	newSurface.vertexBuffer = createBuffer(vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	//find the adress of the vertex buffer
	VkBufferDeviceAddressInfo deviceAdressInfo{
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.buffer = newSurface.vertexBuffer.buffer};
	newSurface.vertexBufferAddress = vkGetBufferDeviceAddress(m_device, &deviceAdressInfo);

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
	//needed by the "Scene Mirror" display window, which samples this image from imgui
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

	//add to deletion queues
	m_mainDeletionQueue.push_function([this]() {
		vkDestroyImageView(m_device, m_drawImage.imageView, nullptr);
		vmaDestroyImage(m_memAllocator, m_drawImage.image, m_drawImage.allocation);

		vkDestroyImageView(m_device, m_depthImage.imageView, nullptr);
		vmaDestroyImage(m_memAllocator, m_depthImage.image, m_depthImage.allocation);
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

	//make the descriptor set layout for our compute draw
	{
		DescriptorLayoutBuilder builder;
		builder.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		m_drawImageDescriptorLayout = builder.build(m_device, VK_SHADER_STAGE_COMPUTE_BIT);
	}

	// allocate a descriptor set for our draw image
	m_drawImageDescriptors = m_globalDescriptorAllocator.allocate(m_device, m_drawImageDescriptorLayout);

	DescriptorWriter writer;
	writer.writeImage(0, m_drawImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

	writer.updateSet(m_device,m_drawImageDescriptors);

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
	
		m_mainDeletionQueue.push_function([&, i]() {
			m_frames[i].frameDescriptors.destroyPools(m_device);
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
		vkDestroyDescriptorSetLayout(m_device, m_drawImageDescriptorLayout, nullptr);
		vkDestroyDescriptorSetLayout(m_device, m_gpuSceneDataDescriptorLayout, nullptr);
		vkDestroyDescriptorSetLayout(m_device, m_singleImageDescriptorLayout, nullptr);
	});
}

void VulkanEngine::initPipeline()
{
	initComputePipelines();
	initMeshPipeline();
	m_metalRoughMaterial.buildPipelines(this);

	m_tonemapPass.init(this);
	m_mainDeletionQueue.push_function([this]() {
		m_tonemapPass.destroy(m_device);
	});
}

void VulkanEngine::initComputePipelines()
{
	VkPipelineLayoutCreateInfo computeLayout{};
	computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	computeLayout.pNext = nullptr;
	computeLayout.pSetLayouts = &m_drawImageDescriptorLayout;
	computeLayout.setLayoutCount = 1;

	VkPushConstantRange pushConstant{};
	pushConstant.offset = 0;
	pushConstant.size = sizeof(ComputePushConstants) ;
	pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	computeLayout.pPushConstantRanges = &pushConstant;
	computeLayout.pushConstantRangeCount = 1;

	checkVkResult(vkCreatePipelineLayout(m_device, &computeLayout, nullptr, &m_computePipelineLayout));

	VkShaderModule gradientShader;
	std::string compPath = m_rootPath + "shaders/gradient_color.comp.spv";
	if (!vkutil::load_shader_module(compPath, m_device, &gradientShader)) {
		fmt::print("Error when building the compute shader \n");
	}
	
	VkShaderModule skyShader;
	compPath = m_rootPath + "shaders/sky.comp.spv";
	if (!vkutil::load_shader_module(compPath, m_device, &skyShader)) {
		fmt::print("Error when building the compute shader \n");
	}
	
	VkPipelineShaderStageCreateInfo stageinfo{};
	stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageinfo.pNext = nullptr;
	stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageinfo.module = gradientShader;
	stageinfo.pName = "main";

	VkComputePipelineCreateInfo computePipelineCreateInfo{};
	computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	computePipelineCreateInfo.pNext = nullptr;
	computePipelineCreateInfo.layout = m_computePipelineLayout;
	computePipelineCreateInfo.stage = stageinfo;

	ComputeEffect gradient;
	gradient.layout = m_computePipelineLayout;
	gradient.name = "gradient";
	gradient.data = {};

	//default colors
	gradient.data.data1 = glm::vec4(0.325, 0.471, 0.584, 1);
	gradient.data.data2 = glm::vec4(0.035, 0.125, 0.247, 1);
	
	checkVkResult(vkCreateComputePipelines(m_device,VK_NULL_HANDLE,1,&computePipelineCreateInfo, nullptr, &gradient.pipeline));

	//change the shader module only to create the sky shader
	computePipelineCreateInfo.stage.module = skyShader;

	ComputeEffect sky;
	sky.layout = m_computePipelineLayout;
	sky.name = "sky";
	sky.data = {};
	//default sky parameters
	sky.data.data1 = glm::vec4(0.1, 0.2, 0.4 ,0.97);

	checkVkResult(vkCreateComputePipelines(m_device,VK_NULL_HANDLE,1,&computePipelineCreateInfo, nullptr, &sky.pipeline));

	//add the 2 background effects into the array
	m_backgroundEffects.push_back(gradient);
	m_backgroundEffects.push_back(sky);


	vkDestroyShaderModule(m_device, gradientShader, nullptr);
	vkDestroyShaderModule(m_device, skyShader, nullptr);

	m_mainDeletionQueue.push_function([&]() {
		vkDestroyPipelineLayout(m_device, m_computePipelineLayout, nullptr);
		//vkDestroyPipeline(m_device, m_computePipeline, nullptr);
		for(int i = 0; i < m_backgroundEffects.size(); i++){
			vkDestroyPipeline(m_device, m_backgroundEffects[i].pipeline, nullptr);}
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

	pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_LESS_OR_EQUAL);

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

	// this initializes the core structures of imgui
	ImGui::CreateContext();

	//let windows be dragged onto each other to form tabbed/docked groups at runtime
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

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

//a unit uv sphere at the origin. Vertex colours are left white so that the per-sphere material's
//colorFactors is what carries the colour - mesh.vert multiplies the two together
static MeshAsset makeUnitSphereMesh(VulkanEngine* engine, uint32_t rings, uint32_t segments)
{
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;

	vertices.reserve((size_t)(rings + 1) * (segments + 1));

	for (uint32_t ring = 0; ring <= rings; ring++) {
		const double theta = RT_PI * (double)ring / (double)rings;
		const double sinTheta = sin(theta);
		const double cosTheta = cos(theta);

		for (uint32_t segment = 0; segment <= segments; segment++) {
			const double phi = 2.0 * RT_PI * (double)segment / (double)segments;

			Vertex vtx;
			vtx.position = glm::vec3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
			//on a unit sphere centred on the origin the position is also the normal
			vtx.normal = vtx.position;
			vtx.color = glm::vec4(1.f);
			vtx.uv_x = (float)segment / (float)segments;
			vtx.uv_y = (float)ring / (float)rings;

			vertices.push_back(vtx);
		}
	}

	indices.reserve((size_t)rings * segments * 6);

	for (uint32_t ring = 0; ring < rings; ring++) {
		for (uint32_t segment = 0; segment < segments; segment++) {
			const uint32_t a = ring * (segments + 1) + segment;
			const uint32_t b = a + segments + 1;

			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(a + 1);

			indices.push_back(a + 1);
			indices.push_back(b);
			indices.push_back(b + 1);
		}
	}

	MeshAsset mesh;
	mesh.name = "raytracer_sphere";

	GeoSurface surface;
	surface.startIndex = 0;
	surface.count = (uint32_t)indices.size();
	surface.bounds.origin = glm::vec3(0.f);
	surface.bounds.extents = glm::vec3(1.f);
	surface.bounds.sphereRadius = 1.f;
	//no material: drawRaytraceSpheres() supplies a per-sphere one rather than going through
	//MeshNode::Draw(), which is what would otherwise read this
	mesh.surfaces.push_back(surface);

	mesh.cpuIndices = indices;
	mesh.cpuVertices = vertices;
	mesh.meshBuffers = engine->uploadMesh(indices, vertices);

	return mesh;
}

void VulkanEngine::drawRaytraceSpheres()
{
	const std::vector<SceneSphere>& spheres = m_raytraceScene.spheres();

	if (m_sphereMesh == nullptr || spheres.empty()) {
		return;
	}

	//one buffer for the whole set, indexed per sphere by offset - the same layout loadGltf()
	//uses for a file's materials. Allocated per frame because the sphere list is live
	const size_t stride = sizeof(GLTFMetallic_Roughness::MaterialConstants);
	const size_t bufferSize = stride * spheres.size();

	AllocatedBuffer materialBuffer = createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	getCurrentFrame().deletionQueue.push_function([this, materialBuffer]() {
		destroyBuffer(materialBuffer);
	});

	GLTFMetallic_Roughness::MaterialConstants* constants = (GLTFMetallic_Roughness::MaterialConstants*)materialBuffer.info.pMappedData;

	m_sphereMaterials.clear();
	m_sphereMaterials.resize(spheres.size());

	const GeoSurface& surface = m_sphereMesh->surfaces[0];

	for (size_t i = 0; i < spheres.size(); i++) {
		const sphere& s = *spheres[i].object;

		constants[i].colorFactors = glm::vec4(materialPreviewColor(*s.mat_ptr), 1.f);
		constants[i].metalRoughFactors = glm::vec4(1.f, 0.5f, 0.f, 0.f);

		GLTFMetallic_Roughness::MaterialResources resources;
		resources.colorImage = m_whiteImage;
		resources.colorSampler = m_defaultSamplerLinear;
		resources.metalRoughImage = m_whiteImage;
		resources.metalRoughSampler = m_defaultSamplerLinear;
		resources.dataBuffer = materialBuffer.buffer;
		resources.dataBufferOffset = (uint32_t)(i * stride);

		m_sphereMaterials[i] = m_metalRoughMaterial.writeMaterial(m_device, MaterialPass::MainColor, resources, getCurrentFrame().frameDescriptors);

		RenderObject obj;
		obj.indexCount = surface.count;
		obj.firstIndex = surface.startIndex;
		obj.indexBuffer = m_sphereMesh->meshBuffers.indexBuffer.buffer;
		obj.material = &m_sphereMaterials[i];
		obj.bounds = surface.bounds;
		obj.transform = glm::translate(glm::mat4(1.f), glm::vec3(s.center)) * glm::scale(glm::mat4(1.f), glm::vec3((float)s.radius));
		obj.vertexBufferAddress = m_sphereMesh->meshBuffers.vertexBufferAddress;

		m_mainDrawContext.opaqueSurfaces.push_back(obj);
	}

	vmaFlushAllocation(m_memAllocator, materialBuffer.allocation, 0, bufferSize);//flush vma on MoltenVK
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

	m_sphereMesh = std::make_shared<MeshAsset>(makeUnitSphereMesh(this, 24, 32));
	m_mainDeletionQueue.push_function([this]() {
		destroyBuffer(m_sphereMesh->meshBuffers.indexBuffer);
		destroyBuffer(m_sphereMesh->meshBuffers.vertexBuffer);
	});

}

void VulkanEngine::cleanup()
{	
	if (m_isInitialized) {
		//make sure the gpu has stopped doing its things
		vkDeviceWaitIdle(m_device);

		//cancels and joins any in-flight raytrace, and releases its output image while the
		//display registry it is registered with is still alive
		m_raytraceJob.shutdown(this);

		m_models.clear();
		m_raytraceMeshData.reset();
		destroyEnvironmentMap();

		m_metalRoughMaterial.clearResources(m_device);

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

void MeshNode::Draw(const glm::mat4& topMatrix, DrawContext& ctx)
{
	glm::mat4 nodeMatrix = topMatrix * worldTransform;

	for (auto& s : mesh->surfaces) {
		RenderObject def;
		def.indexCount = s.count;
		def.firstIndex = s.startIndex;
		def.indexBuffer = mesh->meshBuffers.indexBuffer.buffer;
		def.material = &s.material->data;
		def.bounds = s.bounds;
		def.transform = nodeMatrix;
		def.vertexBufferAddress = mesh->meshBuffers.vertexBufferAddress;
		
		if (s.material->data.passType == MaterialPass::Transparent) {
			ctx.transparentSurfaces.push_back(def);
		} else {
			ctx.opaqueSurfaces.push_back(def);
		}
	}

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

	VkPushConstantRange matrixRange{};
	matrixRange.offset = 0;
	matrixRange.size = sizeof(GPUDrawPushConstants);
	matrixRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	DescriptorLayoutBuilder layoutBuilder;
	layoutBuilder.addBinding(0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	layoutBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	layoutBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

	materialLayout = layoutBuilder.build(engine->m_device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	VkDescriptorSetLayout layouts[] = { engine->m_gpuSceneDataDescriptorLayout, materialLayout };

	VkPipelineLayoutCreateInfo mesh_layout_info = vkinit::pipelineLayoutCreateInfo();
	mesh_layout_info.setLayoutCount = 2;
	mesh_layout_info.pSetLayouts = layouts;
	mesh_layout_info.pPushConstantRanges = &matrixRange;
	mesh_layout_info.pushConstantRangeCount = 1;

	VkPipelineLayout newLayout;
	checkVkResult(vkCreatePipelineLayout(engine->m_device, &mesh_layout_info, nullptr, &newLayout));

	opaquePipeline.layout = newLayout;
	transparentPipeline.layout = newLayout;

	// build the stage-create-info for both vertex and fragment stages. This lets
	// the pipeline know the shader modules per stage
	PipelineBuilder pipelineBuilder;
	pipelineBuilder.setShaders(meshVertexShader, meshFragShader);
	pipelineBuilder.setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.setPolygonMode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipelineBuilder.setMultisamplingNone();
	pipelineBuilder.disableBlending();
	pipelineBuilder.enableDepthtest(true, VK_COMPARE_OP_LESS_OR_EQUAL);

	//render format
	pipelineBuilder.setColorAttachmentFormat(engine->m_drawImage.imageFormat);
	pipelineBuilder.setDepthFormat(engine->m_depthImage.imageFormat);

	// use the triangle layout we created
	pipelineBuilder.m_pipelineLayout = newLayout;

	// finally build the pipeline
	opaquePipeline.pipeline = pipelineBuilder.buildPipeline(engine->m_device);

	// create the transparent variant
	pipelineBuilder.enableBlendingAdditive();

	pipelineBuilder.enableDepthtest(false, VK_COMPARE_OP_LESS_OR_EQUAL);

	transparentPipeline.pipeline = pipelineBuilder.buildPipeline(engine->m_device);
	
	vkDestroyShaderModule(engine->m_device, meshFragShader, nullptr);
	vkDestroyShaderModule(engine->m_device, meshVertexShader, nullptr);
}

void GLTFMetallic_Roughness::clearResources(VkDevice device)
{
	vkDestroyDescriptorSetLayout(device,materialLayout,nullptr);
	vkDestroyPipelineLayout(device,transparentPipeline.layout,nullptr);

	vkDestroyPipeline(device, transparentPipeline.pipeline, nullptr);
	vkDestroyPipeline(device, opaquePipeline.pipeline, nullptr);
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
	writer.writeImage(1, resources.colorImage.imageView, resources.colorSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
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