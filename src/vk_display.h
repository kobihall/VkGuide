#pragma once

#include <vk_types.h>

#include <glm/vec2.hpp>

#include <imgui.h>

// Filtering used when a displayed image is scaled to fit its window. The vulkan imgui
// backend samples every texture through one of two shared samplers, selected per draw
// call, so this is a draw-time choice rather than a VkSampler handed in at registration.
enum class DisplayFilter {
	Linear,
	Nearest
};

// A GPU image made visible in its own imgui window.
//
// The registry never owns the image. Whoever produces the content owns the VkImage and
// VkImageView, and carries two obligations:
//   - transition the image to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL inside draw(),
//     after the pass that writes it and before drawImgui() runs. Nothing here can do it:
//     drawWindows() runs while the imgui frame is being built, with no command buffer open.
//   - call unregisterImage() before destroying the image or its view, never after.
struct DisplayImage {
	std::string name;
	VkImageView imageView;
	VkExtent2D extent;
	DisplayFilter filter;
	ImTextureID textureId;
	bool visible;
	// optional, invoked on every frame the image is being clicked or dragged, with the
	// cursor position as a [0,1] uv over the image content
	std::function<void(glm::vec2 uv)> onInteract;
};

class DisplayRegistry {
public:
	// framesInFlight is used to work out when a released descriptor set has stopped being
	// referenced by recorded command buffers
	void init(uint32_t framesInFlightCount);

	// aborts if name is already registered - a duplicate name cannot be honoured without
	// silently dropping one of the two entries
	void registerImage(const std::string& name, VkImageView imageView, VkExtent2D extent, DisplayFilter filter = DisplayFilter::Linear, std::function<void(glm::vec2 uv)> onInteract = nullptr);
	// does nothing if name is not registered, so a producer's cleanup path can call it unconditionally
	void unregisterImage(const std::string& name);
	bool isRegistered(const std::string& name) const;
	// aborts if name is not registered
	void setVisible(const std::string& name, bool visible);
	bool isVisible(const std::string& name) const;

	// call once per frame before drawWindows(), with the engine's frame counter
	void beginFrame(uint64_t currentFrame);
	// builds one window per visible image, between ImGui::NewFrame() and ImGui::Render().
	// onInteract callbacks must not register or unregister images.
	void drawWindows();
	// contents of the "Windows" menu, called from inside the caller's own ImGui::BeginMenu()
	void drawWindowsMenu();

	// releases every descriptor set at once. Only safe with the device idle, and must run
	// before ImGui_ImplVulkan_Shutdown()
	void destroyAll();

private:
	struct PendingRemoval {
		ImTextureID textureId;
		uint64_t retireFrame;
	};

	DisplayImage* find(const std::string& name);

	std::vector<DisplayImage> images;
	std::vector<PendingRemoval> pendingRemovals;
	uint32_t framesInFlight{ 0 };
	uint64_t frameNumber{ 0 };
};
