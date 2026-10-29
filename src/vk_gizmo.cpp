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

void TransformGizmo::draw(const ViewportView& view, bool hovered)
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

	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist(view.drawList);
	ImGuizmo::SetRect(view.pos.x, view.pos.y, view.size.x, view.size.y);

	//ImGuizmo hit-tests the raw cursor and knows nothing about the windows docked around the preview or floating over
	//it, so it is inert unless the preview itself is hovered - except mid-drag, where the cursor leaving the preview
	//must not drop the handle. This must not test io.WantCaptureMouse: ImGuizmo itself requests mouse capture whenever
	//the cursor is over a handle, so gating on that flag toggled the gizmo off every other frame
	ImGuizmo::Enable(hovered || ImGuizmo::IsUsing());

	glm::mat4 matrix = *current;
	if (ImGuizmo::Manipulate(glm::value_ptr(view.view), glm::value_ptr(view.projection), m_operation, m_mode, glm::value_ptr(matrix))) {
		m_setMatrix(matrix);
	}
}

bool TransformGizmo::wantsMouse() const
{
	return m_active && (ImGuizmo::IsOver() || ImGuizmo::IsUsing());
}
