#pragma once

// The application's windows and workspaces.
//
// Every top-level window is an entry of one registry: its name (the ImGui window name, which layouts and workspaces
// refer to it by), the flag that opens it, the function that draws it, the capability that owns it, if any, and its
// group in the Windows menu. The Windows menu, the workspaces and the frame loop all read the registry, so a window is
// one addWindow() call. A capability's window is drawn and listed only while the scene has that capability on.
//
// The main viewport is one dock space. A workspace (scene_capabilities.h) is a named docking layout and a set of open
// windows: Render always, and one per capability that contributes one, while the scene has it on. The workspace tabs
// sit on the right of the menu bar. Switching saves the current layout and open windows into the workspace being left
// and restores the target's, or builds its default the first time; switching changes only windows, never the scene.
// The layouts are saved per user in imgui.ini, as [Workspace] entries beside ImGui's own; an imgui.ini without them
// (written before workspaces) is discarded once and replaced by the default layout.

#include <scene_capabilities.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

enum class WindowGroup {
	// the windows you work in, at the top of the Windows menu
	Main,
	// the engine's own instruments: Windows -> Debug
	Debug
};

struct AppWindow {
	std::string name;
	bool* open { nullptr };
	// draws the window, Begin() to End(), returning early itself when closed
	std::function<void()> draw;
	// shown, drawn and listed only while the scene has it on; unset for an application window
	std::optional<CapabilityId> capability;
	WindowGroup group { WindowGroup::Main };
	// drawn after every other window: the Viewport, whose gizmo must see this frame's clicks in the panels
	bool drawnLast { false };
};

// The splits the built-in layouts share, each a dock node id: a left column of two, the center, and a right column of
// two. Ratios are of the space left at each split
struct DockColumns {
	uint32_t leftTop { 0 };
	uint32_t leftBottom { 0 };
	uint32_t center { 0 };
	uint32_t rightTop { 0 };
	uint32_t rightBottom { 0 };
};
DockColumns splitDockColumns(uint32_t dockspace, float leftWidth, float rightWidth, float leftTopHeight, float rightTopHeight);
void dockWindow(const char* name, uint32_t node);
// splits `node`: returns the bottom `fraction` of it, and leaves `node` the top
uint32_t splitDockBelow(uint32_t& node, float fraction);

// the built-in Render workspace
const WorkspaceInfo& renderWorkspace();

class WorkspaceManager {
public:
	// registers the [Workspace] ini handler; after ImGui::CreateContext() and before the ini file is read
	void init();
	// after the ini file is read: discards an imgui.ini written before workspaces, so the default layout replaces
	// the floating positions it holds
	void afterIniLoaded();

	void addWindow(AppWindow window);

	// between frames (before ImGui::NewFrame()): a requested switch or reset, which loads a saved layout
	void applyPending(const SceneCapabilities& scene);
	// first in the frame, before any window: the dock space, and the active workspace's default layout when it has none
	void beginFrame();
	// every registered window the scene allows, in registration order
	void drawWindows(const SceneCapabilities& scene);
	// the Windows menu's contents, inside the caller's BeginMenu(); `images` draws the image windows' submenu
	void drawWindowsMenu(const SceneCapabilities& scene, const std::function<void()>& images);
	// the workspace tabs, right-aligned inside the main menu bar
	void drawTabs(const SceneCapabilities& scene);

	void requestSwitch(const std::string& key);
	void requestReset() { m_resetPending = true; }
	const std::string& active() const { return m_active; }
	// where an image window opens the first time: the center of the layout
	uint32_t centerDock() const { return m_centerDock; }

private:
	// what a workspace keeps while another is active: the layout ImGui wrote, and the windows that were open
	struct Saved {
		std::string layout;
		std::vector<std::string> open;
		bool hasOpen { false };
	};

	std::vector<const WorkspaceInfo*> available(const SceneCapabilities& scene) const;
	const WorkspaceInfo* find(const std::string& key) const;
	bool windowAllowed(const AppWindow& window, const SceneCapabilities& scene) const;
	std::vector<std::string> openWindows() const;
	void setOpenWindows(const std::vector<std::string>& names);

	static void* readOpen(struct ImGuiContext*, struct ImGuiSettingsHandler*, const char* name);
	static void readLine(struct ImGuiContext*, struct ImGuiSettingsHandler*, void* entry, const char* line);
	static void writeAll(struct ImGuiContext*, struct ImGuiSettingsHandler*, struct ImGuiTextBuffer* out);

	std::vector<AppWindow> m_windows;
	std::map<std::string, Saved> m_saved;
	std::string m_active { "render" };
	std::string m_pendingSwitch;
	bool m_resetPending { false };
	// the active workspace has no layout yet: beginFrame() builds its default
	bool m_buildDefault { false };
	// the open windows the ini file named for the active workspace, applied by the first applyPending(), once the
	// windows are registered
	bool m_applyOpenPending { false };
	// set while SaveIniSettingsToMemory() captures a layout, which must not hold the workspaces themselves
	bool m_capturing { false };
	bool m_stateRead { false };
	uint32_t m_centerDock { 0 };
	// the open windows last frame, so a change marks imgui.ini to be written
	std::vector<std::string> m_lastOpen;
	// the [Workspace][...] entry being read
	std::string m_reading;
};
