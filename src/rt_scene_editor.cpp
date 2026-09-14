#include <rt_scene_editor.h>

#include <algorithm>
#include <cstdio>

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

#include <rt_material.h>
#include <rt_scene.h>
#include <vk_engine.h>
#include <vk_gizmo.h>
#include <vk_ui.h>

RaytraceSceneEditor::RaytraceSceneEditor()
{
	//the sibling project's hardcoded starting scene (Renderer::Renderer()), verbatim
	auto material_ground = std::make_shared<lambertian>(glm::dvec3(1.0, 1.0, 1.0));
	auto material_center = std::make_shared<lambertian>(glm::dvec3(0.7, 0.3, 0.3));
	auto material_left = std::make_shared<dielectric>(1.5);
	auto material_right = std::make_shared<phong>(glm::dvec3(0.8, 0.6, 0.2), 0.3);

	m_spheres.push_back({ "ground", std::make_shared<sphere>(glm::dvec3(0.0, -100.5, -1.0), 100.0, material_ground) });
	m_spheres.push_back({ "center_sphere", std::make_shared<sphere>(glm::dvec3(0.0, 0.0, -1.0), 0.5, material_center) });
	m_spheres.push_back({ "left_sphere", std::make_shared<sphere>(glm::dvec3(-1.0, 0.0, -1.0), 0.5, material_left) });
	m_spheres.push_back({ "right_sphere", std::make_shared<sphere>(glm::dvec3(1.0, 0.0, -1.0), 0.5, material_right) });
}

void RaytraceSceneEditor::replaceSpheres(std::vector<SceneSphere>&& spheres)
{
	//the old sphere objects die here. If the gizmo was editing one of them its weak reference
	//expires and it stops on its own next frame
	m_spheres = std::move(spheres);
	m_selectionKind = SelectionKind::None;
	m_nextSphereNumber = (int)m_spheres.size() + 1;
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
		auto newMaterial = std::make_shared<lambertian>(glm::dvec3(0.7, 0.7, 0.7));
		m_spheres.push_back({ fmt::format("sphere_{}", m_nextSphereNumber++), std::make_shared<sphere>(glm::dvec3(0.0, 0.0, -1.0), 0.5, newMaterial) });
		m_selectionKind = SelectionKind::Sphere;
		m_selectionIndex = (int)m_spheres.size() - 1;
		markChanged();
	}

	const bool canDelete = m_selectionKind == SelectionKind::Sphere && m_selectionIndex < (int)m_spheres.size();
	ImGui::SameLine();
	ImGui::BeginDisabled(!canDelete);
	if (ImGui::Button("Delete Sphere") && canDelete) {
		//a gizmo editing this sphere notices the expired weak reference and ends itself
		m_spheres.erase(m_spheres.begin() + m_selectionIndex);
		m_selectionKind = SelectionKind::None;
		markChanged();
	}
	ImGui::EndDisabled();

	if (ImGui::BeginListBox("Objects", ImVec2(-FLT_MIN, 8 * ImGui::GetTextLineHeightWithSpacing()))) {
		for (int i = 0; i < (int)m_spheres.size(); i++) {
			const bool selected = m_selectionKind == SelectionKind::Sphere && m_selectionIndex == i;
			ImGui::PushID(i);
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
			ImGui::PushID((int)m_spheres.size() + i);
			if (ImGui::Selectable(gltfNames[i].c_str(), selected)) {
				m_selectionKind = SelectionKind::GltfNode;
				m_selectionIndex = i;
			}
			ImGui::PopID();
		}
		ImGui::EndListBox();
	}

	if (m_selectionKind == SelectionKind::Sphere && m_selectionIndex < (int)m_spheres.size()) {
		drawSphereParams(engine, m_selectionIndex);
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

void RaytraceSceneEditor::drawSphereParams(VulkanEngine* engine, int index)
{
	SceneSphere& entry = m_spheres[index];

	ImGui::SeparatorText(entry.name.c_str());

	//scoping the widget ids to the selected sphere stops an in-progress drag, or any other
	//per-widget state imgui keys by id, from carrying across a selection change
	ImGui::PushID(index);

	bool changed = entry.object->params();

	//the gizmo sits alongside the sliders, not instead of them: sliders for exact numbers, the
	//gizmo for direct manipulation. Opt-in per sphere, and only one sphere at a time
	TransformGizmo& gizmo = engine->m_transformGizmo;
	const bool editing = gizmo.isEditing(entry.object.get());
	if (ImGui::Button(editing ? "Stop Editing Transform" : "Edit Transform")) {
		if (editing) {
			gizmo.endEditing();
		} else {
			beginSphereGizmo(gizmo, entry.object);
		}
	}
	if (editing) {
		ImGui::SameLine();
		ImGui::TextDisabled("(drag the gizmo in the viewport)");
	}

	ImGui::Spacing();

	const MaterialType currentType = entry.object->mat_ptr->type();
	if (ImGui::BeginCombo("material", materialTypeName(currentType))) {
		for (MaterialType type : MATERIAL_TYPES) {
			if (ImGui::Selectable(materialTypeName(type), type == currentType) && type != currentType) {
				//keep the colour across a type change where both types have one. Every sphere owns
				//its material outright, so replacing it here affects this sphere only
				const glm::dvec3 albedo = materialAlbedo(*entry.object->mat_ptr).value_or(glm::dvec3(0.7));
				entry.object->mat_ptr = makeMaterial(type, albedo);
				changed = true;
			}
		}
		ImGui::EndCombo();
	}

	changed |= entry.object->mat_ptr->params();

	if (changed) {
		markChanged();
	}

	ImGui::PopID();
}

void RaytraceSceneEditor::beginSphereGizmo(TransformGizmo& gizmo, const std::shared_ptr<sphere>& target)
{
	//the closures hold the sphere weakly: the editor keeps sole ownership, and deleting or
	//reloading it ends the edit rather than leaving the gizmo pointing at freed memory
	std::weak_ptr<sphere> weak = target;

	gizmo.beginEditing(
		target.get(),
		//sphere -> matrix: translation is the centre, uniform scale is the radius, no rotation
		[weak]() -> std::optional<glm::mat4> {
			std::shared_ptr<sphere> s = weak.lock();
			if (s == nullptr) {
				return std::nullopt;
			}
			return glm::translate(glm::mat4(1.f), glm::vec3(s->center)) * glm::scale(glm::mat4(1.f), glm::vec3((float)s->radius));
		},
		//matrix -> sphere. The scale is uniform by construction (SCALEU); the three axes are
		//averaged anyway so a different operation set could not produce a lopsided radius
		[weak, this](const glm::mat4& m) {
			std::shared_ptr<sphere> s = weak.lock();
			if (s == nullptr) {
				return;
			}
			s->center = glm::dvec3(glm::vec3(m[3]));
			const double scale = (glm::length(glm::vec3(m[0])) + glm::length(glm::vec3(m[1])) + glm::length(glm::vec3(m[2]))) / 3.0;
			s->radius = std::max(scale, 0.001);
			markChanged();
		},
		//a sphere has no meaningful rotation, so translate plus uniform scale (the radius) only
		ImGuizmo::TRANSLATE | ImGuizmo::SCALEU);
}
