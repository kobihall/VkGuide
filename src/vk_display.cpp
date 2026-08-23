#include <vk_display.h>

#include <algorithm>

#include <imgui_impl_vulkan.h>

//ImTextureID is a 64 bit integer, VkDescriptorSet is a handle, so the conversion goes through uintptr_t
static ImTextureID to_texture_id(VkDescriptorSet set)
{
	return (ImTextureID)(uintptr_t)set;
}

static VkDescriptorSet to_descriptor_set(ImTextureID textureId)
{
	return (VkDescriptorSet)(uintptr_t)textureId;
}

//draws the image itself, letterboxed and centered inside whatever space the window has left
static void draw_image_content(DisplayImage& image)
{
	const ImVec2 available = ImGui::GetContentRegionAvail();
	const float imageWidth = (float)image.extent.width;
	const float imageHeight = (float)image.extent.height;

	if (available.x <= 0.f || available.y <= 0.f || imageWidth <= 0.f || imageHeight <= 0.f) {
		return;
	}

	//fit inside the content region, so the whole image stays visible and un-cropped at any window size
	const float scale = std::min(available.x / imageWidth, available.y / imageHeight);
	const ImVec2 drawSize { imageWidth * scale, imageHeight * scale };

	if (drawSize.x < 1.f || drawSize.y < 1.f) {
		return;
	}

	const ImVec2 origin = ImGui::GetCursorPos();
	const ImVec2 imagePos { origin.x + (available.x - drawSize.x) * 0.5f, origin.y + (available.y - drawSize.y) * 0.5f };
	ImGui::SetCursorPos(imagePos);

	//the backend defaults to the linear sampler, so nearest is switched on and back off around the image
	const ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
	const bool overrideSampler = image.filter == DisplayFilter::Nearest && platformIO.DrawCallback_SetSamplerNearest != nullptr && platformIO.DrawCallback_SetSamplerLinear != nullptr;

	if (overrideSampler) {
		ImGui::GetWindowDrawList()->AddCallback(platformIO.DrawCallback_SetSamplerNearest, nullptr);
	}

	ImGui::Image(image.textureId, drawSize);

	if (overrideSampler) {
		ImGui::GetWindowDrawList()->AddCallback(platformIO.DrawCallback_SetSamplerLinear, nullptr);
	}

	if (!image.onInteract) {
		return;
	}

	//ImGui::Image() is not an interactive item, so an invisible button over the same rect is
	//what gives IsItemActive() something to report - and it keeps reporting while the button
	//is held, which is what makes a press-and-drag gesture fire every frame rather than once
	ImGui::SetCursorPos(imagePos);
	ImGui::InvisibleButton("##interact", drawSize);

	if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		const ImVec2 rectMin = ImGui::GetItemRectMin();
		const ImVec2 cursor = ImGui::GetIO().MousePos;

		//a drag can wander outside the rect, clamping pins it to the edge instead of reporting an off-image coordinate
		glm::vec2 uv {
			std::clamp((cursor.x - rectMin.x) / drawSize.x, 0.f, 1.f),
			std::clamp((cursor.y - rectMin.y) / drawSize.y, 0.f, 1.f)
		};

		image.onInteract(uv);
	}
}

void DisplayRegistry::init(uint32_t framesInFlightCount)
{
	framesInFlight = framesInFlightCount;
}

void DisplayRegistry::registerImage(const std::string& name, VkImageView imageView, VkExtent2D extent, DisplayFilter filter, std::function<void(glm::vec2 uv)> onInteract)
{
	if (find(name) != nullptr) {
		fmt::println("DisplayRegistry: image '{}' is already registered", name);
		abort();
	}

	DisplayImage image {};
	image.name = name;
	image.imageView = imageView;
	image.extent = extent;
	image.filter = filter;
	image.textureId = to_texture_id(ImGui_ImplVulkan_AddTexture(imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	image.visible = true;
	image.onInteract = std::move(onInteract);

	images.push_back(std::move(image));
}

void DisplayRegistry::unregisterImage(const std::string& name)
{
	auto found = std::find_if(images.begin(), images.end(), [&name](const DisplayImage& image) { return image.name == name; });
	if (found == images.end()) {
		return;
	}

	//the descriptor set can still be referenced by command buffers that have not finished
	//executing, so freeing it is left to beginFrame() once they all have
	pendingRemovals.push_back({ found->textureId, frameNumber + framesInFlight + 1 });
	images.erase(found);
}

bool DisplayRegistry::isRegistered(const std::string& name) const
{
	return std::any_of(images.begin(), images.end(), [&name](const DisplayImage& image) { return image.name == name; });
}

void DisplayRegistry::setVisible(const std::string& name, bool visible)
{
	DisplayImage* image = find(name);
	if (image == nullptr) {
		fmt::println("DisplayRegistry: image '{}' is not registered", name);
		abort();
	}

	image->visible = visible;
}

void DisplayRegistry::beginFrame(uint64_t currentFrame)
{
	frameNumber = currentFrame;

	//unregisterImage() records the frame a descriptor set stops being recorded into command
	//buffers. draw() waits on frame N's fence at the start of frame N + framesInFlight, and
	//this runs before draw(), so frames up to currentFrame - framesInFlight - 1 are the ones
	//provably finished by now
	std::erase_if(pendingRemovals, [currentFrame](const PendingRemoval& pending) {
		if (currentFrame < pending.retireFrame) {
			return false;
		}

		ImGui_ImplVulkan_RemoveTexture(to_descriptor_set(pending.textureId));
		return true;
	});
}

void DisplayRegistry::drawWindows()
{
	for (DisplayImage& image : images) {
		if (!image.visible) {
			continue;
		}

		//a window that auto-fits to its contents, holding contents that fit themselves to the
		//window, settles at nothing - so give it a real starting size to letterbox into
		if (image.extent.width > 0 && image.extent.height > 0) {
			const float defaultWidth = 480.f;
			const float aspect = (float)image.extent.height / (float)image.extent.width;
			ImGui::SetNextWindowSize({ defaultWidth, defaultWidth * aspect + ImGui::GetFrameHeight() }, ImGuiCond_FirstUseEver);
		}

		//passing &visible gives the window a close button that clears the same flag the Windows menu toggles
		if (ImGui::Begin(image.name.c_str(), &image.visible)) {
			draw_image_content(image);
		}
		ImGui::End();
	}
}

void DisplayRegistry::drawWindowsMenu()
{
	if (images.empty()) {
		ImGui::TextDisabled("(no images registered)");
		return;
	}

	for (DisplayImage& image : images) {
		ImGui::MenuItem(image.name.c_str(), nullptr, &image.visible);
	}
}

void DisplayRegistry::destroyAll()
{
	for (const DisplayImage& image : images) {
		ImGui_ImplVulkan_RemoveTexture(to_descriptor_set(image.textureId));
	}
	for (const PendingRemoval& pending : pendingRemovals) {
		ImGui_ImplVulkan_RemoveTexture(to_descriptor_set(pending.textureId));
	}

	images.clear();
	pendingRemovals.clear();
}

DisplayImage* DisplayRegistry::find(const std::string& name)
{
	auto found = std::find_if(images.begin(), images.end(), [&name](const DisplayImage& image) { return image.name == name; });
	return found == images.end() ? nullptr : &(*found);
}
