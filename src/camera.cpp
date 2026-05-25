#include "camera.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <GLFW/glfw3.h>

void Camera::update()
{
    glm::mat4 cameraRotation = getRotationMatrix();
    position += glm::vec3(cameraRotation * glm::vec4(velocity * 0.5f, 0.f));
}

void Camera::processKeyEvent(int key, int action)
{
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (key == GLFW_KEY_W) velocity.z = -1;
        if (key == GLFW_KEY_S) velocity.z =  1;
        if (key == GLFW_KEY_A) velocity.x = -1;
        if (key == GLFW_KEY_D) velocity.x =  1;
    }
    if (action == GLFW_RELEASE) {
        if (key == GLFW_KEY_W || key == GLFW_KEY_S) velocity.z = 0;
        if (key == GLFW_KEY_A || key == GLFW_KEY_D) velocity.x = 0;
    }
}

void Camera::processMouseMotion(double xrel, double yrel)
{
    yaw   += (float)xrel / 200.f;
    pitch -= (float)yrel / 200.f;
}

glm::mat4 Camera::getViewMatrix()
{
    glm::mat4 cameraTranslation = glm::translate(glm::mat4(1.f), position);
    glm::mat4 cameraRotation = getRotationMatrix();
    return glm::inverse(cameraTranslation * cameraRotation);
}

glm::mat4 Camera::getRotationMatrix()
{
    glm::quat pitchRotation = glm::angleAxis(pitch, glm::vec3 { 1.f, 0.f, 0.f });
    glm::quat yawRotation   = glm::angleAxis(yaw,   glm::vec3 { 0.f, -1.f, 0.f });

    return glm::toMat4(yawRotation) * glm::toMat4(pitchRotation);
}
