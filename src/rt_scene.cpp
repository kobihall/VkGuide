#include <rt_scene.h>

#include <algorithm>
#include <unordered_map>

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

//the one place the CPU backend's sphere/material objects are constructed: from the plain scene
//data, widened to the double precision the ported math runs in
std::shared_ptr<material> makeCpuMaterial(const SphereMaterial& m)
{
	const glm::dvec3 albedo(m.albedo);
	switch (m.type) {
	case MaterialType::Lambertian:
		return std::make_shared<lambertian>(albedo);
	case MaterialType::Metal:
		return std::make_shared<metal>(albedo, (double)m.fuzz);
	case MaterialType::Phong:
		return std::make_shared<phong>(albedo, (double)m.smoothness);
	case MaterialType::Dielectric:
		return std::make_shared<dielectric>((double)m.ir);
	}
	return std::make_shared<lambertian>(albedo);
}

RTCameraSnapshot captureCameraSnapshot(VulkanEngine* engine, const RenderSettings& settings)
{
	RTCameraSnapshot camera;

	//the raster camera looks down -Z in its own space, and its rotation matrix takes camera
	//space to world space
	const glm::mat4 rotation = engine->m_mainCamera.getRotationMatrix();
	const glm::vec3 position = engine->m_mainCamera.position;
	const glm::vec3 forward = glm::vec3(rotation * glm::vec4(0.f, 0.f, -1.f, 0.f));

	camera.lookFrom = position;
	camera.lookAt = position + forward;
	//taking up from the camera rather than assuming world up keeps the framing correct when
	//the camera is pitched straight up or down, where world up and the view direction align
	camera.vUp = glm::vec3(rotation * glm::vec4(0.f, 1.f, 0.f, 0.f));
	camera.vfovDegrees = CAMERA_VERTICAL_FOV_DEGREES;

	//from the settings, never derived from lookAt: lookAt is always exactly one unit ahead here,
	//so a distance derived from it would always be 1. A zero focus distance would collapse the
	//whole image plane onto one point, so it is floored
	camera.aperture = std::max(settings.aperture, 0.f);
	camera.focusDistance = std::max(settings.focusDistance, 0.01f);

	return camera;
}

RaytraceScene buildRaytraceScene(VulkanEngine* engine, const RaytraceSceneEditor& editor, const RenderSettings& settings)
{
	RaytraceScene scene;
	scene.settings = settings;

	//fresh objects built from the plain data, so nothing the worker thread reads is shared with
	//the editor, which keeps mutating its own list while the render runs
	scene.spheres.reserve(editor.spheres().size());
	for (const SceneSphere& entry : editor.spheres()) {
		scene.spheres.push_back(std::make_shared<sphere>(glm::dvec3(entry.center), (double)entry.radius, makeCpuMaterial(entry.material)));
	}

	//an O(1) handle copy of immutable data built when the scene was loaded
	scene.meshData = engine->m_raytraceMeshData;

	scene.camera = captureCameraSnapshot(engine, settings);

	return scene;
}
