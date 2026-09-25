#include <vk_loader.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <iostream>
#include <span>

#include "vk_engine.h"
#include "vk_initializers.h"
#include "vk_types.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>


// A LoadedGLTF map key for a glTF object: its name, or "<kind><index>" when it has none, made
// unique if another object already took it. Names are optional in glTF and need not be unique;
// keying by the bare name collapsed Sponza's 25 unnamed materials and 69 images to one entry each,
// so the path tracer saw one texture and clearAll() destroyed one image
template <typename T>
static std::string uniqueKey(const std::unordered_map<std::string, T>& map, std::string_view name, std::string_view kind, size_t index)
{
	std::string key = name.empty() ? fmt::format("{}{}", kind, index) : std::string(name);
	if (map.count(key) == 0) {
		return key;
	}
	for (size_t suffix = 1;; suffix++) {
		std::string candidate = fmt::format("{}#{}", key, suffix);
		if (map.count(candidate) == 0) {
			return candidate;
		}
	}
}

VkFilter extract_filter(fastgltf::Filter filter)
{
	switch (filter) {
	// nearest samplers
	case fastgltf::Filter::Nearest:
	case fastgltf::Filter::NearestMipMapNearest:
	case fastgltf::Filter::NearestMipMapLinear:
		return VK_FILTER_NEAREST;

	// linear samplers
	case fastgltf::Filter::Linear:
	case fastgltf::Filter::LinearMipMapNearest:
	case fastgltf::Filter::LinearMipMapLinear:
	default:
		return VK_FILTER_LINEAR;
	}
}

VkSamplerMipmapMode extract_mipmap_mode(fastgltf::Filter filter)
{
	switch (filter) {
	case fastgltf::Filter::NearestMipMapNearest:
	case fastgltf::Filter::LinearMipMapNearest:
		return VK_SAMPLER_MIPMAP_MODE_NEAREST;

	case fastgltf::Filter::NearestMipMapLinear:
	case fastgltf::Filter::LinearMipMapLinear:
	default:
		return VK_SAMPLER_MIPMAP_MODE_LINEAR;
	}
}

// Per-vertex tangents from the uv gradients of the triangles around each vertex (Lengyel,
// "Computing Tangent Space Basis Vectors for an Arbitrary Mesh"), Gram-Schmidt'ed against the
// normal, for a primitive whose file carries no TANGENT. Not MikkTSpace, so a normal map baked
// against MikkTSpace tangents can show faint seams here that it would not with the file's own
static void generateTangents(std::span<const uint32_t> indices, std::span<const Vertex> vertices, std::span<glm::vec4> tangents, size_t firstVertex)
{
	std::vector<glm::vec3> tan(tangents.size(), glm::vec3(0.f));
	std::vector<glm::vec3> bitan(tangents.size(), glm::vec3(0.f));
	for (size_t i = 0; i + 2 < indices.size(); i += 3) {
		const size_t a = indices[i] - firstVertex;
		const size_t b = indices[i + 1] - firstVertex;
		const size_t c = indices[i + 2] - firstVertex;
		if (a >= tangents.size() || b >= tangents.size() || c >= tangents.size()) {
			continue;
		}
		const glm::vec3 e1 = vertices[b].position - vertices[a].position;
		const glm::vec3 e2 = vertices[c].position - vertices[a].position;
		const glm::vec2 d1 = glm::vec2(vertices[b].uv_x, vertices[b].uv_y) - glm::vec2(vertices[a].uv_x, vertices[a].uv_y);
		const glm::vec2 d2 = glm::vec2(vertices[c].uv_x, vertices[c].uv_y) - glm::vec2(vertices[a].uv_x, vertices[a].uv_y);
		const float det = d1.x * d2.y - d2.x * d1.y;
		//a triangle with degenerate uvs has no tangent direction to contribute
		if (std::abs(det) < 1e-12f) {
			continue;
		}
		const glm::vec3 t = (e1 * d2.y - e2 * d1.y) / det;
		const glm::vec3 bt = (e2 * d1.x - e1 * d2.x) / det;
		for (const size_t v : { a, b, c }) {
			tan[v] += t;
			bitan[v] += bt;
		}
	}
	for (size_t v = 0; v < tangents.size(); v++) {
		const glm::vec3 n = vertices[v].normal;
		const glm::vec3 t = tan[v] - n * glm::dot(n, tan[v]);
		if (glm::dot(t, t) < 1e-20f) {
			tangents[v] = glm::vec4(0.f, 0.f, 0.f, 1.f);
			continue;
		}
		const float handedness = glm::dot(glm::cross(n, t), bitan[v]) < 0.f ? -1.f : 1.f;
		tangents[v] = glm::vec4(glm::normalize(t), handedness);
	}
}

static GeoSurface loadPrimitiveGeometry(fastgltf::Asset& gltf, fastgltf::Primitive& p,
	std::vector<uint32_t>& indices, std::vector<Vertex>& vertices, std::vector<glm::vec4>& tangents, size_t& outInitialVertex)
{
	GeoSurface newSurface;
	newSurface.startIndex = (uint32_t)indices.size();
	newSurface.count = (uint32_t)gltf.accessors[p.indicesAccessor.value()].count;

	size_t initial_vtx = vertices.size();
	outInitialVertex = initial_vtx;

	// load indexes
	{
		fastgltf::Accessor& indexaccessor = gltf.accessors[p.indicesAccessor.value()];
		indices.reserve(indices.size() + indexaccessor.count);

		fastgltf::iterateAccessor<std::uint32_t>(gltf, indexaccessor,
			[&](std::uint32_t idx) {
				indices.push_back(idx + initial_vtx);
			});
	}

	// load vertex positions
	{
		fastgltf::Accessor& posAccessor = gltf.accessors[p.findAttribute("POSITION")->accessorIndex];
		vertices.resize(vertices.size() + posAccessor.count);

		fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, posAccessor,
			[&](glm::vec3 v, size_t index) {
				Vertex newvtx;
				newvtx.position = v;
				newvtx.normal = { 1, 0, 0 };
				newvtx.color = glm::vec4 { 1.f };
				newvtx.uv_x = 0;
				newvtx.uv_y = 0;
				vertices[initial_vtx + index] = newvtx;
			});
	}

	// load vertex normals
	auto normals = p.findAttribute("NORMAL");
	if (normals != p.attributes.end()) {

		fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[(*normals).accessorIndex],
			[&](glm::vec3 v, size_t index) {
				vertices[initial_vtx + index].normal = v;
			});
	}

	// load UVs
	auto uv = p.findAttribute("TEXCOORD_0");
	if (uv != p.attributes.end()) {

		fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[(*uv).accessorIndex],
			[&](glm::vec2 v, size_t index) {
				vertices[initial_vtx + index].uv_x = v.x;
				vertices[initial_vtx + index].uv_y = v.y;
			});
	}

	// load vertex colors
	auto colors = p.findAttribute("COLOR_0");
	if (colors != p.attributes.end()) {

		fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[(*colors).accessorIndex],
			[&](glm::vec4 v, size_t index) {
				vertices[initial_vtx + index].color = v;
			});
	}

	// load tangents, for the path tracer's normal maps: the file's own, else generated from the uvs
	tangents.resize(vertices.size(), glm::vec4(0.f, 0.f, 0.f, 1.f));
	auto tangentAttribute = p.findAttribute("TANGENT");
	if (tangentAttribute != p.attributes.end()) {
		fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[(*tangentAttribute).accessorIndex],
			[&](glm::vec4 v, size_t index) {
				tangents[initial_vtx + index] = v;
			});
	} else if (uv != p.attributes.end()) {
		generateTangents(std::span(indices).subspan(newSurface.startIndex), std::span(vertices).subspan(initial_vtx),
			std::span(tangents).subspan(initial_vtx), initial_vtx);
	}

	return newSurface;
}

std::optional<std::vector<std::shared_ptr<MeshAsset>>> loadGltfMeshes(VulkanEngine* engine, std::filesystem::path filePath)
{
	std::cout << "Loading GLTF: " << filePath << std::endl;

	auto data = fastgltf::GltfDataBuffer::FromPath(filePath);
	if (data.error() != fastgltf::Error::None) {
		// The file couldn't be loaded, or the buffer could not be allocated.
		// give error
	}

	constexpr auto gltfOptions = fastgltf::Options::LoadExternalBuffers;

	fastgltf::Asset gltf;
	fastgltf::Parser parser;

	auto load = parser.loadGltf(data.get(), filePath.parent_path(), gltfOptions);
	if (auto error = load.error(); error != fastgltf::Error::None) {
		fmt::print("Failed to load glTF: {} \n", fastgltf::to_underlying(load.error()));
		return {};
	}
	gltf = std::move(load.get());

	std::vector<std::shared_ptr<MeshAsset>> meshes;

	// use the same vectors for all meshes so that the memory doesnt reallocate as
	// often
	std::vector<uint32_t> indices;
	std::vector<Vertex> vertices;
	std::vector<glm::vec4> tangents;
	for (fastgltf::Mesh& mesh : gltf.meshes) {
		MeshAsset newmesh;

		newmesh.name = mesh.name;

		// clear the mesh arrays each mesh, we dont want to merge them by error
		indices.clear();
		vertices.clear();
		tangents.clear();

		for (auto&& p : mesh.primitives) {
			size_t initialVertex;
			GeoSurface newSurface = loadPrimitiveGeometry(gltf, p, indices, vertices, tangents, initialVertex);
			newmesh.surfaces.push_back(newSurface);
		}

		// display the vertex normals
		constexpr bool OverrideColors = true;
		if (OverrideColors) {
			for (Vertex& vtx : vertices) {
				vtx.color = glm::vec4(vtx.normal, 1.f);
			}
		}
		//retained cpu-side copy of exactly what goes to the gpu (see MeshAsset::cpuVertices)
		newmesh.cpuIndices = indices;
		newmesh.cpuVertices = vertices;

		newmesh.meshBuffers = engine->uploadMesh(indices, vertices);

		meshes.emplace_back(std::make_shared<MeshAsset>(std::move(newmesh)));
	}

	return meshes;
}

std::optional<std::shared_ptr<LoadedGLTF>> loadGltf(VulkanEngine* engine, std::string_view filePath, std::string* outError)
{
	fmt::println("Loading GLTF: {}", filePath);

	std::shared_ptr<LoadedGLTF> scene = std::make_shared<LoadedGLTF>();
	scene->creator = engine;
	LoadedGLTF& file = *scene.get();

	//extensions fastgltf is not told about are skipped silently, and a file that *requires* one of
	//them fails to load. KHR_lights_punctual is imported as data only (LoadedGLTF::lights)
	fastgltf::Parser parser { fastgltf::Extensions::KHR_lights_punctual | fastgltf::Extensions::KHR_materials_emissive_strength };

	constexpr auto gltfOptions = fastgltf::Options::DontRequireValidAssetMember | fastgltf::Options::AllowDouble | fastgltf::Options::LoadExternalBuffers;

	std::filesystem::path path = filePath;

	//fastgltf's own message for a missing file blames the directory, so check first
	if (std::error_code ec; !std::filesystem::exists(path, ec)) {
		const std::string message = fmt::format("No file at '{}'", path.string());
		fmt::println("{}", message);
		if (outError != nullptr) {
			*outError = message;
		}
		return {};
	}

	auto dataBuffer = fastgltf::GltfDataBuffer::FromPath(path);
	if (dataBuffer.error() != fastgltf::Error::None) {
		const std::string message = fmt::format("Could not read '{}': {}", path.string(), fastgltf::getErrorMessage(dataBuffer.error()));
		fmt::println("{}", message);
		if (outError != nullptr) {
			*outError = message;
		}
		return {};
	}

	fastgltf::Asset gltf;

	auto load = parser.loadGltf(dataBuffer.get(), path.parent_path(), gltfOptions);
	if (load.error() != fastgltf::Error::None) {
		const std::string message = fmt::format("'{}' is not a readable glTF file: {}", path.string(), fastgltf::getErrorMessage(load.error()));
		fmt::println("{}", message);
		if (outError != nullptr) {
			*outError = message;
		}
		return {};
	}
	gltf = std::move(load.get());

		// we can stimate the descriptors we will need accurately
	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };

	file.descriptorPool.init(engine->m_device, gltf.materials.size(), sizes);

	// load samplers
	for (fastgltf::Sampler& sampler : gltf.samplers) {

		VkSamplerCreateInfo sampl = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = nullptr};
		sampl.maxLod = VK_LOD_CLAMP_NONE;
		sampl.minLod = 0;

		sampl.magFilter = extract_filter(sampler.magFilter.value_or(fastgltf::Filter::Nearest));
		sampl.minFilter = extract_filter(sampler.minFilter.value_or(fastgltf::Filter::Nearest));

		sampl.mipmapMode= extract_mipmap_mode(sampler.minFilter.value_or(fastgltf::Filter::Nearest));

		VkSampler newSampler;
		vkCreateSampler(engine->m_device, &sampl, nullptr, &newSampler);

		file.samplers.push_back(newSampler);
	}

	// temporal arrays for all the objects to use while creating the GLTF data
	std::vector<std::shared_ptr<MeshAsset>> meshes;
	std::vector<std::shared_ptr<Node>> nodes;
	std::vector<AllocatedImage> images;
	std::vector<std::shared_ptr<GLTFMaterial>> materials;

	// load all textures
	for (fastgltf::Image& image : gltf.images) {
		std::optional<AllocatedImage> img = load_image(engine, gltf, image, path.parent_path());

		if (img.has_value()) {
			images.push_back(*img);
			file.images[uniqueKey(file.images, image.name, "image", images.size() - 1)] = *img;
		}
		else {
			// we failed to load, so lets give the slot a default white texture to not
			// completely break loading
			images.push_back(engine->m_errorCheckerboardImage);
			std::cout << "gltf failed to load texture " << image.name << std::endl;
		}
	}

	// create buffer to hold the material data
	file.materialDataBuffer = engine->createBuffer(sizeof(GLTFMetallic_Roughness::MaterialConstants) * gltf.materials.size(),
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	int data_index = 0;
	GLTFMetallic_Roughness::MaterialConstants* sceneMaterialConstants = (GLTFMetallic_Roughness::MaterialConstants*)file.materialDataBuffer.info.pMappedData;

	// load materials
	for (fastgltf::Material& mat : gltf.materials) {
		std::shared_ptr<GLTFMaterial> newMat = std::make_shared<GLTFMaterial>();
		materials.push_back(newMat);
		file.materials[uniqueKey(file.materials, mat.name, "material", materials.size() - 1)] = newMat;

		GLTFMetallic_Roughness::MaterialConstants constants;
		constants.colorFactors.x = mat.pbrData.baseColorFactor[0];
		constants.colorFactors.y = mat.pbrData.baseColorFactor[1];
		constants.colorFactors.z = mat.pbrData.baseColorFactor[2];
		constants.colorFactors.w = mat.pbrData.baseColorFactor[3];

		constants.metalRoughFactors.x = mat.pbrData.metallicFactor;
		constants.metalRoughFactors.y = mat.pbrData.roughnessFactor;
		// write material parameters to buffer. The whole buffer is flushed after the loop
		sceneMaterialConstants[data_index] = constants;

		// keep the same factors cpu-side (see GLTFMaterial::colorFactors)
		newMat->colorFactors = constants.colorFactors;
		newMat->metalRoughFactors = glm::vec2(constants.metalRoughFactors);
		newMat->emissiveFactor = glm::vec3(mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2]) * float(mat.emissiveStrength);
		newMat->alphaCutoff = mat.alphaCutoff;
		switch (mat.alphaMode) {
		case fastgltf::AlphaMode::Opaque:
			newMat->alphaMode = GltfAlphaMode::Opaque;
			break;
		case fastgltf::AlphaMode::Mask:
			newMat->alphaMode = GltfAlphaMode::Mask;
			break;
		case fastgltf::AlphaMode::Blend:
			newMat->alphaMode = GltfAlphaMode::Blend;
			break;
		}

		MaterialPass passType = MaterialPass::MainColor;
		if (mat.alphaMode == fastgltf::AlphaMode::Blend) {
			passType = MaterialPass::Transparent;
		}

		GLTFMetallic_Roughness::MaterialResources materialResources;
		// default the material textures
		materialResources.colorImage = engine->m_whiteImage;
		materialResources.colorSampler = engine->m_defaultSamplerLinear;
		materialResources.metalRoughImage = engine->m_whiteImage;
		materialResources.metalRoughSampler = engine->m_defaultSamplerLinear;

		// set the uniform buffer for the material data
		materialResources.dataBuffer = file.materialDataBuffer.buffer;
		materialResources.dataBufferOffset = data_index * sizeof(GLTFMetallic_Roughness::MaterialConstants);
		// grab textures from gltf file. A texture may name no image (one only an extension such as
		// KHR_texture_basisu provides) or no sampler (the spec's default: repeat, auto filtering);
		// either used to throw bad_optional_access and take the whole load down
		auto textureImage = [&](size_t textureIndex) -> std::optional<AllocatedImage> {
			if (textureIndex >= gltf.textures.size() || !gltf.textures[textureIndex].imageIndex.has_value()) {
				return std::nullopt;
			}
			const size_t image = gltf.textures[textureIndex].imageIndex.value();
			return image < images.size() ? std::optional(images[image]) : std::nullopt;
		};
		if (mat.pbrData.baseColorTexture.has_value()) {
			const size_t texture = mat.pbrData.baseColorTexture->textureIndex;
			if (auto image = textureImage(texture)) {
				materialResources.colorImage = *image;
				const auto sampler = gltf.textures[texture].samplerIndex;
				if (sampler.has_value() && *sampler < file.samplers.size()) {
					materialResources.colorSampler = file.samplers[*sampler];
				}
				//kept for the path tracer's texture array (see GLTFMaterial::baseColorImage)
				newMat->baseColorImage = *image;
			}
		}
		// the raster shader has a metal/rough binding it never samples; only the path tracer reads these
		if (mat.pbrData.metallicRoughnessTexture.has_value()) {
			newMat->metalRoughImage = textureImage(mat.pbrData.metallicRoughnessTexture->textureIndex).value_or(AllocatedImage {});
		}
		if (mat.normalTexture.has_value()) {
			newMat->normalImage = textureImage(mat.normalTexture->textureIndex).value_or(AllocatedImage {});
			newMat->normalScale = mat.normalTexture->scale;
		}
		if (mat.emissiveTexture.has_value()) {
			newMat->emissiveImage = textureImage(mat.emissiveTexture->textureIndex).value_or(AllocatedImage {});
		}
		if (newMat->alphaMode == GltfAlphaMode::Blend) {
			fmt::println("loadGltf: material '{}' is alpha-blended; the path tracer draws it opaque", std::string_view(mat.name));
		}
		// build material
		newMat->data = engine->m_metalRoughMaterial.writeMaterial(engine->m_device, passType, materialResources, file.descriptorPool);

		data_index++;
	}

	//MoltenVK exposes no coherent host memory: without this the gpu reads whatever the block held
	//before. At startup that happened to be fresh zero-initialised memory and the factors landed by
	//luck; a scene loaded later reuses a freed block and every material came back black
	vmaFlushAllocation(engine->m_memAllocator, file.materialDataBuffer.allocation, 0, sizeof(GLTFMetallic_Roughness::MaterialConstants) * gltf.materials.size());

	// use the same vectors for all meshes so that the memory doesnt reallocate as
	// often
	std::vector<uint32_t> indices;
	std::vector<Vertex> vertices;
	std::vector<glm::vec4> tangents;

	for (fastgltf::Mesh& mesh : gltf.meshes) {
		std::shared_ptr<MeshAsset> newmesh = std::make_shared<MeshAsset>();
		meshes.push_back(newmesh);
		file.meshes[uniqueKey(file.meshes, mesh.name, "mesh", meshes.size() - 1)] = newmesh;
		newmesh->name = mesh.name;

		// clear the mesh arrays each mesh, we dont want to merge them by error
		indices.clear();
		vertices.clear();
		tangents.clear();

		for (auto&& p : mesh.primitives) {
			size_t initialVertex;
			GeoSurface newSurface = loadPrimitiveGeometry(gltf, p, indices, vertices, tangents, initialVertex);

			if (p.materialIndex.has_value()) {
				newSurface.material = materials[p.materialIndex.value()];
			} else {
				newSurface.material = materials[0];
			}

			// loop the vertices of this surface, find min/max bounds
			glm::vec3 minpos = vertices[initialVertex].position;
			glm::vec3 maxpos = vertices[initialVertex].position;
			for (int i = initialVertex; i < vertices.size(); i++) {
				minpos = glm::min(minpos, vertices[i].position);
				maxpos = glm::max(maxpos, vertices[i].position);
			}
			// calculate origin and extents from the min/max, use extent length for radius
			newSurface.bounds.origin = (maxpos + minpos) / 2.f;
			newSurface.bounds.extents = (maxpos - minpos) / 2.f;
			newSurface.bounds.sphereRadius = glm::length(newSurface.bounds.extents);

			newmesh->surfaces.push_back(newSurface);
		}

		//retained cpu-side copy of exactly what goes to the gpu (see MeshAsset::cpuVertices)
		newmesh->cpuIndices = indices;
		newmesh->cpuVertices = vertices;
		newmesh->cpuTangents = tangents;

		newmesh->meshBuffers = engine->uploadMesh(indices, vertices);
	}

	// load all nodes and their meshes
	for (fastgltf::Node& node : gltf.nodes) {
		std::shared_ptr<Node> newNode;

		// find if the node has a mesh, and if it does hook it to the mesh pointer and allocate it with the meshnode class
		if (node.meshIndex.has_value()) {
			newNode = std::make_shared<MeshNode>();
			static_cast<MeshNode*>(newNode.get())->mesh = meshes[*node.meshIndex];
		} else {
			newNode = std::make_shared<Node>();
		}

		newNode->name = node.name;

		nodes.push_back(newNode);
		file.nodes[uniqueKey(file.nodes, node.name, "node", nodes.size() - 1)] = newNode;

		std::visit(fastgltf::visitor { [&](fastgltf::math::fmat4x4 matrix) {
										  memcpy(&newNode->localTransform, matrix.data(), sizeof(glm::mat4));
									  },
					   [&](fastgltf::TRS transform) {
						   glm::vec3 tl(transform.translation[0], transform.translation[1],
							   transform.translation[2]);
						   glm::quat rot(transform.rotation[3], transform.rotation[0], transform.rotation[1],
							   transform.rotation[2]);
						   glm::vec3 sc(transform.scale[0], transform.scale[1], transform.scale[2]);

						   glm::mat4 tm = glm::translate(glm::mat4(1.f), tl);
						   glm::mat4 rm = glm::toMat4(rot);
						   glm::mat4 sm = glm::scale(glm::mat4(1.f), sc);

						   newNode->localTransform = tm * rm * sm;
					   } },
			node.transform);
	}

	// run loop again to setup transform hierarchy
	for (int i = 0; i < gltf.nodes.size(); i++) {
		fastgltf::Node& node = gltf.nodes[i];
		std::shared_ptr<Node>& sceneNode = nodes[i];

		for (auto& c : node.children) {
			sceneNode->children.push_back(nodes[c]);
			nodes[c]->parent = sceneNode;
		}
	}

	// find the top nodes, with no parents
	for (auto& node : nodes) {
		if (node->parent.lock() == nullptr) {
			file.topNodes.push_back(node);
			node->refreshTransform(glm::mat4 { 1.f });
		}
	}

	// the punctual lights, placed by the nodes that reference them. Read after the transforms above
	// are resolved, since a light's pose is its node's world transform
	for (size_t i = 0; i < gltf.nodes.size(); i++) {
		const auto& lightIndex = gltf.nodes[i].lightIndex;
		if (!lightIndex.has_value() || *lightIndex >= gltf.lights.size()) {
			continue;
		}
		const fastgltf::Light& source = gltf.lights[*lightIndex];
		GltfPunctualLight light;
		light.name = source.name.empty() ? std::string(gltf.nodes[i].name) : std::string(source.name);
		switch (source.type) {
		case fastgltf::LightType::Directional:
			light.type = GltfPunctualLight::Type::Directional;
			break;
		case fastgltf::LightType::Point:
			light.type = GltfPunctualLight::Type::Point;
			break;
		case fastgltf::LightType::Spot:
			light.type = GltfPunctualLight::Type::Spot;
			break;
		}
		light.color = glm::vec3(source.color[0], source.color[1], source.color[2]);
		light.intensity = source.intensity;
		light.range = source.range.value_or(0.f);
		light.innerConeAngle = source.innerConeAngle.value_or(0.f);
		light.outerConeAngle = source.outerConeAngle.value_or(0.7853982f);
		light.worldTransform = nodes[i]->worldTransform;
		file.lights.push_back(std::move(light));
	}
	if (!file.lights.empty()) {
		fmt::println("loadGltf: {} punctual light(s) imported; the path tracer does not render them yet", file.lights.size());
	}
	return scene;
}

std::optional<AllocatedImage> load_image(VulkanEngine* engine, fastgltf::Asset& asset, fastgltf::Image& image, const std::filesystem::path& directory)
{
	AllocatedImage newImage {};

	int width, height, nrChannels;

	std::visit(
		fastgltf::visitor {
			[](auto& arg) {},
			[&](fastgltf::sources::URI& filePath) {
				//a bad image reference in a user-supplied file is not worth aborting over: the
				//caller substitutes the checkerboard for anything that fails here
				if (filePath.fileByteOffset != 0 || !filePath.uri.isLocalPath()) {
					fmt::println("load_image: unsupported image uri '{}' (only local files without byte offsets)", std::string_view(filePath.uri.string()));
					return;
				}

				//relative uris are relative to the glTF file, not to the working directory
				std::filesystem::path imagePath(std::string(filePath.uri.path().begin(), filePath.uri.path().end()));
				if (imagePath.is_relative()) {
					imagePath = directory / imagePath;
				}
				const std::string path = imagePath.string();
				unsigned char* data = stbi_load(path.c_str(), &width, &height, &nrChannels, 4);
				if (data == nullptr) {
					fmt::println("load_image: could not load '{}': {}", path, stbi_failure_reason());
				}
				if (data) {
					VkExtent3D imagesize;
					imagesize.width = width;
					imagesize.height = height;
					imagesize.depth = 1;

					newImage = engine->createImage(data, imagesize, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,false);

					stbi_image_free(data);
				}
			},
			[&](fastgltf::sources::Array& array) {
				unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(array.bytes.data()), static_cast<int>(array.bytes.size()),
					&width, &height, &nrChannels, 4);
				if (data) {
					VkExtent3D imagesize;
					imagesize.width = width;
					imagesize.height = height;
					imagesize.depth = 1;

					newImage = engine->createImage(data, imagesize, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, false);

					stbi_image_free(data);
				}
			},
			[&](fastgltf::sources::BufferView& view) {
				auto& bufferView = asset.bufferViews[view.bufferViewIndex];
				auto& buffer = asset.buffers[bufferView.bufferIndex];

				std::visit(fastgltf::visitor { // We only care about VectorWithMime here, because we
											   // specify LoadExternalBuffers, meaning all buffers
											   // are already loaded into a vector.
							   [](auto& arg) {},
							   [&](fastgltf::sources::Array& array) {
								   unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(array.bytes.data()) + bufferView.byteOffset,
									   static_cast<int>(bufferView.byteLength),
									   &width, &height, &nrChannels, 4);
								   if (data) {
									   VkExtent3D imagesize;
									   imagesize.width = width;
									   imagesize.height = height;
									   imagesize.depth = 1;

									   newImage = engine->createImage(data, imagesize, VK_FORMAT_R8G8B8A8_UNORM,
										   VK_IMAGE_USAGE_SAMPLED_BIT,false);

									   stbi_image_free(data);
								   }
							   } },
					buffer.data);
			},
		},
		image.data);

	// if any of the attempts to load the data failed, we havent written the image
	// so handle is null
	if (newImage.image == VK_NULL_HANDLE) {
		return {};
	} else {
		return newImage;
	}
}

void LoadedGLTF::Draw(const glm::mat4& topMatrix, DrawContext& ctx)
{
	// create renderables from the scenenodes
	for (auto& n : topNodes) {
		n->Draw(topMatrix, ctx);
	}
}

void LoadedGLTF::clearAll()
{
	VkDevice dv = creator->m_device;

	descriptorPool.destroyPools(dv);
	creator->destroyBuffer(materialDataBuffer);

	for (auto& [k, v] : meshes) {

		creator->destroyBuffer(v->meshBuffers.indexBuffer);
		creator->destroyBuffer(v->meshBuffers.vertexBuffer);
	}

	for (auto& [k, v] : images) {
		
		if (v.image == creator->m_errorCheckerboardImage.image) {
			//dont destroy the default images
			continue;
		}
		creator->destroyImage(v);
	}

	for (auto& sampler : samplers) {
		vkDestroySampler(dv, sampler, nullptr);
	}
}