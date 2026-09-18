#include <rt_scene.h>

#include <unordered_map>

#include <rt_textures.h>
#include <vk_engine.h>

namespace {

void visitNode(const std::shared_ptr<Node>& node, uint32_t& nodeIndex, std::string_view modelKey,
	const std::function<void(std::string_view, uint32_t, const MeshNode&)>& visit)
{
	if (node == nullptr) {
		return;
	}

	//same tree walk LoadedGLTF::Draw()/Node::Draw() use, collecting nodes instead of RenderObjects
	if (const MeshNode* meshNode = dynamic_cast<const MeshNode*>(node.get())) {
		visit(modelKey, nodeIndex++, *meshNode);
	}

	for (const std::shared_ptr<Node>& child : node->children) {
		visitNode(child, nodeIndex, modelKey, visit);
	}
}

}

void forEachMeshNode(VulkanEngine* engine, const std::function<void(std::string_view modelKey, uint32_t nodeIndex, const MeshNode& node)>& visit)
{
	//every imported model, in key order so the browser, the mesh data and a saved file agree
	for (const auto& [name, model] : engine->m_models) {
		if (model == nullptr) {
			continue;
		}

		//counts mesh nodes within this model only: a SceneMeshObject's nodeIndex is relative to
		//its own model, so importing or removing another model never renumbers it
		uint32_t nodeIndex = 0;
		for (const std::shared_ptr<Node>& topNode : model->topNodes) {
			visitNode(topNode, nodeIndex, name, visit);
		}
	}
}

std::shared_ptr<const RaytraceMeshData> buildRaytraceMeshData(VulkanEngine* engine)
{
	auto data = std::make_shared<RaytraceMeshData>();

	//a mesh reused by many nodes is stored once; surfaces refer to it by index
	std::unordered_map<const MeshAsset*, size_t> meshIndices;

	forEachMeshNode(engine, [&](std::string_view modelKey, uint32_t nodeIndex, const MeshNode& node) {
		if (node.mesh == nullptr) {
			return;
		}

		const MeshAsset& mesh = *node.mesh;

		auto [it, inserted] = meshIndices.try_emplace(&mesh, data->meshes.size());
		if (inserted) {
			data->meshes.push_back(node.mesh);
		}

		RTMeshNode entry;
		entry.modelKey = std::string(modelKey);
		entry.nodeIndex = nodeIndex;
		entry.name = node.name.empty() ? fmt::format("{}[{}]", modelKey, nodeIndex) : node.name;
		entry.authoredTransform = node.worldTransform;

		//one surface record per GeoSurface, since a surface is the finest grouping with exactly
		//one material
		for (size_t surfaceIndex = 0; surfaceIndex < mesh.surfaces.size(); surfaceIndex++) {
			const GeoSurface& surface = mesh.surfaces[surfaceIndex];

			if ((size_t)surface.startIndex + surface.count > mesh.cpuIndices.size()) {
				//cpuIndices is populated from the same data the surface offsets were computed
				//from, so this only fires if a loader path forgot to retain it
				fmt::println("buildRaytraceMeshData: mesh '{}' surface {} is out of range of its cpu index data", mesh.name, surfaceIndex);
				continue;
			}

			RTMeshSurface record;
			record.meshIndex = it->second;
			record.firstIndex = surface.startIndex;
			record.indexCount = surface.count;
			record.material = surface.material;

			entry.triangleCount += surface.count / 3;
			entry.surfaces.push_back(std::move(record));
		}

		data->triangleCount += entry.triangleCount;
		data->nodes.push_back(std::move(entry));
	});

	fmt::println("buildRaytraceMeshData: {} unique mesh(es), {} mesh node(s), {} triangles", data->meshes.size(), data->nodes.size(), data->triangleCount);

	return data;
}

std::vector<SceneMeshObject> defaultMeshObjects(const RaytraceMeshData& meshData, const std::string& modelKey)
{
	std::vector<SceneMeshObject> objects;
	for (const RTMeshNode& node : meshData.nodes) {
		if (node.modelKey != modelKey) {
			continue;
		}

		SceneMeshObject object;
		object.name = node.name;
		object.modelKey = node.modelKey;
		object.nodeIndex = node.nodeIndex;
		//placed exactly where the file put it, so an imported model looks identical to how the
		//raster pass drew it before objects existed
		object.transform = node.authoredTransform;
		objects.push_back(std::move(object));
	}
	return objects;
}

std::shared_ptr<const RaytraceTriangleData> buildTriangleData(const RaytraceMeshData& meshData,
	const std::vector<SceneMeshObject>& objects, const RaytraceTextureArray& textures)
{
	auto data = std::make_shared<RaytraceTriangleData>();

	//one material per distinct glTF material, and one per object that overrides its own, so the
	//material array stays small however many triangles index into it
	std::unordered_map<const GLTFMaterial*, uint32_t> gltfMaterials;
	std::unordered_map<uint64_t, uint32_t> overrideMaterials;

	for (const SceneMeshObject& object : objects) {
		if (!object.visible) {
			continue;
		}

		const RTMeshNode* node = meshData.findNode(object.modelKey, object.nodeIndex);
		if (node == nullptr) {
			//the model was removed with the object still placed; the editor prunes these, so this
			//is only ever a transient state within one frame
			continue;
		}

		//a normal is transformed by the inverse transpose, which is what keeps it perpendicular
		//to the surface under a non-uniform scale
		const glm::mat3 normalMatrix = glm::mat3(glm::transpose(glm::inverse(object.transform)));

		uint32_t overrideIndex = 0;
		if (object.materialMode == MeshMaterialMode::Override) {
			auto [it, inserted] = overrideMaterials.try_emplace(object.id, (uint32_t)data->materials.size());
			if (inserted) {
				//an override replaces the glTF material outright, texture included
				data->materials.push_back(RaytraceTriMaterial { object.material, -1 });
			}
			overrideIndex = it->second;
		}

		for (const RTMeshSurface& surface : node->surfaces) {
			if (surface.meshIndex >= meshData.meshes.size()) {
				continue;
			}
			const MeshAsset& mesh = *meshData.meshes[surface.meshIndex];

			uint32_t materialIndex = overrideIndex;
			if (object.materialMode == MeshMaterialMode::Gltf) {
				const GLTFMaterial* material = surface.material.get();
				auto [it, inserted] = gltfMaterials.try_emplace(material, (uint32_t)data->materials.size());
				if (inserted) {
					//glTF pbr is not one of the four materials the tracer scatters with, so the
					//base colour becomes a diffuse albedo: the honest reading of a base-colour
					//factor and texture, and the metal/rough factors are deliberately unused
					RaytraceTriMaterial entry;
					entry.material.type = MaterialType::Lambertian;
					entry.material.albedo = material != nullptr ? glm::vec3(material->colorFactors) : glm::vec3(1.f);
					entry.albedoLayer = material != nullptr ? textures.layerOf(material->baseColorImage.image) : -1;
					data->materials.push_back(entry);
				}
				materialIndex = it->second;
			}

			const size_t end = (size_t)surface.firstIndex + surface.indexCount;
			if (end > mesh.cpuIndices.size()) {
				continue;
			}

			for (size_t i = surface.firstIndex; i + 2 < end; i += 3) {
				const Vertex& a = mesh.cpuVertices[mesh.cpuIndices[i]];
				const Vertex& b = mesh.cpuVertices[mesh.cpuIndices[i + 1]];
				const Vertex& c = mesh.cpuVertices[mesh.cpuIndices[i + 2]];

				//baked into world space once, here: without an acceleration structure there is no
				//instance level to hold a transform, and the extend stage should not pay for one
				//matrix multiply per ray per triangle
				RaytraceTriangle triangle;
				triangle.p0 = glm::vec4(glm::vec3(object.transform * glm::vec4(a.position, 1.f)), a.uv_x);
				triangle.p1 = glm::vec4(glm::vec3(object.transform * glm::vec4(b.position, 1.f)), b.uv_x);
				triangle.p2 = glm::vec4(glm::vec3(object.transform * glm::vec4(c.position, 1.f)), c.uv_x);
				triangle.n0 = glm::vec4(normalMatrix * a.normal, a.uv_y);
				triangle.n1 = glm::vec4(normalMatrix * b.normal, b.uv_y);
				triangle.n2 = glm::vec4(normalMatrix * c.normal, c.uv_y);
				triangle.material = materialIndex;
				data->triangles.push_back(triangle);
			}
		}
	}

	fmt::println("buildTriangleData: {} triangle(s), {} material(s)", data->triangles.size(), data->materials.size());

	return data;
}
