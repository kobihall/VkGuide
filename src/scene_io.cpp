#include <scene_io.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <simdjson.h>


namespace {

//bump when the payload's shape changes, and branch on it in parseSceneExtras() for older files.
//1: spheres only. 2: adds "models" and "environmentMap". 3: adds "render" and "environmentIntensity".
//4: adds "cameras" and "renderCamera"; the lens and exposure move from "render" to each camera
//5: adds "meshObjects", the placed glTF nodes. A version-4 file has none, and loading it places
//   every node of every model at its authored transform - exactly what importing does
//6: "spheres" becomes "geometry", every analytic shape in one list, each record naming its "type"
//   and carrying a "position", an "orientation" when the kind has one, and its sizes by name. An
//   older file's "spheres" still reads, as spheres
//7: adds the "emissive" material ("color", "strength") and "background", a solid colour for missed
//   rays; absent means the environment map or the sky, as before
//8: adds "kernels", the wavefront kernel variant selected in each slot as {"NN":"<variant id>"},
//   and "accel", how the BVH those kernels read is built. Both absent in an older file, which
//   then opens with the default selection and the default builder: BVH traversal as before, and
//   MIS (power) direct lighting where the tracer of the time had only BSDF sampling. A following
//   slot's entry (kernel 08's) is written but never read
//9: adds the "pbr" material ("albedo", "metallic", "roughness", "emission", "strength"), glTF's
//   metallic-roughness model. An older file has none, so it reads unchanged
//10: adds "lights", the punctual lights: each a "type" (point, spot, directional), "name",
//   "position", "color", "intensity" in the tracer's radiometric units, and by type an
//   "orientation", a "range", an "innerCone" and "outerCone" in degrees, and the "model" it was
//   imported with. Absent in an older file, whose models' own lights their imports recreate
//11: adds "integrator", the light transport algorithm's key, and "traversal", the id of the
//   traversal every kernel that traces is built for. Kernels 02 and 08 become one variant each,
//   "closest" and "occlusion"; an older file's kernel 02 entry names the traversal instead, and its
//   kernel 08 entry is not read
//12: adds the material library. "materials" holds the user materials in full: a "name", a "type",
//   and the type's parameters under their MATERIAL_TRAITS names. "materialOverrides" holds the
//   edits to glTF materials, each a "model" index, a "material" index in that file and the changed
//   parameters. A shape's and an override mesh object's "material" is a reference: an index into
//   "materials", or {"model":m,"material":i} for a glTF material. An older file's inline material
//   becomes one user material per object, named after it; a mesh object in glTF mode drops the
//   override material older files wrote for it anyway
//13: adds "errorBoundedOffsets" and "watertightTriangles" to "render", the intersection precision
//   settings. Absent in an older file, which reads with both on: its render differs from the one it
//   was saved with by what the old 0.001 ray epsilon and Moller-Trumbore got wrong, and by nothing else
//14: adds "samplerRandomization" to "render" (none, permute_digits, fast_owen, owen), and kernel 01's
//   samplers. A file without a "01" entry in "kernels" (one older than version 8, or written by hand)
//   reads with "pcg_hash", the only variant there was, rather than a new scene's ZSobol, so it renders
//   as it always did
constexpr int64_t SCENE_FILE_FORMAT_VERSION = 14;

//---------------------------------------------------------------- writing

//always a valid json number that reads back as a double: no nan/inf, and always a fraction or
//exponent so simdjson does not hand back an integer. Formatted as the float it is, so the text is
//the shortest that round-trips to the same float (0.7f prints as 0.7, not 0.699999988)
std::string jsonNumber(float value)
{
	if (!std::isfinite(value)) {
		value = 0.f;
	}
	std::string text = fmt::format("{}", value);
	if (text.find_first_of(".eE") == std::string::npos) {
		text += ".0";
	}
	return text;
}

std::string jsonString(std::string_view text)
{
	std::string out = "\"";
	for (const char c : text) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if ((unsigned char)c < 0x20) {
				out += fmt::format("\\u{:04x}", (unsigned int)(unsigned char)c);
			} else {
				out += c;
			}
		}
	}
	out += '"';
	return out;
}

std::string jsonVec3(const glm::vec3& v)
{
	return fmt::format("[{},{},{}]", jsonNumber(v.x), jsonNumber(v.y), jsonNumber(v.z));
}

//x, y, z, w - named explicitly rather than trusting glm's memory order
std::string jsonQuat(const glm::quat& q)
{
	return fmt::format("[{},{},{},{}]", jsonNumber(q.x), jsonNumber(q.y), jsonNumber(q.z), jsonNumber(q.w));
}

//a material's parameters as json fields, by the names MATERIAL_TRAITS gives them: those of its
//type that are in `names` (every one, for a user material), never a read-only one, which is derived
//rather than authored
std::string materialParamsJson(const MaterialDef& material, const std::set<std::string, std::less<>>* names)
{
	std::string json;
	for (const MaterialParam& param : materialTraits(material.type).params) {
		if ((param.flags & PARAM_READ_ONLY) != 0 || (names != nullptr && names->count(param.name) == 0)) {
			continue;
		}
		const ParamValue value = materialValue(material, param.name);
		if (const float* f = std::get_if<float>(&value)) {
			json += fmt::format(R"(,"{}":{})", param.name, jsonNumber(*f));
		} else if (const SpectrumRef* spectrum = std::get_if<SpectrumRef>(&value)) {
			json += fmt::format(R"(,"{}":{})", param.name, jsonVec3(spectrum->rgb));
		} else if (const int* i = std::get_if<int>(&value)) {
			json += fmt::format(R"(,"{}":{})", param.name, *i);
		} else if (const bool* b = std::get_if<bool>(&value)) {
			json += fmt::format(R"(,"{}":{})", param.name, *b);
		}
		//a texture reference is never saved until pbrt-textures.md gives textures a stable identity
	}
	return json;
}

std::string materialRefJson(const SceneMaterialRef& ref)
{
	if (ref.model >= 0) {
		return fmt::format(R"({{"model":{},"material":{}}})", ref.model, ref.gltfIndex);
	}
	return fmt::format("{}", ref.user);
}

//relative to the scene file when the target sits inside the scene's folder or its parent (the
//project's own assets/scenes -> assets layout), so an assets folder moves as a unit; absolute
//otherwise, since on posix everything shares a root and a "relative" path to another volume
//would be a long, fragile ../../.. chain. Forward slashes either way
std::string portablePath(const std::filesystem::path& absolutePath, const std::filesystem::path& sceneDirectory)
{
	std::filesystem::path relative = absolutePath.lexically_relative(sceneDirectory);
	int climbs = 0;
	for (const std::filesystem::path& part : relative) {
		if (part != "..") {
			break;
		}
		climbs++;
	}
	if (relative.empty() || climbs > 1) {
		relative = absolutePath;
	}
	return relative.generic_string();
}

std::string sceneJson(const SceneDescription& scene, const std::filesystem::path& sceneDirectory)
{
	std::string json = fmt::format(R"({{"version":{},"models":[)", SCENE_FILE_FORMAT_VERSION);

	for (size_t i = 0; i < scene.modelPaths.size(); i++) {
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"path":{}}})", jsonString(portablePath(scene.modelPaths[i], sceneDirectory)));
	}
	json += ']';

	if (!scene.environmentMapPath.empty()) {
		json += fmt::format(R"(,"environmentMap":{})", jsonString(portablePath(scene.environmentMapPath, sceneDirectory)));
	}

	json += fmt::format(R"(,"environmentIntensity":{})", jsonNumber(scene.environmentIntensity));
	if (scene.backgroundColor.has_value()) {
		json += fmt::format(R"(,"background":{})", jsonVec3(*scene.backgroundColor));
	}

	//every setting, defaults on read, so a scene shared between machines loses nothing and an
	//older build ignores the block
	const RenderSettings& r = scene.render;
	json += fmt::format(R"(,"render":{{"width":{},"height":{},"matchViewport":{},"antialiasing":{},"maxSamples":{},"unlimitedSamples":{},"rayDepth":{},"useFixedSeed":{},"seed":{},"samplesPerFrame":{},"russianRoulette":{},"minBouncesBeforeRoulette":{},"restartOnChange":{},"errorBoundedOffsets":{},"watertightTriangles":{},"samplerRandomization":{}}})",
		r.width, r.height, r.matchViewport, r.antialiasing, r.maxSamples, r.unlimitedSamples, r.rayDepth, r.useFixedSeed, r.seed, r.samplesPerFrame, r.russianRoulette, r.minBouncesBeforeRoulette, r.restartOnChange,
		r.errorBoundedOffsets, r.watertightTriangles, jsonString(samplerRandomizationId(r.samplerRandomization)));

	//version 11: the integrator and the traversal; version 8: the variant of each stage, by slot
	//number and variant id. Ids rather than indices so that registering a new variant, or reordering
	//a table, never silently changes what an existing scene renders with
	const KernelSelectionIds kernelIds = kernelSelectionToIds(scene.kernels);
	json += fmt::format(R"(,"integrator":{},"traversal":{})", jsonString(kernelIds.integrator), jsonString(kernelIds.traversal));
	json += R"(,"kernels":{)";
	for (size_t i = 0; i < kernelIds.stages.size(); i++) {
		if (i > 0) {
			json += ',';
		}
		json += fmt::format("{}:{}", jsonString(kernelIds.stages[i].first), jsonString(kernelIds.stages[i].second));
	}
	json += '}';

	//version 8: how the BVH is built. The node layout is deliberately NOT here - it follows
	//whichever traversal is selected, so saving it would let a file contradict itself
	const BvhBuildOptions& a = scene.accel.blas;
	json += fmt::format(R"(,"accel":{{"builder":{},"maxLeafSize":{},"binCount":{},"traversalCost":{},"intersectionCost":{},"spatialAlpha":{},"spatialBudget":{},"maxDepth":{}}})",
		jsonString(bvhBuilderName(a.builder)), a.maxLeafSize, a.binCount,
		jsonNumber(a.traversalCost), jsonNumber(a.intersectionCost), jsonNumber(a.spatialAlpha), jsonNumber(a.spatialBudget), a.maxDepth);

	json += fmt::format(R"(,"renderCamera":{},"cameras":[)", scene.renderCamera);
	for (size_t i = 0; i < scene.cameras.size(); i++) {
		const SceneCamera& c = scene.cameras[i];
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"name":{},"position":{},"orientation":{},"fov":{},"aperture":{},"focusDistance":{},"exposure":{}}})",
			jsonString(c.name), jsonVec3(c.position), jsonQuat(c.orientation),
			jsonNumber(c.vfovDegrees), jsonNumber(c.aperture), jsonNumber(c.focusDistance), jsonNumber(c.exposure));
	}
	json += ']';

	//version 10: the punctual lights, each with only what its kind uses
	json += R"(,"lights":[)";
	for (size_t i = 0; i < scene.lights.size(); i++) {
		const SceneLight& light = scene.lights[i].light;
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"type":"{}","name":{},"position":{})", lightKindName(light.kind), jsonString(light.name), jsonVec3(light.position));
		if (light.kind != LightKind::Point) {
			json += fmt::format(R"(,"orientation":{})", jsonQuat(light.orientation));
		}
		json += fmt::format(R"(,"color":{},"intensity":{})", jsonVec3(light.color), jsonNumber(light.intensity));
		if (light.kind != LightKind::Directional) {
			json += fmt::format(R"(,"range":{})", jsonNumber(light.range));
		}
		if (light.kind == LightKind::Spot) {
			json += fmt::format(R"(,"innerCone":{},"outerCone":{})", jsonNumber(light.innerConeDegrees), jsonNumber(light.outerConeDegrees));
		}
		if (scene.lights[i].model >= 0) {
			json += fmt::format(R"(,"model":{})", scene.lights[i].model);
		}
		json += '}';
	}
	json += ']';

	//version 12: the library. User materials in full, kept whether or not anything references them;
	//glTF materials only as what the user changed of them
	json += R"(,"materials":[)";
	for (size_t i = 0; i < scene.materials.size(); i++) {
		const MaterialDef& material = scene.materials[i];
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"name":{},"type":"{}"{}}})", jsonString(material.name), materialTypeName(material.type), materialParamsJson(material, nullptr));
	}
	json += R"(],"materialOverrides":[)";
	for (size_t i = 0; i < scene.materialOverrides.size(); i++) {
		const SceneMaterialOverride& edit = scene.materialOverrides[i];
		if (i > 0) {
			json += ',';
		}
		MaterialDef edited;
		edited.type = MaterialType::Pbr;
		edited.params = edit.params;
		std::set<std::string, std::less<>> names;
		for (const auto& [name, value] : edit.params) {
			names.insert(name);
		}
		json += fmt::format(R"({{"model":{},"material":{}{}}})", edit.model, edit.gltfIndex, materialParamsJson(edited, &names));
	}
	json += ']';

	//every kind through its traits: the sizes go out under the names the editor shows, so a
	//cylinder reads {"radius":0.5,"height":2} rather than as a raw scale vector
	json += R"(,"geometry":[)";
	for (size_t i = 0; i < scene.shapes.size(); i++) {
		const SceneShape& shape = scene.shapes[i].shape;
		const ShapeTraits& traits = shapeTraits(shape.kind);
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"type":"{}","name":{},"position":{})", traits.name, jsonString(shape.name), jsonVec3(shape.position));
		if (traits.rotatable) {
			json += fmt::format(R"(,"orientation":{})", jsonQuat(shape.orientation));
		}
		for (const ShapeParam& param : traits.params) {
			json += fmt::format(R"(,"{}":{})", param.name, jsonNumber(shapeParamValue(shape, param)));
		}
		json += fmt::format(R"(,"material":{}}})", materialRefJson(scene.shapes[i].material));
	}

	json += ']';

	//version 5: the placed mesh objects. The transform goes out as the sixteen raw matrix
	//elements in glm's (column-major) order rather than as decomposed translation/rotation/scale,
	//so a save and reload is bit-exact and no euler ambiguity can creep in
	json += R"(,"meshObjects":[)";
	for (size_t i = 0; i < scene.meshObjects.size(); i++) {
		const SceneMeshObjectRecord& object = scene.meshObjects[i];
		if (i > 0) {
			json += ',';
		}
		std::string matrix;
		for (int element = 0; element < 16; element++) {
			if (element > 0) {
				matrix += ',';
			}
			matrix += jsonNumber((&object.transform[0][0])[element]);
		}
		json += fmt::format(R"({{"name":{},"model":{},"node":{},"transform":[{}],"visible":{},"shading":"{}")",
			jsonString(object.name), object.model, object.nodeIndex, matrix, object.visible,
			object.materialMode == MeshMaterialMode::Override ? "override" : "gltf");
		if (object.materialMode == MeshMaterialMode::Override) {
			json += fmt::format(R"(,"material":{})", materialRefJson(object.material));
		}
		json += '}';
	}

	json += "]}";
	return json;
}

//fastgltf asks for extras once per exported entity; only the single scene gets the payload
std::optional<std::string> writeExtras(std::size_t objectIndex, fastgltf::Category category, void* userPointer)
{
	if (category != fastgltf::Category::Scenes || objectIndex != 0) {
		return std::nullopt;
	}
	return *static_cast<const std::string*>(userPointer);
}

//---------------------------------------------------------------- reading

using JsonElement = simdjson::simdjson_result<simdjson::dom::element>;

bool readDouble(simdjson::dom::element element, double& out)
{
	if (element.get_double().get(out) == simdjson::SUCCESS) {
		return true;
	}
	//a hand-edited file may well say 100 rather than 100.0
	int64_t integer = 0;
	if (element.get_int64().get(integer) == simdjson::SUCCESS) {
		out = (double)integer;
		return true;
	}
	return false;
}

//the lookup form, for fields that may be missing entirely
bool readDouble(JsonElement result, double& out)
{
	simdjson::dom::element element;
	if (result.get(element) != simdjson::SUCCESS) {
		return false;
	}
	return readDouble(element, out);
}

bool readVec3(JsonElement element, glm::vec3& out)
{
	simdjson::dom::array array;
	if (element.get_array().get(array) != simdjson::SUCCESS || array.size() != 3) {
		return false;
	}
	size_t i = 0;
	for (simdjson::dom::element component : array) {
		double value = 0.0;
		if (!readDouble(component, value)) {
			return false;
		}
		out[(glm::length_t)i++] = (float)value;
	}
	return true;
}

//written x, y, z, w by jsonQuat()
bool readQuat(JsonElement element, glm::quat& out)
{
	simdjson::dom::array array;
	if (element.get_array().get(array) != simdjson::SUCCESS || array.size() != 4) {
		return false;
	}
	float q[4];
	size_t i = 0;
	for (simdjson::dom::element component : array) {
		double value = 0.0;
		if (!readDouble(component, value)) {
			return false;
		}
		q[i++] = (float)value;
	}
	//normalized only when it is not unit length already. Normalizing a quaternion that is unit to
	//float precision can move it by an ulp, and normalizing that result moves it back, so a file
	//saved and reopened alternated between two orientations on every save. jsonNumber() writes the
	//shortest text that reads back as the same float, so a value saved unit stays exactly as saved
	const glm::quat read(q[3], q[0], q[1], q[2]);
	const float lengthSquared = glm::dot(read, read);
	if (!(lengthSquared > 1e-12f)) {
		//no rotation can be recovered from a zero quaternion; it reads as none
		out = glm::quat(1.f, 0.f, 0.f, 0.f);
	} else if (std::abs(lengthSquared - 1.f) > 1e-5f) {
		out = glm::normalize(read);
	} else {
		out = read;
	}
	return true;
}

//an inline material of a file before version 12, as a user material: the selected type's fields, by
//the names the parameters still have
bool readLegacyMaterial(JsonElement element, MaterialDef& out, std::string& error)
{
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		error = "\"material\" is missing or not an object";
		return false;
	}

	std::string_view typeName;
	if (object["type"].get_string().get(typeName) != simdjson::SUCCESS) {
		error = "material has no \"type\"";
		return false;
	}

	const std::optional<MaterialType> type = findMaterialType(typeName);
	if (!type.has_value()) {
		error = fmt::format("unknown material type \"{}\"", typeName);
		return false;
	}

	//the defaults stand for every field the file does not carry (the unselected types' parameters)
	out = MaterialDef {};
	out.type = *type;

	//a light's colour is what it emits, not an albedo, and the file said so
	const char* colorKey = *type == MaterialType::Emissive ? "color" : "albedo";
	glm::vec3 albedo(0.f);
	if (*type != MaterialType::Dielectric) {
		if (!readVec3(object[colorKey], albedo)) {
			error = fmt::format("{} material has no valid \"{}\"", typeName, colorKey);
			return false;
		}
		out.params["albedo"] = SpectrumRef { albedo };
	}

	double value = 0.0;
	switch (*type) {
	case MaterialType::Metal:
		if (!readDouble(object["fuzz"], value)) {
			error = "metal material has no valid \"fuzz\"";
			return false;
		}
		out.params["fuzz"] = std::clamp((float)value, 0.f, 1.f);
		break;
	case MaterialType::Phong:
		if (!readDouble(object["smoothness"], value)) {
			error = "phong material has no valid \"smoothness\"";
			return false;
		}
		out.params["smoothness"] = (float)value;
		break;
	case MaterialType::Dielectric:
		if (!readDouble(object["ir"], value)) {
			error = "dielectric material has no valid \"ir\"";
			return false;
		}
		out.params["ir"] = (float)value;
		break;
	case MaterialType::Emissive:
		if (!readDouble(object["strength"], value) || value < 0.0) {
			error = "emissive material has no valid \"strength\"";
			return false;
		}
		out.params["strength"] = (float)value;
		break;
	case MaterialType::Pbr:
		if (!readDouble(object["metallic"], value)) {
			error = "pbr material has no valid \"metallic\"";
			return false;
		}
		out.params["metallic"] = std::clamp((float)value, 0.f, 1.f);
		if (!readDouble(object["roughness"], value)) {
			error = "pbr material has no valid \"roughness\"";
			return false;
		}
		out.params["roughness"] = std::clamp((float)value, 0.f, 1.f);
		if (!readVec3(object["emission"], albedo)) {
			error = "pbr material has no valid \"emission\"";
			return false;
		}
		out.params["emission"] = SpectrumRef { albedo };
		if (!readDouble(object["strength"], value) || value < 0.0) {
			error = "pbr material has no valid \"strength\"";
			return false;
		}
		out.params["strength"] = (float)value;
		break;
	case MaterialType::Lambertian:
		break;
	}

	return true;
}

//optional fields: a missing or malformed one keeps the default, since older files have none
void readOptionalInt(JsonElement element, int& out)
{
	int64_t value = 0;
	if (element.get_int64().get(value) == simdjson::SUCCESS) {
		out = (int)value;
	}
}

void readOptionalUint(JsonElement element, uint32_t& out)
{
	uint64_t value = 0;
	if (element.get_uint64().get(value) == simdjson::SUCCESS) {
		out = (uint32_t)value;
	}
}

void readOptionalBool(JsonElement element, bool& out)
{
	bool value = false;
	if (element.get_bool().get(value) == simdjson::SUCCESS) {
		out = value;
	}
}

void readOptionalFloat(JsonElement element, float& out)
{
	double value = 0.0;
	if (readDouble(element, value)) {
		out = (float)value;
	}
}

//version 8: "kernels", {"00":"thin_lens","02":"closest",...}; version 11: "integrator" and
//"traversal" beside it. Before version 11 the traversal was kernel 02's variant ("02":"bvh_binary")
//and kernel 08 followed it, so an older file's "02" is read as the traversal and its "08" not at
//all. Unknown keys and ids are collected as warnings and left at their default rather than failing
//the load - a scene saved by a build with a strategy this one does not have must still open
void readKernelSelection(simdjson::dom::object& root, int64_t version, SceneDescription& scene)
{
	KernelSelectionIds ids;
	std::string_view text;
	if (root["integrator"].get_string().get(text) == simdjson::SUCCESS) {
		ids.integrator = std::string(text);
	}
	if (root["traversal"].get_string().get(text) == simdjson::SUCCESS) {
		ids.traversal = std::string(text);
	}
	simdjson::dom::object object;
	if (root["kernels"].get_object().get(object) == simdjson::SUCCESS) {
		for (auto field : object) {
			std::string_view value;
			if (field.value.get_string().get(value) != simdjson::SUCCESS) {
				continue;
			}
			if (version < 11 && field.key == "02") {
				ids.traversal = std::string(value);
				continue;
			}
			if (version < 11 && field.key == "08") {
				continue;
			}
			ids.stages.emplace_back(std::string(field.key), std::string(value));
		}
	}
	//version 14: kernel 01 had one variant until then, and a file that names none means it. Every file
	//from version 8 on names it, as "pcg_hash"; an older one has no "kernels" at all
	const bool namesSampler = std::any_of(ids.stages.begin(), ids.stages.end(), [](const auto& stage) { return stage.first == kernelSlotNumber(KernelSlot::GenerateSamples); });
	if (!namesSampler) {
		ids.stages.emplace_back(kernelSlotNumber(KernelSlot::GenerateSamples), "pcg_hash");
	}
	scene.kernels = kernelSelectionFromIds(ids, &scene.kernelWarnings);
}

//version 8. The layout is not read: it follows the selected traversal, and
//RaytraceRenderer::setAccelSettings() puts it right
void readAccelSettings(JsonElement element, AccelSettings& out)
{
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		return;
	}
	BvhBuildOptions& blas = out.blas;
	std::string_view builder;
	if (object["builder"].get_string().get(builder) == simdjson::SUCCESS) {
		for (uint32_t i = 0; i < (uint32_t)BvhBuilder::Count; i++) {
			if (builder == bvhBuilderName((BvhBuilder)i)) {
				blas.builder = (BvhBuilder)i;
				break;
			}
		}
	}
	readOptionalUint(object["maxLeafSize"], blas.maxLeafSize);
	readOptionalUint(object["binCount"], blas.binCount);
	readOptionalUint(object["maxDepth"], blas.maxDepth);
	readOptionalFloat(object["traversalCost"], blas.traversalCost);
	readOptionalFloat(object["intersectionCost"], blas.intersectionCost);
	readOptionalFloat(object["spatialAlpha"], blas.spatialAlpha);
	readOptionalFloat(object["spatialBudget"], blas.spatialBudget);
}

void readRenderSettings(JsonElement element, RenderSettings& out)
{
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		return;
	}
	readOptionalInt(object["width"], out.width);
	readOptionalInt(object["height"], out.height);
	readOptionalBool(object["matchViewport"], out.matchViewport);
	readOptionalBool(object["antialiasing"], out.antialiasing);
	readOptionalInt(object["maxSamples"], out.maxSamples);
	readOptionalBool(object["unlimitedSamples"], out.unlimitedSamples);
	readOptionalInt(object["rayDepth"], out.rayDepth);
	readOptionalBool(object["useFixedSeed"], out.useFixedSeed);
	readOptionalUint(object["seed"], out.seed);
	readOptionalInt(object["samplesPerFrame"], out.samplesPerFrame);
	readOptionalBool(object["russianRoulette"], out.russianRoulette);
	readOptionalInt(object["minBouncesBeforeRoulette"], out.minBouncesBeforeRoulette);
	readOptionalBool(object["restartOnChange"], out.restartOnChange);
	readOptionalBool(object["errorBoundedOffsets"], out.errorBoundedOffsets);
	readOptionalBool(object["watertightTriangles"], out.watertightTriangles);
	//version 14
	std::string_view randomization;
	if (object["samplerRandomization"].get_string().get(randomization) == simdjson::SUCCESS) {
		out.samplerRandomization = samplerRandomizationFromId(std::string(randomization));
	}

	//a hand-edited file cannot land on something the ui could not: the custom-resolution fields
	//hold to the same bounds
	out.width = std::clamp(out.width, MIN_RENDER_DIMENSION, MAX_RENDER_DIMENSION);
	out.height = std::clamp(out.height, MIN_RENDER_DIMENSION, MAX_RENDER_DIMENSION);
	out.maxSamples = std::max(out.maxSamples, 1);
	out.rayDepth = std::max(out.rayDepth, 1);
	out.samplesPerFrame = std::max(out.samplesPerFrame, 1);
	out.minBouncesBeforeRoulette = std::max(out.minBouncesBeforeRoulette, 0);
}

//a version-3 file kept the lens and exposure in "render"; they now belong to every camera, so
//they seed the default camera the loader creates for such a file
void readLegacyCameraSettings(JsonElement element, SceneCamera& out)
{
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		return;
	}
	readOptionalFloat(object["aperture"], out.aperture);
	readOptionalFloat(object["focusDistance"], out.focusDistance);
	readOptionalFloat(object["exposure"], out.exposure);
}

bool readCamera(simdjson::dom::object object, SceneCamera& out, std::string& error)
{
	std::string_view name;
	if (object["name"].get_string().get(name) == simdjson::SUCCESS) {
		out.name = std::string(name);
	}
	if (!readVec3(object["position"], out.position)) {
		error = "camera has no valid \"position\"";
		return false;
	}

	if (!readQuat(object["orientation"], out.orientation)) {
		error = "camera has no valid \"orientation\"";
		return false;
	}

	readOptionalFloat(object["fov"], out.vfovDegrees);
	readOptionalFloat(object["aperture"], out.aperture);
	readOptionalFloat(object["focusDistance"], out.focusDistance);
	readOptionalFloat(object["exposure"], out.exposure);
	out.vfovDegrees = std::clamp(out.vfovDegrees, 1.f, 179.f);
	out.aperture = std::max(out.aperture, 0.f);
	out.focusDistance = std::max(out.focusDistance, 0.01f);
	out.exposure = std::max(out.exposure, 0.f);
	return true;
}

//One "lights" record. The type, the position and a non-negative intensity must be there; the rest
//keeps SceneLight's defaults when missing
bool readLight(simdjson::dom::object object, size_t index, size_t modelCount, SceneLightRecord& out, std::string& error)
{
	std::string_view typeName;
	if (object["type"].get_string().get(typeName) != simdjson::SUCCESS) {
		error = fmt::format("light {} has no \"type\"", index);
		return false;
	}
	std::optional<LightKind> kind;
	for (const LightKind candidate : LIGHT_KINDS) {
		if (typeName == lightKindName(candidate)) {
			kind = candidate;
		}
	}
	if (!kind.has_value()) {
		error = fmt::format("light {} has unknown type \"{}\"", index, typeName);
		return false;
	}
	SceneLight& light = out.light;
	light.kind = *kind;

	std::string_view name;
	if (object["name"].get_string().get(name) == simdjson::SUCCESS) {
		light.name = std::string(name);
	} else {
		light.name = fmt::format("{}_{}", lightKindName(light.kind), index + 1);
	}
	if (!readVec3(object["position"], light.position)) {
		error = fmt::format("light \"{}\" has no valid \"position\"", light.name);
		return false;
	}
	readQuat(object["orientation"], light.orientation);
	readVec3(object["color"], light.color);
	light.color = glm::max(light.color, glm::vec3(0.f));
	double intensity = 0.0;
	if (!readDouble(object["intensity"], intensity) || intensity < 0.0) {
		error = fmt::format("light \"{}\" has no valid \"intensity\"", light.name);
		return false;
	}
	light.intensity = (float)intensity;
	readOptionalFloat(object["range"], light.range);
	readOptionalFloat(object["innerCone"], light.innerConeDegrees);
	readOptionalFloat(object["outerCone"], light.outerConeDegrees);
	light.range = std::max(light.range, 0.f);
	light.outerConeDegrees = std::clamp(light.outerConeDegrees, 0.f, 90.f);
	light.innerConeDegrees = std::clamp(light.innerConeDegrees, 0.f, light.outerConeDegrees);

	int64_t model = -1;
	if (object["model"].get_int64().get(model) == simdjson::SUCCESS && model >= 0 && model < (int64_t)modelCount) {
		out.model = (int)model;
	}
	return true;
}

//One "geometry" record, or - `legacySphere` - one record of an older file's "spheres" list, which
//had no "type" and called the position "center". Every size the kind has must be present and
//positive; a missing orientation is the identity
bool readShape(simdjson::dom::object object, bool legacySphere, size_t index, SceneShape& out, std::string& error)
{
	std::optional<ShapeKind> kind;
	if (legacySphere) {
		kind = ShapeKind::Sphere;
	} else {
		std::string_view typeName;
		if (object["type"].get_string().get(typeName) != simdjson::SUCCESS) {
			error = fmt::format("geometry {} has no \"type\"", index);
			return false;
		}
		for (const ShapeKind candidate : SHAPE_KINDS) {
			if (typeName == shapeTraits(candidate).name) {
				kind = candidate;
			}
		}
		if (!kind.has_value()) {
			error = fmt::format("geometry {} has unknown type \"{}\"", index, typeName);
			return false;
		}
	}

	const ShapeTraits& traits = shapeTraits(*kind);
	out = makeShape(*kind);

	std::string_view name;
	if (object["name"].get_string().get(name) == simdjson::SUCCESS) {
		out.name = std::string(name);
	} else {
		out.name = fmt::format("{}_{}", traits.name, index + 1);
	}

	const char* positionKey = legacySphere ? "center" : "position";
	if (!readVec3(object[positionKey], out.position)) {
		error = fmt::format("{} \"{}\" has no valid \"{}\"", traits.name, out.name, positionKey);
		return false;
	}

	if (traits.rotatable) {
		readQuat(object["orientation"], out.orientation);
	}

	for (const ShapeParam& param : traits.params) {
		double value = 0.0;
		if (!readDouble(object[param.name], value) || !(value > 0.0)) {
			error = fmt::format("{} \"{}\" has no valid positive \"{}\"", traits.name, out.name, param.name);
			return false;
		}
		setShapeParam(out, param, (float)value);
	}

	return true;
}

//the parameters of `type` a version-12 record carries, by name, into `params`. One the record lacks
//keeps its default, and one of the wrong kind is skipped; a read-only one is never in a file
void readMaterialParams(simdjson::dom::object object, MaterialType type, ParamSet& params)
{
	for (const MaterialParam& param : materialTraits(type).params) {
		if ((param.flags & PARAM_READ_ONLY) != 0) {
			continue;
		}
		simdjson::dom::element element;
		if (object[param.name].get(element) != simdjson::SUCCESS) {
			continue;
		}
		switch (param.kind) {
		case ParamKind::Float: {
			double value = 0.0;
			if (readDouble(element, value)) {
				params[param.name] = (float)value;
			}
			break;
		}
		case ParamKind::Spectrum: {
			glm::vec3 value(0.f);
			if (readVec3(object[param.name], value)) {
				params[param.name] = SpectrumRef { value };
			}
			break;
		}
		case ParamKind::Enum: {
			int64_t value = 0;
			if (element.get_int64().get(value) == simdjson::SUCCESS) {
				params[param.name] = (int)value;
			}
			break;
		}
		case ParamKind::Bool: {
			bool value = false;
			if (element.get_bool().get(value) == simdjson::SUCCESS) {
				params[param.name] = value;
			}
			break;
		}
		case ParamKind::Texture:
			break;
		}
	}
}

//one "materials" record: a user material
bool readMaterialDef(simdjson::dom::object object, size_t index, MaterialDef& out, std::string& error)
{
	std::string_view name;
	out.name = object["name"].get_string().get(name) == simdjson::SUCCESS ? std::string(name) : fmt::format("material_{}", index + 1);
	std::string_view typeName;
	if (object["type"].get_string().get(typeName) != simdjson::SUCCESS) {
		error = fmt::format("material \"{}\" has no \"type\"", out.name);
		return false;
	}
	const std::optional<MaterialType> type = findMaterialType(typeName);
	if (!type.has_value()) {
		error = fmt::format("material \"{}\" has unknown type \"{}\"", out.name, typeName);
		return false;
	}
	out.type = *type;
	readMaterialParams(object, out.type, out.params);
	return true;
}

//a version-12 material reference: an index into "materials", or {"model":m,"material":i}. One that
//names nothing the file has reads as unset, and the loader gives its object a default material
SceneMaterialRef readMaterialRef(JsonElement element, size_t materialCount, size_t modelCount)
{
	SceneMaterialRef ref;
	int64_t user = -1;
	if (element.get_int64().get(user) == simdjson::SUCCESS) {
		if (user >= 0 && user < (int64_t)materialCount) {
			ref.user = (int)user;
		}
		return ref;
	}
	int64_t model = -1;
	uint64_t index = 0;
	if (element["model"].get_int64().get(model) == simdjson::SUCCESS && element["material"].get_uint64().get(index) == simdjson::SUCCESS
		&& model >= 0 && model < (int64_t)modelCount) {
		ref.model = (int)model;
		ref.gltfIndex = (uint32_t)index;
	}
	return ref;
}

//An object's material in either format: a reference from version 12, or an older file's inline
//material, which becomes a user material named after the object
bool readObjectMaterial(JsonElement element, int64_t version, const std::string& ownerName, SceneDescription& scene, SceneMaterialRef& out, std::string& error)
{
	if (version >= 12) {
		out = readMaterialRef(element, scene.materials.size(), scene.modelPaths.size());
		return true;
	}
	MaterialDef material;
	if (!readLegacyMaterial(element, material, error)) {
		return false;
	}
	material.name = ownerName;
	out.user = (int)scene.materials.size();
	scene.materials.push_back(std::move(material));
	return true;
}

struct ParseContext {
	std::filesystem::path sceneDirectory;
	SceneDescription scene;
	bool found { false };
	std::string error;
};

std::filesystem::path resolvePath(std::string_view text, const std::filesystem::path& sceneDirectory)
{
	std::filesystem::path path { std::string(text) };
	if (path.is_relative()) {
		path = sceneDirectory / path;
	}
	return path.lexically_normal();
}

//fires from inside the parse, once per entity that has extras. The simdjson object is only valid
//for the duration of the call, so the shapes are built here rather than after loadGltf returns
void parseSceneExtras(simdjson::dom::object* extras, std::size_t objectIndex, fastgltf::Category category, void* userPointer)
{
	if (category != fastgltf::Category::Scenes || extras == nullptr) {
		return;
	}

	ParseContext& ctx = *static_cast<ParseContext*>(userPointer);
	if (ctx.found) {
		//the first scene carrying scene data wins
		return;
	}

	simdjson::dom::object& root = *extras;

	//version 6 on keeps its shapes in "geometry"; every earlier version had "spheres"
	simdjson::dom::array geometry;
	const bool legacySpheres = root["geometry"].get_array().get(geometry) != simdjson::SUCCESS;
	if (legacySpheres && root["spheres"].get_array().get(geometry) != simdjson::SUCCESS) {
		//extras of some other kind - an ordinary glTF file's own application data
		return;
	}
	ctx.found = true;

	int64_t version = 0;
	if (root["version"].get_int64().get(version) != simdjson::SUCCESS) {
		ctx.error = "scene has no \"version\" field";
		return;
	}
	if (version > SCENE_FILE_FORMAT_VERSION) {
		ctx.error = fmt::format("scene file format version {} is newer than the version {} this build understands", version, SCENE_FILE_FORMAT_VERSION);
		return;
	}

	//version 1 had spheres only, so both of these are optional
	simdjson::dom::array models;
	if (root["models"].get_array().get(models) == simdjson::SUCCESS) {
		size_t modelIndex = 0;
		for (simdjson::dom::element entry : models) {
			std::string_view modelPath;
			if (entry["path"].get_string().get(modelPath) != simdjson::SUCCESS || modelPath.empty()) {
				ctx.error = fmt::format("model {} has no \"path\"", modelIndex);
				return;
			}
			ctx.scene.modelPaths.push_back(resolvePath(modelPath, ctx.sceneDirectory));
			modelIndex++;
		}
	}

	std::string_view environmentMap;
	if (root["environmentMap"].get_string().get(environmentMap) == simdjson::SUCCESS && !environmentMap.empty()) {
		ctx.scene.environmentMapPath = resolvePath(environmentMap, ctx.sceneDirectory);
	}

	//version 3: both optional, defaults otherwise
	readOptionalFloat(root["environmentIntensity"], ctx.scene.environmentIntensity);
	ctx.scene.environmentIntensity = std::max(ctx.scene.environmentIntensity, 0.f);
	readRenderSettings(root["render"], ctx.scene.render);

	//version 7: optional, the environment or the sky otherwise
	glm::vec3 background;
	if (readVec3(root["background"], background)) {
		ctx.scene.backgroundColor = glm::max(background, glm::vec3(0.f));
	}

	//version 8: the kernel selection and the BVH settings. Both optional; an older file simply
	//keeps the defaults SceneDescription was constructed with
	readKernelSelection(root, version, ctx.scene);
	readAccelSettings(root["accel"], ctx.scene.accel);

	//version 4: cameras. An older file gets one default camera, carrying the lens settings its
	//"render" block had
	simdjson::dom::array cameras;
	if (root["cameras"].get_array().get(cameras) == simdjson::SUCCESS) {
		size_t cameraIndex = 0;
		for (simdjson::dom::element entry : cameras) {
			simdjson::dom::object cameraObject;
			if (entry.get_object().get(cameraObject) != simdjson::SUCCESS) {
				ctx.error = fmt::format("camera {} is not an object", cameraIndex);
				return;
			}
			SceneCamera camera;
			std::string cameraError;
			if (!readCamera(cameraObject, camera, cameraError)) {
				ctx.error = fmt::format("camera {}: {}", cameraIndex, cameraError);
				return;
			}
			ctx.scene.cameras.push_back(std::move(camera));
			cameraIndex++;
		}
		int64_t renderCamera = -1;
		if (root["renderCamera"].get_int64().get(renderCamera) == simdjson::SUCCESS && renderCamera >= 0 && renderCamera < (int64_t)ctx.scene.cameras.size()) {
			ctx.scene.renderCamera = (int)renderCamera;
		}
	} else {
		SceneCamera camera;
		camera.name = "render_camera";
		readLegacyCameraSettings(root["render"], camera);
		ctx.scene.cameras.push_back(std::move(camera));
		ctx.scene.renderCamera = 0;
	}

	//version 10: the punctual lights. Absent in an older file, which then keeps the lights its
	//models' imports recreate
	simdjson::dom::array lights;
	if (root["lights"].get_array().get(lights) == simdjson::SUCCESS) {
		ctx.scene.hasLights = true;
		size_t lightIndex = 0;
		for (simdjson::dom::element entry : lights) {
			simdjson::dom::object lightObject;
			if (entry.get_object().get(lightObject) != simdjson::SUCCESS) {
				ctx.error = fmt::format("light {} is not an object", lightIndex);
				return;
			}
			SceneLightRecord record;
			if (!readLight(lightObject, lightIndex, ctx.scene.modelPaths.size(), record, ctx.error)) {
				return;
			}
			ctx.scene.lights.push_back(std::move(record));
			lightIndex++;
		}
	}

	//version 12: the library, before the objects that reference it
	simdjson::dom::array materials;
	if (version >= 12 && root["materials"].get_array().get(materials) == simdjson::SUCCESS) {
		size_t materialIndex = 0;
		for (simdjson::dom::element entry : materials) {
			simdjson::dom::object materialObject;
			if (entry.get_object().get(materialObject) != simdjson::SUCCESS) {
				ctx.error = fmt::format("material {} is not an object", materialIndex);
				return;
			}
			MaterialDef material;
			if (!readMaterialDef(materialObject, materialIndex, material, ctx.error)) {
				return;
			}
			ctx.scene.materials.push_back(std::move(material));
			materialIndex++;
		}
	}
	simdjson::dom::array overrides;
	if (version >= 12 && root["materialOverrides"].get_array().get(overrides) == simdjson::SUCCESS) {
		for (simdjson::dom::element entry : overrides) {
			simdjson::dom::object overrideObject;
			int64_t model = -1;
			uint64_t gltfIndex = 0;
			if (entry.get_object().get(overrideObject) != simdjson::SUCCESS || overrideObject["model"].get_int64().get(model) != simdjson::SUCCESS
				|| overrideObject["material"].get_uint64().get(gltfIndex) != simdjson::SUCCESS || model < 0 || model >= (int64_t)ctx.scene.modelPaths.size()) {
				//an override of a model the file does not list has nothing to apply to
				continue;
			}
			SceneMaterialOverride edit;
			edit.model = (int)model;
			edit.gltfIndex = (uint32_t)gltfIndex;
			readMaterialParams(overrideObject, MaterialType::Pbr, edit.params);
			ctx.scene.materialOverrides.push_back(std::move(edit));
		}
	}

	size_t index = 0;
	for (simdjson::dom::element entry : geometry) {
		simdjson::dom::object shapeObject;
		if (entry.get_object().get(shapeObject) != simdjson::SUCCESS) {
			ctx.error = fmt::format("geometry {} is not an object", index);
			return;
		}
		//ids are the editor's to assign; a loaded shape has none yet
		SceneShapeRecord record;
		if (!readShape(shapeObject, legacySpheres, index, record.shape, ctx.error)) {
			return;
		}
		std::string materialError;
		if (!readObjectMaterial(shapeObject["material"], version, record.shape.name, ctx.scene, record.material, materialError)) {
			ctx.error = fmt::format("{} \"{}\": {}", shapeTraits(record.shape.kind).name, record.shape.name, materialError);
			return;
		}
		ctx.scene.shapes.push_back(std::move(record));
		index++;
	}

	//version 5: the placed mesh objects. Absent in an older file, which the loader reads as "place
	//every node at its authored transform"
	simdjson::dom::array meshObjects;
	if (root["meshObjects"].get_array().get(meshObjects) == simdjson::SUCCESS) {
		ctx.scene.hasMeshObjects = true;

		size_t objectIndex = 0;
		for (simdjson::dom::element entry : meshObjects) {
			simdjson::dom::object object;
			if (entry.get_object().get(object) != simdjson::SUCCESS) {
				ctx.error = fmt::format("mesh object {} is not an object", objectIndex);
				return;
			}

			SceneMeshObjectRecord record;

			std::string_view name;
			if (object["name"].get_string().get(name) == simdjson::SUCCESS) {
				record.name = std::string(name);
			} else {
				record.name = fmt::format("mesh_{}", objectIndex + 1);
			}

			int64_t model = -1;
			if (object["model"].get_int64().get(model) != simdjson::SUCCESS || model < 0 || model >= (int64_t)ctx.scene.modelPaths.size()) {
				ctx.error = fmt::format("mesh object \"{}\" names model {}, which the scene does not have", record.name, model);
				return;
			}
			record.model = (int)model;
			readOptionalUint(object["node"], record.nodeIndex);

			simdjson::dom::array matrix;
			if (object["transform"].get_array().get(matrix) != simdjson::SUCCESS || matrix.size() != 16) {
				ctx.error = fmt::format("mesh object \"{}\" has no valid 16-element \"transform\"", record.name);
				return;
			}
			int element = 0;
			for (simdjson::dom::element component : matrix) {
				double value = 0.0;
				if (!readDouble(component, value)) {
					ctx.error = fmt::format("mesh object \"{}\" has a non-numeric \"transform\" element", record.name);
					return;
				}
				(&record.transform[0][0])[element++] = (float)value;
			}

			readOptionalBool(object["visible"], record.visible);

			std::string_view shading;
			if (object["shading"].get_string().get(shading) == simdjson::SUCCESS && shading == "override") {
				record.materialMode = MeshMaterialMode::Override;
			}
			//an override's material. Older files wrote one whichever mode was selected; only an object
			//in override mode keeps it, as a user material named after the object
			if (record.materialMode == MeshMaterialMode::Override) {
				std::string materialError;
				if (!readObjectMaterial(object["material"], version, record.name, ctx.scene, record.material, materialError)) {
					//a file without one gets a default, as it always did
					record.material = {};
				}
			}

			ctx.scene.meshObjects.push_back(std::move(record));
			objectIndex++;
		}
	}
}

}

std::filesystem::path absoluteDirectoryOf(const std::filesystem::path& file)
{
	std::error_code ec;
	std::filesystem::path absolute = std::filesystem::absolute(file, ec);
	return (ec ? file : absolute).lexically_normal().parent_path();
}

IoResult saveSceneFile(const SceneDescription& scene, const std::filesystem::path& fileIn)
{
	if (fileIn.empty()) {
		return IoResult::failure("No path given");
	}

	//fastgltf's exporter rejects a path with no directory component at all ("invalid glTF
	//directory"), so a bare "scene.gltf" would fail for no reason the caller could see. Resolving
	//against the working directory first gives it something to write next to
	std::error_code absoluteEc;
	const std::filesystem::path absolute = std::filesystem::absolute(fileIn, absoluteEc);
	const std::filesystem::path file = absoluteEc ? fileIn : absolute.lexically_normal();

	//FileExporter creates a single missing directory level; the default save location is nested
	if (!file.parent_path().empty()) {
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		if (ec) {
			return IoResult::failure(fmt::format("Could not create directory '{}': {}", file.parent_path().string(), ec.message()));
		}
	}

	//the smallest asset that is still a valid glTF 2.0 document. The schema requires a scene to
	//reference at least one node, so it carries one empty node
	fastgltf::Asset asset;

	fastgltf::AssetInfo info;
	info.gltfVersion = "2.0";
	info.generator = "vulkan_guide";
	asset.assetInfo = std::move(info);

	fastgltf::Node node;
	node.name = "vulkan_guide_scene";
	asset.nodes.push_back(std::move(node));

	fastgltf::Scene gltfScene;
	gltfScene.name = "vulkan_guide scene";
	gltfScene.nodeIndices.push_back(0);
	asset.scenes.push_back(std::move(gltfScene));
	asset.defaultScene = 0;

	std::string json = sceneJson(scene, absoluteDirectoryOf(file));

	fastgltf::FileExporter exporter;
	exporter.setUserPointer(&json);
	exporter.setExtrasWriteCallback(&writeExtras);

	const fastgltf::Error error = exporter.writeGltfJson(asset, file, fastgltf::ExportOptions::PrettyPrintJson);
	if (error != fastgltf::Error::None) {
		return IoResult::failure(fmt::format("Failed to write '{}': {}", file.string(), fastgltf::getErrorMessage(error)));
	}

	return IoResult::success(fmt::format("Saved '{}': {} model(s), {} shape(s), {} light(s), {} camera(s){}", file.filename().string(), scene.modelPaths.size(), scene.shapes.size(), scene.lights.size(), scene.cameras.size(), scene.environmentMapPath.empty() ? "" : ", environment map"));
}

IoResult loadSceneFile(const std::filesystem::path& file, SceneDescription& out)
{
	if (file.empty()) {
		return IoResult::failure("No path given");
	}

	//fastgltf's own message for a missing file blames the directory, so check first
	std::error_code ec;
	if (!std::filesystem::exists(file, ec)) {
		return IoResult::failure(fmt::format("No file at '{}'", file.string()));
	}

	auto data = fastgltf::GltfDataBuffer::FromPath(file);
	if (data.error() != fastgltf::Error::None) {
		return IoResult::failure(fmt::format("Could not read '{}': {}", file.string(), fastgltf::getErrorMessage(data.error())));
	}

	//the same parser setup loadGltf() uses, plus the extras callback and minus every category this
	//has no use for
	ParseContext ctx;
	ctx.sceneDirectory = absoluteDirectoryOf(file);
	fastgltf::Parser parser;
	parser.setUserPointer(&ctx);
	parser.setExtrasParseCallback(&parseSceneExtras);

	auto asset = parser.loadGltf(data.get(), file.parent_path(), fastgltf::Options::None, fastgltf::Category::Scenes | fastgltf::Category::Asset);
	if (asset.error() != fastgltf::Error::None) {
		return IoResult::failure(fmt::format("'{}' is not a readable glTF file: {}", file.string(), fastgltf::getErrorMessage(asset.error())));
	}

	if (!ctx.error.empty()) {
		return IoResult::failure(fmt::format("'{}': {}", file.string(), ctx.error));
	}
	if (!ctx.found) {
		return IoResult::failure(fmt::format("'{}' is a glTF file but not a saved scene (no scene extras with a \"geometry\" array). Use Import glTF Model for model files", file.string()));
	}

	out = std::move(ctx.scene);
	return IoResult::success(fmt::format("Read '{}': {} model(s), {} shape(s), {} camera(s)", file.filename().string(), out.modelPaths.size(), out.shapes.size(), out.cameras.size()));
}
