#pragma once

// Trackpad gestures, which GLFW does not report: it turns a two-finger swipe into scroll offsets without saying whether a
// trackpad or a mouse wheel made them, and drops the pinch. On macOS an NSEvent monitor (trackpad.mm) sees each event
// before GLFW does and keeps what it finds here; elsewhere the stub (trackpad_stub.cpp) finds nothing. The monitor only
// watches, so GLFW, and ImGui after it, still get every event.

#include <glm/glm.hpp>

// what the gestures moved since the last takeTrackpadGestures(). A swipe is in points, as the content moves under the
// system's scroll direction (screen coordinates, y down), under the modifier held when it arrived. The momentum that
// macOS adds after the fingers lift is left out
struct TrackpadGestures {
	glm::vec2 swipe { 0.f };
	glm::vec2 shiftSwipe { 0.f };
	glm::vec2 controlSwipe { 0.f };
	// the pinch's magnification, positive as the fingers spread
	float pinch { 0.f };

	bool empty() const { return swipe == glm::vec2(0.f) && shiftSwipe == glm::vec2(0.f) && controlSwipe == glm::vec2(0.f) && pinch == 0.f; }
};

// once, after the window exists
void installTrackpadMonitor();
TrackpadGestures takeTrackpadGestures();
// whether the scroll event GLFW is delivering now came from a device with precise deltas (a trackpad or a Magic Mouse),
// momentum included, so a scroll callback can leave it to the gestures
bool scrollIsFromTrackpad();
