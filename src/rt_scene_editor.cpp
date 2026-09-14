#include <rt_scene_editor.h>

#include <imgui.h>

#include <rt_material.h>
#include <rt_scene.h>
#include <vk_engine.h>

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

void RaytraceSceneEditor::drawPanel(VulkanEngine* engine)
{
	if (!m_showPanel) {
		return;
	}

	//"Raytracer Scene" rather than "Scene" - the engine already uses "scene" for the loaded
	//glTF files (m_loadedScenes) and the raster uniform block (GPUSceneData)
	if (!ImGui::Begin("Raytracer Scene", &m_showPanel)) {
		ImGui::End();
		return;
	}

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
		m_dirty = true;
	}

	const bool canDelete = m_selectionKind == SelectionKind::Sphere && m_selectionIndex < (int)m_spheres.size();
	ImGui::SameLine();
	ImGui::BeginDisabled(!canDelete);
	if (ImGui::Button("Delete Sphere") && canDelete) {
		m_spheres.erase(m_spheres.begin() + m_selectionIndex);
		m_selectionKind = SelectionKind::None;
		m_dirty = true;
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
		//traceable in this feature - selecting one deliberately shows nothing further
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
		drawSphereParams(m_selectionIndex);
	}

	ImGui::End();
}

void RaytraceSceneEditor::drawSphereParams(int index)
{
	SceneSphere& entry = m_spheres[index];

	ImGui::SeparatorText(entry.name.c_str());

	//scoping the widget ids to the selected sphere stops an in-progress drag, or any other
	//per-widget state imgui keys by id, from carrying across a selection change
	ImGui::PushID(index);

	bool changed = entry.object->params();

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
		m_dirty = true;
	}

	ImGui::PopID();
}
