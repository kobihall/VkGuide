#pragma once

// Scene capabilities (docs/plans/completed/physoptics-scene-capabilities.md): what a scene uses beyond a plain render,
// recorded in its file, each bringing its own folders, menus, windows, settings sections and recommended settings.
// Capabilities combine, and gate UI and defaults only: the renderer, the scene file and every solver handle every
// object that exists whether or not its capability is on. A capability cannot be turned off while objects that need it
// exist, so a scene's data and its capabilities never disagree.
//
// CAPABILITIES (scene_capabilities.cpp) is the only list of them, like the kernel registry. UI elsewhere asks
// capabilityEnabled() or walks the enabled entries' hooks, and never names a capability, except the plans that own its
// parts. The templates New Scene offers live here too: the built-in ones are code, so they always match the current
// defaults, and Save as Template writes the user's own to assets/templates/.

#include <bitset>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

class VulkanEngine;
struct ViewportView;

// append only: the key below is what a scene file saves, the number only orders the UI
enum class CapabilityId : uint32_t {
	OpticalBench,
	Count
};

// one setting a capability would change, as the recommendation note lists it; only settings that differ from the scene's
struct SettingChange {
	std::string label;
	std::string current;
	std::string recommended;
	std::function<void(VulkanEngine*)> apply;
};

// one object that needs a capability, as the refusal to turn it off lists it
struct ObjectRef {
	std::string kind;
	std::string name;
	std::function<void(VulkanEngine*)> erase;
};

// A named docking layout and set of open windows (workspace.h). `buildDefault` splits the dock space with ImGui's
// DockBuilder, docks windows by name and lists the windows the layout opens; the user's rearrangement replaces it until
// Reset Layout
struct WorkspaceInfo {
	// saved in imgui.ini; never rename
	const char* key;
	const char* name;
	void (*buildDefault)(uint32_t dockspace, std::vector<std::string>& open);
};

struct CapabilityInfo {
	CapabilityId id;
	// saved in the scene file; never rename
	const char* key;
	const char* name;
	const char* description;

	// what turning it on offers once, as the note lists it and Apply sets. Lists only what differs from the scene
	void (*recommendedSettings)(const VulkanEngine*, std::vector<SettingChange>&);
	// the objects that need it: turning it off is refused while there are any, and a scene file without the
	// capability that holds some turns it on as it loads
	void (*dependentObjects)(const VulkanEngine*, std::vector<ObjectRef>&);

	// The UI it adds, each called only while the scene has it on, in registry order. Null for none. A capability's
	// windows are entries of the window registry with this capability as their owner (WorkspaceManager::addWindow())
	// the Scene panel: folders after Models in the object tree, entries in + Add, and lines under the status line
	void (*sceneFolders)(VulkanEngine*);
	void (*addMenu)(VulkanEngine*);
	void (*sceneStatus)(VulkanEngine*);
	// the menu bar: whole menus after Windows, and items in File before Export Render
	void (*menus)(VulkanEngine*);
	void (*fileMenu)(VulkanEngine*);
	// sections at the end of the Render Settings window, and folders after the models' in the Materials window, with the
	// details of an entry such a folder selected, drawn while no material is (returns whether it drew anything)
	void (*renderSettings)(VulkanEngine*);
	void (*materialFolders)(VulkanEngine*);
	bool (*materialDetails)(VulkanEngine*);
	// drawn over the Viewport after the camera and light markers
	void (*viewportOverlays)(VulkanEngine*, const ViewportView&);
	// a workspace offered beside Render while the capability is on; null for none
	const WorkspaceInfo* workspace;
};

// the capabilities a scene has on, and the keys of any its file named that this build does not know, which are kept
// and saved back so a scene from a newer build survives a round trip through this one
struct SceneCapabilities {
	std::bitset<(size_t)CapabilityId::Count> enabled;
	std::vector<std::string> unknownKeys;

	bool has(CapabilityId id) const { return enabled.test((size_t)id); }
	bool operator==(const SceneCapabilities&) const = default;
};

std::span<const CapabilityInfo> capabilities();
const CapabilityInfo& capabilityInfo(CapabilityId id);
std::optional<CapabilityId> findCapability(std::string_view key);

// calls `fn` with each capability the scene has on, in registry order: how every hook site walks them
void forEachEnabledCapability(const SceneCapabilities& scene, const std::function<void(const CapabilityInfo&)>& fn);

// the keys a scene file saves, known ones in registry order then the unknown ones; and back, with a warning per
// unknown key
std::vector<std::string> capabilityKeys(const SceneCapabilities& scene);
SceneCapabilities capabilitiesFromKeys(const std::vector<std::string>& keys, std::vector<std::string>& warnings);

// A template New Scene starts from. A built-in one is code (`populate` runs on the scene newScene() leaves); a user's is
// a scene file in assets/templates/, opened untitled. The key is what a scene records as the template it started from:
// a built-in's own, or a file's stem
struct SceneTemplate {
	std::string key;
	std::string name;
	std::string description;
	// empty for a built-in
	std::filesystem::path path;
	void (*populate)(VulkanEngine*) { nullptr };
};

// the built-in templates, Render scene first: it is what File -> New Scene makes
std::span<const SceneTemplate> builtinTemplates();
inline constexpr const char* DEFAULT_TEMPLATE_KEY = "render_scene";
// the built-ins, then each template file in `directory` (sorted by name). A file that does not read is left out
std::vector<SceneTemplate> listTemplates(const std::filesystem::path& directory);
// the name a scene's template key shows as: a built-in's, a file's, or the key itself
std::string templateDisplayName(const std::string& key, const std::filesystem::path& directory);

// The capability UI the engine draws, each between ImGui::NewFrame() and ImGui::Render():
// Edit -> Scene Capabilities..., with the refusal to turn one off while objects need it
void drawCapabilitiesWindow(VulkanEngine* engine, bool* open);
// "Optical bench recommends: ..." with Apply and Dismiss, at the top of the Scene panel
void drawRecommendationNotes(VulkanEngine* engine);
// File -> New from Template, and the Save as Template popup File -> Save as Template... opens
void drawTemplateMenu(VulkanEngine* engine);
void openSaveTemplatePopup();
void drawSaveTemplatePopup(VulkanEngine* engine);
