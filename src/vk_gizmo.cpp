#include <vk_gizmo.h>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

void TransformGizmo::beginEditing(const void* targetId, MatrixGetter getMatrix, MatrixSetter setMatrix, ImGuizmo::OPERATION operation, ImGuizmo::MODE mode)
{
	m_active = true;
	m_targetId = targetId;
	m_getMatrix = std::move(getMatrix);
	m_setMatrix = std::move(setMatrix);
	m_operation = operation;
	m_mode = mode;
}

void TransformGizmo::endEditing()
{
	m_active = false;
	m_targetId = nullptr;
	m_getMatrix = nullptr;
	m_setMatrix = nullptr;
}

void TransformGizmo::beginFrame()
{
	ImGuizmo::BeginFrame();
}

void TransformGizmo::draw(const glm::mat4& view, const glm::mat4& projection)
{
	if (!m_active) {
		return;
	}

	std::optional<glm::mat4> current = m_getMatrix();
	if (!current.has_value()) {
		//the target was deleted or replaced under us
		endEditing();
		return;
	}

	//the raster scene is a full-screen background rather than a panel, so the gizmo's rect is
	//the whole main viewport
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetRect(viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y);

	//ImGuizmo hit-tests the raw cursor and knows nothing about the imgui windows floating over
	//the background. While one of them is under the cursor the gizmo is drawn but inert - except
	//mid-drag, where the cursor crossing a window must not drop the handle. This must test the
	//hovered window, not io.WantCaptureMouse: ImGuizmo itself requests mouse capture whenever the
	//cursor is over a handle, so gating on that flag toggled the gizmo off every other frame
	const bool overWindow = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
	ImGuizmo::Enable(!overWindow || ImGuizmo::IsUsing());

	glm::mat4 matrix = *current;
	if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), m_operation, m_mode, glm::value_ptr(matrix))) {
		m_setMatrix(matrix);
	}
}

bool TransformGizmo::wantsMouse() const
{
	return m_active && (ImGuizmo::IsOver() || ImGuizmo::IsUsing());
}
