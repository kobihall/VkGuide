#include <rt_scene_editor.h>

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include <rt_scene.h>
#include <vk_engine.h>
#include <vk_gizmo.h>
#include <vk_ui.h>

namespace {

//the gizmo identifies its target by an opaque pointer it never dereferences; an object's stable
//id fits in one
const void* gizmoTargetId(uint64_t id)
{
	return (const void*)(uintptr_t)id;
}

//a rotation matrix from the gizmo may carry scale (SCALEU on spheres) or drift slightly; the
//normalised columns are the orientation
glm::quat orientationOf(const glm::mat4& m)
{
	const glm::mat3 basis(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
	return glm::normalize(glm::quat_cast(basis));
}

}

bool drawSphereParams(SceneSphere& sphere)
{
	//the sibling project used SliderScalar with a fixed 0.001-2 radius / -1-1 position range,
	//which cannot represent its own default scene: the ground sphere is radius 100 at y=-100.5,
	//and a slider clamps a value into range the moment it is touched, so selecting the ground
	//sphere and nudging the slider silently collapsed it. Drag for position (unbounded) and a
	//logarithmic slider for radius keep the same controls without that trap
	bool changed = ImGui::SliderFloat("radius", &sphere.radius, 0.001f, 200.f, "%.3f", ImGuiSliderFlags_Logarithmic);
	changed |= ImGui::DragFloat3("position", &sphere.center.x, 0.01f, 0.f, 0.f, "%.3f");
	return changed;
}

bool drawMaterialParams(SphereMaterial& material)
{
	bool changed = false;

	if (ImGui::BeginCombo("material", materialTypeName(material.type))) {
		for (MaterialType type : MATERIAL_TYPES) {
			if (ImGui::Selectable(materialTypeName(type), type == material.type) && type != material.type) {
				//every type's parameters are stored, so nothing is lost switching away and back
				material.type = type;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}

	switch (material.type) {
	case MaterialType::Lambertian:
		changed |= ImGui::ColorEdit3("albedo", &material.albedo.x);
		break;
	case MaterialType::Metal:
		changed |= ImGui::ColorEdit3("albedo", &material.albedo.x);
		//the scatter clamps fuzz to at most 1, so the slider stops there too
		changed |= ImGui::SliderFloat("fuzz", &material.fuzz, 0.f, 1.f, "%.3f");
		break;
	case MaterialType::Phong:
		changed |= ImGui::ColorEdit3("albedo", &material.albedo.x);
		//smoothness 0-1 maps to a lobe exponent of 1-1000 in the scatter
		changed |= ImGui::SliderFloat("smoothness", &material.smoothness, 0.f, 1.f, "%.3f");
		break;
	case MaterialType::Dielectric:
		//1.0 is vacuum; 1.33 water, 1.5 glass, 2.42 diamond
		changed |= ImGui::SliderFloat("index of refraction", &material.ir, 1.f, 3.f, "%.3f");
		break;
	}

	return changed;
}

bool drawCameraParams(SceneCamera& camera, bool& displayChanged)
{
	bool changed = ImGui::DragFloat3("position", &camera.position.x, 0.01f, 0.f, 0.f, "%.3f");

	//euler angles for the widget only; the orientation itself stays a quaternion. Rebuilding
	//the quaternion from the edited angles is exact for the angles the widget shows, so an
	//untouched orientation is never disturbed
	glm::vec3 eulerDegrees = glm::degrees(glm::eulerAngles(camera.orientation));
	if (ImGui::DragFloat3("rotation (pitch, yaw, roll)", &eulerDegrees.x, 0.5f, 0.f, 0.f, "%.1f")) {
		camera.orientation = glm::normalize(glm::quat(glm::radians(eulerDegrees)));
		changed = true;
	}

	changed |= ImGui::SliderFloat("vertical fov", &camera.vfovDegrees, 10.f, 120.f, "%.1f");
	changed |= ImGui::SliderFloat("aperture", &camera.aperture, 0.f, 1.f, "%.3f");
	changed |= ImGui::SliderFloat("focus distance", &camera.focusDistance, 0.1f, 50.f, "%.2f", ImGuiSliderFlags_Logarithmic);

	//display only: applied by the tonemap every frame, so a running render need not restart
	displayChanged = ImGui::SliderFloat("exposure", &camera.exposure, 0.f, 4.f);

	return changed;
}

RaytraceSceneEditor::RaytraceSceneEditor()
{
	//the sibling project's hardcoded starting scene (Renderer::Renderer()), verbatim
	SceneSphere ground { 0, "ground", glm::vec3(0.f, -100.5f, -1.f), 100.f, makeSphereMaterial(MaterialType::Lambertian, glm::vec3(1.f, 1.f, 1.f)) };
	SceneSphere center { 0, "center_sphere", glm::vec3(0.f, 0.f, -1.f), 0.5f, makeSphereMaterial(MaterialType::Lambertian, glm::vec3(0.7f, 0.3f, 0.3f)) };
	SceneSphere left { 0, "left_sphere", glm::vec3(-1.f, 0.f, -1.f), 0.5f, makeSphereMaterial(MaterialType::Dielectric) };
	left.material.ir = 1.5f;
	SceneSphere right { 0, "right_sphere", glm::vec3(1.f, 0.f, -1.f), 0.5f, makeSphereMaterial(MaterialType::Phong, glm::vec3(0.8f, 0.6f, 0.2f)) };
	right.material.smoothness = 0.3f;

	addSphere(std::move(ground));
	addSphere(std::move(center));
	addSphere(std::move(left));
	addSphere(std::move(right));

	//one camera where the free camera starts, looking at the spheres
	SceneCamera camera;
	camera.name = "render_camera";
	addCamera(std::move(camera));
}

void RaytraceSceneEditor::addSphere(SceneSphere sphere)
{
	sphere.id = m_nextId++;
	m_spheres.push_back(std::move(sphere));
}

uint64_t RaytraceSceneEditor::addCamera(SceneCamera camera)
{
	camera.id = m_nextId++;
	if (camera.name.empty()) {
		camera.name = fmt::format("camera_{}", m_nextCameraNumber);
	}
	m_nextCameraNumber++;
	const uint64_t id = camera.id;
	m_cameras.push_back(std::move(camera));
	markChanged();
	return id;
}

SceneSphere* RaytraceSceneEditor::findSphere(uint64_t id)
{
	auto found = std::find_if(m_spheres.begin(), m_spheres.end(), [id](const SceneSphere& s) { return s.id == id; });
	return found == m_spheres.end() ? nullptr : &(*found);
}

const SceneSphere* RaytraceSceneEditor::findSphere(uint64_t id) const
{
	auto found = std::find_if(m_spheres.begin(), m_spheres.end(), [id](const SceneSphere& s) { return s.id == id; });
	return found == m_spheres.end() ? nullptr : &(*found);
}

SceneCamera* RaytraceSceneEditor::findCamera(uint64_t id)
{
	auto found = std::find_if(m_cameras.begin(), m_cameras.end(), [id](const SceneCamera& c) { return c.id == id; });
	return found == m_cameras.end() ? nullptr : &(*found);
}

const SceneCamera* RaytraceSceneEditor::findCamera(uint64_t id) const
{
	auto found = std::find_if(m_cameras.begin(), m_cameras.end(), [id](const SceneCamera& c) { return c.id == id; });
	return found == m_cameras.end() ? nullptr : &(*found);
}

void RaytraceSceneEditor::replaceSpheres(std::vector<SceneSphere>&& spheres)
{
	//the old spheres' ids die here. If the gizmo was editing one of them its lookup fails and it
	//stops on its own next frame
	m_spheres.clear();
	for (SceneSphere& sphere : spheres) {
		addSphere(std::move(sphere));
	}
	m_selectionKind = SelectionKind::None;
	m_nextSphereNumber = (int)m_spheres.size() + 1;
	markChanged();
}

void RaytraceSceneEditor::replaceCameras(std::vector<SceneCamera>&& cameras)
{
	m_cameras.clear();
	m_nextCameraNumber = 1;
	for (SceneCamera& camera : cameras) {
		addCamera(std::move(camera));
	}
	m_selectionKind = SelectionKind::None;
	markChanged();
}

void RaytraceSceneEditor::setCameraPose(uint64_t id, const glm::vec3& position, const glm::quat& orientation)
{
	SceneCamera* camera = findCamera(id);
	if (camera == nullptr) {
		return;
	}
	if (camera->position == position && camera->orientation == orientation) {
		return;
	}
	camera->position = position;
	camera->orientation = orientation;
	markChanged();
}

void RaytraceSceneEditor::drawPanel(VulkanEngine* engine)
{
	if (!m_showPanel) {
		return;
	}

	if (!ImGui::Begin("Scene", &m_showPanel)) {
		ImGui::End();
		return;
	}

	//which file this scene is, if any. Opening and saving live in the File menu
	if (engine->m_scenePath.empty()) {
		ImGui::TextDisabled("Unsaved scene");
	} else {
		ImGui::Text("%s", engine->m_scenePath.filename().string().c_str());
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", engine->m_scenePath.string().c_str());
		}
	}

	drawModels(engine);

	ImGui::SeparatorText("Objects");

	//read fresh every frame, so a scene loaded or unloaded at runtime shows up straight away
	std::vector<std::string> gltfNames;
	forEachMeshNode(engine, [&gltfNames](const MeshNode& node) {
		gltfNames.push_back(node.name.empty() ? "(unnamed node)" : node.name);
	});

	if (ImGui::Button("Add Sphere")) {
		SceneSphere sphere;
		sphere.name = fmt::format("sphere_{}", m_nextSphereNumber++);
		addSphere(std::move(sphere));
		m_selectionKind = SelectionKind::Sphere;
		m_selectionIndex = (int)m_spheres.size() - 1;
		markChanged();
	}

	ImGui::SameLine();
	if (ImGui::Button("Add Camera")) {
		//where the viewport is looking right now, so the new camera frames what the user sees
		SceneCamera camera = engine->cameraFromViewport();
		addCamera(std::move(camera));
		m_selectionKind = SelectionKind::Camera;
		m_selectionIndex = (int)m_cameras.size() - 1;
	}

	const bool canDeleteSphere = m_selectionKind == SelectionKind::Sphere && m_selectionIndex < (int)m_spheres.size();
	const bool canDeleteCamera = m_selectionKind == SelectionKind::Camera && m_selectionIndex < (int)m_cameras.size();
	ImGui::SameLine();
	ImGui::BeginDisabled(!(canDeleteSphere || canDeleteCamera));
	if (ImGui::Button("Delete")) {
		//a gizmo or first-person edit on this object fails its id lookup and ends itself
		if (canDeleteSphere) {
			m_spheres.erase(m_spheres.begin() + m_selectionIndex);
		} else if (canDeleteCamera) {
			m_cameras.erase(m_cameras.begin() + m_selectionIndex);
		}
		m_selectionKind = SelectionKind::None;
		markChanged();
	}
	ImGui::EndDisabled();

	if (ImGui::BeginListBox("Objects", ImVec2(-FLT_MIN, 8 * ImGui::GetTextLineHeightWithSpacing()))) {
		int row = 0;
		for (int i = 0; i < (int)m_cameras.size(); i++) {
			const bool selected = m_selectionKind == SelectionKind::Camera && m_selectionIndex == i;
			ImGui::PushID(row++);
			const std::string label = fmt::format("{} (camera)", m_cameras[i].name);
			if (ImGui::Selectable(label.c_str(), selected)) {
				m_selectionKind = SelectionKind::Camera;
				m_selectionIndex = i;
			}
			ImGui::PopID();
		}

		for (int i = 0; i < (int)m_spheres.size(); i++) {
			const bool selected = m_selectionKind == SelectionKind::Sphere && m_selectionIndex == i;
			ImGui::PushID(row++);
			if (ImGui::Selectable(m_spheres[i].name.c_str(), selected)) {
				m_selectionKind = SelectionKind::Sphere;
				m_selectionIndex = i;
			}
			ImGui::PopID();
		}

		//glTF meshes are listed so the browser already knows they exist, but they are not
		//traceable - selecting one deliberately shows nothing further. This is the placeholder
		//for a future mesh-tracing feature, not for transform editing
		for (int i = 0; i < (int)gltfNames.size(); i++) {
			const bool selected = m_selectionKind == SelectionKind::GltfNode && m_selectionIndex == i;
			ImGui::PushID(row++);
			if (ImGui::Selectable(gltfNames[i].c_str(), selected)) {
				m_selectionKind = SelectionKind::GltfNode;
				m_selectionIndex = i;
			}
			ImGui::PopID();
		}
		ImGui::EndListBox();
	}

	if (m_selectionKind == SelectionKind::Sphere && m_selectionIndex < (int)m_spheres.size()) {
		drawSelectedSphere(engine, m_selectionIndex);
	} else if (m_selectionKind == SelectionKind::Camera && m_selectionIndex < (int)m_cameras.size()) {
		drawSelectedCamera(engine, m_selectionIndex);
	}

	//the outcome of the last File-menu action, kept until the next one replaces it
	ImGui::Spacing();
	drawIoResultLine(engine->m_lastFileResult);

	ImGui::End();
}

void RaytraceSceneEditor::drawModels(VulkanEngine* engine)
{
	ImGui::SeparatorText("Models");

	if (engine->m_models.empty()) {
		ImGui::TextDisabled("none - File > Import glTF Model...");
		return;
	}

	//removal happens after the loop: erasing while iterating the map is not an option
	std::string toRemove;
	for (const auto& [name, model] : engine->m_models) {
		ImGui::PushID(name.c_str());
		if (ImGui::SmallButton("Remove")) {
			toRemove = name;
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(name.c_str());
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", model->sourcePath.string().c_str());
		}
		ImGui::PopID();
	}

	if (!toRemove.empty()) {
		engine->removeGltf(toRemove);
		//the glTF rows below are about to change length, so a glTF selection is meaningless now
		if (m_selectionKind == SelectionKind::GltfNode) {
			m_selectionKind = SelectionKind::None;
		}
	}
}

void RaytraceSceneEditor::drawSelectedSphere(VulkanEngine* engine, int index)
{
	SceneSphere& sphere = m_spheres[index];

	ImGui::SeparatorText(sphere.name.c_str());

	//scoping the widget ids to the selected object stops an in-progress drag, or any other
	//per-widget state imgui keys by id, from carrying across a selection change
	ImGui::PushID((int)sphere.id);

	bool changed = drawSphereParams(sphere);

	//the gizmo sits alongside the sliders, not instead of them: sliders for exact numbers, the
	//gizmo for direct manipulation. Opt-in per object, and only one object at a time
	TransformGizmo& gizmo = engine->m_transformGizmo;
	const bool editing = gizmo.isEditing(gizmoTargetId(sphere.id));
	if (ImGui::Button(editing ? "Stop Editing Transform" : "Edit Transform")) {
		if (editing) {
			gizmo.endEditing();
		} else {
			beginSphereGizmo(gizmo, sphere.id);
		}
	}
	if (editing) {
		ImGui::SameLine();
		ImGui::TextDisabled("(drag the gizmo in the viewport)");
	}

	ImGui::Spacing();

	changed |= drawMaterialParams(sphere.material);

	if (changed) {
		markChanged();
	}

	ImGui::PopID();
}

void RaytraceSceneEditor::drawSelectedCamera(VulkanEngine* engine, int index)
{
	SceneCamera& camera = m_cameras[index];

	ImGui::SeparatorText(camera.name.c_str());
	ImGui::PushID((int)camera.id);

	//while the viewport drives this camera its pose comes from there, not from the widgets
	const bool firstPerson = engine->m_firstPersonCameraId == camera.id;

	ImGui::BeginDisabled(firstPerson);
	bool displayChanged = false;
	bool changed = drawCameraParams(camera, displayChanged);
	ImGui::EndDisabled();

	TransformGizmo& gizmo = engine->m_transformGizmo;
	const bool editing = gizmo.isEditing(gizmoTargetId(camera.id));
	ImGui::BeginDisabled(firstPerson);
	if (ImGui::Button(editing ? "Stop Editing Transform" : "Edit Transform")) {
		if (editing) {
			gizmo.endEditing();
		} else {
			beginCameraGizmo(gizmo, camera.id);
		}
	}
	ImGui::EndDisabled();

	//the other way to place a camera: look through it. The viewport takes this camera's pose
	//and fov, and every move of the viewport moves the camera
	ImGui::SameLine();
	if (ImGui::Button(firstPerson ? "Stop First-Person Edit" : "First-Person Edit")) {
		if (firstPerson) {
			engine->endFirstPersonEdit();
		} else {
			gizmo.endEditing();
			engine->beginFirstPersonEdit(camera.id);
		}
	}
	if (editing) {
		ImGui::SameLine();
		ImGui::TextDisabled("(drag the gizmo in the viewport)");
	} else if (firstPerson) {
		ImGui::SameLine();
		ImGui::TextDisabled("(fly the viewport: RMB look, WASD)");
	}

	if (changed) {
		markChanged();
	}

	ImGui::PopID();
}

void RaytraceSceneEditor::beginSphereGizmo(TransformGizmo& gizmo, uint64_t id)
{
	//the closures hold the sphere by id and look it up each frame: the editor keeps sole
	//ownership, and deleting or reloading the sphere ends the edit rather than leaving the gizmo
	//pointing at a vector slot that now holds something else
	gizmo.beginEditing(
		gizmoTargetId(id),
		//sphere -> matrix: translation is the centre, uniform scale is the radius, no rotation
		[this, id]() -> std::optional<glm::mat4> {
			const SceneSphere* s = findSphere(id);
			if (s == nullptr) {
				return std::nullopt;
			}
			return glm::translate(glm::mat4(1.f), s->center) * glm::scale(glm::mat4(1.f), glm::vec3(s->radius));
		},
		//matrix -> sphere. The scale is uniform by construction (SCALEU); the three axes are
		//averaged anyway so a different operation set could not produce a lopsided radius
		[this, id](const glm::mat4& m) {
			SceneSphere* s = findSphere(id);
			if (s == nullptr) {
				return;
			}
			s->center = glm::vec3(m[3]);
			const float scale = (glm::length(glm::vec3(m[0])) + glm::length(glm::vec3(m[1])) + glm::length(glm::vec3(m[2]))) / 3.f;
			s->radius = std::max(scale, 0.001f);
			markChanged();
		},
		//a sphere has no meaningful rotation, so translate plus uniform scale (the radius) only
		ImGuizmo::TRANSLATE | ImGuizmo::SCALEU);
}

void RaytraceSceneEditor::beginCameraGizmo(TransformGizmo& gizmo, uint64_t id)
{
	gizmo.beginEditing(
		gizmoTargetId(id),
		[this, id]() -> std::optional<glm::mat4> {
			const SceneCamera* c = findCamera(id);
			if (c == nullptr) {
				return std::nullopt;
			}
			return c->transform();
		},
		[this, id](const glm::mat4& m) {
			SceneCamera* c = findCamera(id);
			if (c == nullptr) {
				return;
			}
			c->position = glm::vec3(m[3]);
			c->orientation = orientationOf(m);
			markChanged();
		},
		//a camera is placed and aimed; its size is its fov. Local mode so the rotation rings
		//follow the camera's own axes
		ImGuizmo::TRANSLATE | ImGuizmo::ROTATE, ImGuizmo::LOCAL);
}
