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
//   then opens with the default selection (BVH traversal, BSDF scattering) and the default
//   builder - exactly what every scene saved before this used
constexpr int64_t SCENE_FILE_FORMAT_VERSION = 8;

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

//every material carries its complete parameters inline - the selected type's only, since that is
//what the material *is*; the other types' stored values are editing convenience, not scene data.
//The field names are the source fields'
std::string materialJson(const SceneMaterial& mat)
{
	const char* typeName = materialTypeName(mat.type);

	switch (mat.type) {
	case MaterialType::Lambertian:
		return fmt::format(R"({{"type":"{}","albedo":{}}})", typeName, jsonVec3(mat.albedo));
	case MaterialType::Metal:
		return fmt::format(R"({{"type":"{}","albedo":{},"fuzz":{}}})", typeName, jsonVec3(mat.albedo), jsonNumber(mat.fuzz));
	case MaterialType::Phong:
		return fmt::format(R"({{"type":"{}","albedo":{},"smoothness":{}}})", typeName, jsonVec3(mat.albedo), jsonNumber(mat.smoothness));
	case MaterialType::Dielectric:
		return fmt::format(R"({{"type":"{}","ir":{}}})", typeName, jsonNumber(mat.ir));
	case MaterialType::Emissive:
		return fmt::format(R"({{"type":"{}","color":{},"strength":{}}})", typeName, jsonVec3(mat.albedo), jsonNumber(mat.strength));
	}

	return fmt::format(R"({{"type":"{}"}})", typeName);
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
	json += fmt::format(R"(,"render":{{"width":{},"height":{},"matchViewport":{},"antialiasing":{},"maxSamples":{},"unlimitedSamples":{},"rayDepth":{},"useFixedSeed":{},"seed":{},"samplesPerFrame":{},"russianRoulette":{},"minBouncesBeforeRoulette":{},"restartOnChange":{}}})",
		r.width, r.height, r.matchViewport, r.antialiasing, r.maxSamples, r.unlimitedSamples, r.rayDepth, r.useFixedSeed, r.seed, r.samplesPerFrame, r.russianRoulette, r.minBouncesBeforeRoulette, r.restartOnChange);

	//version 8: the wavefront kernel selection, by slot number and variant id. Ids rather than
	//indices so that registering a new variant, or reordering the table, never silently changes
	//what an existing scene renders with
	json += R"(,"kernels":{)";
	const std::vector<std::pair<std::string, std::string>> kernelIds = kernelSelectionToIds(scene.kernels);
	for (size_t i = 0; i < kernelIds.size(); i++) {
		if (i > 0) {
			json += ',';
		}
		json += fmt::format("{}:{}", jsonString(kernelIds[i].first), jsonString(kernelIds[i].second));
	}
	json += '}';

	//version 8: how the BVH is built. The node layout is deliberately NOT here - it follows
	//whichever kernel 02 variant is selected, so saving it would let a file contradict itself
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

	//every kind through its traits: the sizes go out under the names the editor shows, so a
	//cylinder reads {"radius":0.5,"height":2} rather than as a raw scale vector
	json += R"(,"geometry":[)";
	for (size_t i = 0; i < scene.shapes.size(); i++) {
		const SceneShape& shape = scene.shapes[i];
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
		json += fmt::format(R"(,"material":{}}})", materialJson(shape.material));
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
		json += fmt::format(R"({{"name":{},"model":{},"node":{},"transform":[{}],"visible":{},"shading":"{}","material":{}}})",
			jsonString(object.name), object.model, object.nodeIndex, matrix, object.visible,
			object.materialMode == MeshMaterialMode::Override ? "override" : "gltf", materialJson(object.material));
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
	out = glm::normalize(glm::quat(q[3], q[0], q[1], q[2]));
	return true;
}

bool readMaterial(JsonElement element, SceneMaterial& out, std::string& error)
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

	std::optional<MaterialType> type;
	for (MaterialType candidate : MATERIAL_TYPES) {
		if (typeName == materialTypeName(candidate)) {
			type = candidate;
		}
	}
	if (!type.has_value()) {
		error = fmt::format("unknown material type \"{}\"", typeName);
		return false;
	}

	//defaults for every field the file does not carry (the unselected types' parameters)
	out = makeSceneMaterial(*type);

	//a light's colour is what it emits, not an albedo, and the file says so
	const char* colorKey = *type == MaterialType::Emissive ? "color" : "albedo";
	if (*type != MaterialType::Dielectric && !readVec3(object[colorKey], out.albedo)) {
		error = fmt::format("{} material has no valid \"{}\"", typeName, colorKey);
		return false;
	}

	double value = 0.0;
	switch (*type) {
	case MaterialType::Metal:
		if (!readDouble(object["fuzz"], value)) {
			error = "metal material has no valid \"fuzz\"";
			return false;
		}
		out.fuzz = std::clamp((float)value, 0.f, 1.f);
		break;
	case MaterialType::Phong:
		if (!readDouble(object["smoothness"], value)) {
			error = "phong material has no valid \"smoothness\"";
			return false;
		}
		out.smoothness = (float)value;
		break;
	case MaterialType::Dielectric:
		if (!readDouble(object["ir"], value)) {
			error = "dielectric material has no valid \"ir\"";
			return false;
		}
		out.ir = (float)value;
		break;
	case MaterialType::Emissive:
		if (!readDouble(object["strength"], value) || value < 0.0) {
			error = "emissive material has no valid \"strength\"";
			return false;
		}
		out.strength = (float)value;
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

//version 8: {"00":"thin_lens","02":"bvh_binary",...}. Unknown slots and unknown variant ids are
//collected as warnings and left at their default rather than failing the load - a scene saved by
//a build with a strategy this one does not have must still open
void readKernelSelection(JsonElement element, SceneDescription& scene)
{
	simdjson::dom::object object;
	if (element.get_object().get(object) != simdjson::SUCCESS) {
		return;
	}
	std::vector<std::pair<std::string, std::string>> ids;
	for (auto field : object) {
		std::string_view value;
		if (field.value.get_string().get(value) != simdjson::SUCCESS) {
			continue;
		}
		ids.emplace_back(std::string(field.key), std::string(value));
	}
	scene.kernels = kernelSelectionFromIds(ids, &scene.kernelWarnings);
}

//version 8. The layout is not read: it follows the selected kernel 02 variant, and
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

	std::string materialError;
	if (!readMaterial(object["material"], out.material, materialError)) {
		error = fmt::format("{} \"{}\": {}", traits.name, out.name, materialError);
		return false;
	}
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
	readKernelSelection(root["kernels"], ctx.scene);
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

	size_t index = 0;
	for (simdjson::dom::element entry : geometry) {
		simdjson::dom::object shapeObject;
		if (entry.get_object().get(shapeObject) != simdjson::SUCCESS) {
			ctx.error = fmt::format("geometry {} is not an object", index);
			return;
		}
		//ids are the editor's to assign; a loaded shape has none yet
		SceneShape shape;
		if (!readShape(shapeObject, legacySpheres, index, shape, ctx.error)) {
			return;
		}
		ctx.scene.shapes.push_back(std::move(shape));
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
			//the override's parameters are written whichever mode is selected, so switching a saved
			//object back to "override" finds what it had. A file without them keeps the defaults
			std::string materialError;
			readMaterial(object["material"], record.material, materialError);

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

	return IoResult::success(fmt::format("Saved '{}': {} model(s), {} shape(s), {} camera(s){}", file.filename().string(), scene.modelPaths.size(), scene.shapes.size(), scene.cameras.size(), scene.environmentMapPath.empty() ? "" : ", environment map"));
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
