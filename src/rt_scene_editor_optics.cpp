// The optical elements' part of RaytraceSceneEditor (docs/plans/completed/physoptics-optical-elements.md): the list, the slot
// materials each element owns, the Optics folder of the Scene panel, the element's panel and its gizmo. The Optical bench
// capability draws the folder and the Add entries through its hooks (src/scene_capabilities.cpp); everything else here
// is the same machinery the shapes use.

#include <rt_scene_editor.h>

#include <length_unit.h>
#include <optic_library.h>
#include <optics_ui.h>
#include <vk_engine.h>
#include <vk_gizmo.h>

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

namespace {

constexpr double PI = 3.14159265358979323846;

const void* opticGizmoTarget(uint64_t id)
{
	return reinterpret_cast<const void*>(static_cast<uintptr_t>(id));
}

// a material compared as the kind's default is: by type and parameters, whatever its id and name
bool sameMaterial(const MaterialDef& a, const MaterialDef& b)
{
	return a.type == b.type && a.params == b.params && a.lightGroup == b.lightGroup;
}

}

SceneOptic* RaytraceSceneEditor::findOptic(uint64_t id)
{
	auto found = std::find_if(m_optics.begin(), m_optics.end(), [id](const SceneOptic& o) { return o.id == id; });
	return found == m_optics.end() ? nullptr : &(*found);
}

const SceneOptic* RaytraceSceneEditor::findOptic(uint64_t id) const
{
	return const_cast<RaytraceSceneEditor*>(this)->findOptic(id);
}

uint64_t RaytraceSceneEditor::addOptic(SceneOptic optic)
{
	optic.id = m_nextId++;
	if (optic.name.empty()) {
		optic.name = nextOpticName(optic.kind);
	}
	const size_t slots = opticTraits(optic.kind).slots.size();
	for (size_t slot = 0; slot < MAX_OPTIC_MATERIAL_SLOTS; slot++) {
		optic.materials[slot] = slot < slots ? addMaterial(defaultSlotMaterial(optic, slot)) : 0;
	}
	const uint64_t id = optic.id;
	m_optics.push_back(std::move(optic));
	markChanged();
	return id;
}

void RaytraceSceneEditor::removeOptic(uint64_t id)
{
	const SceneOptic* optic = findOptic(id);
	if (optic == nullptr) {
		return;
	}
	const std::array<uint64_t, MAX_OPTIC_MATERIAL_SLOTS> owned = optic->materials;
	std::erase_if(m_optics, [id](const SceneOptic& o) { return o.id == id; });
	std::erase_if(m_materials, [&owned](const MaterialDef& m) { return m.id != 0 && std::find(owned.begin(), owned.end(), m.id) != owned.end(); });
	if (m_selectionKind == SelectionKind::Optic && m_selectionId == id) {
		m_selectionKind = SelectionKind::None;
		m_selectionId = 0;
	}
	markChanged();
}

void RaytraceSceneEditor::replaceOptics(std::vector<SceneOptic>&& optics)
{
	while (!m_optics.empty()) {
		removeOptic(m_optics.back().id);
	}
	m_nextOpticNumber.fill(1);
	for (SceneOptic& optic : optics) {
		m_nextOpticNumber[(size_t)optic.kind]++;
		addOptic(std::move(optic));
	}
	markChanged();
}

void RaytraceSceneEditor::setOpticSlotMaterial(uint64_t opticId, size_t slot, MaterialDef material)
{
	const SceneOptic* optic = findOptic(opticId);
	if (optic == nullptr || slot >= MAX_OPTIC_MATERIAL_SLOTS) {
		return;
	}
	MaterialDef* current = findMaterial(optic->materials[slot]);
	if (current == nullptr) {
		return;
	}
	material.id = current->id;
	material.name = current->name;
	material.opticOrigin = current->opticOrigin;
	material.origin.reset();
	*current = std::move(material);
	markChanged();
}

std::string RaytraceSceneEditor::nextOpticName(OpticKind kind)
{
	return fmt::format("{}_{}", opticTraits(kind).name, m_nextOpticNumber[(size_t)kind]++);
}

void RaytraceSceneEditor::refreshDefaultSlots(const SceneOptic& before, SceneOptic& after)
{
	for (size_t slot = 0; slot < opticTraits(after.kind).slots.size(); slot++) {
		MaterialDef* material = findMaterial(after.materials[slot]);
		if (material == nullptr) {
			continue;
		}
		// a slot the user has not touched follows the kind's default (a splitter's coating follows "polarizing")
		const MaterialDef old = defaultSlotMaterial(before, slot);
		const MaterialDef fresh = defaultSlotMaterial(after, slot);
		if (sameMaterial(*material, old) && !sameMaterial(old, fresh)) {
			material->type = fresh.type;
			material->params = fresh.params;
		}
	}
}

void RaytraceSceneEditor::createOptic(VulkanEngine* engine, OpticKind kind)
{
	// in front of the viewport, facing it: light from the viewer's side is what a new element is looked at with
	const SceneCamera view = engine->cameraFromViewport();
	SceneOptic optic = makeOptic(kind);
	optic.position = glm::dvec3(view.position + 0.3f * view.forward());
	optic.orientation = glm::dquat(view.orientation * glm::angleAxis(glm::pi<float>(), glm::vec3(0.f, 1.f, 0.f)));
	const uint64_t id = addOptic(std::move(optic));
	select(SelectionKind::Optic, id);
}

void RaytraceSceneEditor::drawOpticAddMenu(VulkanEngine* engine)
{
	if (!ImGui::BeginMenu("Optic")) {
		return;
	}
	for (size_t k = 0; k < OPTIC_KIND_COUNT; k++) {
		if (ImGui::MenuItem(opticTraits((OpticKind)k).label)) {
			createOptic(engine, (OpticKind)k);
		}
	}
	ImGui::EndMenu();
}

void RaytraceSceneEditor::drawOpticRows(VulkanEngine*)
{
	if (!ImGui::TreeNodeEx("Optics", ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}
	if (m_optics.empty()) {
		ImGui::TextDisabled("none - Add > Optic");
	}
	// a folder per kind the scene has, in the kinds' order, as the shapes have
	bool alive = true;
	for (size_t k = 0; k < OPTIC_KIND_COUNT && alive; k++) {
		const OpticKind kind = (OpticKind)k;
		const auto ofKind = [kind](const SceneOptic& optic) { return optic.kind == kind; };
		if (std::none_of(m_optics.begin(), m_optics.end(), ofKind)) {
			continue;
		}
		if (!ImGui::TreeNodeEx(opticTraits(kind).plural, ImGuiTreeNodeFlags_DefaultOpen)) {
			continue;
		}
		for (size_t i = 0; i < m_optics.size() && alive; i++) {
			if (m_optics[i].kind == kind) {
				alive = drawObjectRow(SelectionKind::Optic, m_optics[i].id, m_optics[i].name);
			}
		}
		ImGui::TreePop();
	}
	ImGui::TreePop();
}

void RaytraceSceneEditor::drawOpticMaterialRows()
{
	if (m_optics.empty() || !ImGui::TreeNodeEx("Optics")) {
		return;
	}
	for (const SceneOptic& optic : m_optics) {
		if (!ImGui::TreeNodeEx(optic.name.c_str())) {
			continue;
		}
		const OpticTraits& traits = opticTraits(optic.kind);
		for (size_t slot = 0; slot < traits.slots.size(); slot++) {
			const MaterialDef* material = findMaterial(optic.materials[slot]);
			if (material == nullptr) {
				continue;
			}
			ImGui::PushID((int)material->id);
			if (ImGui::Selectable(traits.slots[slot].name, material->id == m_selectedMaterialId)) {
				m_selectedMaterialId = material->id;
			}
			ImGui::PopID();
		}
		ImGui::TreePop();
	}
	ImGui::TreePop();
}

void RaytraceSceneEditor::drawSelectedOptic(VulkanEngine* engine, SceneOptic& optic)
{
	const OpticTraits& traits = opticTraits(optic.kind);
	ImGui::SeparatorText(optic.name.c_str());
	ImGui::TextDisabled("%s", traits.label);
	ImGui::PushID((int)optic.id);
	const SceneOptic before = optic;
	bool changed = false;

	// the size: a preset (Thorlabs' part at that size) or custom
	const char* sizeLabel = optic.sizePreset >= 0 && optic.sizePreset < (int)traits.sizes.size() ? traits.sizes[(size_t)optic.sizePreset].label : "Custom";
	if (ImGui::BeginCombo("size", sizeLabel)) {
		for (int i = 0; i < (int)traits.sizes.size(); i++) {
			if (ImGui::Selectable(traits.sizes[(size_t)i].label, i == optic.sizePreset) && i != optic.sizePreset) {
				setOpticSize(optic, i);
				changed = true;
			}
		}
		if (ImGui::Selectable("Custom", optic.sizePreset < 0) && optic.sizePreset >= 0) {
			setOpticSize(optic, -1);
			changed = true;
		}
		ImGui::EndCombo();
	}

	for (const OpticParam& param : traits.params) {
		if (param.visible != nullptr && !param.visible(optic.params)) {
			continue;
		}
		ImGui::PushID(param.name);
		const OpticValue value = opticValue(optic, param.name);
		switch (param.kind) {
		case OpticParamKind::Length: {
			double meters = opticNumber(optic, param.name);
			if (inputLength(param.label, meters, param.min, param.max)) {
				changed |= setOpticValue(optic, param.name, meters);
			}
			break;
		}
		case OpticParamKind::Angle: {
			double degrees = opticNumber(optic, param.name) * 180.0 / PI;
			if (ImGui::InputDouble(param.label, &degrees, 0.0, 0.0, "%.4f deg", ImGuiInputTextFlags_EnterReturnsTrue)) {
				changed |= setOpticValue(optic, param.name, degrees * PI / 180.0);
			}
			break;
		}
		case OpticParamKind::Number: {
			double number = opticNumber(optic, param.name);
			if (ImGui::InputDouble(param.label, &number, 0.0, 0.0, "%.6g", ImGuiInputTextFlags_EnterReturnsTrue)) {
				changed |= setOpticValue(optic, param.name, number);
			}
			break;
		}
		case OpticParamKind::Enum: {
			const int current = std::clamp(opticEnum(optic, param.name), 0, (int)param.labels.size() - 1);
			if (!param.labels.empty() && ImGui::BeginCombo(param.label, param.labels[(size_t)current])) {
				for (int i = 0; i < (int)param.labels.size(); i++) {
					if (ImGui::Selectable(param.labels[(size_t)i], i == current) && i != current) {
						changed |= setOpticValue(optic, param.name, i);
					}
				}
				ImGui::EndCombo();
			}
			break;
		}
		case OpticParamKind::Bool: {
			bool b = opticBool(optic, param.name);
			if (ImGui::Checkbox(param.label, &b)) {
				changed |= setOpticValue(optic, param.name, b);
			}
			break;
		}
		case OpticParamKind::Glass: {
			std::string name = opticString(optic, param.name);
			if (drawGlassCombo(param.label, name, true)) {
				changed |= setOpticValue(optic, param.name, name);
			}
			break;
		}
		}
		(void)value;
		if (param.tooltip != nullptr && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", param.tooltip);
		}
		ImGui::PopID();
	}

	// what follows from the parameters, at the design wavelength, or why they make no body
	OpticReadout readout;
	const OpticPrescription body = buildOptic(optic, engine->opticBuildContext(), readout);
	if (!readout.error.empty()) {
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextColored(ImVec4(1.f, 0.45f, 0.3f, 1.f), "%s", readout.error.c_str());
		ImGui::PopTextWrapPos();
	} else {
		for (const auto& [label, meters] : readout.lengths) {
			ImGui::TextDisabled("%s: %s", label.c_str(), formatLength(meters).c_str());
		}
		for (const auto& [label, radians] : readout.angles) {
			ImGui::TextDisabled("%s: %.4f deg", label.c_str(), radians * 180.0 / PI);
		}
		for (const auto& [label, text] : readout.lines) {
			ImGui::TextDisabled("%s: %s", label.c_str(), text.c_str());
		}
		ImGui::TextDisabled("%zu surfaces", body.surfaces.size());
	}

	ImGui::Spacing();
	changed |= dragLength3("position", optic.position, 0.001);
	glm::quat orientation(optic.orientation);
	glm::vec3 euler = glm::degrees(glm::eulerAngles(orientation));
	if (ImGui::DragFloat3("rotation", &euler.x, 0.5f, 0.f, 0.f, "%.2f deg")) {
		optic.orientation = glm::dquat(glm::quat(glm::radians(euler)));
		changed = true;
	}

	TransformGizmo& gizmo = engine->m_transformGizmo;
	const bool editing = gizmo.isEditing(opticGizmoTarget(optic.id));
	if (ImGui::Button(editing ? "Stop Editing Transform" : "Edit Transform")) {
		if (editing) {
			gizmo.endEditing();
		} else {
			beginOpticGizmo(gizmo, optic.id);
		}
	}
	if (editing) {
		ImGui::SameLine();
		ImGui::TextDisabled("(drag the gizmo in the viewport)");
	}

	// the slots: each a library entry the element owns, its coating and finish shown here, the rest in Materials
	ImGui::SeparatorText("Materials");
	for (size_t slot = 0; slot < traits.slots.size(); slot++) {
		MaterialDef* material = findMaterial(optic.materials[slot]);
		if (material == nullptr) {
			continue;
		}
		ImGui::PushID((int)slot);
		ImGui::TextUnformatted(traits.slots[slot].name);
		ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.3f);
		if (material->type == MaterialType::OpticalInterface) {
			std::string coating = materialCoating(*material, "coating");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
			if (drawCoatingCombo("##coating", coating)) {
				material->params["coating"] = CoatingRef { coating };
				changed = true;
			}
			const OpticCoating* found = engine->m_opticalLibrary.findCoating(coating);
			if (found != nullptr && found->polarizing && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Polarizing: without polarization the renderer uses the average of s and p");
			}
		} else {
			ImGui::TextDisabled("%s", materialTraits(material->type).label);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Edit")) {
			openMaterial(material->id);
		}
		ImGui::PopID();
	}

	// what the beam axis and the mounts will use (physoptics-beam-axis.md, physoptics-bench-and-mounts.md)
	if (ImGui::TreeNode("Ports and mounts")) {
		for (const OpticPort& port : traits.ports) {
			ImGui::BulletText("%s: %zu event(s)", port.name, port.events.size());
		}
		static const char* const MOUNT_NAMES[] = { "kinematic", "fixed", "plate holder", "rotation", "prism mount", "iris assembly" };
		if (!traits.mounts.empty()) {
			ImGui::TextDisabled("default mount: %s", MOUNT_NAMES[(size_t)traits.mounts[0]]);
		}
		ImGui::TreePop();
	}

	if (changed) {
		refreshDefaultSlots(before, optic);
		markChanged();
	}
	ImGui::PopID();
}

void RaytraceSceneEditor::beginOpticGizmo(TransformGizmo& gizmo, uint64_t id)
{
	gizmo.beginEditing(
		opticGizmoTarget(id),
		[this, id]() -> std::optional<glm::mat4> {
			const SceneOptic* o = findOptic(id);
			if (o == nullptr) {
				return std::nullopt;
			}
			return glm::mat4(o->bodyToWorld());
		},
		[this, id](const glm::mat4& m) {
			SceneOptic* o = findOptic(id);
			if (o == nullptr) {
				return;
			}
			o->position = glm::dvec3(m[3]);
			const glm::mat3 r(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
			o->orientation = glm::dquat(glm::quat_cast(r));
			markChanged();
		},
		// an optic is placed and turned; its size is its parameters
		ImGuizmo::TRANSLATE | ImGuizmo::ROTATE, ImGuizmo::LOCAL);
}
