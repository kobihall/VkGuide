// Converts a pbrt-v4 scene to glTF 2.0, through the same importer the renderer uses (src/pbrt_import.h), so the scene
// can be opened in a standard raster renderer as a performance and appearance baseline: Blender, a three.js or
// Babylon.js viewer, Unity through glTFast, or this renderer's own glTF import.
//
// What it writes, beside OUT.gltf:
//  - OUT.bin: every mesh's positions, normals, uvs and indices. A mesh placed more than once is written once and
//    referenced by each node that places it, as glTF allows.
//  - Materials as glTF's metallic-roughness model, which is what the importer's substitutes already are: Lambertian
//    is metallic 0 and roughness 1, a dielectric adds KHR_materials_transmission and KHR_materials_ior, an emitter
//    KHR_materials_emissive_strength. Base color and normal textures point at the scene's own image files; an image
//    glTF cannot hold (EXR, TGA) is left out.
//  - The camera, the punctual lights (KHR_lights_punctual, in candela and lux), and spheres as triangle meshes.
//  - OUT_environment.hdr: the environment map, equirectangular, for a viewer's world or background.
//
// --instancing groups the placements of a mesh used at least twice into one node with EXT_mesh_gpu_instancing, which
// three.js, Babylon.js and glTFast read as hardware instances. A viewer that does not know the extension shows one
// copy of each such mesh; leave the option out for one, and every placement is its own node sharing the mesh. --weld merges vertices that
// are identical in position, normal and uv, which many pbrt PLY files never share between triangles.
//
// Run:  ./bin/pbrt_to_gltf SCENE.pbrt OUT.gltf [--instancing] [--weld]

#define GLM_ENABLE_EXPERIMENTAL
#include <pbrt_import.h>

#include <image_io.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>

#include <fmt/format.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace {

struct Writer {
	std::string bin;
	std::string accessors;
	std::string bufferViews;
	int accessorCount { 0 };

	// appends `bytes` as a buffer view and an accessor over it; returns the accessor's index
	int add(const void* data, size_t bytes, int componentType, size_t count, const char* type, int target, const std::string& extra = {})
	{
		while (bin.size() % 4 != 0) {
			bin.push_back('\0');
		}
		const size_t offset = bin.size();
		bin.append((const char*)data, bytes);
		const int view = accessorCount;
		bufferViews += fmt::format(R"({}{{"buffer":0,"byteOffset":{},"byteLength":{}{}}})", accessorCount == 0 ? "" : ",", offset, bytes,
			target != 0 ? fmt::format(R"(,"target":{})", target) : "");
		accessors += fmt::format(R"({}{{"bufferView":{},"componentType":{},"count":{},"type":"{}"{}}})", accessorCount == 0 ? "" : ",", view,
			componentType, count, type, extra);
		return accessorCount++;
	}
};

constexpr int FLOAT = 5126;
constexpr int UINT = 5125;
constexpr int ARRAY_BUFFER = 34962;
constexpr int ELEMENT_ARRAY_BUFFER = 34963;

std::string num(float v)
{
	return std::isfinite(v) ? fmt::format("{:.9g}", v) : "0";
}

std::string vec(const glm::vec3& v)
{
	return fmt::format("[{},{},{}]", num(v.x), num(v.y), num(v.z));
}

// merges vertices identical in every attribute the mesh has
void weld(ImportedMesh& mesh)
{
	struct Key {
		float data[8];
		bool operator==(const Key& other) const { return std::memcmp(data, other.data, sizeof(data)) == 0; }
	};
	struct Hash {
		size_t operator()(const Key& k) const
		{
			size_t h = 1469598103934665603ull;
			const unsigned char* p = (const unsigned char*)k.data;
			for (size_t i = 0; i < sizeof(k.data); i++) {
				h = (h ^ p[i]) * 1099511628211ull;
			}
			return h;
		}
	};
	const bool normals = mesh.normals.size() == mesh.positions.size();
	const bool uvs = mesh.uvs.size() == mesh.positions.size();
	const bool tangents = mesh.tangents.size() == mesh.positions.size();
	std::unordered_map<Key, uint32_t, Hash> seen;
	std::vector<uint32_t> remap(mesh.positions.size());
	ImportedMesh out;
	for (size_t v = 0; v < mesh.positions.size(); v++) {
		Key key {};
		std::memcpy(key.data, &mesh.positions[v], 12);
		if (normals) {
			std::memcpy(key.data + 3, &mesh.normals[v], 12);
		}
		if (uvs) {
			std::memcpy(key.data + 6, &mesh.uvs[v], 8);
		}
		const auto [it, inserted] = seen.try_emplace(key, (uint32_t)out.positions.size());
		if (inserted) {
			out.positions.push_back(mesh.positions[v]);
			if (normals) {
				out.normals.push_back(mesh.normals[v]);
			}
			if (uvs) {
				out.uvs.push_back(mesh.uvs[v]);
			}
			if (tangents) {
				out.tangents.push_back(mesh.tangents[v]);
			}
		}
		remap[v] = it->second;
	}
	for (uint32_t& index : mesh.indices) {
		index = remap[index];
	}
	mesh.positions = std::move(out.positions);
	mesh.normals = std::move(out.normals);
	mesh.uvs = std::move(out.uvs);
	mesh.tangents = std::move(out.tangents);
}

ImportedMesh unitSphere()
{
	ImportedMesh mesh;
	mesh.name = "sphere";
	const int rings = 32;
	const int segments = 64;
	for (int j = 0; j <= rings; j++) {
		const float polar = glm::pi<float>() * j / rings;
		for (int i = 0; i <= segments; i++) {
			const float phi = 2.f * glm::pi<float>() * i / segments;
			const glm::vec3 n(std::sin(polar) * std::cos(phi), std::cos(polar), std::sin(polar) * std::sin(phi));
			mesh.positions.push_back(n);
			mesh.normals.push_back(n);
			mesh.uvs.emplace_back((float)i / segments, (float)j / rings);
		}
	}
	for (int j = 0; j < rings; j++) {
		for (int i = 0; i < segments; i++) {
			const uint32_t a = j * (segments + 1) + i;
			const uint32_t b = a + segments + 1;
			mesh.indices.insert(mesh.indices.end(), { a, a + 1, b, a + 1, b + 1, b });
		}
	}
	return mesh;
}

bool glTFImage(const std::filesystem::path& path)
{
	std::string ext = path.extension().string();
	for (char& c : ext) {
		c = (char)std::tolower((unsigned char)c);
	}
	return ext == ".png" || ext == ".jpg" || ext == ".jpeg";
}

}

int main(int argc, char** argv)
{
	if (argc < 3) {
		fmt::println("usage: pbrt_to_gltf SCENE.pbrt OUT.gltf [--instancing] [--weld]");
		return 2;
	}
	const std::filesystem::path input = argv[1];
	const std::filesystem::path output = std::filesystem::absolute(argv[2]);
	bool instancing = false;
	bool welding = false;
	for (int i = 3; i < argc; i++) {
		instancing = instancing || std::strcmp(argv[i], "--instancing") == 0;
		welding = welding || std::strcmp(argv[i], "--weld") == 0;
	}

	ImportedModel model;
	std::string error;
	if (!importPbrtFile(input, model, &error)) {
		fmt::println("pbrt_to_gltf: {}", error);
		return 1;
	}

	Writer w;
	std::string images;
	std::string textures;
	int imageCount = 0;
	std::map<int, int> textureOf;
	auto texture = [&](int image) -> int {
		if (image < 0 || !glTFImage(model.images[(size_t)image].path)) {
			return -1;
		}
		if (const auto found = textureOf.find(image); found != textureOf.end()) {
			return found->second;
		}
		const std::string uri = std::filesystem::relative(std::filesystem::absolute(model.images[(size_t)image].path), output.parent_path()).generic_string();
		images += fmt::format(R"({}{{"uri":"{}"}})", imageCount == 0 ? "" : ",", uri);
		textures += fmt::format(R"({}{{"source":{}}})", imageCount == 0 ? "" : ",", imageCount);
		return textureOf[image] = imageCount++;
	};

	//materials, then one more for spheres without a material of their own
	std::string materials;
	bool transmission = false;
	bool emissiveStrength = false;
	for (size_t i = 0; i < model.materials.size(); i++) {
		const ImportedMaterial& m = model.materials[i];
		const MaterialDef& d = m.definition;
		glm::vec3 base = materialColor(d, "albedo");
		float metallic = 0.f;
		float roughness = 1.f;
		std::string extensions;
		std::string emissive;
		switch (d.type) {
		case MaterialType::Pbr:
			metallic = materialFloat(d, "metallic");
			roughness = materialFloat(d, "roughness");
			break;
		case MaterialType::Dielectric:
			base = glm::vec3(1.f);
			roughness = 0.f;
			extensions = fmt::format(R"("KHR_materials_transmission":{{"transmissionFactor":1}},"KHR_materials_ior":{{"ior":{}}})", num(materialFloat(d, "ir")));
			transmission = true;
			break;
		case MaterialType::Emissive:
			emissive = fmt::format(R"(,"emissiveFactor":{})", vec(base));
			extensions = fmt::format(R"("KHR_materials_emissive_strength":{{"emissiveStrength":{}}})", num(materialFloat(d, "strength")));
			base = glm::vec3(0.f);
			emissiveStrength = true;
			break;
		case MaterialType::Metal:
			metallic = 1.f;
			roughness = materialFloat(d, "fuzz");
			break;
		default:
			break;
		}
		const int baseTexture = texture(m.baseColorImage);
		const int normalTexture = texture(m.normalImage);
		materials += fmt::format(R"({}{{"name":"{}","pbrMetallicRoughness":{{"baseColorFactor":[{},{},{},1],"metallicFactor":{},"roughnessFactor":{}{}}}{}{}{}}})",
			i == 0 ? "" : ",", d.name, num(base.x), num(base.y), num(base.z), num(metallic), num(roughness),
			baseTexture >= 0 ? fmt::format(R"(,"baseColorTexture":{{"index":{}}})", baseTexture) : "",
			normalTexture >= 0 ? fmt::format(R"(,"normalTexture":{{"index":{}}})", normalTexture) : "", emissive,
			extensions.empty() ? "" : ",\"extensions\":{" + extensions + "}");
	}

	//the meshes, spheres as one shared unit sphere at the end
	std::vector<ImportedMesh>& meshes = model.meshes;
	const int sphereMesh = model.shapes.empty() ? -1 : (int)meshes.size();
	if (sphereMesh >= 0) {
		meshes.push_back(unitSphere());
	}
	size_t vertices = 0;
	size_t triangles = 0;
	std::string meshJson;
	for (size_t i = 0; i < meshes.size(); i++) {
		ImportedMesh& mesh = meshes[i];
		if (welding) {
			weld(mesh);
		}
		vertices += mesh.positions.size();
		triangles += mesh.indices.size() / 3;
		glm::vec3 lo(std::numeric_limits<float>::max());
		glm::vec3 hi(-std::numeric_limits<float>::max());
		for (const glm::vec3& p : mesh.positions) {
			lo = glm::min(lo, p);
			hi = glm::max(hi, p);
		}
		const int position = w.add(mesh.positions.data(), mesh.positions.size() * 12, FLOAT, mesh.positions.size(), "VEC3", ARRAY_BUFFER,
			fmt::format(R"(,"min":{},"max":{})", vec(lo), vec(hi)));
		std::string attributes = fmt::format(R"("POSITION":{})", position);
		if (mesh.normals.size() == mesh.positions.size()) {
			for (glm::vec3& n : mesh.normals) {
				n = glm::dot(n, n) > 0.f ? glm::normalize(n) : glm::vec3(0.f, 0.f, 1.f);
			}
			attributes += fmt::format(R"(,"NORMAL":{})", w.add(mesh.normals.data(), mesh.normals.size() * 12, FLOAT, mesh.normals.size(), "VEC3", ARRAY_BUFFER));
		}
		if (mesh.uvs.size() == mesh.positions.size()) {
			attributes += fmt::format(R"(,"TEXCOORD_0":{})", w.add(mesh.uvs.data(), mesh.uvs.size() * 8, FLOAT, mesh.uvs.size(), "VEC2", ARRAY_BUFFER));
		}
		std::string primitives;
		const std::vector<ImportedSurface> surfaces = mesh.surfaces.empty() ? std::vector<ImportedSurface> { { 0, (uint32_t)mesh.indices.size(), 0 } } : mesh.surfaces;
		for (size_t s = 0; s < surfaces.size(); s++) {
			const ImportedSurface& surface = surfaces[s];
			const int indices = w.add(mesh.indices.data() + surface.firstIndex, surface.indexCount * 4u, UINT, surface.indexCount, "SCALAR", ELEMENT_ARRAY_BUFFER);
			const int material = (int)i == sphereMesh ? -1 : (int)surface.material;
			primitives += fmt::format(R"({}{{"attributes":{{{}}},"indices":{}{}}})", s == 0 ? "" : ",", attributes, indices,
				material >= 0 ? fmt::format(R"(,"material":{})", material) : "");
		}
		meshJson += fmt::format(R"({}{{"name":"{}","primitives":[{}]}})", i == 0 ? "" : ",", mesh.name, primitives);
	}

	//the placements: a node each, or one EXT_mesh_gpu_instancing node per mesh placed at least twice
	struct Placement {
		uint32_t mesh;
		glm::mat4 transform;
		std::string name;
	};
	std::vector<Placement> placements;
	for (const ImportedNode& node : model.nodes) {
		placements.push_back({ node.mesh, node.transform, node.name });
	}
	for (const ImportedShape& shape : model.shapes) {
		placements.push_back({ (uint32_t)sphereMesh, glm::translate(glm::mat4(1.f), shape.shape.position) * glm::scale(glm::mat4(1.f), shape.shape.size), shape.shape.name });
	}
	std::string nodes;
	std::string roots;
	int nodeCount = 0;
	bool usedInstancing = false;
	auto addNode = [&](const std::string& body) {
		nodes += (nodeCount == 0 ? "" : ",") + body;
		roots += fmt::format("{}{}", nodeCount == 0 ? "" : ",", nodeCount);
		nodeCount++;
	};
	std::map<uint32_t, std::vector<size_t>> byMesh;
	for (size_t i = 0; i < placements.size(); i++) {
		byMesh[placements[i].mesh].push_back(i);
	}
	auto matrixNode = [&](const Placement& p) {
		std::string m;
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				m += fmt::format("{}{}", c == 0 && r == 0 ? "" : ",", num(p.transform[c][r]));
			}
		}
		addNode(fmt::format(R"({{"name":"{}","mesh":{},"matrix":[{}]}})", p.name, p.mesh, m));
	};
	size_t instancedPlacements = 0;
	for (const auto& [mesh, indices] : byMesh) {
		std::vector<glm::vec3> translations;
		std::vector<glm::quat> rotations;
		std::vector<glm::vec3> scales;
		bool decomposable = instancing && indices.size() >= 2;
		for (size_t index : indices) {
			if (!decomposable) {
				break;
			}
			glm::vec3 scale;
			glm::quat rotation;
			glm::vec3 translation;
			glm::vec3 skew;
			glm::vec4 perspective;
			const glm::mat4& m = placements[index].transform;
			//a mirrored transform (every one, when the import mirrors pbrt's world) is a rotation and a scale with one
			//negative axis: decompose it mirrored back in z, then negate that scale. A sheared one has no TRS at all
			const bool mirrored = glm::determinant(glm::mat3(m)) < 0.f;
			const glm::mat4 unmirrored = mirrored ? m * glm::scale(glm::mat4(1.f), glm::vec3(1.f, 1.f, -1.f)) : m;
			decomposable = glm::decompose(unmirrored, scale, rotation, translation, skew, perspective);
			if (mirrored) {
				scale.z = -scale.z;
			}
			const glm::mat4 rebuilt = glm::translate(glm::mat4(1.f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.f), scale);
			for (int c = 0; c < 4 && decomposable; c++) {
				decomposable = glm::length(rebuilt[c] - m[c]) <= 1e-3f * std::max(1.f, glm::length(m[c]));
			}
			translations.push_back(translation);
			rotations.push_back(glm::normalize(rotation));
			scales.push_back(scale);
		}
		if (!decomposable) {
			for (size_t index : indices) {
				matrixNode(placements[index]);
			}
			continue;
		}
		std::vector<float> r;
		for (const glm::quat& q : rotations) {
			r.insert(r.end(), { q.x, q.y, q.z, q.w });
		}
		const int t = w.add(translations.data(), translations.size() * 12, FLOAT, translations.size(), "VEC3", 0);
		const int ro = w.add(r.data(), r.size() * 4, FLOAT, rotations.size(), "VEC4", 0);
		const int s = w.add(scales.data(), scales.size() * 12, FLOAT, scales.size(), "VEC3", 0);
		addNode(fmt::format(R"({{"name":"{} x{}","mesh":{},"extensions":{{"EXT_mesh_gpu_instancing":{{"attributes":{{"TRANSLATION":{},"ROTATION":{},"SCALE":{}}}}}}}}})",
			meshes[mesh].name, indices.size(), mesh, t, ro, s));
		usedInstancing = true;
		instancedPlacements += indices.size();
	}

	//the camera
	std::string cameras;
	if (!model.cameras.empty() && model.settings.has_value()) {
		const SceneCamera& c = model.cameras[0];
		const float aspect = (float)model.settings->render.width / (float)model.settings->render.height;
		cameras = fmt::format(R"({{"type":"perspective","perspective":{{"yfov":{},"aspectRatio":{},"znear":0.01}}}})", num(glm::radians(c.vfovDegrees)), num(aspect));
		addNode(fmt::format(R"({{"name":"{}","camera":0,"translation":{},"rotation":[{},{},{},{}]}})", c.name, vec(c.position), num(c.orientation.x),
			num(c.orientation.y), num(c.orientation.z), num(c.orientation.w)));
	}

	//the lights, back in photometric units: candela for point and spot, lux for directional
	std::string lights;
	for (size_t i = 0; i < model.lights.size(); i++) {
		const SceneLight& l = model.lights[i];
		const float intensity = l.intensity / PHOTOMETRIC_TO_RADIOMETRIC;
		std::string type = l.kind == LightKind::Point ? "point" : l.kind == LightKind::Spot ? "spot" : "directional";
		std::string spot = l.kind == LightKind::Spot ? fmt::format(R"(,"spot":{{"innerConeAngle":{},"outerConeAngle":{}}})", num(glm::radians(l.innerConeDegrees)), num(glm::radians(l.outerConeDegrees))) : "";
		lights += fmt::format(R"({}{{"type":"{}","color":{},"intensity":{}{}}})", i == 0 ? "" : ",", type, vec(exactRgb(l.color, SpectrumRole::Illuminant, {})), num(intensity), spot);
		addNode(fmt::format(R"({{"name":"{}","translation":{},"rotation":[{},{},{},{}],"extensions":{{"KHR_lights_punctual":{{"light":{}}}}}}})", l.name, vec(l.position),
			num(l.orientation.x), num(l.orientation.y), num(l.orientation.z), num(l.orientation.w), i));
	}

	std::vector<std::string> used;
	if (usedInstancing) {
		used.push_back("EXT_mesh_gpu_instancing");
	}
	if (transmission) {
		used.push_back("KHR_materials_transmission");
		used.push_back("KHR_materials_ior");
	}
	if (emissiveStrength) {
		used.push_back("KHR_materials_emissive_strength");
	}
	if (!lights.empty()) {
		used.push_back("KHR_lights_punctual");
	}
	std::string extensionsUsed;
	for (size_t i = 0; i < used.size(); i++) {
		extensionsUsed += fmt::format(R"({}"{}")", i == 0 ? "" : ",", used[i]);
	}

	std::filesystem::path binPath = output;
	binPath.replace_extension(".bin");
	std::string json = fmt::format(R"({{"asset":{{"version":"2.0","generator":"vulkan_guide pbrt_to_gltf"}},"extensionsUsed":[{}],"scene":0,"scenes":[{{"nodes":[{}]}}],"nodes":[{}],"meshes":[{}],"materials":[{}])",
		extensionsUsed, roots, nodes, meshJson, materials);
	if (imageCount > 0) {
		json += fmt::format(R"(,"images":[{}],"textures":[{}])", images, textures);
	}
	if (!cameras.empty()) {
		json += fmt::format(R"(,"cameras":[{}])", cameras);
	}
	if (!lights.empty()) {
		json += fmt::format(R"(,"extensions":{{"KHR_lights_punctual":{{"lights":[{}]}}}})", lights);
	}
	json += fmt::format(R"(,"buffers":[{{"uri":"{}","byteLength":{}}}],"bufferViews":[{}],"accessors":[{}]}})", binPath.filename().string(), w.bin.size(), w.bufferViews, w.accessors);
	std::ofstream(output, std::ios::binary) << json;
	std::ofstream(binPath, std::ios::binary).write(w.bin.data(), (std::streamsize)w.bin.size());

	if (model.environment.has_value()) {
		Image environment;
		environment.width = model.environment->width;
		environment.height = model.environment->height;
		environment.rgb.resize(environment.pixelCount() * 3);
		for (size_t p = 0; p < environment.pixelCount(); p++) {
			for (int c = 0; c < 3; c++) {
				environment.rgb[p * 3 + c] = model.environment->rgba[p * 4 + c] * model.environment->intensity;
			}
		}
		std::filesystem::path envPath = output;
		envPath.replace_filename(output.stem().string() + "_environment.hdr");
		saveImage(envPath, environment);
	}

	fmt::println("pbrt_to_gltf: wrote {} ({:.1f} MB): {} meshes, {} vertices, {} triangles, {} nodes ({} placements as instances), {} materials, {} images{}",
		output.filename().string(), w.bin.size() / 1e6, meshes.size(), vertices, triangles, nodeCount, instancedPlacements, model.materials.size(), imageCount,
		model.environment.has_value() ? ", and the environment map" : "");
	return 0;
}
