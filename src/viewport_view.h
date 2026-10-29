#pragma once

// Where the raster preview is on screen this frame, and how to put a world point there: what every overlay over the
// Viewport window and the gizmo draw from. VulkanEngine::drawViewportWindow() fills it once per frame.

#include <glm/glm.hpp>

#include <imgui.h>

struct ViewportView {
	// the preview's rectangle in ImGui's screen coordinates (points)
	ImVec2 pos { 0.f, 0.f };
	ImVec2 size { 0.f, 0.f };
	// the raster camera's, the projection in OpenGL convention (y up), as the gizmo wants it
	glm::mat4 view { 1.f };
	glm::mat4 projection { 1.f };
	glm::mat4 viewProj { 1.f };
	// the Viewport window's draw list, clipped to the preview
	ImDrawList* drawList { nullptr };

	// world -> screen; false for a point behind the eye, which nothing should be drawn to
	bool project(const glm::vec3& world, ImVec2& out) const
	{
		const glm::vec4 clip = viewProj * glm::vec4(world, 1.f);
		if (clip.w <= 0.001f) {
			return false;
		}
		const glm::vec3 ndc = glm::vec3(clip) / clip.w;
		out = ImVec2(pos.x + (ndc.x * 0.5f + 0.5f) * size.x, pos.y + (0.5f - ndc.y * 0.5f) * size.y);
		return true;
	}

	void line(const glm::vec3& a, const glm::vec3& b, ImU32 color, float thickness = 1.5f) const
	{
		ImVec2 pa, pb;
		if (project(a, pa) && project(b, pb)) {
			drawList->AddLine(pa, pb, color, thickness);
		}
	}
};
