#include <bench_scene.h>

#include <algorithm>
#include <cmath>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>

#include <imported_model.h>
#include <mount_library.h>
#include <rt_scene_editor.h>
#include <spectrum_ref.h>
#include <vk_engine.h>
#include <vk_loader.h>

namespace {

constexpr double PI = 3.14159265358979323846;

// the holes' floors: the dark of a blind hole, darker than the plate around it
constexpr float HOLE_FLOOR_REFLECTANCE = 0.01f;

// The breadboard's skins: brushed stainless steel, pbrt's conductor from a normal-incidence reflectance, with the brushing
// along x (the mesh's dp/du). The grooves run along the brushing, so the surface is nearly smooth along x and rough
// across it, which spreads a highlight into a streak across the grain
constexpr glm::vec3 SKIN_REFLECTANCE { 0.62f, 0.62f, 0.63f };
constexpr float SKIN_ALPHA_ALONG = 0.05f;
constexpr float SKIN_ALPHA_ACROSS = 0.30f;

MaterialDef brushedStainless()
{
	MaterialDef material = makeMaterial(MaterialType::Conductor);
	material.name = "brushed stainless";
	material.params["useReflectance"] = true;
	material.params["albedo"] = rgbSpectrum(SKIN_REFLECTANCE);
	material.params["remapRoughness"] = false;
	material.params["uRoughness"] = SKIN_ALPHA_ALONG;
	material.params["vRoughness"] = SKIN_ALPHA_ACROSS;
	return material;
}

ImportedMaterial previewMaterial(MaterialDef definition, glm::vec4 color, float metallic, float roughness)
{
	ImportedMaterial material;
	material.definition = std::move(definition);
	material.previewColor = color;
	material.previewMetallic = metallic;
	material.previewRoughness = roughness;
	return material;
}

ImportedMaterial finishPreview(MountFinish finish)
{
	const MountFinishInfo& info = mountFinishInfo(finish);
	return previewMaterial(mountFinishMaterial(finish), info.baseColor, info.metallic, info.roughness);
}

ImportedModel breadboardModel(const BenchSettings& bench)
{
	const BenchMesh built = buildBreadboardMesh(bench);
	ImportedModel model;
	// the preview has no anisotropic metal: a gray metal between the brushing's two roughnesses
	model.materials.push_back(previewMaterial(brushedStainless(), glm::vec4(SKIN_REFLECTANCE, 1.f), 1.f, 0.35f));
	model.materials.push_back(finishPreview(MountFinish::AnodizedBlack));
	model.materials.push_back(previewMaterial(makeMaterial(MaterialType::Lambertian, glm::vec3(HOLE_FLOOR_REFLECTANCE)), glm::vec4(glm::vec3(HOLE_FLOOR_REFLECTANCE), 1.f), 0.f, 1.f));
	ImportedMesh mesh;
	mesh.name = "breadboard";
	mesh.positions = built.positions;
	mesh.normals = built.normals;
	mesh.uvs = built.uvs;
	mesh.indices = built.indices;
	const uint32_t walls = built.skinIndexCount;
	const uint32_t floors = walls + built.wallIndexCount;
	mesh.surfaces.push_back(ImportedSurface { 0, built.skinIndexCount, 0 });
	mesh.surfaces.push_back(ImportedSurface { walls, built.wallIndexCount, 1 });
	mesh.surfaces.push_back(ImportedSurface { floors, (uint32_t)built.indices.size() - floors, 2 });
	model.meshes.push_back(std::move(mesh));
	model.nodes.push_back(ImportedNode { "breadboard", 0, glm::mat4(1.f) });
	return model;
}

// the shape library's unit cylinder (shape.h): radius 1, y from -0.5 to 0.5, both caps, in stainless steel
ImportedModel postModel()
{
	constexpr int SEGMENTS = 32;
	ImportedModel model;
	model.materials.push_back(finishPreview(MountFinish::Stainless));
	ImportedMesh mesh;
	mesh.name = "post";
	auto vertex = [&](glm::vec3 p, glm::vec3 n) {
		mesh.positions.push_back(p);
		mesh.normals.push_back(n);
		return (uint32_t)mesh.positions.size() - 1;
	};
	for (int i = 0; i < SEGMENTS; i++) {
		const float a0 = (float)(2.0 * PI * i / SEGMENTS);
		const float a1 = (float)(2.0 * PI * (i + 1) / SEGMENTS);
		const glm::vec3 n0(std::cos(a0), 0.f, std::sin(a0));
		const glm::vec3 n1(std::cos(a1), 0.f, std::sin(a1));
		const uint32_t b0 = vertex(n0 + glm::vec3(0.f, -0.5f, 0.f), n0);
		const uint32_t b1 = vertex(n1 + glm::vec3(0.f, -0.5f, 0.f), n1);
		const uint32_t t0 = vertex(n0 + glm::vec3(0.f, 0.5f, 0.f), n0);
		const uint32_t t1 = vertex(n1 + glm::vec3(0.f, 0.5f, 0.f), n1);
		mesh.indices.insert(mesh.indices.end(), { b0, t1, b1, b0, t0, t1 });
		for (const float y : { -0.5f, 0.5f }) {
			const glm::vec3 n(0.f, y > 0.f ? 1.f : -1.f, 0.f);
			const uint32_t c = vertex(glm::vec3(0.f, y, 0.f), n);
			const uint32_t p0 = vertex(n0 + glm::vec3(0.f, y, 0.f), n);
			const uint32_t p1 = vertex(n1 + glm::vec3(0.f, y, 0.f), n);
			if (y > 0.f) {
				mesh.indices.insert(mesh.indices.end(), { c, p1, p0 });
			} else {
				mesh.indices.insert(mesh.indices.end(), { c, p0, p1 });
			}
		}
	}
	mesh.surfaces.push_back(ImportedSurface { 0, (uint32_t)mesh.indices.size(), 0 });
	model.meshes.push_back(std::move(mesh));
	model.nodes.push_back(ImportedNode { "post", 0, glm::mat4(1.f) });
	return model;
}

// one tiny triangle carrying the red a blocked fork is drawn in
ImportedModel markerModel()
{
	ImportedModel model;
	model.materials.push_back(previewMaterial(makeMaterial(MaterialType::Lambertian, glm::vec3(0.8f, 0.05f, 0.05f)), glm::vec4(0.9f, 0.1f, 0.08f, 1.f), 0.f, 0.6f));
	ImportedMesh mesh;
	mesh.name = "marker";
	mesh.positions = { glm::vec3(0.f), glm::vec3(1e-6f, 0.f, 0.f), glm::vec3(0.f, 1e-6f, 0.f) };
	mesh.normals = { glm::vec3(0.f, 0.f, 1.f), glm::vec3(0.f, 0.f, 1.f), glm::vec3(0.f, 0.f, 1.f) };
	mesh.indices = { 0, 1, 2 };
	mesh.surfaces.push_back(ImportedSurface { 0, 3, 0 });
	model.meshes.push_back(std::move(mesh));
	model.nodes.push_back(ImportedNode { "marker", 0, glm::mat4(1.f) });
	return model;
}

MeshNode* firstMeshNode(const LoadedModel& model)
{
	for (const std::shared_ptr<Node>& node : model.topNodes) {
		if (MeshNode* mesh = dynamic_cast<MeshNode*>(node.get())) {
			return mesh;
		}
	}
	return nullptr;
}

// a post: the unit cylinder from its bottom to its top
glm::mat4 postTransform(const OpticStack& stack)
{
	const glm::dvec3 middle = 0.5 * (stack.postBottom + stack.postTop);
	const double length = stack.postTop.y - stack.postBottom.y;
	const double radius = 0.5 * POST_DIAMETER;
	return glm::mat4(glm::scale(glm::translate(glm::dmat4(1.0), middle), glm::dvec3(radius, length, radius)));
}

uint64_t finishMaterialId(MountFinish finish)
{
	return BENCH_MATERIAL_ID + (uint64_t)finish;
}

constexpr uint64_t HOLE_FLOOR_MATERIAL_ID = BENCH_MATERIAL_ID + MOUNT_FINISH_COUNT;
constexpr uint64_t SKIN_MATERIAL_ID = BENCH_MATERIAL_ID + MOUNT_FINISH_COUNT + 1;

}

const BenchScene::PartMesh* BenchScene::findPart(const std::string& model, const std::string& part) const
{
	const auto found = m_parts.find({ model, part });
	return found == m_parts.end() ? nullptr : &found->second;
}

void BenchScene::retire(VulkanEngine* engine, std::shared_ptr<LoadedModel> model)
{
	if (model != nullptr) {
		engine->getCurrentFrame().deletionQueue.push_function([model]() mutable { model.reset(); });
	}
}

bool BenchScene::update(VulkanEngine* engine)
{
	const RaytraceSceneEditor& editor = engine->m_raytraceScene;
	const BenchSettings& bench = editor.bench();
	bool meshesChanged = false;

	// the breadboard at the scene's size
	if (bench.table && (m_table == nullptr || m_tableFor.size != bench.size)) {
		retire(engine, std::move(m_table));
		m_table = loadImportedModel(engine, breadboardModel(bench));
		m_tableFor = bench;
		meshesChanged = true;
	} else if (!bench.table && m_table != nullptr) {
		retire(engine, std::move(m_table));
		m_table = nullptr;
		meshesChanged = true;
	}

	// each model a stack names, uploaded by the mount library the first time; its parts indexed by name
	bool anyPost = false;
	bool anyBlocked = false;
	for (const OpticStack& stack : editor.benchStacks()) {
		anyPost |= stack.post;
		anyBlocked |= stack.forkBlocked;
		for (const StackPart& part : stack.parts) {
			const std::string id = part.model->id;
			if (m_models.count(id) != 0) {
				continue;
			}
			const std::shared_ptr<LoadedModel> model = engine->m_mountLibrary.meshes(id);
			if (model == nullptr) {
				continue;
			}
			m_models[id] = model;
			for (const std::shared_ptr<Node>& node : model->topNodes) {
				MeshNode* mesh = dynamic_cast<MeshNode*>(node.get());
				if (mesh == nullptr || mesh->mesh == nullptr) {
					continue;
				}
				PartMesh entry;
				entry.mesh = mesh->mesh;
				entry.node = mesh;
				// each surface's finish from the material it was uploaded with
				for (const GeoSurface& surface : mesh->mesh->surfaces) {
					MountFinish finish = MountFinish::AnodizedBlack;
					const auto index = std::find(model->materialsByIndex.begin(), model->materialsByIndex.end(), surface.material);
					if (index != model->materialsByIndex.end() && model->imported.has_value()) {
						const size_t i = (size_t)(index - model->materialsByIndex.begin());
						if (i < model->imported->materials.size()) {
							finish = mountFinishOfMaterial(model->imported->materials[i].definition.name).value_or(finish);
						}
					}
					entry.finishes.push_back(finish);
				}
				m_parts[{ id, node->name }] = std::move(entry);
			}
			meshesChanged = true;
		}
	}
	if (anyPost && m_post == nullptr) {
		m_post = loadImportedModel(engine, postModel());
		m_revision++;
	}
	if (anyBlocked && m_marker == nullptr) {
		m_marker = loadImportedModel(engine, markerModel());
		m_revision++;
	}

	if (meshesChanged) {
		m_tracedMeshes.clear();
		if (m_table != nullptr) {
			if (const MeshNode* node = firstMeshNode(*m_table)) {
				m_tracedMeshes.push_back(node->mesh);
			}
		}
		for (const auto& [key, part] : m_parts) {
			m_tracedMeshes.push_back(part.mesh);
		}
		m_revision++;
	}
	return meshesChanged;
}

void BenchScene::draw(VulkanEngine* engine)
{
	DrawContext& ctx = engine->m_mainDrawContext;
	if (m_table != nullptr) {
		for (const std::shared_ptr<Node>& node : m_table->topNodes) {
			node->Draw(glm::mat4(1.f), ctx);
		}
	}
	MaterialInstance* red = m_marker != nullptr && !m_marker->materialsByIndex.empty() ? &m_marker->materialsByIndex[0]->data : nullptr;
	MeshNode* post = m_post != nullptr ? firstMeshNode(*m_post) : nullptr;
	for (const OpticStack& stack : engine->m_raytraceScene.benchStacks()) {
		for (const StackPart& part : stack.parts) {
			const PartMesh* mesh = findPart(part.model->id, part.part);
			if (mesh == nullptr || mesh->node == nullptr) {
				continue;
			}
			const size_t first = ctx.opaqueSurfaces.size();
			mesh->node->Draw(glm::mat4(part.toWorld), ctx);
			if (part.role == StackRole::Fork && stack.forkBlocked && red != nullptr) {
				for (size_t i = first; i < ctx.opaqueSurfaces.size(); i++) {
					ctx.opaqueSurfaces[i].material = red;
				}
			}
		}
		if (stack.post && post != nullptr) {
			post->Draw(postTransform(stack), ctx);
		}
	}
}

void BenchScene::clear()
{
	m_table.reset();
	m_post.reset();
	m_marker.reset();
	m_parts.clear();
	m_models.clear();
	m_tracedMeshes.clear();
	m_revision++;
}

BenchTrace BenchScene::traceInput(const RaytraceSceneEditor& editor, const RaytraceMeshData& meshData) const
{
	BenchTrace trace;
	const std::vector<OpticStack>& stacks = editor.benchStacks();
	if (m_table == nullptr && std::all_of(stacks.begin(), stacks.end(), [](const OpticStack& s) { return s.parts.empty() && !s.post; })) {
		return trace;
	}
	// the finishes, plain: the decals are the preview's alone
	for (size_t f = 0; f < MOUNT_FINISH_COUNT; f++) {
		MaterialDef material = mountFinishMaterial((MountFinish)f);
		material.id = finishMaterialId((MountFinish)f);
		trace.materials.push_back(std::move(material));
	}
	MaterialDef floor = makeMaterial(MaterialType::Lambertian, glm::vec3(HOLE_FLOOR_REFLECTANCE));
	floor.id = HOLE_FLOOR_MATERIAL_ID;
	floor.name = "hole floor";
	trace.materials.push_back(std::move(floor));
	MaterialDef skin = brushedStainless();
	skin.id = SKIN_MATERIAL_ID;
	trace.materials.push_back(std::move(skin));

	auto tag = [&](uint64_t optic, std::string part) {
		trace.tags.push_back(BenchTag { optic, std::move(part) });
		return (uint32_t)trace.tags.size() - 1;
	};
	if (m_table != nullptr) {
		const MeshNode* node = firstMeshNode(*m_table);
		const auto index = node != nullptr ? meshData.benchMeshes.find(node->mesh.get()) : meshData.benchMeshes.end();
		if (index != meshData.benchMeshes.end()) {
			trace.meshes.push_back(BenchMeshInstance { index->second, glm::mat4(1.f), { SKIN_MATERIAL_ID, finishMaterialId(MountFinish::AnodizedBlack), HOLE_FLOOR_MATERIAL_ID }, tag(0, "breadboard") });
		}
	}
	const std::vector<SceneOptic>& optics = editor.optics();
	for (size_t i = 0; i < stacks.size() && i < optics.size(); i++) {
		const OpticStack& stack = stacks[i];
		for (const StackPart& part : stack.parts) {
			const PartMesh* mesh = part.traced ? findPart(part.model->id, part.part) : nullptr;
			const auto index = mesh != nullptr ? meshData.benchMeshes.find(mesh->mesh.get()) : meshData.benchMeshes.end();
			if (index == meshData.benchMeshes.end()) {
				continue;
			}
			BenchMeshInstance instance;
			instance.meshIndex = index->second;
			instance.objectToWorld = glm::mat4(part.toWorld);
			for (const MountFinish finish : mesh->finishes) {
				instance.surfaceMaterials.push_back(finishMaterialId(finish));
			}
			instance.tag = tag(optics[i].id, fmt::format("{} {}", part.model->id, part.part));
			trace.meshes.push_back(std::move(instance));
		}
		if (stack.post) {
			trace.posts.push_back(BenchPostInstance { postTransform(stack), finishMaterialId(MountFinish::Stainless), tag(optics[i].id, "post") });
		}
	}
	return trace;
}
