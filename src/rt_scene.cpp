#include <rt_scene.h>

#include <unordered_map>

#include <rt_scene_editor.h>
#include <vk_engine.h>

namespace {

void visitNode(const std::shared_ptr<Node>& node, const std::function<void(const MeshNode&)>& visit)
{
	if (node == nullptr) {
		return;
	}

	//same tree walk LoadedGLTF::Draw()/Node::Draw() use, collecting nodes instead of RenderObjects
	if (const MeshNode* meshNode = dynamic_cast<const MeshNode*>(node.get())) {
		visit(*meshNode);
	}

	for (const std::shared_ptr<Node>& child : node->children) {
		visitNode(child, visit);
	}
}

}

void forEachMeshNode(VulkanEngine* engine, const std::function<void(const MeshNode& node)>& visit)
{
	//every imported model, in name order so the browser and the mesh data are stable
	for (const auto& [name, model] : engine->m_models) {
		if (model == nullptr) {
			continue;
		}

		for (const std::shared_ptr<Node>& topNode : model->topNodes) {
			visitNode(topNode, visit);
		}
	}
}

std::shared_ptr<const RaytraceMeshData> buildRaytraceMeshData(VulkanEngine* engine)
{
	auto data = std::make_shared<RaytraceMeshData>();

	//a mesh reused by many nodes is stored once; instances refer to it by index
	std::unordered_map<const MeshAsset*, size_t> meshIndices;

	forEachMeshNode(engine, [&](const MeshNode& node) {
		if (node.mesh == nullptr) {
			return;
		}

		const MeshAsset& mesh = *node.mesh;

		auto [it, inserted] = meshIndices.try_emplace(&mesh, data->meshes.size());
		if (inserted) {
			data->meshes.push_back(node.mesh);
		}

		//one instance per GeoSurface, since a surface is the finest grouping with exactly one material
		for (size_t surfaceIndex = 0; surfaceIndex < mesh.surfaces.size(); surfaceIndex++) {
			const GeoSurface& surface = mesh.surfaces[surfaceIndex];

			if ((size_t)surface.startIndex + surface.count > mesh.cpuIndices.size()) {
				//cpuIndices is populated from the same data the surface offsets were computed
				//from, so this only fires if a loader path forgot to retain it
				fmt::println("buildRaytraceMeshData: mesh '{}' surface {} is out of range of its cpu index data", mesh.name, surfaceIndex);
				continue;
			}

			RTMeshInstance instance;
			instance.name = mesh.surfaces.size() > 1 ? fmt::format("{}[{}]", node.name, surfaceIndex) : node.name;
			instance.meshIndex = it->second;
			instance.firstIndex = surface.startIndex;
			instance.indexCount = surface.count;
			instance.worldTransform = node.worldTransform;

			if (surface.material != nullptr) {
				instance.colorFactors = surface.material->colorFactors;
				instance.metalRoughFactors = surface.material->metalRoughFactors;
			}

			data->triangleCount += surface.count / 3;
			data->instances.push_back(std::move(instance));
		}
	});

	fmt::println("buildRaytraceMeshData: {} unique mesh(es), {} surface instance(s), {} triangles", data->meshes.size(), data->instances.size(), data->triangleCount);

	return data;
}
