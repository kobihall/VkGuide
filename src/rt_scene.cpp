#include <rt_scene.h>

#include <unordered_map>

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
