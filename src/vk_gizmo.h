#pragma once

// A single, object-agnostic 3D manipulation gizmo (ImGuizmo) for editing one scene object's
// transform directly in the viewport.
//
// At most one target is active at a time, and only after an explicit "Edit Transform" action on
// that object's own panel - never bound implicitly to a list selection. The target is described
// by a pair of closures rather than a raw matrix pointer or a virtual interface: none of the
// object types that want gizmo editing (a sphere is centre + radius, a future simulation plane
// is origin + orientation + extent) own a glm::mat4, so each supplies its own conversion to and
// from one. The getter may report that its object no longer exists, at which point editing ends
// on its own - deleting or reloading the edited object can never leave a dangling target.

#include <functional>
#include <optional>

#include <glm/glm.hpp>
//ImGuizmo.h does not include imgui itself and needs it first
#include <imgui.h>
#include <ImGuizmo.h>

class TransformGizmo {
public:
	// the target's current transform, or nothing once the target has gone away
	using MatrixGetter = std::function<std::optional<glm::mat4>()>;
	// receives the manipulated transform; the owner converts it back and records the change
	using MatrixSetter = std::function<void(const glm::mat4&)>;

	// replaces whatever target was active before. targetId only identifies the target for
	// isEditing() and is never dereferenced
	void beginEditing(const void* targetId, MatrixGetter getMatrix, MatrixSetter setMatrix,
		ImGuizmo::OPERATION operation, ImGuizmo::MODE mode = ImGuizmo::WORLD);
	void endEditing();

	bool isActive() const { return m_active; }
	bool isEditing(const void* targetId) const { return m_active && m_targetId == targetId; }

	// once per frame straight after ImGui::NewFrame(), before any other window is built, so the
	// full-screen window ImGuizmo draws into sits behind everything else
	void beginFrame();

	// once per frame after the panels are built, so a click on "Edit Transform" shows the gizmo
	// the same frame. view/projection are the raster camera's, with the projection in OpenGL
	// convention (y up): ImGuizmo maps clip space to the screen itself and must not be given
	// the vulkan y-flipped matrix
	void draw(const glm::mat4& view, const glm::mat4& projection);

	// true while the cursor is over or dragging a gizmo handle. The camera-capture entry trigger
	// checks this so a click on the gizmo is never mistaken for a look-around request
	bool wantsMouse() const;

private:
	bool m_active { false };
	const void* m_targetId { nullptr };
	MatrixGetter m_getMatrix;
	MatrixSetter m_setMatrix;
	ImGuizmo::OPERATION m_operation { ImGuizmo::TRANSLATE };
	ImGuizmo::MODE m_mode { ImGuizmo::WORLD };
};
