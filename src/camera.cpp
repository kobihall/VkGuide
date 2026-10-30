#include "camera.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>

namespace {

// just short of straight up or down, where yaw stops meaning anything
constexpr float PITCH_LIMIT = glm::radians(89.5f);

}

glm::mat4 Camera::getViewMatrix() const
{
	glm::mat4 cameraTranslation = glm::translate(glm::mat4(1.f), position);
	glm::mat4 cameraRotation = getRotationMatrix();
	return glm::inverse(cameraTranslation * cameraRotation);
}

glm::mat4 Camera::getRotationMatrix() const
{
	glm::quat pitchRotation = glm::angleAxis(pitch, glm::vec3 { 1.f, 0.f, 0.f });
	glm::quat yawRotation	= glm::angleAxis(yaw,	glm::vec3 { 0.f, -1.f, 0.f });

	return glm::toMat4(yawRotation) * glm::toMat4(pitchRotation);
}

glm::vec3 Camera::forward() const
{
	return glm::vec3(getRotationMatrix() * glm::vec4(0.f, 0.f, -1.f, 0.f));
}

glm::vec3 Camera::right() const
{
	return glm::vec3(getRotationMatrix() * glm::vec4(1.f, 0.f, 0.f, 0.f));
}

glm::vec3 Camera::up() const
{
	return glm::vec3(getRotationMatrix() * glm::vec4(0.f, 1.f, 0.f, 0.f));
}

void Camera::setPose(const glm::vec3& at, const glm::vec3& direction)
{
	//getRotationMatrix() yaws about -y then pitches about +x, giving
	//forward = (sin yaw cos pitch, sin pitch, -cos yaw cos pitch)
	const glm::vec3 d = glm::normalize(direction);
	position = at;
	pitch = std::clamp(glm::asin(glm::clamp(d.y, -1.f, 1.f)), -PITCH_LIMIT, PITCH_LIMIT);
	yaw = glm::atan(d.x, -d.z);
}

void Camera::look(float yawDelta, float pitchDelta, bool carryPivot)
{
	const float depth = glm::length(pivot - position);
	yaw += yawDelta;
	pitch = std::clamp(pitch + pitchDelta, -PITCH_LIMIT, PITCH_LIMIT);
	if (carryPivot) {
		pivot = position + forward() * depth;
	}
}

void Camera::orbit(const glm::vec3& center, float yawDelta, float pitchDelta, bool carryPivot)
{
	//the rotation that takes the old frame to the new one, applied to the camera's offset from the center: what the
	//camera sees of the center, its coordinates in the camera's frame, does not change
	const glm::mat3 before = glm::mat3(getRotationMatrix());
	yaw += yawDelta;
	pitch = std::clamp(pitch + pitchDelta, -PITCH_LIMIT, PITCH_LIMIT);
	const glm::mat3 turn = glm::mat3(getRotationMatrix()) * glm::transpose(before);
	position = center + turn * (position - center);
	if (carryPivot) {
		pivot = center + turn * (pivot - center);
	}
}

void Camera::dolly(const glm::vec3& center, float factor, bool carryPivot)
{
	position = center + (position - center) * factor;
	if (carryPivot) {
		pivot = center + (pivot - center) * factor;
	}
}

void Camera::translate(const glm::vec3& offset, bool carryPivot)
{
	position += offset;
	if (carryPivot) {
		pivot += offset;
	}
}

void Camera::pivotToViewAxis(float minDepth)
{
	pivot = position + forward() * std::max(glm::dot(pivot - position, forward()), minDepth);
}
