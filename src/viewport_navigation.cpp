#include <viewport_navigation.h>

#include <length_unit.h>

#include <GLFW/glfw3.h>
#include <fmt/core.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// the look and orbit rate at a sensitivity of 1, what the camera always turned at: 200 points a radian
constexpr float LOOK_RADIANS_PER_POINT = 0.005f;
// an Option + RMB drag or a Control swipe of 100 points moves the camera to e^-0.5 of its distance from the pivot
constexpr float DOLLY_PER_POINT = 0.005f;
// a pinch's magnification is about 1 from fingers together to a full spread
constexpr float PINCH_DOLLY = 1.5f;
// a wheel step is this much of a second's flight
constexpr float WHEEL_SECONDS = 0.2f;
// a wheel step while looking multiplies the speed by this, as RMB + wheel does in Unreal
constexpr float SPEED_STEP = 1.2f;

constexpr float MIN_SENSITIVITY = 0.1f;
constexpr float MAX_SENSITIVITY = 5.f;

const char* orbitPivotKey(ViewportOrbitPivot pivot)
{
	return pivot == ViewportOrbitPivot::LookAt ? "look_at" : "view_center";
}

void* readOpen(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name)
{
	return std::strcmp(name, "Preferences") == 0 ? handler->UserData : nullptr;
}

void readLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
{
	ViewportCameraPrefs& prefs = static_cast<ViewportNavigation*>(entry)->prefs;
	float number = 0.f;
	int flag = 0;
	char key[32] {};
	if (std::sscanf(line, "LookSensitivity=%f", &number) == 1) {
		prefs.lookSensitivity = std::clamp(number, MIN_SENSITIVITY, MAX_SENSITIVITY);
	} else if (std::sscanf(line, "TrackpadGestures=%d", &flag) == 1) {
		prefs.trackpadGestures = flag != 0;
	} else if (std::sscanf(line, "OrbitPivot=%31s", key) == 1) {
		prefs.orbitPivot = std::strcmp(key, orbitPivotKey(ViewportOrbitPivot::LookAt)) == 0 ? ViewportOrbitPivot::LookAt : ViewportOrbitPivot::ViewCenter;
	}
}

void writeAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out)
{
	const ViewportCameraPrefs& prefs = static_cast<ViewportNavigation*>(handler->UserData)->prefs;
	out->appendf("[%s][Preferences]\n", handler->TypeName);
	out->appendf("LookSensitivity=%g\n", prefs.lookSensitivity);
	out->appendf("TrackpadGestures=%d\n", prefs.trackpadGestures ? 1 : 0);
	out->appendf("OrbitPivot=%s\n", orbitPivotKey(prefs.orbitPivot));
	out->append("\n");
}

// a logarithmic slider over a length in meters, shown in the scene's unit with three significant digits, which the
// clip planes need from tens of micrometers to thousands of kilometers. `perSecond` for a speed
bool sliderLengthLog(const char* label, float& meters, float min, float max, bool perSecond)
{
	const float scale = (float)lengthScale();
	const std::string format = fmt::format("%.3g {}{}", lengthSuffix(), perSecond ? "/s" : "");
	float shown = meters * scale;
	if (ImGui::SliderFloat(label, &shown, min * scale, max * scale, format.c_str(), ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
		meters = shown / scale;
		return true;
	}
	return false;
}

void controlLine(const char* input, const char* action)
{
	ImGui::TextUnformatted(input);
	ImGui::SameLine(190.f);
	ImGui::TextDisabled("%s", action);
}

}

void ViewportNavigation::registerSettingsHandler()
{
	ImGuiSettingsHandler handler;
	handler.TypeName = "ViewportCamera";
	handler.TypeHash = ImHashStr("ViewportCamera");
	handler.ReadOpenFn = readOpen;
	handler.ReadLineFn = readLine;
	handler.WriteAllFn = writeAll;
	handler.UserData = this;
	ImGui::AddSettingsHandler(&handler);
}

ViewportDrag ViewportNavigation::dragFor(int button, int mods)
{
	const bool option = (mods & GLFW_MOD_ALT) != 0;
	switch (button) {
	case GLFW_MOUSE_BUTTON_LEFT:
		return option ? ViewportDrag::Orbit : ViewportDrag::None;
	case GLFW_MOUSE_BUTTON_RIGHT:
		return option ? ViewportDrag::Dolly : ViewportDrag::Look;
	case GLFW_MOUSE_BUTTON_MIDDLE:
		return ViewportDrag::Pan;
	default:
		return ViewportDrag::None;
	}
}

void ViewportNavigation::beginDrag(ViewportDrag drag, int button)
{
	m_drag = drag;
	m_dragButton = button;
	m_dragCenter.reset();
	m_mouseDelta = glm::vec2(0.f);
}

void ViewportNavigation::endDrag()
{
	m_drag = ViewportDrag::None;
	m_dragButton = -1;
	m_dragCenter.reset();
}

void ViewportNavigation::mouseMoved(double dx, double dy)
{
	if (m_drag != ViewportDrag::None) {
		m_mouseDelta += glm::vec2((float)dx, (float)dy);
	}
}

void ViewportNavigation::scrolled(double yoffset)
{
	if (m_drag == ViewportDrag::Look) {
		settings.speed = std::clamp(settings.speed * std::pow(SPEED_STEP, (float)yoffset), ViewportCameraSettings::MIN_SPEED, ViewportCameraSettings::MAX_SPEED);
	} else {
		m_wheel += yoffset;
	}
}

float ViewportNavigation::minDepth() const
{
	return std::max(settings.nearClip, ViewportCameraSettings::MIN_NEAR);
}

glm::vec3 ViewportNavigation::center(const ViewportFrameInput& input)
{
	if (m_drag != ViewportDrag::None && m_dragCenter) {
		return *m_dragCenter;
	}
	const std::optional<glm::vec3> selected = input.selectionCenter ? input.selectionCenter() : std::nullopt;
	const glm::vec3 around = selected.value_or(camera.pivot);
	if (m_drag != ViewportDrag::None) {
		m_dragCenter = around;
	}
	return around;
}

void ViewportNavigation::pan(const glm::vec2& points, const glm::vec3& around, const ViewportFrameInput& input)
{
	//the size of a point at the pivot's depth, so the scene there follows the cursor. A look-at point the camera has
	//flown past is behind it, and its distance still sets a pace that suits where it is
	const float depth = std::max(std::abs(glm::dot(around - camera.position, camera.forward())), minDepth());
	const float worldPerPoint = 2.f * depth * std::tan(input.vfovRadians * 0.5f) / std::max(input.viewportHeight, 1.f);
	camera.translate((camera.up() * points.y - camera.right() * points.x) * worldPerPoint, carriesPivot());
}

void ViewportNavigation::update(const ViewportFrameInput& input)
{
	const bool carry = carriesPivot();
	const glm::vec2 mouse = m_mouseDelta;
	m_mouseDelta = glm::vec2(0.f);
	const float wheel = (float)m_wheel;
	m_wheel = 0.0;
	const TrackpadGestures gestures = input.hovered && prefs.trackpadGestures && m_drag == ViewportDrag::None ? input.gestures : TrackpadGestures {};

	//any other move takes over from a frame() where it has got to
	if (m_transition && (mouse != glm::vec2(0.f) || input.move != glm::vec3(0.f) || wheel != 0.f || !gestures.empty())) {
		m_transition.reset();
		if (carry) {
			camera.pivotToViewAxis(minDepth());
		}
	}
	if (m_transition) {
		m_transition->t = std::min(m_transition->t + input.dt / FRAME_SECONDS, 1.f);
		const float eased = 1.f - std::pow(1.f - m_transition->t, 3.f);
		camera.position = glm::mix(m_transition->from, m_transition->to, eased);
		if (m_transition->t >= 1.f) {
			m_transition.reset();
		}
	}

	const float rate = LOOK_RADIANS_PER_POINT * prefs.lookSensitivity;
	switch (m_drag) {
	case ViewportDrag::None:
		break;
	case ViewportDrag::Look:
		camera.look(mouse.x * rate, -mouse.y * rate, carry);
		if (input.move != glm::vec3(0.f)) {
			const glm::vec3 direction = camera.right() * input.move.x + glm::vec3(0.f, input.move.y, 0.f) + camera.forward() * input.move.z;
			camera.translate(direction * settings.speed * input.dt, carry);
		}
		break;
	case ViewportDrag::Orbit:
		if (mouse != glm::vec2(0.f)) {
			camera.orbit(center(input), mouse.x * rate, -mouse.y * rate, carry);
		}
		break;
	case ViewportDrag::Dolly:
		if (mouse != glm::vec2(0.f)) {
			//right or up moves in
			camera.dolly(center(input), std::exp(-(mouse.x - mouse.y) * DOLLY_PER_POINT), carry);
		}
		break;
	case ViewportDrag::Pan:
		if (mouse != glm::vec2(0.f)) {
			pan(mouse, center(input), input);
		}
		break;
	}

	if (wheel != 0.f) {
		camera.translate(camera.forward() * wheel * settings.speed * WHEEL_SECONDS, false);
		if (carry) {
			camera.pivotToViewAxis(minDepth());
		}
	}

	if (!gestures.empty()) {
		//the content's motion, as a mouse drag over it
		const glm::vec3 around = center(input);
		if (gestures.swipe != glm::vec2(0.f)) {
			camera.orbit(around, gestures.swipe.x * rate, -gestures.swipe.y * rate, carry);
		}
		if (gestures.shiftSwipe != glm::vec2(0.f)) {
			pan(gestures.shiftSwipe, around, input);
		}
		//content moving up, or fingers spreading, moves in
		const float dolly = gestures.controlSwipe.y * DOLLY_PER_POINT - gestures.pinch * PINCH_DOLLY;
		if (dolly != 0.f) {
			camera.dolly(around, std::exp(dolly), carry);
		}
	}
}

void ViewportNavigation::frame(const Aabb& box, float vfovRadians, float aspect)
{
	const glm::vec3 middle = 0.5f * (box.min + box.max);
	const float radius = std::max(0.5f * glm::length(box.max - box.min), 1e-4f);
	//the box's bounding sphere fills the narrower of the two half angles
	const float halfVertical = 0.5f * vfovRadians;
	const float halfHorizontal = std::atan(std::tan(halfVertical) * aspect);
	const float distance = radius / std::sin(std::min(halfVertical, halfHorizontal));
	camera.pivot = middle;
	m_transition = Transition { camera.position, middle - camera.forward() * distance, 0.f };
}

void ViewportNavigation::lookAt(const glm::vec3& at, const glm::vec3& target)
{
	m_transition.reset();
	camera.setPose(at, target - at);
	camera.pivot = target;
}

void ViewportNavigation::drawMenuSettings(bool fovLocked)
{
	using Limits = ViewportCameraSettings;
	ImGui::SetNextItemWidth(180.f);
	sliderLengthLog("Speed", settings.speed, Limits::MIN_SPEED, Limits::MAX_SPEED, true);
	ImGui::SetItemTooltip("How fast W A S D, Q and E fly, saved with the scene. RMB + wheel changes it while looking");

	ImGui::BeginDisabled(fovLocked);
	ImGui::SetNextItemWidth(180.f);
	ImGui::SliderFloat("Vertical FOV", &settings.vfovDegrees, Limits::MIN_FOV, Limits::MAX_FOV, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip(fovLocked ? "First-person edit fits the flown camera's frame in the viewport" : "The preview's, not a render camera's; a camera added from the view takes it");

	ImGui::SetNextItemWidth(180.f);
	if (sliderLengthLog("Near Clip", settings.nearClip, Limits::MIN_NEAR, Limits::MAX_NEAR, false)) {
		settings.farClip = std::max(settings.farClip, settings.nearClip * 2.f);
	}
	ImGui::SetNextItemWidth(180.f);
	if (sliderLengthLog("Far Clip", settings.farClip, Limits::MIN_FAR, Limits::MAX_FAR, false)) {
		settings.nearClip = std::min(settings.nearClip, settings.farClip * 0.5f);
	}
	if (ImGui::MenuItem("Reset Settings")) {
		settings = ViewportCameraSettings {};
	}
	ImGui::SetItemTooltip("Speed, fov and clip planes back to a new scene's");

	ImGui::SeparatorText("Preferences");
	bool prefsChanged = false;
	if (ImGui::BeginMenu("Orbit Pivot")) {
		if (ImGui::MenuItem("View Center", nullptr, prefs.orbitPivot == ViewportOrbitPivot::ViewCenter)) {
			prefs.orbitPivot = ViewportOrbitPivot::ViewCenter;
			camera.pivotToViewAxis(minDepth());
			prefsChanged = true;
		}
		ImGui::SetItemTooltip("A point ahead of the camera that flying carries along: an orbit circles the middle of the screen");
		if (ImGui::MenuItem("Look-At Point", nullptr, prefs.orbitPivot == ViewportOrbitPivot::LookAt)) {
			prefs.orbitPivot = ViewportOrbitPivot::LookAt;
			prefsChanged = true;
		}
		ImGui::SetItemTooltip("Unreal's: the point the last F framed stays put while you fly away from it");
		ImGui::EndMenu();
	}
	ImGui::SetItemTooltip("What an orbit turns around with nothing selected; with an object selected it is the object");
	ImGui::SetNextItemWidth(180.f);
	prefsChanged |= ImGui::SliderFloat("Look Sensitivity", &prefs.lookSensitivity, MIN_SENSITIVITY, MAX_SENSITIVITY, "%.2fx", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
	prefsChanged |= ImGui::MenuItem("Trackpad Gestures", nullptr, &prefs.trackpadGestures);
	ImGui::SetItemTooltip("Blender's: swipe to orbit, Shift + swipe to pan, pinch or Control + swipe to dolly. Off, a swipe moves along the view as a wheel does");
	if (prefsChanged) {
		ImGui::MarkIniSettingsDirty();
	}

	if (ImGui::BeginMenu("Controls")) {
		controlLine("RMB drag", "look around");
		controlLine("RMB + W A S D", "fly");
		controlLine("RMB + Q / E", "down / up");
		controlLine("RMB + wheel", "speed");
		controlLine("Option + LMB drag", "orbit");
		controlLine("Option + RMB drag", "dolly");
		controlLine("MMB drag", "pan");
		controlLine("Wheel", "move along the view");
		controlLine("F", "frame selected");
		if (prefs.trackpadGestures) {
			controlLine("Two-finger swipe", "orbit");
			controlLine("Shift + swipe", "pan");
			controlLine("Pinch, Control + swipe", "dolly");
		}
		ImGui::EndMenu();
	}
}
