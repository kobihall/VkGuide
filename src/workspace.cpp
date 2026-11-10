#include <workspace.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>

namespace {

// the dock space every layout is built in; one id for the life of the app, so a saved layout's nodes match it
constexpr const char* DOCKSPACE_NAME = "MainDockSpace";
// written in [Workspace][State]; an ini file without it predates workspaces
constexpr int WORKSPACE_STATE_VERSION = 1;

// a layout is ini text, which a single ini line holds escaped: \n for a line break, \\ for a backslash
std::string escapeLines(std::string_view text)
{
	std::string out;
	out.reserve(text.size() + text.size() / 16);
	for (const char c : text) {
		if (c == '\\') {
			out += "\\\\";
		} else if (c == '\n') {
			out += "\\n";
		} else if (c != '\r') {
			out += c;
		}
	}
	return out;
}

std::string unescapeLines(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\\' && i + 1 < text.size()) {
			out += text[i + 1] == 'n' ? '\n' : text[i + 1];
			i++;
		} else {
			out += text[i];
		}
	}
	return out;
}

// window names hold spaces and colons, never a '|'
std::vector<std::string> splitNames(std::string_view text)
{
	std::vector<std::string> names;
	size_t start = 0;
	while (start <= text.size()) {
		const size_t end = std::min(text.find('|', start), text.size());
		if (end > start) {
			names.emplace_back(text.substr(start, end - start));
		}
		start = end + 1;
	}
	return names;
}

std::string joinNames(const std::vector<std::string>& names)
{
	std::string out;
	for (const std::string& name : names) {
		if (!out.empty()) {
			out += '|';
		}
		out += name;
	}
	return out;
}

ImGuiID dockspaceId()
{
	//hashed from the root of the id stack, so it is the same id in every frame and every run
	return ImHashStr(DOCKSPACE_NAME);
}

// Raytracing: the scene and the materials on the left, the viewport and the render's output in the middle, the stats and
// the render's settings on the right, and the instruments as closed tabs behind the settings, so they open there
void buildRenderLayout(uint32_t dockspace, std::vector<std::string>& open)
{
	const DockColumns columns = splitDockColumns(dockspace, 0.2f, 0.25f, 0.55f, 0.45f);
	dockWindow("Scene", columns.leftTop);
	dockWindow("Materials", columns.leftBottom);
	dockWindow("Raytraced Output", columns.center);
	dockWindow("Viewport", columns.center);
	dockWindow("Stats", columns.rightTop);
	dockWindow("Render Settings", columns.rightBottom);
	for (const char* name : { "Raytracer Shaders", "Scene Capabilities", "Import Report", "GPU self-tests" }) {
		dockWindow(name, columns.rightBottom);
	}
	open = { "Scene", "Materials", "Viewport", "Raytraced Output", "Stats", "Render Settings" };
}

// named Raytracing, so its tab is not mistaken for the Render button beside it; the key stays "render", which saved
// layouts are filed under
const WorkspaceInfo RENDER_WORKSPACE { "render", "Raytracing", buildRenderLayout };

} // namespace

DockColumns splitDockColumns(uint32_t dockspace, float leftWidth, float rightWidth, float leftTopHeight, float rightTopHeight)
{
	DockColumns columns;
	ImGuiID rest = dockspace;
	ImGuiID left = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, leftWidth, nullptr, &rest);
	ImGuiID right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, rightWidth / (1.f - leftWidth), nullptr, &rest);
	columns.center = rest;
	columns.leftTop = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, leftTopHeight, nullptr, &columns.leftBottom);
	columns.rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, rightTopHeight, nullptr, &columns.rightBottom);
	return columns;
}

uint32_t splitDockBelow(uint32_t& node, float fraction)
{
	ImGuiID top = node;
	const ImGuiID bottom = ImGui::DockBuilderSplitNode(node, ImGuiDir_Down, fraction, nullptr, &top);
	node = top;
	return bottom;
}

void dockWindow(const char* name, uint32_t node)
{
	ImGui::DockBuilderDockWindow(name, node);
}

const WorkspaceInfo& renderWorkspace()
{
	return RENDER_WORKSPACE;
}

void WorkspaceManager::init()
{
	ImGuiSettingsHandler handler;
	handler.TypeName = "Workspace";
	handler.TypeHash = ImHashStr("Workspace");
	handler.ReadOpenFn = readOpen;
	handler.ReadLineFn = readLine;
	handler.WriteAllFn = writeAll;
	handler.UserData = this;
	ImGui::AddSettingsHandler(&handler);
}

void WorkspaceManager::afterIniLoaded()
{
	if (m_stateRead) {
		m_applyOpenPending = m_saved.contains(m_active) && m_saved[m_active].hasOpen;
		//the active workspace's layout is ImGui's own, already applied; the copy kept for it is stale
		m_saved[m_active].layout.clear();
		return;
	}
	//absolute positions from before the dock space, sized for one screen: the default layout replaces them
	ImGui::ClearIniSettings();
	m_saved.clear();
	m_active = RENDER_WORKSPACE.key;
	m_buildDefault = true;
}

void WorkspaceManager::addWindow(AppWindow window)
{
	m_windows.push_back(std::move(window));
}

std::vector<const WorkspaceInfo*> WorkspaceManager::available(const SceneCapabilities& scene) const
{
	std::vector<const WorkspaceInfo*> list { &RENDER_WORKSPACE };
	forEachEnabledCapability(scene, [&](const CapabilityInfo& capability) {
		if (capability.workspace != nullptr) {
			list.push_back(capability.workspace);
		}
	});
	return list;
}

const WorkspaceInfo* WorkspaceManager::find(const std::string& key) const
{
	if (key == RENDER_WORKSPACE.key) {
		return &RENDER_WORKSPACE;
	}
	for (const CapabilityInfo& capability : capabilities()) {
		if (capability.workspace != nullptr && key == capability.workspace->key) {
			return capability.workspace;
		}
	}
	return nullptr;
}

bool WorkspaceManager::windowAllowed(const AppWindow& window, const SceneCapabilities& scene) const
{
	return !window.capability.has_value() || scene.has(*window.capability);
}

std::vector<std::string> WorkspaceManager::openWindows() const
{
	std::vector<std::string> names;
	for (const AppWindow& window : m_windows) {
		if (*window.open) {
			names.push_back(window.name);
		}
	}
	return names;
}

void WorkspaceManager::setOpenWindows(const std::vector<std::string>& names)
{
	for (const AppWindow& window : m_windows) {
		*window.open = std::find(names.begin(), names.end(), window.name) != names.end();
	}
}

void WorkspaceManager::requestSwitch(const std::string& key)
{
	if (key != m_active) {
		m_pendingSwitch = key;
	}
}

void WorkspaceManager::applyPending(const SceneCapabilities& scene)
{
	//the open windows the ini file named, before anything below captures them as the active workspace's
	if (m_applyOpenPending) {
		m_applyOpenPending = false;
		setOpenWindows(m_saved[m_active].open);
	}

	//a workspace whose capability the scene no longer has (a new scene, another file) gives way to Render
	if (m_pendingSwitch.empty()) {
		const std::vector<const WorkspaceInfo*> list = available(scene);
		if (std::none_of(list.begin(), list.end(), [&](const WorkspaceInfo* w) { return m_active == w->key; })) {
			m_pendingSwitch = RENDER_WORKSPACE.key;
		}
	}

	if (!m_pendingSwitch.empty()) {
		const WorkspaceInfo* target = find(m_pendingSwitch);
		m_pendingSwitch.clear();
		if (target != nullptr && target->key != m_active) {
			//the layout being left, without the [Workspace] entries themselves (writeAll() skips them while capturing)
			m_capturing = true;
			Saved& leaving = m_saved[m_active];
			leaving.layout = ImGui::SaveIniSettingsToMemory();
			m_capturing = false;
			leaving.open = openWindows();
			leaving.hasOpen = true;

			m_active = target->key;
			Saved& arriving = m_saved[m_active];
			if (!arriving.layout.empty()) {
				ImGui::LoadIniSettingsFromMemory(arriving.layout.c_str(), arriving.layout.size());
				//the active workspace's layout lives in ImGui from here on
				arriving.layout.clear();
				if (arriving.hasOpen) {
					setOpenWindows(arriving.open);
				}
			} else {
				m_buildDefault = true;
			}
			ImGui::MarkIniSettingsDirty();
		}
	}

	if (m_resetPending) {
		m_resetPending = false;
		m_saved.erase(m_active);
		m_buildDefault = true;
	}
}

void WorkspaceManager::beginFrame()
{
	const ImGuiID dockspace = dockspaceId();
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	if (m_buildDefault) {
		m_buildDefault = false;
		const WorkspaceInfo* workspace = find(m_active);
		if (workspace == nullptr) {
			workspace = &RENDER_WORKSPACE;
			m_active = RENDER_WORKSPACE.key;
		}
		ImGui::DockBuilderRemoveNode(dockspace);
		ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);
		std::vector<std::string> open;
		workspace->buildDefault(dockspace, open);
		ImGui::DockBuilderFinish(dockspace);
		setOpenWindows(open);
		ImGui::MarkIniSettingsDirty();
	}

	ImGui::DockSpaceOverViewport(dockspace, viewport);
	const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace);
	m_centerDock = central != nullptr ? central->ID : dockspace;

	//ImGui does not save which windows are open; the [Workspace] entry does, so a change must mark the file
	std::vector<std::string> open = openWindows();
	if (open != m_lastOpen) {
		m_lastOpen = std::move(open);
		ImGui::MarkIniSettingsDirty();
	}
}

void WorkspaceManager::drawWindows(const SceneCapabilities& scene)
{
	for (const bool last : { false, true }) {
		for (const AppWindow& window : m_windows) {
			if (window.drawnLast == last && windowAllowed(window, scene)) {
				//a window the layout has no place for (one added since the layout was saved) opens as a tab in the
				//center. Only for an open window, whose draw() calls Begin() next: a closed one would hand the dock
				//to whichever window begins after it
				if (*window.open) {
					ImGui::SetNextWindowDockID(m_centerDock, ImGuiCond_FirstUseEver);
				}
				window.draw();
			}
		}
	}
}

void WorkspaceManager::drawWindowsMenu(const SceneCapabilities& scene, const std::function<void()>& images)
{
	for (const AppWindow& window : m_windows) {
		if (window.group == WindowGroup::Main && windowAllowed(window, scene)) {
			ImGui::MenuItem(window.name.c_str(), nullptr, window.open);
		}
	}
	ImGui::Separator();
	if (ImGui::BeginMenu("Images")) {
		images();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Debug")) {
		for (const AppWindow& window : m_windows) {
			if (window.group == WindowGroup::Debug && windowAllowed(window, scene)) {
				ImGui::MenuItem(window.name.c_str(), nullptr, window.open);
			}
		}
		ImGui::EndMenu();
	}
	ImGui::Separator();
	if (ImGui::BeginMenu("Workspace")) {
		for (const WorkspaceInfo* workspace : available(scene)) {
			if (ImGui::MenuItem(workspace->name, nullptr, m_active == workspace->key)) {
				requestSwitch(workspace->key);
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Reset Layout")) {
			requestReset();
		}
		ImGui::SetItemTooltip("Back to this workspace's built-in layout and windows");
		ImGui::EndMenu();
	}
}

void WorkspaceManager::drawTabs(const SceneCapabilities& scene)
{
	const std::vector<const WorkspaceInfo*> list = available(scene);
	const ImGuiStyle& style = ImGui::GetStyle();
	float width = 0.f;
	for (const WorkspaceInfo* workspace : list) {
		width += ImGui::CalcTextSize(workspace->name).x + style.FramePadding.x * 2.f + style.ItemSpacing.x;
	}
	const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width;
	if (x > ImGui::GetCursorPosX()) {
		ImGui::SetCursorPosX(x);
	}
	//a tab is named after its workspace, as a capability's menu in the same bar may be (Optics): its own id scope keeps
	//the two apart
	ImGui::PushID("WorkspaceTabs");
	for (const WorkspaceInfo* workspace : list) {
		const bool active = m_active == workspace->key;
		const ImVec2 size { ImGui::CalcTextSize(workspace->name).x + style.FramePadding.x * 2.f, 0.f };
		if (active) {
			ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
		}
		if (ImGui::Selectable(workspace->name, active, ImGuiSelectableFlags_None, size) && !active) {
			requestSwitch(workspace->key);
		}
		if (active) {
			ImGui::PopStyleColor();
		}
		if (ImGui::BeginPopupContextItem(workspace->key)) {
			ImGui::BeginDisabled(!active);
			if (ImGui::MenuItem("Reset Layout")) {
				requestReset();
			}
			ImGui::EndDisabled();
			ImGui::EndPopup();
		}
		ImGui::SetItemTooltip("Workspace: a layout of windows, kept per user. Right-click to reset it");
	}
	ImGui::PopID();
}

void* WorkspaceManager::readOpen(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name)
{
	WorkspaceManager* self = (WorkspaceManager*)handler->UserData;
	self->m_reading = name;
	return self;
}

void WorkspaceManager::readLine(ImGuiContext*, ImGuiSettingsHandler* handler, void*, const char* line)
{
	WorkspaceManager* self = (WorkspaceManager*)handler->UserData;
	const std::string_view text(line);
	const size_t equals = text.find('=');
	if (equals == std::string_view::npos) {
		return;
	}
	const std::string_view key = text.substr(0, equals);
	const std::string_view value = text.substr(equals + 1);
	if (self->m_reading == "State") {
		if (key == "Version" && value == std::to_string(WORKSPACE_STATE_VERSION)) {
			self->m_stateRead = true;
		} else if (key == "Active") {
			self->m_active = std::string(value);
		}
		return;
	}
	Saved& saved = self->m_saved[self->m_reading];
	if (key == "Open") {
		saved.open = splitNames(value);
		saved.hasOpen = true;
	} else if (key == "Layout") {
		saved.layout = unescapeLines(value);
	}
}

void WorkspaceManager::writeAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* out)
{
	WorkspaceManager* self = (WorkspaceManager*)handler->UserData;
	if (self->m_capturing) {
		return;
	}
	out->appendf("[Workspace][State]\nVersion=%d\nActive=%s\n\n", WORKSPACE_STATE_VERSION, self->m_active.c_str());
	//the active workspace's layout is ImGui's own entries; only its open windows are written here
	out->appendf("[Workspace][%s]\nOpen=%s\n\n", self->m_active.c_str(), joinNames(self->openWindows()).c_str());
	for (const auto& [key, saved] : self->m_saved) {
		if (key == self->m_active || key == "State") {
			continue;
		}
		out->appendf("[Workspace][%s]\n", key.c_str());
		if (saved.hasOpen) {
			out->appendf("Open=%s\n", joinNames(saved.open).c_str());
		}
		if (!saved.layout.empty()) {
			out->appendf("Layout=%s\n", escapeLines(saved.layout).c_str());
		}
		out->append("\n");
	}
}
