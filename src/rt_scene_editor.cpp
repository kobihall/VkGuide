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
	return mode == MeshMaterialMode::Gltf ? "glTF material" : "override";
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

bool drawMaterialParams(SceneMaterial& material)
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
	case MaterialType::Emissive:
		changed |= ImGui::ColorEdit3("colour", &material.albedo.x);
		changed |= ImGui::SliderFloat("strength", &material.strength, 0.f, 1000.f, "%.2f", ImGuiSliderFlags_Logarithmic);
		break;
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

	ImGui::Spacing();

	if (ImGui::BeginCombo("shading", meshMaterialModeName(object.materialMode))) {
		for (MeshMaterialMode mode : { MeshMaterialMode::Gltf, MeshMaterialMode::Override }) {
			if (ImGui::Selectable(meshMaterialModeName(mode), mode == object.materialMode) && mode != object.materialMode) {
				object.materialMode = mode;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}

	if (object.materialMode == MeshMaterialMode::Gltf) {
		ImGui::TextDisabled("base colour and texture, as authored");
	} else {
		//an override replaces the glTF material outright, texture included
		changed |= drawMaterialParams(object.material);
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
			bool any = false;
			for (size_t i = 0; i < m_meshObjects.size(); i++) {
				if (m_meshObjects[i].modelKey != key) {
					continue;
				}
				any = true;
				//a hidden object still lists, just greyed, so it can be found and shown again
				const bool visible = m_meshObjects[i].visible;
				if (!visible) {
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				}
				const bool alive = drawObjectRow(SelectionKind::MeshObject, m_meshObjects[i].id, m_meshObjects[i].name);
				if (!visible) {
					ImGui::PopStyleColor();
				}
				if (!alive) {
					break;
				}
			}
			if (!any) {
				ImGui::TextDisabled("no objects - right-click to restore");
			}
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
		engine->removeGltf(toRemove);
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

	changed |= drawMaterialParams(shape.material);

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
			markChanged();
		},
		//a camera is placed and aimed; its size is its fov. Local mode so the rotation rings
		//follow the camera's own axes
		ImGuizmo::TRANSLATE | ImGuizmo::ROTATE, ImGuizmo::LOCAL);
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
