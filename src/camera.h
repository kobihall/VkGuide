#pragma once

#include <vk_types.h>

// The raster viewport's free camera: a position, a yaw and a pitch (no roll), and the pivot an orbit turns around and a
// dolly moves toward, Unreal's look-at point. It holds no input state: VulkanEngine::updateViewportCamera() turns the
// mouse, the keyboard and the trackpad into these moves, and a first-person edit writes the pose into a scene camera.
//
// Every move that takes `carryPivot` applies the same motion to the pivot when it is true, so a pivot on the view axis
// stays on it (the view center, ViewportOrbitPivot::ViewCenter); false leaves the pivot where it is in the world
// (ViewportOrbitPivot::LookAt)
class Camera {
public:
	glm::vec3 position { 0.f, 0.f, 5.f };
	// vertical rotation, held inside (-90, 90) degrees so the view never turns over
	float pitch { 0.f };
	// horizontal rotation
	float yaw { 0.f };
	glm::vec3 pivot { 0.f };

	glm::mat4 getViewMatrix() const;
	glm::mat4 getRotationMatrix() const;
	glm::vec3 forward() const;
	glm::vec3 right() const;
	glm::vec3 up() const;

	// at `at`, looking along `direction`, reduced to its yaw and pitch
	void setPose(const glm::vec3& at, const glm::vec3& direction);
	// turns the view in place by these angles in radians, as a mouse-look does
	void look(float yawDelta, float pitchDelta, bool carryPivot);
	// turns the camera around `center` by these angles: the center keeps its place on screen
	void orbit(const glm::vec3& center, float yawDelta, float pitchDelta, bool carryPivot);
	// scales the camera's distance from `center` by `factor`: the center keeps its place on screen
	void dolly(const glm::vec3& center, float factor, bool carryPivot);
	void translate(const glm::vec3& offset, bool carryPivot);
	// puts the pivot on the view axis at its current depth, or at `minDepth` when it is closer or behind
	void pivotToViewAxis(float minDepth);
};
