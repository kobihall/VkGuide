#include <scene_capabilities.h>

#include <scene_io.h>
#include <vk_engine.h>
#include <vk_ui.h>
#include <workspace.h>

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>

namespace {

//---------------------------------------------------------------- Optical bench
// The laser optics simulator's editor (docs/plans/physoptics-laser-optics.md). Each physoptics plan adds its part of
// the capability here as it lands: its folders, Add entries, menus, windows, settings sections, recommended settings
// and dependent objects. docs/physical-optics-overview.md lists what is registered so far

void opticalBenchRecommendedSettings(const VulkanEngine* engine, std::vector<SettingChange>& out)
{
	//renders compared against references and against each other need the same noise every time
	if (!engine->m_raytracer.settings().useFixedSeed) {
		out.push_back(SettingChange {
			.label = "Fixed seed",
			.current = "off",
			.recommended = "on",
			.apply = [](VulkanEngine* e) {
				RenderSettings settings = e->m_raytracer.settings();
				settings.useFixedSeed = true;
				e->m_raytracer.setSettings(settings);
			},
		});
	}
}

void opticalBenchDependentObjects(const VulkanEngine*, std::vector<ObjectRef>&)
{
	//no optics object exists yet: physoptics-optical-elements.md adds the optics, then the lasers, beam axes and
	//detectors follow
}

void opticalBenchSceneStatus(VulkanEngine* engine)
{
	//optomechanical models whose converted files could not be read, and stand in as boxes (mount_library.h)
	const std::vector<std::string>& modelWarnings = engine->m_mountLibrary.warnings();
	if (modelWarnings.empty()) {
		return;
	}
	ImGui::Spacing();
	ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "%zu optical models are placeholder boxes", modelWarnings.size());
	if (ImGui::IsItemHovered()) {
		std::string tip;
		for (const std::string& line : modelWarnings) {
			tip += line + "\n";
		}
		ImGui::SetTooltip("%s", tip.c_str());
	}
}

// Optics: a wider Scene panel for the bench's object tree, the viewport in the middle and the render on the right.
// The Beam and Detectors windows dock below the viewport when their plans add them
void buildOpticsLayout(uint32_t dockspace, std::vector<std::string>& open)
{
	const DockColumns columns = splitDockColumns(dockspace, 0.26f, 0.22f, 0.62f, 0.3f);
	dockWindow("Scene", columns.leftTop);
	dockWindow("Materials", columns.leftBottom);
	dockWindow("Scene Capabilities", columns.leftBottom);
	dockWindow("Raytraced Output", columns.center);
	dockWindow("Viewport", columns.center);
	dockWindow("Raytrace Render", columns.rightTop);
	dockWindow("Render Settings", columns.rightBottom);
	for (const char* name : { "Raytracer Shaders", "Import Report", "Stats", "background", "GPU self-tests" }) {
		dockWindow(name, columns.rightBottom);
	}
	open = { "Scene", "Materials", "Viewport", "Raytrace Render", "Render Settings" };
}

const WorkspaceInfo OPTICS_WORKSPACE { "optics", "Optics", buildOpticsLayout };

// the bench seen from above and in front, as a camera at a table would see it: the 300 x 300 mm breadboard the plans
// put at the origin, with its surface at y = 0 and the beam 100 mm above it
void populateOpticalBench(VulkanEngine* engine)
{
	engine->enableCapability(CapabilityId::OpticalBench, false);
	std::vector<SettingChange> changes;
	opticalBenchRecommendedSettings(engine, changes);
	for (const SettingChange& change : changes) {
		change.apply(engine);
	}
	const glm::vec3 position { 0.f, 0.55f, 0.45f };
	const glm::vec3 target { 0.f, 0.1f, 0.f };
	SceneCamera camera;
	camera.name = "bench_camera";
	camera.position = position;
	camera.orientation = glm::quatLookAt(glm::normalize(target - position), glm::vec3(0.f, 1.f, 0.f));
	engine->m_raytraceScene.replaceCameras({ camera });
	engine->m_raytracer.setRenderCamera(engine->m_raytraceScene.cameras().front().id);
	engine->setViewportPose(position, target - position);
	//an empty environment: a bench is lit by its lasers, and stray light is what it is there to show
	engine->m_solidBackground = true;
	engine->m_backgroundColor = glm::vec3(0.f);
}

//---------------------------------------------------------------- the registry

const CapabilityInfo CAPABILITIES[] = {
	CapabilityInfo {
		.id = CapabilityId::OpticalBench,
		.key = "optical_bench",
		.name = "Optical bench",
		.description = "Optics on posts on a breadboard, lasers, beam axes and detectors: the laser optics simulator. "
			"Adds their folders, menus, windows and settings, and the Optics workspace. Changes nothing about how anything renders.",
		.recommendedSettings = opticalBenchRecommendedSettings,
		.dependentObjects = opticalBenchDependentObjects,
		.sceneFolders = nullptr,
		.addMenu = nullptr,
		.sceneStatus = opticalBenchSceneStatus,
		.menus = nullptr,
		.fileMenu = nullptr,
		.renderSettings = nullptr,
		.materialFolders = nullptr,
		.viewportOverlays = nullptr,
		.workspace = &OPTICS_WORKSPACE,
	},
};
static_assert(std::size(CAPABILITIES) == (size_t)CapabilityId::Count, "one CAPABILITIES row per CapabilityId, in order");

const SceneTemplate BUILTIN_TEMPLATES[] = {
	SceneTemplate { .key = DEFAULT_TEMPLATE_KEY, .name = "Render scene", .description = "One camera and the default settings: what New Scene makes", .path = {}, .populate = nullptr },
	SceneTemplate { .key = "optical_bench", .name = "Optical bench",
		.description = "The Optical bench capability on, a camera looking down at the table, a fixed seed and an empty environment", .path = {}, .populate = populateOpticalBench },
};

// a template's file name from its name: lowercase letters, digits and underscores
std::string templateSlug(const std::string& name)
{
	std::string slug;
	for (const char c : name) {
		if (std::isalnum((unsigned char)c)) {
			slug += (char)std::tolower((unsigned char)c);
		} else if (!slug.empty() && slug.back() != '_') {
			slug += '_';
		}
	}
	while (!slug.empty() && slug.back() == '_') {
		slug.pop_back();
	}
	return slug;
}

//---------------------------------------------------------------- UI state, which only these windows read

// the capability whose turning off was refused, and what needs it, while the refusal is open
std::optional<CapabilityId> s_refusing;
std::vector<ObjectRef> s_refusingObjects;
// the template list, read when the menu opens
std::vector<SceneTemplate> s_templates;
// Save as Template
bool s_openSavePopup = false;
char s_templateName[128] = "";
char s_templateDescription[256] = "";

} // namespace

std::span<const CapabilityInfo> capabilities()
{
	return CAPABILITIES;
}

const CapabilityInfo& capabilityInfo(CapabilityId id)
{
	return CAPABILITIES[(size_t)id];
}

std::optional<CapabilityId> findCapability(std::string_view key)
{
	for (const CapabilityInfo& capability : CAPABILITIES) {
		if (key == capability.key) {
			return capability.id;
		}
	}
	return std::nullopt;
}

void forEachEnabledCapability(const SceneCapabilities& scene, const std::function<void(const CapabilityInfo&)>& fn)
{
	for (const CapabilityInfo& capability : CAPABILITIES) {
		if (scene.has(capability.id)) {
			fn(capability);
		}
	}
}

std::vector<std::string> capabilityKeys(const SceneCapabilities& scene)
{
	std::vector<std::string> keys;
	forEachEnabledCapability(scene, [&](const CapabilityInfo& capability) { keys.emplace_back(capability.key); });
	keys.insert(keys.end(), scene.unknownKeys.begin(), scene.unknownKeys.end());
	return keys;
}

SceneCapabilities capabilitiesFromKeys(const std::vector<std::string>& keys, std::vector<std::string>& warnings)
{
	SceneCapabilities scene;
	for (const std::string& key : keys) {
		if (const std::optional<CapabilityId> id = findCapability(key)) {
			scene.enabled.set((size_t)*id);
		} else if (std::find(scene.unknownKeys.begin(), scene.unknownKeys.end(), key) == scene.unknownKeys.end()) {
			scene.unknownKeys.push_back(key);
			warnings.push_back(fmt::format("the scene uses capability '{}', which this build does not know; it is kept and saved back", key));
		}
	}
	return scene;
}

std::span<const SceneTemplate> builtinTemplates()
{
	return BUILTIN_TEMPLATES;
}

std::vector<SceneTemplate> listTemplates(const std::filesystem::path& directory)
{
	std::vector<SceneTemplate> list(std::begin(BUILTIN_TEMPLATES), std::end(BUILTIN_TEMPLATES));
	std::error_code ec;
	std::vector<SceneTemplate> files;
	for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".gltf") {
			continue;
		}
		SceneDescription scene;
		if (!loadSceneFile(entry.path(), scene).ok) {
			continue;
		}
		SceneTemplate entryTemplate;
		entryTemplate.key = entry.path().stem().string();
		entryTemplate.name = scene.templateInfo.has_value() && !scene.templateInfo->name.empty() ? scene.templateInfo->name : entryTemplate.key;
		entryTemplate.description = scene.templateInfo.has_value() ? scene.templateInfo->description : std::string();
		entryTemplate.path = entry.path();
		files.push_back(std::move(entryTemplate));
	}
	std::sort(files.begin(), files.end(), [](const SceneTemplate& a, const SceneTemplate& b) { return a.name < b.name; });
	list.insert(list.end(), files.begin(), files.end());
	return list;
}

std::string templateDisplayName(const std::string& key, const std::filesystem::path& directory)
{
	for (const SceneTemplate& builtin : BUILTIN_TEMPLATES) {
		if (key == builtin.key) {
			return builtin.name;
		}
	}
	for (const SceneTemplate& file : s_templates) {
		if (key == file.key) {
			return file.name;
		}
	}
	//not listed since the menu last opened: read its own file
	SceneDescription scene;
	if (loadSceneFile(directory / (key + ".gltf"), scene).ok && scene.templateInfo.has_value() && !scene.templateInfo->name.empty()) {
		return scene.templateInfo->name;
	}
	return key;
}

void drawCapabilitiesWindow(VulkanEngine* engine, bool* open)
{
	if (!*open) {
		return;
	}
	if (!ImGui::Begin("Scene Capabilities", open)) {
		ImGui::End();
		return;
	}
	ImGui::TextWrapped("What this scene uses beyond a plain render. Each adds its folders, menus, windows and settings, and none changes how anything renders. "
		"They combine, and are saved with the scene.");
	if (!engine->m_sceneTemplate.empty()) {
		ImGui::TextDisabled("Started from the %s template", templateDisplayName(engine->m_sceneTemplate, engine->templatesDirectory()).c_str());
	}
	ImGui::Spacing();

	for (const CapabilityInfo& capability : capabilities()) {
		ImGui::PushID(capability.key);
		bool on = engine->capabilityEnabled(capability.id);
		if (ImGui::Checkbox(capability.name, &on)) {
			if (on) {
				engine->enableCapability(capability.id, true);
			} else if (!engine->disableCapability(capability.id)) {
				s_refusing = capability.id;
				s_refusingObjects.clear();
				capability.dependentObjects(engine, s_refusingObjects);
				ImGui::OpenPopup("Turn off capability");
			}
		}
		ImGui::Indent();
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextDisabled("%s", capability.description);
		ImGui::PopTextWrapPos();
		ImGui::Unindent();

		if (ImGui::BeginPopupModal("Turn off capability", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			const CapabilityInfo& refused = capabilityInfo(*s_refusing);
			ImGui::Text("%s cannot be off while these need it:", refused.name);
			for (const ObjectRef& object : s_refusingObjects) {
				ImGui::BulletText("%s (%s)", object.name.c_str(), object.kind.c_str());
			}
			if (ImGui::Button("Delete them and turn off")) {
				for (const ObjectRef& object : s_refusingObjects) {
					object.erase(engine);
				}
				engine->disableCapability(refused.id);
				s_refusing.reset();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) {
				s_refusing.reset();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}

	if (!engine->m_capabilities.unknownKeys.empty()) {
		ImGui::SeparatorText("From a newer build");
		ImGui::TextDisabled("Kept, and saved back with the scene:");
		for (const std::string& key : engine->m_capabilities.unknownKeys) {
			ImGui::BulletText("%s", key.c_str());
		}
	}
	ImGui::End();
}

void drawRecommendationNotes(VulkanEngine* engine)
{
	std::vector<CapabilityId>& pending = engine->m_pendingRecommendations;
	for (size_t i = 0; i < pending.size();) {
		const CapabilityInfo& capability = capabilityInfo(pending[i]);
		std::vector<SettingChange> changes;
		if (capability.recommendedSettings != nullptr) {
			capability.recommendedSettings(engine, changes);
		}
		//nothing left to change: the scene already has every setting the capability would offer
		if (changes.empty() || !engine->capabilityEnabled(capability.id)) {
			pending.erase(pending.begin() + i);
			continue;
		}
		ImGui::PushID((int)capability.id);
		bool remove = false;
		if (ImGui::BeginChild("##note", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
			ImGui::Text("%s recommends:", capability.name);
			for (const SettingChange& change : changes) {
				ImGui::BulletText("%s: %s -> %s", change.label.c_str(), change.current.c_str(), change.recommended.c_str());
			}
			if (ImGui::Button("Apply")) {
				for (const SettingChange& change : changes) {
					change.apply(engine);
				}
				remove = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Dismiss")) {
				remove = true;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(offered once)");
		}
		ImGui::EndChild();
		ImGui::PopID();
		if (remove) {
			pending.erase(pending.begin() + i);
		} else {
			i++;
		}
	}
}

void drawTemplateMenu(VulkanEngine* engine)
{
	if (!ImGui::BeginMenu("New from Template")) {
		return;
	}
	if (ImGui::IsWindowAppearing()) {
		s_templates = listTemplates(engine->templatesDirectory());
	}
	bool userTemplates = false;
	for (const SceneTemplate& entry : s_templates) {
		if (!entry.path.empty() && !userTemplates) {
			ImGui::Separator();
			userTemplates = true;
		}
		if (ImGui::MenuItem(entry.name.c_str())) {
			engine->requestNewScene(entry);
		}
		if (!entry.description.empty()) {
			ImGui::SetItemTooltip("%s", entry.description.c_str());
		}
	}
	if (!userTemplates) {
		ImGui::Separator();
		ImGui::TextDisabled("Save as Template... adds your own");
	}
	ImGui::EndMenu();
}

void openSaveTemplatePopup()
{
	s_openSavePopup = true;
}

void drawSaveTemplatePopup(VulkanEngine* engine)
{
	if (s_openSavePopup) {
		s_openSavePopup = false;
		ImGui::OpenPopup("Save as Template");
	}
	if (!ImGui::BeginPopupModal("Save as Template", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		return;
	}
	ImGui::TextWrapped("Saves this scene to %s, listed under File -> New from Template. The scene itself stays as it is.", engine->templatesDirectory().string().c_str());
	ImGui::InputText("Name", s_templateName, sizeof(s_templateName));
	ImGui::InputText("Description", s_templateDescription, sizeof(s_templateDescription));

	const std::string key = templateSlug(s_templateName);
	const bool builtin = std::any_of(std::begin(BUILTIN_TEMPLATES), std::end(BUILTIN_TEMPLATES), [&](const SceneTemplate& t) { return key == t.key; });
	std::error_code ec;
	const bool exists = !key.empty() && std::filesystem::exists(engine->templatesDirectory() / (key + ".gltf"), ec);
	if (builtin) {
		ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "A built-in template has that name");
	} else if (exists) {
		ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "Replaces the template in %s.gltf", key.c_str());
	}

	ImGui::BeginDisabled(key.empty() || builtin);
	if (ImGui::Button("Save")) {
		engine->m_lastFileResult = engine->saveTemplate(key, s_templateName, s_templateDescription);
		s_templateName[0] = '\0';
		s_templateDescription[0] = '\0';
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}
