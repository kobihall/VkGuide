#include <rt_scene.h>

#include <rt_material.h>
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

//collects one RTMeshInstance per GeoSurface, since a surface is the finest grouping that has
//exactly one material. Nothing in this feature reads the result - see RTMeshInstance
void collectMeshInstances(const MeshNode& node, std::vector<RTMeshInstance>& out)
{
	if (node.mesh == nullptr) {
		return;
	}

	const MeshAsset& mesh = *node.mesh;

	for (size_t surfaceIndex = 0; surfaceIndex < mesh.surfaces.size(); surfaceIndex++) {
		const GeoSurface& surface = mesh.surfaces[surfaceIndex];

		RTMeshInstance instance;
		instance.name = mesh.surfaces.size() > 1 ? fmt::format("{}[{}]", node.name, surfaceIndex) : node.name;

		if (surface.material != nullptr) {
			instance.colorFactors = surface.material->colorFactors;
			instance.metalRoughFactors = surface.material->metalRoughFactors;
		}

		const size_t end = (size_t)surface.startIndex + surface.count;
		if (end > mesh.cpuIndices.size()) {
			//cpuIndices is populated from the same data the surface offsets were computed from,
			//so this only fires if a loader path forgot to retain it
			fmt::println("buildRaytraceScene: mesh '{}' surface {} is out of range of its cpu index data", mesh.name, surfaceIndex);
			continue;
		}

		instance.triangles.reserve(surface.count / 3);
		for (size_t i = surface.startIndex; i + 2 < end; i += 3) {
			RTTriangle triangle;
			triangle.v0 = glm::dvec3(node.worldTransform * glm::vec4(mesh.cpuVertices[mesh.cpuIndices[i + 0]].position, 1.f));
			triangle.v1 = glm::dvec3(node.worldTransform * glm::vec4(mesh.cpuVertices[mesh.cpuIndices[i + 1]].position, 1.f));
			triangle.v2 = glm::dvec3(node.worldTransform * glm::vec4(mesh.cpuVertices[mesh.cpuIndices[i + 2]].position, 1.f));
			instance.triangles.push_back(triangle);
		}

		out.push_back(std::move(instance));
	}
}

}

bool RaytraceScene::hit(const ray& r, double t_min, double t_max, hit_record& rec) const
{
	hit_record temp_rec;
	bool hit_anything = false;
	auto closest_so_far = t_max;

	for (const auto& object : spheres) {
		if (object->hit(r, t_min, closest_so_far, temp_rec)) {
			hit_anything = true;
			closest_so_far = temp_rec.t;
			rec = temp_rec;
		}
	}

	return hit_anything;
}

void forEachMeshNode(VulkanEngine* engine, const std::function<void(const MeshNode& node)>& visit)
{
	//only m_loadedScenes. m_loadedNodes holds the engine's own test meshes, which updateScene()
	//draws with transforms passed in at the call site rather than through worldTransform, so
	//they have no meaningful world placement to report here
	for (const auto& [name, scene] : engine->m_loadedScenes) {
		if (scene == nullptr) {
			continue;
		}

		for (const std::shared_ptr<Node>& topNode : scene->topNodes) {
			visitNode(topNode, visit);
		}
	}
}

RaytraceScene buildRaytraceScene(VulkanEngine* engine, const RaytraceSceneEditor& editor, const RenderSettings& settings)
{
	RaytraceScene scene;
	scene.settings = settings;

	//deep copies of both the spheres and their materials, not shared_ptr copies. Sharing either
	//with the worker thread would let a slider drag in the editor mutate data a render is
	//actively reading
	scene.spheres.reserve(editor.spheres().size());
	for (const SceneSphere& entry : editor.spheres()) {
		auto copy = std::make_shared<sphere>(*entry.object);
		copy->mat_ptr = entry.object->mat_ptr->clone();
		scene.spheres.push_back(std::move(copy));
	}

	forEachMeshNode(engine, [&scene](const MeshNode& node) {
		collectMeshInstances(node, scene.meshInstances);
	});

	//the raster camera looks down -Z in its own space, and its rotation matrix takes camera
	//space to world space
	const glm::mat4 rotation = engine->m_mainCamera.getRotationMatrix();
	const glm::dvec3 position = engine->m_mainCamera.position;
	const glm::dvec3 forward = glm::dvec3(rotation * glm::vec4(0.f, 0.f, -1.f, 0.f));

	scene.camera.lookFrom = position;
	scene.camera.lookAt = position + forward;
	//taking up from the camera rather than assuming world up keeps the framing correct when
	//the camera is pitched straight up or down, where world up and the view direction align
	scene.camera.vUp = glm::dvec3(rotation * glm::vec4(0.f, 1.f, 0.f, 0.f));
	scene.camera.vfovDegrees = CAMERA_VERTICAL_FOV_DEGREES;

	return scene;
}
