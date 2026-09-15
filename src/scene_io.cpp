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
//1: spheres only. 2: adds "models" and "environmentMap". 3: adds "render" and "environmentIntensity"
constexpr int64_t SCENE_FILE_FORMAT_VERSION = 3;

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

//every material carries its complete parameters inline - the selected type's only, since that is
//what the material *is*; the other types' stored values are editing convenience, not scene data.
//The field names are the source fields'
std::string materialJson(const SphereMaterial& mat)
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

	//every setting, defaults on read, so a scene shared between machines loses nothing and an
	//older build ignores the block
	const RenderSettings& r = scene.render;
	json += fmt::format(R"(,"render":{{"width":{},"height":{},"matchViewport":{},"antialiasing":{},"maxSamples":{},"rayDepth":{},"useFixedSeed":{},"seed":{},"samplesPerFrame":{},"russianRoulette":{},"minBouncesBeforeRoulette":{},"aperture":{},"focusDistance":{},"exposure":{}}})",
		r.width, r.height, r.matchViewport, r.antialiasing, r.maxSamples, r.rayDepth, r.useFixedSeed, r.seed, r.samplesPerFrame, r.russianRoulette, r.minBouncesBeforeRoulette,
		jsonNumber(r.aperture), jsonNumber(r.focusDistance), jsonNumber(r.exposure));

	json += R"(,"spheres":[)";
	for (size_t i = 0; i < scene.spheres.size(); i++) {
		const SceneSphere& entry = scene.spheres[i];
		if (i > 0) {
			json += ',';
		}
		json += fmt::format(R"({{"name":{},"center":{},"radius":{},"material":{}}})",
			jsonString(entry.name), jsonVec3(entry.center), jsonNumber(entry.radius), materialJson(entry.material));
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

bool readMaterial(JsonElement element, SphereMaterial& out, std::string& error)
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
	out = makeSphereMaterial(*type);

	if (*type != MaterialType::Dielectric && !readVec3(object["albedo"], out.albedo)) {
		error = fmt::format("{} material has no valid \"albedo\"", typeName);
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
	readOptionalInt(object["rayDepth"], out.rayDepth);
	readOptionalBool(object["useFixedSeed"], out.useFixedSeed);
	readOptionalUint(object["seed"], out.seed);
	readOptionalInt(object["samplesPerFrame"], out.samplesPerFrame);
	readOptionalBool(object["russianRoulette"], out.russianRoulette);
	readOptionalInt(object["minBouncesBeforeRoulette"], out.minBouncesBeforeRoulette);
	readOptionalFloat(object["aperture"], out.aperture);
	readOptionalFloat(object["focusDistance"], out.focusDistance);
	readOptionalFloat(object["exposure"], out.exposure);

	//a hand-edited file cannot land on something the ui could not
	out.width = std::max(out.width, 2);
	out.height = std::max(out.height, 2);
	out.maxSamples = std::max(out.maxSamples, 1);
	out.rayDepth = std::max(out.rayDepth, 1);
	out.samplesPerFrame = std::max(out.samplesPerFrame, 1);
	out.minBouncesBeforeRoulette = std::max(out.minBouncesBeforeRoulette, 0);
	out.aperture = std::max(out.aperture, 0.f);
	out.focusDistance = std::max(out.focusDistance, 0.01f);
	out.exposure = std::max(out.exposure, 0.f);
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
//for the duration of the call, so the spheres are built here rather than after loadGltf returns
void parseSceneExtras(simdjson::dom::object* extras, std::size_t objectIndex, fastgltf::Category category, void* userPointer)
{
	if (category != fastgltf::Category::Scenes || extras == nullptr) {
		return;
	}

	ParseContext& ctx = *static_cast<ParseContext*>(userPointer);
	if (ctx.found) {
		//the first scene carrying sphere data wins
		return;
	}

	simdjson::dom::object& root = *extras;

	simdjson::dom::array spheres;
	if (root["spheres"].get_array().get(spheres) != simdjson::SUCCESS) {
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

	size_t index = 0;
	for (simdjson::dom::element entry : spheres) {
		simdjson::dom::object sphereObject;
		if (entry.get_object().get(sphereObject) != simdjson::SUCCESS) {
			ctx.error = fmt::format("sphere {} is not an object", index);
			return;
		}

		SceneSphere sphereEntry;

		std::string_view name;
		if (sphereObject["name"].get_string().get(name) == simdjson::SUCCESS) {
			sphereEntry.name = std::string(name);
		} else {
			sphereEntry.name = fmt::format("sphere_{}", index + 1);
		}

		if (!readVec3(sphereObject["center"], sphereEntry.center)) {
			ctx.error = fmt::format("sphere \"{}\" has no valid \"center\"", sphereEntry.name);
			return;
		}

		double radius = 0.0;
		if (!readDouble(sphereObject["radius"], radius) || !(radius > 0.0)) {
			ctx.error = fmt::format("sphere \"{}\" has no valid positive \"radius\"", sphereEntry.name);
			return;
		}
		sphereEntry.radius = (float)radius;

		std::string materialError;
		if (!readMaterial(sphereObject["material"], sphereEntry.material, materialError)) {
			ctx.error = fmt::format("sphere \"{}\": {}", sphereEntry.name, materialError);
			return;
		}

		//ids are the editor's to assign; a loaded sphere has none yet
		ctx.scene.spheres.push_back(std::move(sphereEntry));
		index++;
	}
}

}

std::filesystem::path absoluteDirectoryOf(const std::filesystem::path& file)
{
	std::error_code ec;
	std::filesystem::path absolute = std::filesystem::absolute(file, ec);
	return (ec ? file : absolute).lexically_normal().parent_path();
}

IoResult saveSceneFile(const SceneDescription& scene, const std::filesystem::path& file)
{
	if (file.empty()) {
		return IoResult::failure("No path given");
	}

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

	return IoResult::success(fmt::format("Saved '{}': {} model(s), {} sphere(s){}", file.filename().string(), scene.modelPaths.size(), scene.spheres.size(), scene.environmentMapPath.empty() ? "" : ", environment map"));
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
		return IoResult::failure(fmt::format("'{}' is a glTF file but not a saved scene (no scene extras with a \"spheres\" array). Use Import glTF Model for model files", file.string()));
	}

	out = std::move(ctx.scene);
	return IoResult::success(fmt::format("Read '{}': {} model(s), {} sphere(s)", file.filename().string(), out.modelPaths.size(), out.spheres.size()));
}
