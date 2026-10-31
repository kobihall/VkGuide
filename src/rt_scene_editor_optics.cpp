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

bool reflectsFromFront(OpticKind kind)
{
	return kind == OpticKind::PlanarMirror || kind == OpticKind::ConcaveMirror || kind == OpticKind::OffAxisParabola;
}

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
	SceneOptic optic = makeOptic(kind);
	standOnBench(engine, optic, engine->cameraFromViewport());
	const uint64_t id = addOptic(std::move(optic));
	select(SelectionKind::Optic, id);
	resolveBench(engine);
}

void RaytraceSceneEditor::standOnBench(VulkanEngine* engine, SceneOptic& optic, const SceneCamera& view)
{
	// the table point under the middle of the view, or 0.3 m ahead of it when the view does not look down at the table
	const glm::dvec3 eye(view.position);
	const glm::dvec3 forward(view.forward());
	glm::dvec3 ahead = eye + 0.3 * forward;
	if (forward.y < -1e-3) {
		const double t = -eye.y / forward.y;
		if (t > 0.0 && t < 3.0) {
			ahead = eye + t * forward;
		}
	}
	// facing the viewer: light from the viewer's side is what a new element is looked at with
	const double yaw = std::atan2(eye.x - ahead.x, eye.z - ahead.z);
	if (optic.mount.isFree()) {
		optic.mount.freePosition = eye + 0.3 * forward;
		optic.mount.freeOrientation = glm::dquat(view.orientation * glm::angleAxis(glm::pi<float>(), glm::vec3(0.f, 1.f, 0.f)));
		optic.world = optic.mount.freeTransform();
		return;
	}
	// the origin there: the post stands behind it
	placeOnBench(optic, glm::dvec2(ahead.x, ahead.z), m_bench.beamHeight, yaw, m_bench.snap, engine->benchContext(), otherStacks(0));
}

std::vector<OpticStack> RaytraceSceneEditor::otherStacks(uint64_t opticId) const
{
	std::vector<OpticStack> others;
	for (size_t i = 0; i < m_optics.size() && i < m_stacks.size(); i++) {
		if (m_optics[i].id != opticId) {
			others.push_back(m_stacks[i]);
		}
	}
	return others;
}

void RaytraceSceneEditor::setBench(const BenchSettings& bench)
{
	BenchSettings next = bench;
	next.size = snapBenchSize(next.size);
	if (!(next == m_bench)) {
		m_bench = next;
		markChanged();
	}
}

void RaytraceSceneEditor::resolveBench(VulkanEngine* engine)
{
	const BenchContext context = engine->benchContext();
	// a post that moved under its origin (a thicker optic, another mount) takes its fork to a hole again, and a fork another
	// stack was put on moves out from under it. In order, against the stacks as each refit leaves them, so that two forks
	// never trade places from frame to frame
	bool refit = false;
	m_stacks = assembleBench(m_optics, context);
	for (size_t i = 0; i < m_optics.size(); i++) {
		if (refitFork(m_optics[i], context, otherStacks(m_optics[i].id))) {
			m_stacks[i] = assembleStack(m_optics[i], context);
			refit = true;
		}
	}
	std::vector<OpticStack> stacks = assembleBench(m_optics, context);
	bool moved = refit;
	for (size_t i = 0; i < m_optics.size(); i++) {
		if (!(m_optics[i].world == stacks[i].opticToWorld)) {
			m_optics[i].world = stacks[i].opticToWorld;
			moved = true;
		}
	}
	m_stacks = std::move(stacks);
	if (moved) {
		markChanged();
	}
}

void RaytraceSceneEditor::drawBenchSettings(VulkanEngine*)
{
	BenchSettings bench = m_bench;
	bool changed = ImGui::Checkbox("breadboard", &bench.table);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Thorlabs' metric breadboard: M6 holes on a 25 mm grid, a hole at the origin, the top face at y = 0");
	}
	glm::dvec2 size = bench.size;
	if (dragLength2("size (x, z)", size, BREADBOARD_PITCH)) {
		bench.size = snapBenchSize(size);
		changed = true;
	}
	const BenchGrid grid = benchGrid(bench);
	ImGui::TextDisabled("%d x %d holes, %s to %s in x", grid.count.x, grid.count.y, formatLength(grid.min.x).c_str(), formatLength(grid.max.x).c_str());
	changed |= dragLength("beam height", bench.beamHeight, 0.001, 0.02, 1.0);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The optical axis height a new mount is set to");
	}
	changed |= ImGui::Checkbox("snap to holes", &bench.snap);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Dragging a post puts it on the nearest hole's center; holding Alt inverts this for the drag");
	}
	if (changed) {
		setBench(bench);
	}
}

void RaytraceSceneEditor::drawSelectedBench(VulkanEngine* engine)
{
	ImGui::SeparatorText("Breadboard");
	drawBenchSettings(engine);
}

void RaytraceSceneEditor::drawOpticAddMenu(VulkanEngine* engine)
{
	if (!m_bench.table && ImGui::MenuItem("Breadboard")) {
		BenchSettings bench = m_bench;
		bench.table = true;
		setBench(bench);
		select(SelectionKind::Bench, 0);
	}
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
	// the breadboard, the one bench object without an id
	if (m_bench.table) {
		ImGui::PushID("breadboard");
		if (ImGui::Selectable("Breadboard", m_selectionKind == SelectionKind::Bench)) {
			select(SelectionKind::Bench, 0);
		}
		if (ImGui::BeginPopupContextItem("##context")) {
			if (ImGui::MenuItem("Delete", "Del")) {
				select(SelectionKind::Bench, 0);
				deleteSelection();
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
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
	changed |= drawOpticMount(engine, optic);

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

	// what the beam axis will use (physoptics-beam-axis.md)
	if (ImGui::TreeNode("Ports")) {
		for (const OpticPort& port : traits.ports) {
			ImGui::BulletText("%s: %zu event(s)", port.name, port.events.size());
		}
		ImGui::TreePop();
	}

	if (changed) {
		refreshDefaultSlots(before, optic);
		markChanged();
	}
	ImGui::PopID();
}

bool RaytraceSceneEditor::drawOpticMount(VulkanEngine* engine, SceneOptic& optic)
{
	bool changed = false;
	ImGui::SeparatorText("Mount");
	const BenchContext context = engine->benchContext();
	const size_t index = (size_t)(&optic - m_optics.data());
	const OpticStack* stack = index < m_stacks.size() ? &m_stacks[index] : nullptr;

	// the kind, named with the model it gives at this size
	auto mountLabel = [&](MountKind kind) {
		SceneOptic probe = optic;
		probe.mount.kind = kind;
		const MountModelInfo* model = opticMountModel(probe);
		return model != nullptr ? fmt::format("{} ({})", mountKindLabel(kind), model->id) : std::string(mountKindLabel(kind));
	};
	if (ImGui::BeginCombo("mount", mountLabel(optic.mount.kind).c_str())) {
		for (const MountKind kind : allowedMounts(optic)) {
			if (ImGui::Selectable(mountLabel(kind).c_str(), kind == optic.mount.kind) && kind != optic.mount.kind) {
				const bool wasFree = optic.mount.isFree();
				optic.mount.kind = kind;
				optic.mount.fine = FineAdjust {};
				if (kind == MountKind::None) {
					// no stack: only where the optic is
					optic.mount = MountPose {};
					optic.mount.setFreeTransform(optic.world);
				} else if (wasFree && !optic.mount.freePlacement) {
					// out of the air and onto the bench, under where the optic was
					optic.mount.setFreeTransform(optic.world);
					dropOntoBench(optic, context, otherStacks(optic.id));
				} else if (!optic.mount.isFree()) {
					placeOnBench(optic, optic.mount.origin, optic.mount.height, optic.mount.yaw, false, context, otherStacks(optic.id));
				}
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	if (stack != nullptr) {
		for (const std::string& note : stack->notes) {
			ImGui::PushTextWrapPos(0.f);
			ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "%s", note.c_str());
			ImGui::PopTextWrapPos();
		}
	}

	if (optic.mount.kind != MountKind::None) {
		bool free = optic.mount.freePlacement;
		if (ImGui::Checkbox("free placement", &free)) {
			if (free) {
				optic.mount.setFreeTransform(optic.world);
				optic.mount.freePlacement = true;
			} else {
				optic.mount.setFreeTransform(optic.world);
				dropOntoBench(optic, context, otherStacks(optic.id));
			}
			changed = true;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Six degrees of freedom for a periscope or a part off the table, without a post, holder or fork. Turning it off stands the optic\n"
							  "on a post under its datum, turned by its axis's horizontal part, at its datum's height");
		}
	}

	TransformGizmo& gizmo = engine->m_transformGizmo;
	const bool editing = gizmo.isEditing(opticGizmoTarget(optic.id));
	if (optic.mount.isFree()) {
		// six degrees of freedom, at the optic's origin: a turn keeps the origin where it is
		const glm::dvec3 local = opticOrigin(optic, context);
		glm::dvec3 origin = optic.mount.freePosition + optic.mount.freeOrientation * local;
		if (dragLength3("origin", origin, 0.001)) {
			optic.mount.freePosition = origin - optic.mount.freeOrientation * local;
			changed = true;
		}
		glm::vec3 euler = glm::degrees(glm::eulerAngles(glm::quat(optic.mount.freeOrientation)));
		if (ImGui::DragFloat3("rotation", &euler.x, 0.5f, 0.f, 0.f, "%.2f deg")) {
			optic.mount.freeOrientation = glm::dquat(glm::quat(glm::radians(euler)));
			optic.mount.freePosition = origin - optic.mount.freeOrientation * local;
			changed = true;
		}
	} else {
		// the stack's four, at the optic's origin: across the table, the turn about its vertical, the height
		glm::dvec2 origin = optic.mount.origin;
		double yawDegrees = optic.mount.yaw * 180.0 / PI;
		double height = optic.mount.height;
		bool moved = dragLength2("origin (x, z)", origin, 0.0005);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s: what the optic is placed by. The post stands wherever this puts it",
				reflectsFromFront(optic.kind) ? "The center of the mirror's front face" : "The optic's center, on its axis");
		}
		moved |= ImGui::DragScalar("yaw", ImGuiDataType_Double, &yawDegrees, 0.25f, nullptr, nullptr, "%.3f deg");
		const double lowest = stack != nullptr ? stack->minimumHeight : 0.0;
		moved |= dragLength("height", height, 0.0005, lowest, 1.0);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("The optical axis above the table, from %s: the post's top at its holder's top face", formatLength(lowest).c_str());
		}
		if (moved) {
			placeOnBench(optic, origin, height, yawDegrees * PI / 180.0, false, context, otherStacks(optic.id));
			changed = true;
		}
		if (stack != nullptr && stack->post) {
			ImGui::TextDisabled("post: at (%s, %s), %s long, %s of it in the holder", formatLength(stack->postAxis.x).c_str(), formatLength(stack->postAxis.y).c_str(),
				formatLength(stack->postTop.y - stack->postBottom.y).c_str(), formatLength(stack->holderTop - stack->postBottom.y).c_str());
			const glm::dvec2 bolt = stack->bolt;
			if (stack->forkBlocked) {
				ImGui::PushTextWrapPos(0.f);
				ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "Fork blocked: %s",
					isHole(m_bench, bolt, 1e-6) ? "it overlaps another stack's fork or post holder, and no free hole is in reach" : "no hole is under its bolt");
				ImGui::PopTextWrapPos();
			} else {
				ImGui::TextDisabled("fork: %.1f deg, bolt %s from the post at (%s, %s)", optic.mount.forkAngle * 180.0 / PI, formatLength(optic.mount.forkReach).c_str(),
					formatLength(bolt.x).c_str(), formatLength(bolt.y).c_str());
			}
		}
		if (ImGui::SmallButton("Snap to nearest hole")) {
			placeOnBench(optic, optic.mount.origin, optic.mount.height, optic.mount.yaw, true, context, otherStacks(optic.id));
			changed = true;
		}
	}
	if (ImGui::Button(editing ? "Stop Editing Transform" : "Edit Transform")) {
		if (editing) {
			gizmo.endEditing();
		} else {
			beginOpticGizmo(engine, gizmo, optic.id);
		}
	}
	if (editing) {
		ImGui::SameLine();
		ImGui::TextDisabled(optic.mount.isFree() ? "(drag the gizmo in the viewport)" : "(drag in the viewport; Alt inverts snapping)");
	}

	// the fine adjustment the mount has
	if (stack != nullptr && stack->mount != nullptr && !optic.mount.freePlacement) {
		ImGui::SeparatorText("Fine adjustment");
		FineAdjust& fine = optic.mount.fine;
		if (stack->kinematic) {
			const KinematicReadout& k = *stack->kinematic;
			const double limits[2] = { k.turnLimit.x, k.turnLimit.y };
			double* turns[2] = { &fine.adjusterX, &fine.adjusterY };
			const char* labels[2] = { "adjuster X", "adjuster Y" };
			for (int a = 0; a < 2; a++) {
				const double lo = -limits[a];
				const double hi = limits[a];
				if (ImGui::DragScalar(labels[a], ImGuiDataType_Double, turns[a], 0.005f, &lo, &hi, "%.3f rev", ImGuiSliderFlags_AlwaysClamp)) {
					changed = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%.1f mrad of plate per revolution (Thorlabs), an effective pitch of %.4f mm over the modeled lever. The screw stops at %.2f revolutions,\n"
									  "where the plate reaches the mount's %.1f deg",
						1e3 * k.anglePerRev[a], 1e3 * k.pitch[a], limits[a], k.tiltRange * 180.0 / PI);
				}
			}
			ImGui::TextDisabled("plate: %.1f urad about the pivot and Y, %.1f urad about the pivot and X", k.angleX * 1e6, k.angleY * 1e6);
			ImGui::TextDisabled("datum moves (%.2f, %.2f, %.2f) um", k.datumShift.x * 1e6, k.datumShift.y * 1e6, k.datumShift.z * 1e6);
			if (!k.converged) {
				ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "The adjusters' solution did not converge");
			}
		} else if (stack->mount->kind == MountModelKind::RotationMount) {
			// continuous, or whole graduations (2 deg, labeled every 20) while Ctrl is held
			double degrees = fine.rotor * 180.0 / PI;
			const double lo = -90.0;
			const double hi = 90.0;
			if (ImGui::DragScalar("rotor", ImGuiDataType_Double, &degrees, 0.1f, &lo, &hi, "%.3f deg", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp)) {
				if (ImGui::GetIO().KeyCtrl) {
					degrees = 2.0 * std::round(degrees / 2.0);
				}
				fine.rotor = degrees * PI / 180.0;
				changed = true;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Hold Ctrl while dragging to stop on the 2 deg graduations. The angle counts from where the model has the rotor; the scale's zero is not identified");
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.f);
			if (ImGui::InputDouble("##rotorTyped", &degrees, 0.0, 0.0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
				fine.rotor = std::clamp(degrees, lo, hi) * PI / 180.0;
				changed = true;
			}
			const double graduation = 2.0 * std::floor(degrees / 2.0);
			ImGui::TextDisabled("between the %.0f and %.0f deg graduations", graduation, graduation + 2.0);
		} else if (stack->mount->kind == MountModelKind::Iris) {
			ImGui::TextDisabled("the lever and the blades follow the opening");
		} else {
			ImGui::TextDisabled("none: %s holds the optic fixed", stack->mount->id);
		}
	}
	return changed;
}

void RaytraceSceneEditor::beginOpticGizmo(VulkanEngine* engine, TransformGizmo& gizmo, uint64_t id)
{
	const SceneOptic* optic = findOptic(id);
	if (optic == nullptr) {
		return;
	}
	if (optic->mount.isFree()) {
		// at the optic's origin, in its body frame's axes
		gizmo.beginEditing(
			opticGizmoTarget(id),
			[this, id, engine]() -> std::optional<glm::mat4> {
				const SceneOptic* o = findOptic(id);
				if (o == nullptr || !o->mount.isFree()) {
					return std::nullopt;
				}
				return glm::mat4(o->mount.freeTransform() * glm::translate(glm::dmat4(1.0), opticOrigin(*o, engine->benchContext())));
			},
			[this, id, engine](const glm::mat4& m) {
				SceneOptic* o = findOptic(id);
				if (o == nullptr) {
					return;
				}
				const glm::mat3 r(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
				o->mount.freeOrientation = glm::dquat(glm::quat_cast(r));
				o->mount.freePosition = glm::dvec3(m[3]) - o->mount.freeOrientation * opticOrigin(*o, engine->benchContext());
				markChanged();
			},
			// an optic is placed and turned; its size is its parameters
			ImGuizmo::TRANSLATE | ImGuizmo::ROTATE, ImGuizmo::LOCAL);
		return;
	}
	// on the bench: at the optic's origin, sliding across the table, up and down, and turning about its vertical only
	gizmo.beginEditing(
		opticGizmoTarget(id),
		[this, id]() -> std::optional<glm::mat4> {
			const SceneOptic* o = findOptic(id);
			if (o == nullptr || o->mount.isFree()) {
				return std::nullopt;
			}
			glm::dmat4 m = glm::rotate(glm::dmat4(1.0), o->mount.yaw, glm::dvec3(0.0, 1.0, 0.0));
			m[3] = glm::dvec4(o->mount.origin.x, o->mount.height, o->mount.origin.y, 1.0);
			return glm::mat4(m);
		},
		[this, id, engine](const glm::mat4& m) {
			SceneOptic* o = findOptic(id);
			if (o == nullptr) {
				return;
			}
			const bool snap = m_bench.snap != ImGui::GetIO().KeyAlt;
			const double yaw = std::atan2((double)m[2].x, (double)m[2].z);
			placeOnBench(*o, glm::dvec2(m[3].x, m[3].z), m[3].y, yaw, snap, engine->benchContext(), otherStacks(id));
			markChanged();
		},
		ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y | ImGuizmo::TRANSLATE_Z | ImGuizmo::ROTATE_Y, ImGuizmo::WORLD);
}
