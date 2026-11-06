// The optical elements' part of RaytraceSceneEditor (docs/plans/completed/physoptics-optical-elements.md): the list, the slot
// materials each element owns, the Optics folder of the Scene panel, the element's panel and its gizmo. The Optical bench
// capability draws the folder and the Add entries through its hooks (src/scene_capabilities.cpp); everything else here
// is the same machinery the shapes use.

#include <rt_scene_editor.h>

#include <length_unit.h>
#include <optic_laser.h>
#include <optic_library.h>
#include <detectors_ui.h>
#include <optic_detectors.h>
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
	// a stack's member shares the stack's materials, which go with the stack
	std::array<uint64_t, MAX_OPTIC_MATERIAL_SLOTS> owned = optic->materials;
	if (optic->stack != 0) {
		owned.fill(0);
	}
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
	// a laser at the beam height on the table point, its beam across the view (along the view's right), and on a hole
	// with its beam along the grid while the bench snaps
	if (opticTraits(optic.kind).snapsFree) {
		const glm::dvec3 right = glm::dvec3(view.orientation * glm::vec3(1.f, 0.f, 0.f));
		const double across = std::hypot(right.x, right.z) > 1e-9 ? std::atan2(right.x, right.z) : 0.0;
		optic.mount.freeOrientation = glm::angleAxis(across, glm::dvec3(0.0, 1.0, 0.0));
		optic.mount.freePosition = glm::dvec3(ahead.x, m_bench.beamHeight, ahead.z);
		if (m_bench.snap) {
			snapFreeToBench(optic, engine->benchContext());
		}
		optic.world = optic.mount.freeTransform();
		return;
	}
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
	resolveSensorStacks();
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
	// the optics and the laser; the detectors are sensors, in + Add -> Sensor (drawDetectorAddItems())
	if (!ImGui::BeginMenu("Optic")) {
		return;
	}
	for (size_t k = 0; k < OPTIC_KIND_COUNT; k++) {
		if (!isDetectorKind((OpticKind)k) && ImGui::MenuItem(opticTraits((OpticKind)k).label)) {
			createOptic(engine, (OpticKind)k);
		}
	}
	ImGui::EndMenu();
}

void RaytraceSceneEditor::drawDetectorAddItems(VulkanEngine* engine)
{
	for (size_t k = 0; k < OPTIC_KIND_COUNT; k++) {
		if (isDetectorKind((OpticKind)k) && ImGui::MenuItem(opticTraits((OpticKind)k).label)) {
			createOptic(engine, (OpticKind)k);
		}
		if (isDetectorKind((OpticKind)k)) {
			ImGui::SetItemTooltip("Placed on the bench as an optic is, facing you");
		}
	}
	if (ImGui::BeginMenu("Sensor stack")) {
		for (size_t k = 0; k < OPTIC_KIND_COUNT; k++) {
			if (isDetectorKind((OpticKind)k) && ImGui::MenuItem(opticTraits((OpticKind)k).plural)) {
				createSensorStack(engine, (OpticKind)k);
			}
		}
		ImGui::EndMenu();
	}
	ImGui::SetItemTooltip("Identical detectors spaced evenly from a start to an end point, as one detector slid along a translation stage");
}

void RaytraceSceneEditor::drawDetectorRows(VulkanEngine*)
{
	// a folder per detector kind the scene has, after the cameras', and the stacks'; a stack's members are under it
	bool alive = true;
	for (size_t k = 0; k < OPTIC_KIND_COUNT && alive; k++) {
		const OpticKind kind = (OpticKind)k;
		if (!isDetectorKind(kind) || std::none_of(m_optics.begin(), m_optics.end(), [kind](const SceneOptic& o) { return o.kind == kind && o.stack == 0; })) {
			continue;
		}
		if (!ImGui::TreeNodeEx(opticTraits(kind).plural, ImGuiTreeNodeFlags_DefaultOpen)) {
			continue;
		}
		for (size_t i = 0; i < m_optics.size() && alive; i++) {
			if (m_optics[i].kind == kind && m_optics[i].stack == 0) {
				alive = drawObjectRow(SelectionKind::Optic, m_optics[i].id, m_optics[i].name);
			}
		}
		ImGui::TreePop();
	}
	if (alive) {
		drawSensorStackRows();
	}
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
	if (std::all_of(m_optics.begin(), m_optics.end(), [](const SceneOptic& o) { return isDetectorKind(o.kind); })) {
		ImGui::TextDisabled("none - Add > Optic");
	}
	// a folder per kind the scene has, in the kinds' order, as the shapes have; the detectors are in the Sensors folder
	bool alive = true;
	auto drawKind = [&](OpticKind kind) {
		const auto ofKind = [kind](const SceneOptic& optic) { return optic.kind == kind; };
		if (!alive || std::none_of(m_optics.begin(), m_optics.end(), ofKind)) {
			return;
		}
		if (!ImGui::TreeNodeEx(opticTraits(kind).plural, ImGuiTreeNodeFlags_DefaultOpen)) {
			return;
		}
		for (size_t i = 0; i < m_optics.size() && alive; i++) {
			if (m_optics[i].kind == kind) {
				alive = drawObjectRow(SelectionKind::Optic, m_optics[i].id, m_optics[i].name);
			}
		}
		ImGui::TreePop();
	};
	for (size_t k = 0; k < OPTIC_KIND_COUNT; k++) {
		if (!isDetectorKind((OpticKind)k)) {
			drawKind((OpticKind)k);
		}
	}
	ImGui::TreePop();
}

void RaytraceSceneEditor::drawOpticMaterialRows()
{
	if ((m_optics.empty() && m_sensorStacks.empty()) || !ImGui::TreeNodeEx("Optics")) {
		return;
	}
	// a stack's members share its materials: the stack's sensor stands for them
	std::vector<const SceneOptic*> owners;
	for (const SceneOptic& optic : m_optics) {
		if (optic.stack == 0) {
			owners.push_back(&optic);
		}
	}
	std::vector<SceneOptic> stackSensors;
	for (const SensorStack& stack : m_sensorStacks) {
		stackSensors.push_back(stack.sensor);
		stackSensors.back().name = stack.name;
	}
	for (const SceneOptic& sensor : stackSensors) {
		owners.push_back(&sensor);
	}
	for (const SceneOptic* owner : owners) {
		const SceneOptic& optic = *owner;
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

// A laser's beam as a data sheet gives it: the 1/e^2 diameter at the output and the full divergence per axis, which set
// the waist radius and M^2 at the waist position the panel holds (fitLaserAxis()). A pair no Gaussian beam has is refused
static bool drawLaserDataSheet(SceneOptic& optic)
{
	static std::string refused;
	bool changed = false;
	const LaserBeam beam = laserBeam(optic);
	const bool astigmatic = opticBool(optic, "astigmatic");
	ImGui::SeparatorText("As a data sheet gives it");
	for (int axis = 0; axis < (astigmatic ? 2 : 1); axis++) {
		const LaserAxisBeam& a = axis == 0 ? beam.x : beam.y;
		const char* suffix = axis == 0 ? (astigmatic ? " x" : "") : " y";
		double diameter = 2.0 * a.radius(0.0);
		double divergence = 2e3 * a.halfAngle;
		ImGui::PushID(axis);
		bool edited = inputLength(fmt::format("output diameter{}", suffix).c_str(), diameter, 1e-7, 0.1);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("The 1/e^2 intensity diameter at the output aperture");
		}
		edited |= ImGui::InputDouble(fmt::format("full divergence{}", suffix).c_str(), &divergence, 0.0, 0.0, "%.4g mrad", ImGuiInputTextFlags_EnterReturnsTrue);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("The full far-field angle at 1/e^2: twice M^2 lambda / (pi w0)");
		}
		if (edited) {
			const std::optional<LaserAxisFit> fit = fitLaserAxis(diameter, divergence * 1e-3, a.waistPosition, beam.wavelength);
			if (fit) {
				changed |= setOpticValue(optic, axis == 0 ? "waistRadius" : "waistRadiusY", fit->waist);
				changed |= setOpticValue(optic, axis == 0 ? "m2" : "m2Y", fit->m2);
				refused.clear();
			} else {
				refused = fmt::format("No Gaussian beam is {} across with a full divergence of {:.4g} mrad at {:.6g} nm, with its waist {} from the output: M^2 would be below 1",
					formatLength(diameter), divergence, beam.wavelength * 1e9, formatLength(a.waistPosition));
			}
		}
		ImGui::PopID();
	}
	if (!refused.empty()) {
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextColored(ImVec4(1.f, 0.45f, 0.3f, 1.f), "%s", refused.c_str());
		ImGui::PopTextWrapPos();
	}
	return changed;
}

void RaytraceSceneEditor::drawSelectedOptic(VulkanEngine* engine, SceneOptic& optic)
{
	if (optic.stack != 0) {
		drawSelectedStackMember(engine, optic);
		return;
	}
	const OpticTraits& traits = opticTraits(optic.kind);
	ImGui::SeparatorText(optic.name.c_str());
	ImGui::TextDisabled("%s", traits.label);
	ImGui::PushID((int)optic.id);
	const SceneOptic before = optic;
	bool changed = false;

	// the size: a preset (Thorlabs' part at that size) or custom
	const char* sizeLabel = optic.sizePreset >= 0 && optic.sizePreset < (int)traits.sizes.size() ? traits.sizes[(size_t)optic.sizePreset].label : "Custom";
	// a kind without presets (the detectors) is sized by its parameters alone
	if (!traits.sizes.empty() && ImGui::BeginCombo(traits.sizeLabel, sizeLabel)) {
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

	changed |= drawOpticParams(optic);

	if (optic.kind == OpticKind::Laser) {
		changed |= drawLaserDataSheet(optic);
	}
	if (isDetectorKind(optic.kind)) {
		drawDetectorTargetButton(engine, optic);
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

bool RaytraceSceneEditor::drawOpticParams(SceneOptic& optic)
{
	const OpticTraits& traits = opticTraits(optic.kind);
	const bool wasPhysical = findOpticParam(optic.kind, "physicalSensor") != nullptr && opticBool(optic, "physicalSensor");
	bool changed = false;
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
		case OpticParamKind::Wavelength: {
			double nm = opticNumber(optic, param.name) * 1e9;
			if (ImGui::InputDouble(param.label, &nm, 0.0, 0.0, "%.6g nm", ImGuiInputTextFlags_EnterReturnsTrue)) {
				changed |= setOpticValue(optic, param.name, nm * 1e-9);
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
	// turning a physical sensor on records every one of its pixels, until the film is set coarser
	if (findOpticParam(optic.kind, "physicalSensor") != nullptr && opticBool(optic, "physicalSensor") && !wasPhysical) {
		changed |= setOpticValue(optic, "pixelsX", opticNumber(optic, "nativeX"));
		changed |= setOpticValue(optic, "pixelsY", opticNumber(optic, "nativeY"));
	}
	return changed;
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
	const bool snapsFree = opticTraits(optic.kind).snapsFree;
	if (snapsFree) {
		ImGui::TextDisabled("Placed freely: no post or mount yet");
	} else if (ImGui::BeginCombo("mount", mountLabel(optic.mount.kind).c_str())) {
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
		// a laser's output on a hole, its beam along the grid, as a gizmo drag does while the bench snaps
		if (snapsFree && ImGui::SmallButton("Snap to nearest hole")) {
			snapFreeToBench(optic, context);
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
		ImGui::TextDisabled(optic.mount.isFree() && !snapsFree ? "(drag the gizmo in the viewport)" : "(drag in the viewport; Alt inverts snapping)");
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
	if (opticTraits(optic->kind).snapsFree) {
		// a laser as a post is moved: across the table, up and down, and about its vertical, at its output. While the bench
		// snaps (Alt inverts it) the output goes on the nearest hole and the turn steps in quarter turns of the drag's own
		gizmo.beginEditing(
			opticGizmoTarget(id),
			[this, id, engine]() -> std::optional<glm::mat4> {
				const SceneOptic* o = findOptic(id);
				if (o == nullptr) {
					return std::nullopt;
				}
				if (!ImGuizmo::IsUsing()) {
					m_laserDragYaw = std::numeric_limits<double>::quiet_NaN();
				}
				return glm::mat4(o->mount.freeTransform() * glm::translate(glm::dmat4(1.0), opticOrigin(*o, engine->benchContext())));
			},
			[this, id, engine](const glm::mat4& m) {
				SceneOptic* o = findOptic(id);
				if (o == nullptr) {
					return;
				}
				const BenchContext context = engine->benchContext();
				const bool snap = m_bench.snap != ImGui::GetIO().KeyAlt;
				auto yawOf = [](const glm::dvec3& axis) { return std::hypot(axis.x, axis.z) > 1e-9 ? std::atan2(axis.x, axis.z) : 0.0; };
				auto wrap = [](double a) { return std::remainder(a, 2.0 * PI); };
				const double before = yawOf(o->mount.freeOrientation * glm::dvec3(0.0, 0.0, 1.0));
				if (std::isnan(m_laserDragYaw)) {
					m_laserDragYaw = before;
				}
				m_laserDragYaw += wrap(yawOf(glm::dvec3(m[2])) - before);
				const double target = snap ? 0.5 * PI * std::round(m_laserDragYaw / (0.5 * PI)) : m_laserDragYaw;
				o->mount.freeOrientation = glm::angleAxis(wrap(target - before), glm::dvec3(0.0, 1.0, 0.0)) * o->mount.freeOrientation;
				const glm::dvec3 local = opticOrigin(*o, context);
				glm::dvec3 origin(m[3]);
				if (snap) {
					const glm::dvec2 hole = nearestHole(m_bench, glm::dvec2(origin.x, origin.z));
					origin = glm::dvec3(hole.x, origin.y, hole.y);
				}
				o->mount.freePosition = origin - o->mount.freeOrientation * local;
				markChanged();
			},
			ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y | ImGuizmo::TRANSLATE_Z | ImGuizmo::ROTATE_Y, ImGuizmo::WORLD);
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

std::optional<double> RaytraceSceneEditor::standOpticOnBench(VulkanEngine* engine, uint64_t id)
{
	SceneOptic* optic = findOptic(id);
	if (optic == nullptr || optic->mount.isFree()) {
		return std::nullopt;
	}
	const glm::dquat placed = optic->mount.freeOrientation;
	const BenchContext context = engine->benchContext();
	dropOntoBench(*optic, context, otherStacks(id));
	const OpticStack stack = assembleStack(*optic, context);
	if (stack.mount == nullptr) {
		return std::nullopt;
	}
	optic->world = stack.opticToWorld;
	markChanged();
	const glm::dquat stood = glm::normalize(glm::quat_cast(glm::dmat3(stack.opticToWorld)));
	return 2.0 * std::acos(std::clamp(std::abs(glm::dot(placed, stood)), 0.0, 1.0));
}

//---------------------------------------------------------------- sensor stacks

namespace {

// a stack's three gizmos, apart from every object's own id
const void* stackGizmoTarget(uint64_t id, int part)
{
	return reinterpret_cast<const void*>(static_cast<uintptr_t>((1ull << 60) | (id << 2) | (uint64_t)part));
}

// the frame a sensor facing along `z` has with no turn about it: x level, as near horizontal as the axis allows
glm::dvec3 levelX(const glm::dvec3& z)
{
	glm::dvec3 x = glm::cross(glm::dvec3(0.0, 1.0, 0.0), z);
	if (glm::length(x) < 1e-9) {
		x = glm::cross(glm::dvec3(1.0, 0.0, 0.0), z);
	}
	return glm::normalize(x);
}

}

SensorStack* RaytraceSceneEditor::findSensorStack(uint64_t id)
{
	auto found = std::find_if(m_sensorStacks.begin(), m_sensorStacks.end(), [id](const SensorStack& s) { return s.id == id; });
	return found == m_sensorStacks.end() ? nullptr : &(*found);
}

const SensorStack* RaytraceSceneEditor::findSensorStack(uint64_t id) const
{
	return const_cast<RaytraceSceneEditor*>(this)->findSensorStack(id);
}

uint64_t RaytraceSceneEditor::addSensorStack(SensorStack stack)
{
	stack.id = m_nextId++;
	if (stack.name.empty()) {
		stack.name = fmt::format("{}_stack_{}", opticTraits(stack.sensor.kind).name, m_nextStackNumber++);
	}
	stack.count = std::clamp(stack.count, 1, MAX_STACK_SENSORS);
	const size_t slots = opticTraits(stack.sensor.kind).slots.size();
	for (size_t slot = 0; slot < MAX_OPTIC_MATERIAL_SLOTS; slot++) {
		stack.sensor.materials[slot] = slot < slots ? addMaterial(defaultSlotMaterial(stack.sensor, slot)) : 0;
	}
	const uint64_t id = stack.id;
	m_sensorStacks.push_back(std::move(stack));
	resolveSensorStacks();
	markChanged();
	return id;
}

void RaytraceSceneEditor::removeSensorStack(uint64_t id)
{
	const SensorStack* stack = findSensorStack(id);
	if (stack == nullptr) {
		return;
	}
	const std::array<uint64_t, MAX_OPTIC_MATERIAL_SLOTS> owned = stack->sensor.materials;
	std::erase_if(m_sensorStacks, [id](const SensorStack& s) { return s.id == id; });
	std::erase_if(m_optics, [id](const SceneOptic& o) { return o.stack == id; });
	std::erase_if(m_materials, [&owned](const MaterialDef& m) { return m.id != 0 && std::find(owned.begin(), owned.end(), m.id) != owned.end(); });
	if ((m_selectionKind == SelectionKind::SensorStack && m_selectionId == id) || (m_selectionKind == SelectionKind::Optic && findOptic(m_selectionId) == nullptr)) {
		m_selectionKind = SelectionKind::None;
		m_selectionId = 0;
	}
	markChanged();
}

void RaytraceSceneEditor::replaceSensorStacks(std::vector<SensorStack>&& stacks)
{
	while (!m_sensorStacks.empty()) {
		removeSensorStack(m_sensorStacks.back().id);
	}
	m_nextStackNumber = (int)stacks.size() + 1;
	for (SensorStack& stack : stacks) {
		addSensorStack(std::move(stack));
	}
	markChanged();
}

void RaytraceSceneEditor::setSensorStackSlotMaterial(uint64_t stackId, size_t slot, MaterialDef material)
{
	const SensorStack* stack = findSensorStack(stackId);
	if (stack == nullptr || slot >= MAX_OPTIC_MATERIAL_SLOTS) {
		return;
	}
	MaterialDef* current = findMaterial(stack->sensor.materials[slot]);
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

void RaytraceSceneEditor::resolveSensorStacks()
{
	// the members as they were, each keeping its id while its stack keeps its place, so a selection, a render target and
	// a reading follow it through any edit of the stack
	std::map<std::pair<uint64_t, int>, uint64_t> ids;
	std::map<uint64_t, int> counted;
	std::vector<SceneOptic> others;
	std::vector<SceneOptic> before;
	for (SceneOptic& optic : m_optics) {
		if (optic.stack == 0) {
			others.push_back(std::move(optic));
		} else {
			ids[{ optic.stack, counted[optic.stack]++ }] = optic.id;
			before.push_back(std::move(optic));
		}
	}
	std::vector<SceneOptic> members;
	for (SensorStack& stack : m_sensorStacks) {
		stack.count = std::clamp(stack.count, 1, MAX_STACK_SENSORS);
		const glm::dquat orientation = sensorStackOrientation(stack);
		for (int i = 0; i < stack.count; i++) {
			SceneOptic member = stack.sensor;
			const auto found = ids.find({ stack.id, i });
			member.id = found != ids.end() ? found->second : m_nextId++;
			member.stack = stack.id;
			member.name = sensorStackMemberName(stack, i);
			member.sizePreset = -1;
			member.mount = MountPose {};
			member.mount.freePosition = sensorStackPosition(stack, i);
			member.mount.freeOrientation = orientation;
			member.world = member.mount.freeTransform();
			members.push_back(std::move(member));
		}
	}
	const bool changed = members != before;
	m_optics = std::move(others);
	m_optics.insert(m_optics.end(), std::make_move_iterator(members.begin()), std::make_move_iterator(members.end()));
	if (changed) {
		markChanged();
	}
}

void RaytraceSceneEditor::createSensorStack(VulkanEngine* engine, OpticKind kind)
{
	// on the table under the middle of the view at the beam height, running across the view, as a new laser's beam does;
	// from a hole, along the grid, while the bench snaps
	SensorStack stack = makeSensorStack(kind);
	const SceneCamera view = engine->cameraFromViewport();
	const glm::dvec3 eye(view.position);
	const glm::dvec3 forward(view.forward());
	glm::dvec3 ahead = eye + 0.3 * forward;
	if (forward.y < -1e-3) {
		const double t = -eye.y / forward.y;
		if (t > 0.0 && t < 3.0) {
			ahead = eye + t * forward;
		}
	}
	const glm::dvec3 right = glm::dvec3(view.orientation * glm::vec3(1.f, 0.f, 0.f));
	double across = std::hypot(right.x, right.z) > 1e-9 ? std::atan2(right.x, right.z) : 0.0;
	glm::dvec2 start(ahead.x, ahead.z);
	if (m_bench.snap) {
		across = 0.5 * PI * std::round(across / (0.5 * PI));
		start = nearestHole(m_bench, start);
	}
	const double length = glm::length(stack.end - stack.start);
	stack.start = glm::dvec3(start.x, m_bench.beamHeight, start.y);
	stack.end = stack.start + length * glm::dvec3(std::sin(across), 0.0, std::cos(across));
	select(SelectionKind::SensorStack, addSensorStack(std::move(stack)));
	resolveBench(engine);
}

void RaytraceSceneEditor::drawSensorStackRows()
{
	if (m_sensorStacks.empty() || !ImGui::TreeNodeEx("Sensor stacks", ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}
	bool alive = true;
	for (size_t s = 0; s < m_sensorStacks.size() && alive; s++) {
		// the stack's own row, which renames, duplicates and deletes it, beside the arrow that opens its members
		const uint64_t id = m_sensorStacks[s].id;
		const std::string name = m_sensorStacks[s].name;
		ImGui::PushID((int)id);
		const bool open = ImGui::TreeNodeEx("##members", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowOverlap);
		ImGui::SameLine();
		alive = drawObjectRow(SelectionKind::SensorStack, id, name);
		if (open) {
			for (size_t i = 0; alive && i < m_optics.size(); i++) {
				if (m_optics[i].stack != id) {
					continue;
				}
				ImGui::PushID((int)m_optics[i].id);
				if (ImGui::Selectable(m_optics[i].name.c_str(), m_selectionKind == SelectionKind::Optic && m_selectionId == m_optics[i].id)) {
					select(SelectionKind::Optic, m_optics[i].id);
				}
				ImGui::PopID();
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	ImGui::TreePop();
}

void RaytraceSceneEditor::drawSelectedStackMember(VulkanEngine* engine, SceneOptic& member)
{
	ImGui::SeparatorText(member.name.c_str());
	ImGui::TextDisabled("%s, one of a sensor stack", opticTraits(member.kind).label);
	const SensorStack* stack = findSensorStack(member.stack);
	if (stack != nullptr) {
		ImGui::TextDisabled("%s from the stack's start", formatLength(glm::length(member.mount.freePosition - stack->start)).c_str());
		if (ImGui::SmallButton(fmt::format("Edit {}", stack->name).c_str())) {
			select(SelectionKind::SensorStack, stack->id);
			return;
		}
		ImGui::SetItemTooltip("A member is its stack's sensor at one of its places: the stack sets its size, film and placement");
	}
	// the stack is one render target: a member of the target's stack is recorded with it
	const SceneOptic* target = findOptic(engine->m_raytracer.renderDetectorId());
	if (target != nullptr && target->stack == member.stack) {
		ImGui::Spacing();
		ImGui::TextDisabled("Its stack is the render target: light tracing records every member");
	} else {
		drawDetectorTargetButton(engine, member);
	}
}

void RaytraceSceneEditor::drawSelectedSensorStack(VulkanEngine* engine, SensorStack& stack)
{
	ImGui::SeparatorText(stack.name.c_str());
	ImGui::TextDisabled("Sensor stack");
	ImGui::PushID((int)stack.id);
	const SensorStack before = stack;
	bool changed = false;

	// the sensor every member copies: a new kind starts over with its defaults and its slots' default materials
	if (ImGui::BeginCombo("sensor", opticTraits(stack.sensor.kind).label)) {
		for (size_t k = 0; k < OPTIC_KIND_COUNT; k++) {
			const OpticKind kind = (OpticKind)k;
			if (isDetectorKind(kind) && ImGui::Selectable(opticTraits(kind).label, kind == stack.sensor.kind) && kind != stack.sensor.kind) {
				const std::array<uint64_t, MAX_OPTIC_MATERIAL_SLOTS> owned = stack.sensor.materials;
				std::erase_if(m_materials, [&owned](const MaterialDef& m) { return m.id != 0 && std::find(owned.begin(), owned.end(), m.id) != owned.end(); });
				stack.sensor = makeOptic(kind);
				const size_t slots = opticTraits(kind).slots.size();
				for (size_t slot = 0; slot < MAX_OPTIC_MATERIAL_SLOTS; slot++) {
					stack.sensor.materials[slot] = slot < slots ? addMaterial(defaultSlotMaterial(stack.sensor, slot)) : 0;
				}
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	changed |= drawOpticParams(stack.sensor);
	OpticReadout readout;
	buildOptic(stack.sensor, engine->opticBuildContext(), readout);
	for (const auto& [label, meters] : readout.lengths) {
		ImGui::TextDisabled("%s: %s", label.c_str(), formatLength(meters).c_str());
	}
	for (const auto& [label, text] : readout.lines) {
		ImGui::TextDisabled("%s: %s", label.c_str(), text.c_str());
	}

	ImGui::SeparatorText("Along the axis");
	int count = stack.count;
	if (ImGui::InputInt("sensors", &count)) {
		stack.count = std::clamp(count, 1, MAX_STACK_SENSORS);
		changed = true;
	}
	ImGui::SetItemTooltip("Spaced evenly from the start to the end, each the same size: one sensor slid along the axis");
	changed |= inputLength3("start", stack.start);
	ImGui::SetItemTooltip("Type each coordinate and press Enter. The start's and the end's gizmos snap to the breadboard's hole lines while the bench snaps");
	changed |= inputLength3("end", stack.end);
	const double length = glm::length(stack.end - stack.start);
	ImGui::TextDisabled("%s long, %s between sensors", formatLength(length).c_str(), formatLength(stack.count > 1 ? length / (stack.count - 1) : 0.0).c_str());
	const size_t detectors = (size_t)std::count_if(m_optics.begin(), m_optics.end(), [](const SceneOptic& o) { return isDetectorKind(o.kind); });
	if (detectors > MAX_DETECTORS) {
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "The scene has %zu detectors; a render records the first %u", detectors, MAX_DETECTORS);
		ImGui::PopTextWrapPos();
	}

	ImGui::SeparatorText("Rotation");
	TransformGizmo& gizmo = engine->m_transformGizmo;
	bool align = stack.alignToAxis;
	if (ImGui::Checkbox("align normal to stack axis", &align)) {
		// where the sensors face now, so turning it off moves nothing
		stack.orientation = sensorStackOrientation(stack);
		stack.alignToAxis = align;
		if (gizmo.isEditing(stackGizmoTarget(stack.id, 2))) {
			gizmo.endEditing();
		}
		changed = true;
	}
	ImGui::SetItemTooltip("Each sensor faces along the axis from the start to the end, light travelling that way reaching its front; the rotation turns it about the axis only");
	const glm::dvec3 axis = sensorStackAxis(stack);
	if (stack.alignToAxis && axis != glm::dvec3(0.0)) {
		// the turn about the axis, from the sensor whose x is level
		const glm::dvec3 x0 = levelX(axis);
		const glm::dvec3 y0 = glm::cross(axis, x0);
		const glm::dvec3 x = sensorStackOrientation(stack) * glm::dvec3(1.0, 0.0, 0.0);
		double degrees = std::atan2(glm::dot(x, y0), glm::dot(x, x0)) * 180.0 / PI;
		if (ImGui::DragScalar("turn about the axis", ImGuiDataType_Double, &degrees, 0.25f, nullptr, nullptr, "%.3f deg")) {
			const double a = degrees * PI / 180.0;
			const glm::dvec3 turned = std::cos(a) * x0 + std::sin(a) * y0;
			stack.orientation = glm::normalize(glm::quat_cast(glm::dmat3(turned, glm::cross(axis, turned), axis)));
			changed = true;
		}
	} else {
		glm::vec3 euler = glm::degrees(glm::eulerAngles(glm::quat(stack.orientation)));
		if (ImGui::DragFloat3("rotation", &euler.x, 0.5f, 0.f, 0.f, "%.2f deg")) {
			stack.orientation = glm::dquat(glm::quat(glm::radians(euler)));
			changed = true;
		}
	}

	// the whole stack as the render target, through the member the output window shows first
	const auto first = std::find_if(m_optics.begin(), m_optics.end(), [&](const SceneOptic& o) { return o.stack == stack.id; });
	const SceneOptic* target = findOptic(engine->m_raytracer.renderDetectorId());
	if (target != nullptr && target->stack == stack.id) {
		ImGui::TextDisabled("The render target: light tracing records every member");
	} else if (first != m_optics.end() && ImGui::SmallButton("Render to this stack")) {
		engine->m_raytracer.setRenderDetector(first->id);
	}

	// a gizmo each for the start, the end and the rotation, at the stack's middle
	const char* parts[3] = { "Start", "End", "Rotation" };
	for (int part = 0; part < 3; part++) {
		const bool editing = gizmo.isEditing(stackGizmoTarget(stack.id, part));
		if (part > 0) {
			ImGui::SameLine();
		}
		if (ImGui::Button(fmt::format("{} {}", editing ? "Stop" : "Edit", parts[part]).c_str())) {
			if (editing) {
				gizmo.endEditing();
			} else {
				beginSensorStackGizmo(gizmo, stack.id, part);
			}
		}
	}

	// the slots: one material each, which every member shares
	if (!opticTraits(stack.sensor.kind).slots.empty()) {
		ImGui::SeparatorText("Materials");
	}
	for (size_t slot = 0; slot < opticTraits(stack.sensor.kind).slots.size(); slot++) {
		const MaterialDef* material = findMaterial(stack.sensor.materials[slot]);
		if (material == nullptr) {
			continue;
		}
		ImGui::PushID((int)slot);
		ImGui::TextUnformatted(opticTraits(stack.sensor.kind).slots[slot].name);
		ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.3f);
		ImGui::TextDisabled("%s", materialTraits(material->type).label);
		ImGui::SameLine();
		if (ImGui::SmallButton("Edit")) {
			openMaterial(material->id);
		}
		ImGui::PopID();
	}

	if (changed || !(stack == before)) {
		resolveSensorStacks();
		markChanged();
	}
	ImGui::PopID();
}

void RaytraceSceneEditor::beginSensorStackGizmo(TransformGizmo& gizmo, uint64_t id, int part)
{
	const SensorStack* stack = findSensorStack(id);
	if (stack == nullptr) {
		return;
	}
	// the start and the end move in world axes; the rotation turns about the axis alone while the sensors face along it
	const bool aligned = stack->alignToAxis && sensorStackAxis(*stack) != glm::dvec3(0.0);
	gizmo.beginEditing(
		stackGizmoTarget(id, part),
		[this, id, part]() -> std::optional<glm::mat4> {
			const SensorStack* s = findSensorStack(id);
			if (s == nullptr) {
				return std::nullopt;
			}
			glm::dmat4 m = glm::mat4_cast(sensorStackOrientation(*s));
			m[3] = glm::dvec4(part == 0 ? s->start : part == 1 ? s->end : 0.5 * (s->start + s->end), 1.0);
			return glm::mat4(m);
		},
		[this, id, part](const glm::mat4& m) {
			SensorStack* s = findSensorStack(id);
			if (s == nullptr) {
				return;
			}
			if (part < 2) {
				// while the bench snaps (Alt inverts it) x and z fall on the breadboard's hole lines, each only as the drag moves
				// it, so a drag along one line keeps to it; the height is free
				glm::dvec3& point = part == 0 ? s->start : s->end;
				const bool snap = m_bench.snap != ImGui::GetIO().KeyAlt;
				const BenchGrid grid = benchGrid(m_bench);
				const glm::dvec3 moved(m[3]);
				const glm::vec3 was(point);
				for (int axis = 0; axis < 3; axis++) {
					if (was[axis] == m[3][axis]) {
						continue;
					}
					if (snap && axis != 1) {
						const int line = axis == 0 ? 0 : 1;
						point[axis] = grid.first[line] + BREADBOARD_PITCH * std::round((moved[axis] - grid.first[line]) / BREADBOARD_PITCH);
					} else {
						point[axis] = moved[axis];
					}
				}
			} else {
				const glm::mat3 r(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
				s->orientation = glm::dquat(glm::quat_cast(r));
			}
			resolveSensorStacks();
			markChanged();
		},
		part < 2 ? ImGuizmo::TRANSLATE : (aligned ? ImGuizmo::ROTATE_Z : ImGuizmo::ROTATE), part < 2 ? ImGuizmo::WORLD : ImGuizmo::LOCAL);
}
