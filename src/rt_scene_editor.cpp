#include <spectrum_edit.h>
#include <rt_scene_editor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

//a rotation matrix from the gizmo may carry scale (a shape's size) or drift slightly; the
//normalised columns are the orientation
glm::quat orientationOf(const glm::mat4& m)
{
	const glm::mat3 basis(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
	return glm::normalize(glm::quat_cast(basis));
}

//euler angles for the widget only; the orientation itself stays a quaternion. Rebuilding the
//quaternion from the edited angles is exact for the angles the widget shows, so an untouched
//orientation is never disturbed
bool drawOrientation(glm::quat& orientation)
{
	glm::vec3 eulerDegrees = glm::degrees(glm::eulerAngles(orientation));
	if (ImGui::DragFloat3("rotation (pitch, yaw, roll)", &eulerDegrees.x, 0.5f, 0.f, 0.f, "%.1f")) {
		orientation = glm::normalize(glm::quat(glm::radians(eulerDegrees)));
		return true;
	}
	return false;
}

const char* meshMaterialModeName(MeshMaterialMode mode)
{
	return mode == MeshMaterialMode::Model ? "glTF materials" : "override";
}

// what the material combo calls an entry: a glTF material by its model too
std::string materialLabel(const MaterialDef& material);

// whether choosing `candidate` as a child of the mix `target` would close a cycle: a mix chain from the candidate that
// comes back to the target. The shader bounds its walk down a chain, but a cycle has no material to end at
bool mixChainReaches(const std::vector<MaterialDef>& library, uint64_t candidate, uint64_t target, int depth = 0)
{
	if (candidate == target) {
		return true;
	}
	if (depth > 16) {
		return false;
	}
	const auto found = std::find_if(library.begin(), library.end(), [candidate](const MaterialDef& m) { return m.id == candidate; });
	if (found == library.end() || found->type != MaterialType::Mix) {
		return false;
	}
	for (const char* name : { "materialA", "materialB" }) {
		const ParamValue value = materialValue(*found, name);
		if (const MaterialLink* link = std::get_if<MaterialLink>(&value); link != nullptr && link->id != 0 && mixChainReaches(library, link->id, target, depth + 1)) {
			return true;
		}
	}
	return false;
}

// one parameter's widget, by its kind. Returns whether the value changed
bool drawMaterialParam(MaterialDef& material, const MaterialParam& param, const std::vector<MaterialDef>& library)
{
	ParamValue value = materialValue(material, param.name);
	bool changed = false;
	if ((param.flags & PARAM_READ_ONLY) != 0) {
		if (const TexRef* texture = std::get_if<TexRef>(&value)) {
			if (texture->layer >= 0) {
				ImGui::TextDisabled("%s: layer %d", param.label, texture->layer);
			} else {
				ImGui::TextDisabled("%s: none", param.label);
			}
		} else if (const float* f = std::get_if<float>(&value)) {
			ImGui::TextDisabled("%s: %.3f", param.label, *f);
		}
	} else if (float* f = std::get_if<float>(&value)) {
		const ImGuiSliderFlags flags = (param.flags & PARAM_LOGARITHMIC) != 0 ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;
		changed = ImGui::SliderFloat(param.label, f, param.min, param.max, "%.3f", flags);
	} else if (SpectrumRef* spectrum = std::get_if<SpectrumRef>(&value)) {
		changed = spectrumEdit(param.label, *spectrum, SpectrumEditOptions { .role = param.role, .colorFlags = ImGuiColorEditFlags_Float });
	} else if (bool* b = std::get_if<bool>(&value)) {
		changed = ImGui::Checkbox(param.label, b);
	} else if (int* i = std::get_if<int>(&value)) {
		changed = ImGui::InputInt(param.label, i);
	} else if (MaterialLink* link = std::get_if<MaterialLink>(&value)) {
		//a library material that scatters and does not lead back to this mix
		const auto current = std::find_if(library.begin(), library.end(), [link](const MaterialDef& m) { return m.id == link->id; });
		const std::string preview = current != library.end() ? materialLabel(*current) : std::string("(none)");
		if (ImGui::BeginCombo(param.label, preview.c_str())) {
			for (const MaterialDef& candidate : library) {
				if ((materialTraits(candidate.type).flags & MATERIAL_TYPE_SCATTERS) == 0 || mixChainReaches(library, candidate.id, material.id)) {
					continue;
				}
				ImGui::PushID((int)candidate.id);
				if (ImGui::Selectable(materialLabel(candidate).c_str(), candidate.id == link->id) && candidate.id != link->id) {
					link->id = candidate.id;
					changed = true;
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
	}
	if (param.tooltip != nullptr && ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", param.tooltip);
	}
	if (changed) {
		material.params[param.name] = value;
	}
	return changed;
}

// The generic, traits-driven editor: one widget per parameter of the material's type, in the order
// the traits list them. For a glTF material, `imported` is the entry as its file defines it: a
// parameter the user changed is marked, recorded in `edited`, and can be reverted to the import
bool drawMaterialParams(MaterialDef& material, const MaterialDef* imported, const std::vector<MaterialDef>& library)
{
	bool changed = false;
	for (const MaterialParam& param : materialTraits(material.type).params) {
		ImGui::PushID(param.name);
		const bool edited = material.edited.count(param.name) != 0;
		if (imported != nullptr && edited) {
			if (ImGui::SmallButton("revert")) {
				material.params[param.name] = materialValue(*imported, param.name);
				material.edited.erase(material.edited.find(param.name));
				changed = true;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Back to the value the glTF file gives; the next save drops the override");
			}
			ImGui::SameLine();
		}
		if (drawMaterialParam(material, param, library)) {
			changed = true;
			if (imported != nullptr) {
				material.edited.insert(param.name);
			}
		}
		ImGui::PopID();
	}
	return changed;
}

// what the material combo calls an entry: a glTF material by its model too
std::string materialLabel(const MaterialDef& material)
{
	return material.origin.has_value() ? fmt::format("{} / {}", material.origin->modelKey, material.name) : material.name;
}

}

bool drawShapeParams(SceneShape& shape)
{
	const ShapeTraits& traits = shapeTraits(shape.kind);

	bool changed = ImGui::DragFloat3("position", &shape.position.x, 0.01f, 0.f, 0.f, "%.3f");
	if (traits.rotatable) {
		changed |= drawOrientation(shape.orientation);
	}

	//the sibling project used SliderScalar with a fixed 0.001-2 radius / -1-1 position range,
	//which cannot represent its own default scene: the ground sphere is radius 100 at y=-100.5,
	//and a slider clamps a value into range the moment it is touched, so selecting the ground
	//sphere and nudging the slider silently collapsed it. Drag for position (unbounded) and a
	//logarithmic slider for every size keep the same controls without that trap
	for (const ShapeParam& param : traits.params) {
		float value = shapeParamValue(shape, param);
		if (ImGui::SliderFloat(param.name, &value, 0.001f, 200.f, "%.3f", ImGuiSliderFlags_Logarithmic)) {
			setShapeParam(shape, param, value);
			changed = true;
		}
	}
	return changed;
}

bool drawCameraParams(SceneCamera& camera, bool& displayChanged)
{
	bool changed = ImGui::DragFloat3("position", &camera.position.x, 0.01f, 0.f, 0.f, "%.3f");
	changed |= drawOrientation(camera.orientation);

	changed |= ImGui::SliderFloat("vertical fov", &camera.vfovDegrees, 10.f, 120.f, "%.1f");
	changed |= ImGui::SliderFloat("aperture", &camera.aperture, 0.f, 1.f, "%.3f");
	changed |= ImGui::SliderFloat("focus distance", &camera.focusDistance, 0.1f, 50.f, "%.2f", ImGuiSliderFlags_Logarithmic);

	//display only: applied by the tonemap every frame, so a running render need not restart
	displayChanged = ImGui::SliderFloat("exposure", &camera.exposure, 0.f, 4.f);

	return changed;
}

bool drawLightParams(SceneLight& light)
{
	ImGui::TextDisabled("%s", lightKindLabel(light.kind));

	//a directional light has no position that matters to the render: it only places the viewport's
	//arrow. Its direction is all there is
	bool changed = ImGui::DragFloat3("position", &light.position.x, 0.01f, 0.f, 0.f, "%.3f");
	if (light.kind != LightKind::Point) {
		changed |= drawOrientation(light.orientation);
	}

	changed |= spectrumEdit("colour", light.color, SpectrumEditOptions { .role = SpectrumRole::Illuminant });
	//the tracer's radiometric units, on the scale an environment map's radiance is read in
	const char* intensityLabel = light.kind == LightKind::Directional ? "irradiance (W/m^2)" : "intensity (W/sr)";
	changed |= ImGui::SliderFloat(intensityLabel, &light.intensity, 0.f, 100000.f, "%.3f", ImGuiSliderFlags_Logarithmic);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("A glTF light's candela (lux, for a directional one) divided by 683 on import");
	}

	if (light.kind != LightKind::Directional) {
		changed |= ImGui::SliderFloat("range", &light.range, 0.f, 1000.f, light.range > 0.f ? "%.2f" : "none", ImGuiSliderFlags_Logarithmic);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("glTF's window: the light fades smoothly to nothing at this distance. 0 for a plain inverse square");
		}
	}
	if (light.kind == LightKind::Spot) {
		changed |= ImGui::SliderFloat("outer cone", &light.outerConeDegrees, 0.f, 90.f, "%.1f deg");
		changed |= ImGui::SliderFloat("inner cone", &light.innerConeDegrees, 0.f, light.outerConeDegrees, "%.1f deg");
		light.innerConeDegrees = std::min(light.innerConeDegrees, light.outerConeDegrees);
	}
	return changed;
}

bool drawMeshObjectParams(SceneMeshObject& object)
{
	//a mesh object owns a full transform, so the widgets show it the way every editor does -
	//translation, euler rotation, scale. ImGuizmo's own decompose/recompose pair is what the
	//gizmo uses internally, so dragging a field and dragging a handle agree exactly. Recomposed
	//only when something was actually edited: round-tripping every frame would let the matrix
	//drift through the euler representation
	float translation[3];
	float rotation[3];
	float scale[3];
	ImGuizmo::DecomposeMatrixToComponents(&object.transform[0][0], translation, rotation, scale);

	bool transformChanged = ImGui::DragFloat3("position", translation, 0.01f, 0.f, 0.f, "%.3f");
	transformChanged |= ImGui::DragFloat3("rotation", rotation, 0.5f, 0.f, 0.f, "%.1f");
	transformChanged |= ImGui::DragFloat3("scale", scale, 0.01f, 0.f, 0.f, "%.3f");
	if (transformChanged) {
		//a zero scale is a matrix that cannot be decomposed back into anything, so the object
		//could never be dragged out of it again
		for (int axis = 0; axis < 3; axis++) {
			if (std::abs(scale[axis]) < 1e-4f) {
				scale[axis] = scale[axis] < 0.f ? -1e-4f : 1e-4f;
			}
		}
		ImGuizmo::RecomposeMatrixFromComponents(translation, rotation, scale, &object.transform[0][0]);
	}

	bool changed = transformChanged;
	changed |= ImGui::Checkbox("visible", &object.visible);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Hidden objects are drawn by neither the viewport nor the raytracer");
	}

	return changed;
}

RaytraceSceneEditor::RaytraceSceneEditor()
{
	m_nextShapeNumber.fill(1);

	//an empty scene still has somewhere to render from. Everything else arrives from the file the
	//engine opens at startup, or from File > New Scene
	SceneCamera camera;
	camera.name = "render_camera";
	addCamera(std::move(camera));
}

//---------------------------------------------------------------- object storage

void RaytraceSceneEditor::addShape(SceneShape shape)
{
	shape.id = m_nextId++;
	m_shapes.push_back(std::move(shape));
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

void RaytraceSceneEditor::addLight(SceneLight light)
{
	light.id = m_nextId++;
	if (light.name.empty()) {
		light.name = fmt::format("{}_{}", lightKindName(light.kind), m_nextLightNumber);
	}
	m_nextLightNumber++;
	m_lights.push_back(std::move(light));
}

void RaytraceSceneEditor::addLights(std::vector<SceneLight>&& lights)
{
	if (lights.empty()) {
		return;
	}
	for (SceneLight& light : lights) {
		addLight(std::move(light));
	}
	markChanged();
}

void RaytraceSceneEditor::removeLightsOf(const std::string& modelKey)
{
	const size_t before = m_lights.size();
	std::erase_if(m_lights, [&modelKey](const SceneLight& light) { return !light.modelKey.empty() && light.modelKey == modelKey; });
	if (m_lights.size() != before) {
		markChanged();
	}
}

void RaytraceSceneEditor::addShapes(std::vector<SceneShape>&& shapes)
{
	if (shapes.empty()) {
		return;
	}
	for (SceneShape& shape : shapes) {
		addShape(std::move(shape));
	}
	markChanged();
}

void RaytraceSceneEditor::removeShapesOf(const std::string& modelKey)
{
	const size_t before = m_shapes.size();
	std::erase_if(m_shapes, [&modelKey](const SceneShape& shape) { return !shape.modelKey.empty() && shape.modelKey == modelKey; });
	if (m_shapes.size() != before) {
		markChanged();
	}
}

void RaytraceSceneEditor::addMeshObjects(std::vector<SceneMeshObject>&& objects)
{
	if (objects.empty()) {
		return;
	}
	for (SceneMeshObject& object : objects) {
		object.id = m_nextId++;
		m_meshObjects.push_back(std::move(object));
	}
	markChanged();
}

void RaytraceSceneEditor::removeMeshObjectsOf(const std::string& modelKey)
{
	const size_t before = m_meshObjects.size();
	std::erase_if(m_meshObjects, [&modelKey](const SceneMeshObject& object) { return object.modelKey == modelKey; });
	if (m_meshObjects.size() != before) {
		//the selection may have been one of them; it is looked up by id, so a stale one resolves
		//to nothing and the details panel simply shows nothing
		markChanged();
	}
}

SceneShape* RaytraceSceneEditor::findShape(uint64_t id)
{
	auto found = std::find_if(m_shapes.begin(), m_shapes.end(), [id](const SceneShape& s) { return s.id == id; });
	return found == m_shapes.end() ? nullptr : &(*found);
}

const SceneShape* RaytraceSceneEditor::findShape(uint64_t id) const
{
	auto found = std::find_if(m_shapes.begin(), m_shapes.end(), [id](const SceneShape& s) { return s.id == id; });
	return found == m_shapes.end() ? nullptr : &(*found);
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

SceneLight* RaytraceSceneEditor::findLight(uint64_t id)
{
	auto found = std::find_if(m_lights.begin(), m_lights.end(), [id](const SceneLight& l) { return l.id == id; });
	return found == m_lights.end() ? nullptr : &(*found);
}

const SceneLight* RaytraceSceneEditor::findLight(uint64_t id) const
{
	auto found = std::find_if(m_lights.begin(), m_lights.end(), [id](const SceneLight& l) { return l.id == id; });
	return found == m_lights.end() ? nullptr : &(*found);
}

SceneMeshObject* RaytraceSceneEditor::findMeshObject(uint64_t id)
{
	auto found = std::find_if(m_meshObjects.begin(), m_meshObjects.end(), [id](const SceneMeshObject& o) { return o.id == id; });
	return found == m_meshObjects.end() ? nullptr : &(*found);
}

const SceneMeshObject* RaytraceSceneEditor::findMeshObject(uint64_t id) const
{
	auto found = std::find_if(m_meshObjects.begin(), m_meshObjects.end(), [id](const SceneMeshObject& o) { return o.id == id; });
	return found == m_meshObjects.end() ? nullptr : &(*found);
}

MaterialDef* RaytraceSceneEditor::findMaterial(uint64_t id)
{
	auto found = std::find_if(m_materials.begin(), m_materials.end(), [id](const MaterialDef& m) { return m.id == id; });
	return found == m_materials.end() ? nullptr : &(*found);
}

const MaterialDef* RaytraceSceneEditor::findMaterial(uint64_t id) const
{
	auto found = std::find_if(m_materials.begin(), m_materials.end(), [id](const MaterialDef& m) { return m.id == id; });
	return found == m_materials.end() ? nullptr : &(*found);
}

MaterialDef* RaytraceSceneEditor::findModelMaterial(std::string_view modelKey, uint32_t index)
{
	auto found = std::find_if(m_materials.begin(), m_materials.end(), [&](const MaterialDef& m) {
		return m.origin.has_value() && m.origin->index == index && m.origin->modelKey == modelKey;
	});
	return found == m_materials.end() ? nullptr : &(*found);
}

const MaterialDef* RaytraceSceneEditor::findModelMaterial(std::string_view modelKey, uint32_t index) const
{
	return const_cast<RaytraceSceneEditor*>(this)->findModelMaterial(modelKey, index);
}

uint64_t RaytraceSceneEditor::addMaterial(MaterialDef material)
{
	material.id = m_nextId++;
	if (material.name.empty()) {
		material.name = fmt::format("material_{}", material.id);
	}
	const uint64_t id = material.id;
	m_materials.push_back(std::move(material));
	markChanged();
	return id;
}

uint64_t RaytraceSceneEditor::addMaterialFor(const std::string& ownerName)
{
	MaterialDef material = makeMaterial(MaterialType::Lambertian);
	material.name = ownerName;
	return addMaterial(std::move(material));
}

MaterialDef RaytraceSceneEditor::userCopyOf(const MaterialDef& source, std::string name) const
{
	MaterialDef copy = source;
	copy.id = 0;
	copy.name = std::move(name);
	copy.origin.reset();
	copy.edited.clear();
	//a texture layer means something only for as long as the array is not rebuilt, and nothing in a
	//scene file: a user material keeps none until texture references exist (pbrt-textures.md). Every type's,
	//since a model's material carries its file's textures whatever its type, and the copy may change type
	for (const MaterialType type : MATERIAL_TYPES) {
		for (const MaterialParam& param : materialTraits(type).params) {
			if ((param.flags & PARAM_READ_ONLY) != 0) {
				copy.params.erase(param.name);
			}
		}
	}
	return copy;
}

std::vector<std::string> RaytraceSceneEditor::removeMaterialsOf(const std::string& modelKey)
{
	std::vector<std::string> converted;
	for (MaterialDef& material : m_materials) {
		if (!material.origin.has_value() || material.origin->modelKey != modelKey || materialUsers(material.id) == 0) {
			continue;
		}
		//still referenced by a shape or an override: it stays, as the user's own
		MaterialDef copy = userCopyOf(material, material.name);
		copy.id = material.id;
		material = std::move(copy);
		converted.push_back(material.name);
	}
	const size_t before = m_materials.size();
	std::erase_if(m_materials, [&modelKey](const MaterialDef& m) { return m.origin.has_value() && m.origin->modelKey == modelKey; });
	if (m_materials.size() != before || !converted.empty()) {
		markChanged();
	}
	return converted;
}

void RaytraceSceneEditor::refreshModelMaterials(const std::function<bool(MaterialDef&)>& refresh)
{
	bool changed = false;
	for (MaterialDef& material : m_materials) {
		if (material.origin.has_value()) {
			changed |= refresh(material);
		}
	}
	if (changed) {
		markChanged();
	}
}

size_t RaytraceSceneEditor::materialUsers(uint64_t id) const
{
	size_t users = (size_t)std::count_if(m_shapes.begin(), m_shapes.end(), [id](const SceneShape& s) { return s.materialId == id; });
	users += (size_t)std::count_if(m_meshObjects.begin(), m_meshObjects.end(), [id](const SceneMeshObject& o) {
		return o.materialMode == MeshMaterialMode::Override && o.materialId == id;
	});
	return users;
}

void RaytraceSceneEditor::openMaterial(uint64_t id)
{
	m_selectedMaterialId = id;
	m_showMaterials = true;
	ImGui::SetWindowFocus("Materials");
}

void RaytraceSceneEditor::replaceShapes(std::vector<SceneShape>&& shapes)
{
	//the old shapes' ids die here. If the gizmo was editing one of them its lookup fails and it
	//stops on its own next frame
	m_shapes.clear();
	m_nextShapeNumber.fill(1);
	for (SceneShape& shape : shapes) {
		m_nextShapeNumber[(size_t)shape.kind]++;
		addShape(std::move(shape));
	}
	m_selectionKind = SelectionKind::None;
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

void RaytraceSceneEditor::replaceLights(std::vector<SceneLight>&& lights)
{
	m_lights.clear();
	m_nextLightNumber = 1;
	for (SceneLight& light : lights) {
		addLight(std::move(light));
	}
	m_selectionKind = SelectionKind::None;
	markChanged();
}

void RaytraceSceneEditor::replaceMeshObjects(std::vector<SceneMeshObject>&& objects)
{
	m_meshObjects.clear();
	for (SceneMeshObject& object : objects) {
		object.id = m_nextId++;
		m_meshObjects.push_back(std::move(object));
	}
	m_selectionKind = SelectionKind::None;
	markChanged();
}

void RaytraceSceneEditor::setLightGroups(std::vector<std::string> names, uint32_t environmentGroup)
{
	if (names.empty()) {
		names.push_back("Default");
	}
	if (names.size() > MAX_LIGHT_GROUPS) {
		names.resize(MAX_LIGHT_GROUPS);
	}
	m_lightGroupNames = std::move(names);
	const uint32_t last = (uint32_t)m_lightGroupNames.size() - 1u;
	m_environmentGroup = std::min(environmentGroup, last);
	for (SceneLight& light : m_lights) {
		light.lightGroup = std::min(light.lightGroup, last);
	}
	for (MaterialDef& material : m_materials) {
		material.lightGroup = std::min(material.lightGroup, last);
	}
	markChanged();
}

bool RaytraceSceneEditor::drawLightGroupCombo(const char* label, uint32_t& group) const
{
	const uint32_t shown = std::min(group, (uint32_t)m_lightGroupNames.size() - 1u);
	bool changed = false;
	if (ImGui::BeginCombo(label, m_lightGroupNames[shown].c_str())) {
		for (uint32_t g = 0; g < (uint32_t)m_lightGroupNames.size(); g++) {
			ImGui::PushID((int)g);
			if (ImGui::Selectable(m_lightGroupNames[g].c_str(), g == shown) && g != group) {
				group = g;
				changed = true;
			}
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	return changed;
}

void RaytraceSceneEditor::drawLightGroupEditor()
{
	bool changed = false;
	std::optional<uint32_t> removed;
	for (uint32_t g = 0; g < (uint32_t)m_lightGroupNames.size(); g++) {
		ImGui::PushID((int)g);
		char name[64];
		std::snprintf(name, sizeof(name), "%s", m_lightGroupNames[g].c_str());
		ImGui::SetNextItemWidth(ImGui::CalcItemWidth() * 0.7f);
		//a name is not part of what renders, so renaming restarts nothing
		if (ImGui::InputText("##name", name, sizeof(name)) && name[0] != '\0') {
			m_lightGroupNames[g] = name;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(m_lightGroupNames.size() <= 1);
		if (ImGui::SmallButton("Remove")) {
			removed = g;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("group %u", g);
		ImGui::PopID();
	}
	if (removed.has_value()) {
		//everything in the group moves to group 0, and the groups above it move down one
		const uint32_t gone = *removed;
		auto shift = [gone](uint32_t& group) {
			if (group == gone) {
				group = 0;
			} else if (group > gone) {
				group--;
			}
		};
		for (SceneLight& light : m_lights) {
			shift(light.lightGroup);
		}
		for (MaterialDef& material : m_materials) {
			shift(material.lightGroup);
		}
		shift(m_environmentGroup);
		m_lightGroupNames.erase(m_lightGroupNames.begin() + gone);
		changed = true;
	}
	ImGui::BeginDisabled(m_lightGroupNames.size() >= MAX_LIGHT_GROUPS);
	if (ImGui::Button("Add group")) {
		m_lightGroupNames.push_back(fmt::format("Group {}", m_lightGroupNames.size()));
		changed = true;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextDisabled("%zu of %u", m_lightGroupNames.size(), MAX_LIGHT_GROUPS);

	changed |= drawLightGroupCombo("Background", m_environmentGroup);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The group the environment map, the sky gradient or the solid background colour belongs to.\nA light is put in a group in the Scene panel, and an emissive material in the Materials window.");
	}
	if (changed) {
		markChanged();
	}
}

void RaytraceSceneEditor::replaceMaterials(std::vector<MaterialDef>&& materials)
{
	m_materials.clear();
	for (MaterialDef& material : materials) {
		material.id = m_nextId++;
		m_materials.push_back(std::move(material));
	}
	m_selectedMaterialId = 0;
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
	markCameraChanged();
}

//---------------------------------------------------------------- creating and removing

void RaytraceSceneEditor::select(SelectionKind kind, uint64_t id)
{
	m_selectionKind = kind;
	m_selectionId = id;
	//a rename in progress belongs to the row that started it
	m_renamingId = 0;
}

void RaytraceSceneEditor::createShape(ShapeKind kind)
{
	SceneShape shape = makeShape(kind);
	shape.name = fmt::format("{}_{}", shapeTraits(kind).name, m_nextShapeNumber[(size_t)kind]++);
	//its own material, as every shape had before materials were a library: editing it touches
	//nothing else until the user picks a shared one
	shape.materialId = addMaterialFor(shape.name);
	addShape(std::move(shape));
	markChanged();
	select(SelectionKind::Shape, m_shapes.back().id);
}

void RaytraceSceneEditor::createCamera(VulkanEngine* engine)
{
	//where the viewport is looking right now, so the new camera frames what the user sees
	const uint64_t id = addCamera(engine->cameraFromViewport());
	select(SelectionKind::Camera, id);
}

void RaytraceSceneEditor::createLight(VulkanEngine* engine, LightKind kind)
{
	//at the viewport's pose, like a new camera: a spot light then shines where the user is looking,
	//and a directional one travels that way
	const SceneCamera view = engine->cameraFromViewport();
	SceneLight light;
	light.kind = kind;
	light.position = view.position;
	light.orientation = view.orientation;
	light.intensity = kind == LightKind::Directional ? 3.f : 10.f;
	addLight(std::move(light));
	markChanged();
	select(SelectionKind::Light, m_lights.back().id);
}

void RaytraceSceneEditor::duplicateSelection()
{
	switch (m_selectionKind) {
	case SelectionKind::Shape: {
		const SceneShape* source = findShape(m_selectionId);
		if (source == nullptr) {
			return;
		}
		SceneShape copy = *source;
		copy.name = fmt::format("{}_copy", source->name);
		//the copy gets a copy of the material too, so editing one never changes the other
		if (const MaterialDef* material = findMaterial(source->materialId)) {
			copy.materialId = addMaterial(userCopyOf(*material, fmt::format("{}_copy", material->name)));
		}
		addShape(std::move(copy));
		markChanged();
		select(SelectionKind::Shape, m_shapes.back().id);
		break;
	}
	case SelectionKind::MeshObject: {
		//the copy places the same model node a second time - the geometry is shared, only the
		//placement is duplicated, which is what makes one imported file usable as many objects
		const SceneMeshObject* source = findMeshObject(m_selectionId);
		if (source == nullptr) {
			return;
		}
		SceneMeshObject copy = *source;
		copy.name = fmt::format("{}_copy", source->name);
		if (const MaterialDef* material = findMaterial(source->materialId); material != nullptr && copy.materialMode == MeshMaterialMode::Override) {
			copy.materialId = addMaterial(userCopyOf(*material, fmt::format("{}_copy", material->name)));
		}
		std::vector<SceneMeshObject> one { std::move(copy) };
		addMeshObjects(std::move(one));
		select(SelectionKind::MeshObject, m_meshObjects.back().id);
		break;
	}
	case SelectionKind::Camera: {
		const SceneCamera* source = findCamera(m_selectionId);
		if (source == nullptr) {
			return;
		}
		SceneCamera copy = *source;
		copy.name = fmt::format("{}_copy", source->name);
		select(SelectionKind::Camera, addCamera(std::move(copy)));
		break;
	}
	case SelectionKind::Light: {
		const SceneLight* source = findLight(m_selectionId);
		if (source == nullptr) {
			return;
		}
		SceneLight copy = *source;
		copy.name = fmt::format("{}_copy", source->name);
		//a copy is the user's own light, not the model's: removing the model leaves it
		copy.modelKey.clear();
		addLight(std::move(copy));
		markChanged();
		select(SelectionKind::Light, m_lights.back().id);
		break;
	}
	case SelectionKind::None:
		break;
	}
}

void RaytraceSceneEditor::deleteSelection()
{
	//a gizmo or first-person edit on this object fails its id lookup next frame and ends itself
	switch (m_selectionKind) {
	case SelectionKind::Shape:
		std::erase_if(m_shapes, [this](const SceneShape& s) { return s.id == m_selectionId; });
		break;
	case SelectionKind::Camera:
		std::erase_if(m_cameras, [this](const SceneCamera& c) { return c.id == m_selectionId; });
		break;
	case SelectionKind::Light:
		std::erase_if(m_lights, [this](const SceneLight& l) { return l.id == m_selectionId; });
		break;
	case SelectionKind::MeshObject:
		//the model stays loaded: another object may place the same node, and the model row's
		//"Restore objects" can put this one back
		std::erase_if(m_meshObjects, [this](const SceneMeshObject& o) { return o.id == m_selectionId; });
		break;
	case SelectionKind::None:
		return;
	}

	m_selectionKind = SelectionKind::None;
	m_selectionId = 0;
	m_renamingId = 0;
	markChanged();
}

//---------------------------------------------------------------- the panel

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

	//what a ray that leaves the scene sees in a render. The environment map itself is set from the
	//File menu and shown by the viewport's "environment" background effect
	if (ImGui::CollapsingHeader("Background")) {
		ImGui::Checkbox("solid colour", &engine->m_solidBackground);
		if (engine->m_solidBackground) {
			ImGui::ColorEdit3("colour", &engine->m_backgroundColor.x);
		} else {
			ImGui::TextDisabled(engine->m_environmentMap.image != VK_NULL_HANDLE ? "environment map" : "sky gradient");
		}
	}

	ImGui::Spacing();
	drawToolbar(engine);
	drawObjectTree(engine);
	drawSelection(engine);

	//the outcome of the last File-menu action, kept until the next one replaces it
	ImGui::Spacing();
	drawIoResultLine(engine->m_lastFileResult);

	ImGui::End();
}

void RaytraceSceneEditor::drawToolbar(VulkanEngine* engine)
{
	//one add button with a typed menu behind it, the way every scene editor does it, rather than
	//one button per object kind growing along the toolbar
	if (ImGui::Button("+ Add")) {
		ImGui::OpenPopup("##add");
	}

	if (ImGui::BeginPopup("##add")) {
		//the same three groups as the tree below
		if (ImGui::BeginMenu("Geometry")) {
			for (const ShapeKind kind : SHAPE_KINDS) {
				if (ImGui::MenuItem(shapeTraits(kind).label)) {
					createShape(kind);
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Camera")) {
			createCamera(engine);
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("Placed at the viewport's current pose");
		}
		if (ImGui::BeginMenu("Light")) {
			for (const LightKind kind : LIGHT_KINDS) {
				if (ImGui::MenuItem(lightKindLabel(kind))) {
					createLight(engine, kind);
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("Placed at the viewport's current pose, shining the way it looks.\nFor a light with a surface, give a shape an emissive material");
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Mesh from glTF file...")) {
			//the native dialog runs a modal loop, so it cannot open from inside the imgui frame;
			//the engine runs it once this frame's ui is finished, exactly as the File menu does
			engine->m_pendingFileAction = VulkanEngine::FileAction::ImportGltf;
		}
		ImGui::EndPopup();
	}

	const bool hasSelection = m_selectionKind != SelectionKind::None;
	ImGui::SameLine();
	ImGui::BeginDisabled(!hasSelection);
	if (ImGui::Button("Duplicate")) {
		duplicateSelection();
	}
	ImGui::SameLine();
	if (ImGui::Button("Delete")) {
		deleteSelection();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Right-click an object to rename, duplicate or delete it.\nDel deletes the selection.");
	}
}

void RaytraceSceneEditor::drawObjectTree(VulkanEngine* engine)
{
	//a bordered scrolling region, so a scene with a hundred mesh nodes does not push the object's
	//own parameters off the bottom of the panel
	const float height = 12 * ImGui::GetTextLineHeightWithSpacing();
	if (ImGui::BeginChild("##objects", ImVec2(0, height), ImGuiChildFlags_Borders)) {
		drawCameraRows();
		drawLightRows();
		drawGeometryRows();
		drawModelRows(engine);
	}
	ImGui::EndChild();

	//Del anywhere in the panel, as long as no text field is taking keys
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
		deleteSelection();
	}
}

bool RaytraceSceneEditor::drawObjectRow(SelectionKind kind, uint64_t id, const std::string& name)
{
	bool alive = true;
	ImGui::PushID((int)id);

	if (m_renamingId == id) {
		//the row becomes its own text field until the edit is committed or abandoned
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (!ImGui::IsAnyItemActive()) {
			ImGui::SetKeyboardFocusHere();
		}
		const bool entered = ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		//Enter, or clicking away with the text changed, commits; Escape leaves imgui's own buffer
		//reverted and only deactivates, so it cancels
		if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
			std::string* target = nullptr;
			switch (kind) {
			case SelectionKind::Camera:
				if (SceneCamera* camera = findCamera(id)) {
					target = &camera->name;
				}
				break;
			case SelectionKind::Shape:
				if (SceneShape* shape = findShape(id)) {
					target = &shape->name;
				}
				break;
			case SelectionKind::Light:
				if (SceneLight* light = findLight(id)) {
					target = &light->name;
				}
				break;
			case SelectionKind::MeshObject:
				if (SceneMeshObject* object = findMeshObject(id)) {
					target = &object->name;
				}
				break;
			case SelectionKind::None:
				break;
			}
			if (target != nullptr && m_renameBuffer[0] != '\0') {
				*target = m_renameBuffer;
				markChanged();
			}
		}
		if (entered || ImGui::IsItemDeactivated()) {
			m_renamingId = 0;
		}
		ImGui::PopID();
		return alive;
	}

	const bool selected = m_selectionKind == kind && m_selectionId == id;
	if (ImGui::Selectable(name.c_str(), selected)) {
		select(kind, id);
	}

	if (ImGui::BeginPopupContextItem("##context")) {
		//right-clicking a row acts on that row, not on whatever was selected before
		if (!(m_selectionKind == kind && m_selectionId == id)) {
			select(kind, id);
		}
		if (ImGui::MenuItem("Rename")) {
			std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", name.c_str());
			m_renamingId = id;
		}
		if (ImGui::MenuItem("Duplicate")) {
			duplicateSelection();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Delete", "Del")) {
			deleteSelection();
			alive = false;
		}
		ImGui::EndPopup();
	}

	ImGui::PopID();
	return alive;
}

void RaytraceSceneEditor::drawCameraRows()
{
	if (!ImGui::TreeNodeEx("Cameras", ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}
	if (m_cameras.empty()) {
		ImGui::TextDisabled("none");
	}
	for (size_t i = 0; i < m_cameras.size(); i++) {
		//a deleting row invalidates the vector, so the walk stops there and picks up next frame
		if (!drawObjectRow(SelectionKind::Camera, m_cameras[i].id, m_cameras[i].name)) {
			break;
		}
	}
	ImGui::TreePop();
}

void RaytraceSceneEditor::drawLightRows()
{
	//only shown once there is a light: most scenes are lit by emissive shapes and the background,
	//and an empty folder would suggest those are missing
	if (m_lights.empty()) {
		return;
	}
	if (!ImGui::TreeNodeEx("Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}
	for (size_t i = 0; i < m_lights.size(); i++) {
		//a deleting row invalidates the vector, so the walk stops there and picks up next frame
		if (!drawObjectRow(SelectionKind::Light, m_lights[i].id, m_lights[i].name)) {
			break;
		}
	}
	ImGui::TreePop();
}

void RaytraceSceneEditor::drawGeometryRows()
{
	if (!ImGui::TreeNodeEx("Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}
	if (m_shapes.empty()) {
		ImGui::TextDisabled("none - Add > Geometry");
	}

	//a folder per kind the scene has any of, in the kinds' own order; within one, creation order
	bool alive = true;
	for (const ShapeKind kind : SHAPE_KINDS) {
		const auto ofKind = [kind](const SceneShape& shape) { return shape.kind == kind; };
		if (!alive || std::none_of(m_shapes.begin(), m_shapes.end(), ofKind)) {
			continue;
		}
		if (!ImGui::TreeNodeEx(shapeTraits(kind).plural, ImGuiTreeNodeFlags_DefaultOpen)) {
			continue;
		}
		for (size_t i = 0; i < m_shapes.size() && alive; i++) {
			//a deleting row invalidates the vector, so the walk stops there and picks up next frame
			if (m_shapes[i].kind == kind) {
				alive = drawObjectRow(SelectionKind::Shape, m_shapes[i].id, m_shapes[i].name);
			}
		}
		ImGui::TreePop();
	}

	ImGui::TreePop();
}

void RaytraceSceneEditor::drawModelRows(VulkanEngine* engine)
{
	if (!ImGui::TreeNodeEx("Models", ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}

	if (engine->m_models.empty()) {
		ImGui::TextDisabled("none - Add > Mesh from glTF file...");
		ImGui::TreePop();
		return;
	}

	//a model removed mid-iteration would invalidate the map, so the action is deferred
	std::string toRemove;
	std::string toRestore;

	for (const auto& [key, model] : engine->m_models) {
		ImGui::PushID(key.c_str());
		const bool open = ImGui::TreeNodeEx(key.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
		if (ImGui::IsItemHovered() && model != nullptr) {
			ImGui::SetTooltip("%s", model->sourcePath.string().c_str());
		}

		if (ImGui::BeginPopupContextItem("##model")) {
			if (ImGui::MenuItem("Restore objects")) {
				//puts back the nodes of this model that have no object placing them - the way out
				//of having deleted one and wanted it back
				toRestore = key;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Remove model")) {
				toRemove = key;
			}
			ImGui::EndPopup();
		}

		if (open) {
			//each model's objects by index, found once per edit rather than every frame
			if (m_modelRowsRevision != m_contentRevision) {
				m_modelRows.clear();
				for (size_t i = 0; i < m_meshObjects.size(); i++) {
					m_modelRows[m_meshObjects[i].modelKey].push_back(i);
				}
				m_modelRowsRevision = m_contentRevision;
			}
			static const std::vector<size_t> none;
			const auto found = m_modelRows.find(key);
			const std::vector<size_t>& rows = found != m_modelRows.end() ? found->second : none;
			if (rows.empty()) {
				ImGui::TextDisabled("no objects - right-click to restore");
			}
			//only the rows in view are built: a pbrt scene's model places tens of thousands of objects
			ImGuiListClipper clipper;
			clipper.Begin((int)rows.size());
			bool alive = true;
			while (alive && clipper.Step()) {
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
					const SceneMeshObject& object = m_meshObjects[rows[(size_t)row]];
					//a hidden object still lists, just greyed, so it can be found and shown again
					const bool visible = object.visible;
					if (!visible) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					}
					alive = drawObjectRow(SelectionKind::MeshObject, object.id, object.name);
					if (!visible) {
						ImGui::PopStyleColor();
					}
					if (!alive) {
						break;
					}
				}
			}
			clipper.End();
			ImGui::TreePop();
		}

		ImGui::PopID();
	}

	ImGui::TreePop();

	if (!toRestore.empty() && engine->m_raytraceMeshData != nullptr) {
		std::vector<SceneMeshObject> missing;
		for (SceneMeshObject& candidate : defaultMeshObjects(*engine->m_raytraceMeshData, toRestore)) {
			const bool placed = std::any_of(m_meshObjects.begin(), m_meshObjects.end(), [&](const SceneMeshObject& existing) {
				return existing.modelKey == candidate.modelKey && existing.nodeIndex == candidate.nodeIndex;
			});
			if (!placed) {
				missing.push_back(std::move(candidate));
			}
		}
		addMeshObjects(std::move(missing));
	}

	if (!toRemove.empty()) {
		engine->removeModel(toRemove);
	}
}

void RaytraceSceneEditor::drawSelection(VulkanEngine* engine)
{
	switch (m_selectionKind) {
	case SelectionKind::Shape:
		if (SceneShape* shape = findShape(m_selectionId)) {
			drawSelectedShape(engine, *shape);
		}
		break;
	case SelectionKind::Camera:
		if (SceneCamera* camera = findCamera(m_selectionId)) {
			drawSelectedCamera(engine, *camera);
		}
		break;
	case SelectionKind::Light:
		if (SceneLight* light = findLight(m_selectionId)) {
			drawSelectedLight(engine, *light);
		}
		break;
	case SelectionKind::MeshObject:
		if (SceneMeshObject* object = findMeshObject(m_selectionId)) {
			drawSelectedMeshObject(engine, *object);
		}
		break;
	case SelectionKind::None:
		break;
	}
}

void RaytraceSceneEditor::drawSelectedShape(VulkanEngine* engine, SceneShape& shape)
{
	ImGui::SeparatorText(shape.name.c_str());

	//scoping the widget ids to the selected object stops an in-progress drag, or any other
	//per-widget state imgui keys by id, from carrying across a selection change
	ImGui::PushID((int)shape.id);

	bool changed = drawShapeParams(shape);

	//the gizmo sits alongside the sliders, not instead of them: sliders for exact numbers, the
	//gizmo for direct manipulation. Opt-in per object, and only one object at a time
	TransformGizmo& gizmo = engine->m_transformGizmo;
	const bool editing = gizmo.isEditing(gizmoTargetId(shape.id));
	if (ImGui::Button(editing ? "Stop Editing Transform" : "Edit Transform")) {
		if (editing) {
			gizmo.endEditing();
		} else {
			beginShapeGizmo(gizmo, shape.id);
		}
	}
	if (editing) {
		ImGui::SameLine();
		ImGui::TextDisabled("(drag the gizmo in the viewport)");
	}

	ImGui::Spacing();

	//a reference into the library: what the material is, is edited in the Materials window
	changed |= drawMaterialReference(shape.materialId, shape.name);
	ImGui::TextDisabled("on a shape, a material's textures are left out");

	if (changed) {
		markChanged();
	}

	ImGui::PopID();
}

void RaytraceSceneEditor::drawSelectedCamera(VulkanEngine* engine, SceneCamera& camera)
{
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
		markCameraChanged();
	}

	ImGui::PopID();
}

void RaytraceSceneEditor::drawSelectedLight(VulkanEngine* engine, SceneLight& light)
{
	ImGui::SeparatorText(light.name.c_str());
	ImGui::PushID((int)light.id);

	if (!light.modelKey.empty()) {
		ImGui::TextDisabled("imported with %s", light.modelKey.c_str());
	}
	bool changed = drawLightParams(light);
	changed |= drawLightGroupCombo("light group", light.lightGroup);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The group this light's contribution is kept in when a render keeps light groups: Raytrace Render > Outputs.");
	}

	TransformGizmo& gizmo = engine->m_transformGizmo;
	const bool editing = gizmo.isEditing(gizmoTargetId(light.id));
	if (ImGui::Button(editing ? "Stop Editing Transform" : "Edit Transform")) {
		if (editing) {
			gizmo.endEditing();
		} else {
			beginLightGizmo(gizmo, light.id);
		}
	}
	if (editing) {
		ImGui::SameLine();
		ImGui::TextDisabled("(drag the gizmo in the viewport)");
	}

	if (changed) {
		markChanged();
	}

	ImGui::PopID();
}

void RaytraceSceneEditor::drawSelectedMeshObject(VulkanEngine* engine, SceneMeshObject& object)
{
	ImGui::SeparatorText(object.name.c_str());
	ImGui::PushID((int)object.id);

	//which geometry this object places, and how much of it there is to trace
	if (engine->m_raytraceMeshData != nullptr) {
		if (const RTMeshNode* node = engine->m_raytraceMeshData->findNode(object.modelKey, object.nodeIndex)) {
			ImGui::TextDisabled("%s / %s - %zu triangle(s)", object.modelKey.c_str(), node->name.c_str(), node->triangleCount);
		} else {
			ImGui::TextDisabled("%s - geometry missing", object.modelKey.c_str());
		}
	}

	bool changed = drawMeshObjectParams(object);

	ImGui::Spacing();
	if (ImGui::BeginCombo("shading", meshMaterialModeName(object.materialMode))) {
		for (MeshMaterialMode mode : { MeshMaterialMode::Model, MeshMaterialMode::Override }) {
			if (ImGui::Selectable(meshMaterialModeName(mode), mode == object.materialMode) && mode != object.materialMode) {
				object.materialMode = mode;
				//the first switch to an override gives the object a material of its own
				if (mode == MeshMaterialMode::Override && findMaterial(object.materialId) == nullptr) {
					object.materialId = addMaterialFor(object.name);
				}
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	if (object.materialMode == MeshMaterialMode::Override) {
		changed |= drawMaterialReference(object.materialId, object.name);
	} else if (engine->m_raytraceMeshData != nullptr) {
		//the library entries this object's surfaces use: its model's glTF materials, edited in place
		const RTMeshNode* node = engine->m_raytraceMeshData->findNode(object.modelKey, object.nodeIndex);
		const auto model = engine->m_models.find(object.modelKey);
		if (node != nullptr && model != engine->m_models.end() && model->second != nullptr) {
			std::vector<uint64_t> listed;
			for (const RTMeshSurface& surface : node->surfaces) {
				const auto& byIndex = model->second->materialsByIndex;
				const auto index = std::find(byIndex.begin(), byIndex.end(), surface.material);
				const MaterialDef* material = index != byIndex.end() ? findModelMaterial(object.modelKey, (uint32_t)(index - byIndex.begin())) : nullptr;
				if (material == nullptr || std::find(listed.begin(), listed.end(), material->id) != listed.end()) {
					continue;
				}
				listed.push_back(material->id);
				ImGui::PushID((int)material->id);
				if (ImGui::SmallButton("Edit")) {
					openMaterial(material->id);
				}
				ImGui::SameLine();
				ImGui::TextUnformatted(material->name.c_str());
				ImGui::PopID();
			}
		}
	}

	//the same opt-in gizmo a shape gets, with the full operation set a mesh object can use
	TransformGizmo& gizmo = engine->m_transformGizmo;
	const bool editing = gizmo.isEditing(gizmoTargetId(object.id));
	if (ImGui::Button(editing ? "Stop Editing Transform" : "Edit Transform")) {
		if (editing) {
			gizmo.endEditing();
		} else {
			beginMeshGizmo(gizmo, object.id);
		}
	}
	if (editing) {
		ImGui::SameLine();
		ImGui::TextDisabled("(drag the gizmo in the viewport)");
	}

	if (changed) {
		markChanged();
	}

	ImGui::PopID();
}

bool RaytraceSceneEditor::drawMaterialReference(uint64_t& materialId, const std::string& ownerName)
{
	bool changed = false;
	const MaterialDef* current = findMaterial(materialId);
	const std::string preview = current != nullptr ? materialLabel(*current) : std::string("(none)");
	if (ImGui::BeginCombo("material", preview.c_str())) {
		//the user's materials, then each model's
		for (const bool user : { true, false }) {
			for (const MaterialDef& material : m_materials) {
				if (material.origin.has_value() == user) {
					continue;
				}
				ImGui::PushID((int)material.id);
				if (ImGui::Selectable(materialLabel(material).c_str(), material.id == materialId) && material.id != materialId) {
					materialId = material.id;
					changed = true;
				}
				ImGui::PopID();
			}
		}
		ImGui::EndCombo();
	}
	if (current != nullptr) {
		const size_t users = materialUsers(materialId);
		if (users > 1) {
			ImGui::SameLine();
			ImGui::TextDisabled("(shared by %zu)", users);
		}
	}

	ImGui::BeginDisabled(current == nullptr);
	if (ImGui::Button("Edit")) {
		openMaterial(materialId);
	}
	ImGui::SameLine();
	if (ImGui::Button("New from this")) {
		materialId = addMaterial(userCopyOf(*current, ownerName));
		openMaterial(materialId);
		changed = true;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("A new user material copying this one, used by this object alone");
	}
	ImGui::EndDisabled();
	return changed;
}

//---------------------------------------------------------------- the Materials window

void RaytraceSceneEditor::drawMaterialsWindow(VulkanEngine* engine)
{
	if (!m_showMaterials) {
		return;
	}
	if (!ImGui::Begin("Materials", &m_showMaterials)) {
		ImGui::End();
		return;
	}

	MaterialDef* selected = findMaterial(m_selectedMaterialId);

	if (ImGui::Button("+ New")) {
		MaterialDef material = makeMaterial(MaterialType::Lambertian);
		material.name = fmt::format("material_{}", m_nextId);
		m_selectedMaterialId = addMaterial(std::move(material));
		selected = findMaterial(m_selectedMaterialId);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(selected == nullptr);
	if (ImGui::Button("Duplicate")) {
		m_selectedMaterialId = addMaterial(userCopyOf(*selected, fmt::format("{}_copy", selected->name)));
		selected = findMaterial(m_selectedMaterialId);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	//a glTF material belongs to its model, and one in use would leave its objects pointing at nothing
	const bool deletable = selected != nullptr && !selected->origin.has_value() && materialUsers(selected->id) == 0;
	ImGui::BeginDisabled(!deletable);
	if (ImGui::Button("Delete")) {
		const uint64_t id = selected->id;
		std::erase_if(m_materials, [id](const MaterialDef& m) { return m.id == id; });
		m_selectedMaterialId = 0;
		selected = nullptr;
		markChanged();
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && selected != nullptr && !deletable) {
		ImGui::SetTooltip(selected->origin.has_value() ? "A glTF material goes when its model is removed" : "Still used by an object");
	}
	ImGui::SameLine();
	if (ImGui::Button("Remove unused")) {
		std::erase_if(m_materials, [this](const MaterialDef& m) { return !m.origin.has_value() && materialUsers(m.id) == 0; });
		markChanged();
		selected = findMaterial(m_selectedMaterialId);
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Deletes every user material no shape or override uses");
	}

	drawMaterialTree();
	selected = findMaterial(m_selectedMaterialId);
	if (selected != nullptr) {
		drawMaterialDetails(engine, *selected);
	}

	ImGui::End();
}

void RaytraceSceneEditor::drawMaterialTree()
{
	const float height = 12 * ImGui::GetTextLineHeightWithSpacing();
	if (ImGui::BeginChild("##materials", ImVec2(0, height), ImGuiChildFlags_Borders)) {
		auto row = [this](const MaterialDef& material) {
			ImGui::PushID((int)material.id);
			const size_t users = materialUsers(material.id);
			const std::string label = users > 0 ? fmt::format("{}  ({})", material.name, users) : material.name;
			if (ImGui::Selectable(label.c_str(), material.id == m_selectedMaterialId)) {
				m_selectedMaterialId = material.id;
			}
			ImGui::PopID();
		};
		if (ImGui::TreeNodeEx("User", ImGuiTreeNodeFlags_DefaultOpen)) {
			for (const MaterialDef& material : m_materials) {
				if (!material.origin.has_value()) {
					row(material);
				}
			}
			ImGui::TreePop();
		}
		//a folder per model, in the order the entries were imported
		std::vector<std::string> models;
		for (const MaterialDef& material : m_materials) {
			if (material.origin.has_value() && std::find(models.begin(), models.end(), material.origin->modelKey) == models.end()) {
				models.push_back(material.origin->modelKey);
			}
		}
		for (const std::string& model : models) {
			if (ImGui::TreeNodeEx(model.c_str())) {
				for (const MaterialDef& material : m_materials) {
					if (material.origin.has_value() && material.origin->modelKey == model) {
						row(material);
					}
				}
				ImGui::TreePop();
			}
		}
	}
	ImGui::EndChild();
}

void RaytraceSceneEditor::drawMaterialDetails(VulkanEngine* engine, MaterialDef& material)
{
	ImGui::PushID((int)material.id);
	bool changed = false;

	//the entry as its model's file defines it, which edits are marked against and reverted to
	std::optional<MaterialDef> imported;
	if (material.origin.has_value()) {
		const auto model = engine->m_models.find(material.origin->modelKey);
		if (model != engine->m_models.end() && model->second != nullptr && material.origin->index < model->second->materialsByIndex.size()) {
			imported = modelMaterialDef(*model->second, material.origin->index, engine->m_raytraceTextures);
		}
	}

	if (material.origin.has_value()) {
		ImGui::SeparatorText(material.name.c_str());
		ImGui::TextDisabled("material %u of %s, edited in place", material.origin->index, material.origin->modelKey.c_str());
		if (!material.edited.empty() && imported.has_value()) {
			ImGui::SameLine();
			if (ImGui::SmallButton("Revert all")) {
				for (const std::string& name : material.edited) {
					material.params[name] = materialValue(*imported, name);
				}
				material.edited.clear();
				changed = true;
			}
		}
	} else {
		char name[128];
		std::snprintf(name, sizeof(name), "%s", material.name.c_str());
		if (ImGui::InputText("name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0] != '\0') {
			material.name = name;
			changed = true;
		}
	}
	const size_t users = materialUsers(material.id);
	ImGui::TextDisabled(material.origin.has_value() ? "its model's objects, and %zu shape(s) or override(s)" : "used by %zu shape(s) or override(s)", users);

	//a glTF material is the glTF model: its type is not the user's to change
	ImGui::BeginDisabled(material.origin.has_value());
	if (ImGui::BeginCombo("type", materialTraits(material.type).label)) {
		for (const MaterialType type : MATERIAL_TYPES) {
			if (ImGui::Selectable(materialTraits(type).label, type == material.type) && type != material.type) {
				//every parameter ever set is kept by name, so nothing is lost switching away and back
				material.type = type;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();

	changed |= drawMaterialParams(material, imported.has_value() ? &*imported : nullptr, m_materials);

	//a material that emits is a light, and its light is kept in a group
	if (material.type == MaterialType::Emissive || material.type == MaterialType::Pbr) {
		changed |= drawLightGroupCombo("light group", material.lightGroup);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("The group this material's emission is kept in when a render keeps light groups: Raytrace Render > Outputs.");
		}
	}

	if (changed) {
		markChanged();
	}
	ImGui::PopID();
}

//---------------------------------------------------------------- gizmo adapters

void RaytraceSceneEditor::beginShapeGizmo(TransformGizmo& gizmo, uint64_t id)
{
	const SceneShape* shape = findShape(id);
	if (shape == nullptr) {
		return;
	}
	const ShapeTraits& traits = shapeTraits(shape->kind);

	//the handles the kind has a use for: always a move; a turn unless turning changes nothing; and
	//a scale handle for exactly the axes its params drive - a single uniform one for a sphere's
	//radius, so the handles never offer a stretch the shape would ignore
	ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
	if (traits.rotatable) {
		operation = operation | ImGuizmo::ROTATE;
	}
	uint8_t scaledAxes = 0;
	for (const ShapeParam& param : traits.params) {
		scaledAxes |= param.axes;
	}
	if (traits.params.size() == 1 && scaledAxes == 0b111) {
		operation = operation | ImGuizmo::SCALEU;
	} else {
		const ImGuizmo::OPERATION axisScales[3] = { ImGuizmo::SCALE_X, ImGuizmo::SCALE_Y, ImGuizmo::SCALE_Z };
		for (int axis = 0; axis < 3; axis++) {
			if (scaledAxes & (1u << axis)) {
				operation = operation | axisScales[axis];
			}
		}
	}

	//the closures hold the shape by id and look it up each frame: the editor keeps sole ownership,
	//and deleting or reloading the shape ends the edit rather than leaving the gizmo pointing at a
	//vector slot that now holds something else
	gizmo.beginEditing(
		gizmoTargetId(id),
		[this, id]() -> std::optional<glm::mat4> {
			const SceneShape* s = findShape(id);
			if (s == nullptr) {
				return std::nullopt;
			}
			return s->objectToWorld();
		},
		//matrix -> shape: the translation is the position, the normalised columns the orientation,
		//and their lengths the size. A param driving several axes (a cylinder's radius) takes
		//whichever of them the handle moved, so dragging either one resizes it fully
		[this, id](const glm::mat4& m) {
			SceneShape* s = findShape(id);
			if (s == nullptr) {
				return;
			}
			const ShapeTraits& traits = shapeTraits(s->kind);
			s->position = glm::vec3(m[3]);
			if (traits.rotatable) {
				s->orientation = orientationOf(m);
			}
			const glm::vec3 scale(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
			for (const ShapeParam& param : traits.params) {
				const float current = shapeParamValue(*s, param);
				float moved = current;
				for (int axis = 0; axis < 3; axis++) {
					if ((param.axes & (1u << axis)) && std::abs(scale[axis] - current) > std::abs(moved - current)) {
						moved = scale[axis];
					}
				}
				setShapeParam(*s, param, std::max(moved, 0.001f));
			}
			markChanged();
		},
		//local, so the rotation rings and scale handles follow the shape's own axes
		operation, traits.rotatable ? ImGuizmo::LOCAL : ImGuizmo::WORLD);
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
			markCameraChanged();
		},
		//a camera is placed and aimed; its size is its fov. Local mode so the rotation rings
		//follow the camera's own axes
		ImGuizmo::TRANSLATE | ImGuizmo::ROTATE, ImGuizmo::LOCAL);
}

void RaytraceSceneEditor::beginLightGizmo(TransformGizmo& gizmo, uint64_t id)
{
	const SceneLight* light = findLight(id);
	if (light == nullptr) {
		return;
	}
	//a point light only moves; a spot light and a directional one also aim, and neither has a size
	//a handle could drag
	const ImGuizmo::OPERATION operation = light->kind == LightKind::Point ? ImGuizmo::TRANSLATE : (ImGuizmo::TRANSLATE | ImGuizmo::ROTATE);
	gizmo.beginEditing(
		gizmoTargetId(id),
		[this, id]() -> std::optional<glm::mat4> {
			const SceneLight* l = findLight(id);
			if (l == nullptr) {
				return std::nullopt;
			}
			return l->transform();
		},
		[this, id](const glm::mat4& m) {
			SceneLight* l = findLight(id);
			if (l == nullptr) {
				return;
			}
			l->position = glm::vec3(m[3]);
			if (l->kind != LightKind::Point) {
				l->orientation = orientationOf(m);
			}
			markChanged();
		},
		operation, ImGuizmo::LOCAL);
}

void RaytraceSceneEditor::beginMeshGizmo(TransformGizmo& gizmo, uint64_t id)
{
	gizmo.beginEditing(
		gizmoTargetId(id),
		//the only object whose transform *is* a matrix, so both directions are the identity
		[this, id]() -> std::optional<glm::mat4> {
			const SceneMeshObject* object = findMeshObject(id);
			if (object == nullptr) {
				return std::nullopt;
			}
			return object->transform;
		},
		[this, id](const glm::mat4& m) {
			SceneMeshObject* object = findMeshObject(id);
			if (object == nullptr) {
				return;
			}
			object->transform = m;
			markChanged();
		},
		//the full set: a mesh object is the one thing in the scene with a meaningful rotation and
		//a meaningful non-uniform scale
		ImGuizmo::TRANSLATE | ImGuizmo::ROTATE | ImGuizmo::SCALE);
}
