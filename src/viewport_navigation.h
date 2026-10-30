#pragma once

// The raster viewport's navigation: Unreal's mouse and keyboard controls, Blender's trackpad gestures on macOS, framing
// a box, and the Camera menu's settings. It owns the free camera, the settings saved with the scene and the
// preferences saved in imgui.ini. VulkanEngine forwards GLFW's input to it, captures the cursor for a drag, and calls
// update() once a frame before anything draws.
//
// The controls, with RMB the look button as in Unreal:
//   RMB drag              look around; W A S D fly along the view, Q and E down and up along world y; the wheel
//                         changes the speed
//   Option + LMB drag     orbit around the pivot
//   Option + RMB drag     dolly toward or away from the pivot
//   MMB drag              pan, keeping what is at the pivot's depth under the cursor (Option + MMB too)
//   wheel                 move along the view by a fifth of a second's flight
//   F                     frame the selection, or the whole scene with nothing selected
//   two-finger swipe      orbit; with Shift, pan; with Control, or a pinch, dolly (the trackpad gestures, Blender's)
// The pivot is the selected object's center while something is selected, and otherwise the camera's own pivot, which
// ViewportOrbitPivot says how to keep.

#include <bvh.h>
#include <camera.h>
#include <rt_scene_types.h>
#include <trackpad.h>

#include <cstdint>
#include <functional>
#include <optional>

// how the camera's own pivot moves, which an orbit turns around and a dolly moves toward with nothing selected
enum class ViewportOrbitPivot : uint8_t {
	// a point on the view axis that looking, flying and panning carry along, at the depth the last frame, orbit or dolly
	// left it, so an orbit circles what is in the middle of the screen (Blender's view center). The wheel leaves it
	// where it is, so moving in on a framed object keeps the object the pivot, and pushes it ahead once the camera
	// reaches it
	ViewCenter,
	// a point in the world that only framing moves (Unreal's look-at): flying away leaves it behind
	LookAt
};

// app-wide, kept in imgui.ini as [ViewportCamera][Preferences]
struct ViewportCameraPrefs {
	// times the base rate of radians per point, for looking and orbiting
	float lookSensitivity { 1.f };
	bool trackpadGestures { true };
	ViewportOrbitPivot orbitPivot { ViewportOrbitPivot::ViewCenter };
};

// a mouse drag over the preview, which holds the cursor captured until its button is released
enum class ViewportDrag : uint8_t {
	None,
	Look,
	Orbit,
	Dolly,
	Pan
};

// one frame's input, and what it acts on
struct ViewportFrameInput {
	// seconds since the last update
	float dt { 0.f };
	// the cursor is over the preview with no window in front
	bool hovered { false };
	// the keys held while looking: right, world up and forward, each -1, 0 or 1
	glm::vec3 move { 0.f };
	TrackpadGestures gestures;
	// the preview's vertical fov in radians and height in points, which a pan needs to move the pivot with the cursor
	float vfovRadians { 1.f };
	float viewportHeight { 1.f };
	// the selection's center, nullopt with nothing selected; asked for only when an orbit, a dolly or a pan needs it
	std::function<std::optional<glm::vec3>()> selectionCenter;
};

class ViewportNavigation {
public:
	Camera camera;
	ViewportCameraSettings settings;
	ViewportCameraPrefs prefs;

	// before imgui.ini is read
	void registerSettingsHandler();

	// whether looking, flying and panning carry the pivot (ViewportOrbitPivot::ViewCenter)
	bool carriesPivot() const { return prefs.orbitPivot == ViewportOrbitPivot::ViewCenter; }

	// the drag a press of this GLFW button with these modifiers starts over the preview, None for none
	static ViewportDrag dragFor(int button, int mods);
	void beginDrag(ViewportDrag drag, int button);
	void endDrag();
	ViewportDrag drag() const { return m_drag; }
	int dragButton() const { return m_dragButton; }
	// GLFW's input, forwarded: the cursor's motion while a drag holds it, in points, and a scroll over the preview, a
	// wheel's or, with the gestures off, a trackpad's
	void mouseMoved(double dx, double dy);
	void scrolled(double yoffset);

	void update(const ViewportFrameInput& input);

	// eases the camera over FRAME_SECONDS to where `box` fills a view of this fov and aspect, looking the way it looks
	// now, and makes the box's center the pivot
	void frame(const Aabb& box, float vfovRadians, float aspect);
	// the camera at `at` looking at `target`, which becomes the pivot
	void lookAt(const glm::vec3& at, const glm::vec3& target);

	// the Camera menu's settings and preferences; the fov is greyed out while `fovLocked`
	void drawMenuSettings(bool fovLocked);

	static constexpr float FRAME_SECONDS = 0.25f;

private:
	// the pivot an orbit, a dolly or a pan uses: the selection's center, or the camera's own
	glm::vec3 center(const ViewportFrameInput& input);
	void pan(const glm::vec2& points, const glm::vec3& around, const ViewportFrameInput& input);
	float minDepth() const;

	ViewportDrag m_drag { ViewportDrag::None };
	int m_dragButton { -1 };
	// the drag's center, read once when it first needs one: the selection cannot change while the cursor is held
	std::optional<glm::vec3> m_dragCenter;
	glm::vec2 m_mouseDelta { 0.f };
	double m_wheel { 0.0 };

	// a frame() under way
	struct Transition {
		glm::vec3 from;
		glm::vec3 to;
		float t;
	};
	std::optional<Transition> m_transition;
};
