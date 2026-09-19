#include <rt_accel.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <future>
#include <thread>

#include <fmt/format.h>

#include <rt_textures.h>
#include <vk_loader.h>

AccelSettings defaultAccelSettings()
{
	//spatial splits: the fewest primitive tests of the four builders, and level with sweep SAH on
	//time; the build cost (well under a second for structure.glb) is paid once at import. The binary
	//layout: CWBVH visits ~30% fewer nodes, but on this project's GPU (Radeon Pro 560X through
	//MoltenVK) it was ~5% slower for the incoherent bounces inside a model and only ~4% faster for
	//mostly-sky views. docs/plans/completed/bvh-acceleration.md §9
	AccelSettings settings;
	settings.blas.builder = BvhBuilder::SpatialSah;
	settings.layout = BvhLayout::Binary;
	return settings;
}

BvhBuildOptions effectiveBlasOptions(const AccelSettings& settings)
{
	BvhBuildOptions options = settings.blas;
	if (settings.layout == BvhLayout::Cwbvh8) {
		options.maxLeafSize = std::min(options.maxLeafSize, BVH_CWBVH_MAX_LEAF_SIZE);
	}
	return options;
}

namespace {

// one mesh's triangles in object space and their shading attributes, in GeoSurface order
std::shared_ptr<const RaytraceBlas> buildMeshBlas(const MeshAsset& mesh, const AccelSettings& settings)
{
	auto out = std::make_shared<RaytraceBlas>();
	out->surfaceCount = (uint32_t)mesh.surfaces.size();

	std::vector<glm::vec3> vertices;
	for (uint32_t surfaceIndex = 0; surfaceIndex < mesh.surfaces.size(); surfaceIndex++) {
		const GeoSurface& surface = mesh.surfaces[surfaceIndex];
		const size_t end = (size_t)surface.startIndex + surface.count;
		if (end > mesh.cpuIndices.size()) {
			continue;
		}
		for (size_t i = surface.startIndex; i + 2 < end; i += 3) {
			const Vertex& a = mesh.cpuVertices[mesh.cpuIndices[i]];
			const Vertex& b = mesh.cpuVertices[mesh.cpuIndices[i + 1]];
			const Vertex& c = mesh.cpuVertices[mesh.cpuIndices[i + 2]];
			vertices.push_back(a.position);
			vertices.push_back(b.position);
			vertices.push_back(c.position);

			GpuTriangleAttributes attributes;
			attributes.n0 = glm::vec4(a.normal, a.uv_x);
			attributes.n1 = glm::vec4(b.normal, b.uv_x);
			attributes.n2 = glm::vec4(c.normal, c.uv_x);
			attributes.vAndSurface = glm::vec4(a.uv_y, b.uv_y, c.uv_y, std::bit_cast<float>(surfaceIndex));
			out->attributes.push_back(attributes);
		}
	}

	out->blas = buildBlas(vertices, effectiveBlasOptions(settings), settings.layout);
	if (!out->blas.error.empty()) {
		fmt::println("RaytraceBlasCache: mesh '{}' is not traced - {}", mesh.name, out->blas.error);
	}
	return out;
}

}

std::shared_ptr<const RaytraceBlasSet> RaytraceBlasCache::build(const RaytraceMeshData& meshData, const AccelSettings& settings)
{
	const auto start = std::chrono::steady_clock::now();

	if (!(settings == m_settings)) {
		m_cache.clear();
		m_settings = settings;
	}

	//only the meshes not already cached are built, several at once: each is independent, and a
	//large model is a hundred or more of them
	std::vector<const MeshAsset*> toBuild;
	for (const std::shared_ptr<const MeshAsset>& mesh : meshData.meshes) {
		if (mesh != nullptr && m_cache.count(mesh.get()) == 0 && std::find(toBuild.begin(), toBuild.end(), mesh.get()) == toBuild.end()) {
			toBuild.push_back(mesh.get());
		}
	}

	const size_t threads = std::max(std::thread::hardware_concurrency(), 1u);
	std::vector<std::shared_ptr<const RaytraceBlas>> built(toBuild.size());
	std::vector<std::future<void>> workers;
	std::atomic<size_t> next { 0 };
	for (size_t t = 0; t < std::min(threads, toBuild.size()); t++) {
		workers.push_back(std::async(std::launch::async, [&]() {
			for (size_t i = next++; i < toBuild.size(); i = next++) {
				built[i] = buildMeshBlas(*toBuild[i], settings);
			}
		}));
	}
	for (auto& worker : workers) {
		worker.get();
	}
	for (size_t i = 0; i < toBuild.size(); i++) {
		m_cache[toBuild[i]] = built[i];
	}

	//meshes that are no longer loaded give their BLAS back
	std::unordered_map<const MeshAsset*, std::shared_ptr<const RaytraceBlas>> kept;
	auto set = std::make_shared<RaytraceBlasSet>();
	set->settings = settings;
	double sahWeighted = 0.0;
	for (const std::shared_ptr<const MeshAsset>& mesh : meshData.meshes) {
		std::shared_ptr<const RaytraceBlas> blas = mesh != nullptr ? m_cache[mesh.get()] : nullptr;
		if (mesh != nullptr) {
			kept[mesh.get()] = blas;
		}
		set->blases.push_back(blas);
		if (blas == nullptr) {
			continue;
		}
		const Blas& b = blas->blas;
		set->triangles += b.triangleCount;
		set->triangleRefs += b.triangles.size();
		set->nodes += b.bvh.nodeCount;
		set->bytes += b.bvh.nodes.size() * sizeof(glm::uvec4) + b.triangles.size() * sizeof(BvhTriangle) + blas->attributes.size() * sizeof(GpuTriangleAttributes);
		set->maxDepth = std::max(set->maxDepth, b.bvh.depth);
		sahWeighted += (double)b.stats.sahCost * b.triangleCount;
		if (!b.error.empty()) {
			set->errors.push_back(fmt::format("{}: {}", mesh->name, b.error));
		}
	}
	m_cache = std::move(kept);

	set->meanSahCost = set->triangles > 0 ? (float)(sahWeighted / (double)set->triangles) : 0.f;
	set->buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	fmt::println("RaytraceBlasCache: {} mesh(es) ({} built now) with {} / {} in {:.0f} ms - {} triangles, {} nodes, {:.1f} MB, mean SAH {:.1f}",
		set->blases.size(), toBuild.size(), bvhBuilderName(settings.blas.builder), bvhLayoutName(settings.layout), set->buildMs,
		set->triangles, set->nodes, (double)set->bytes / (1024.0 * 1024.0), set->meanSahCost);
	return set;
}

RaytraceGeometry packGeometry(const RaytraceBlasSet& blases)
{
	RaytraceGeometry geometry;
	//every BLAS in a set shares its layout, so node indices scale to words the same way for all
	const size_t wordsPerNode = blases.settings.layout == BvhLayout::Cwbvh8 ? 5 : 4;
	for (const std::shared_ptr<const RaytraceBlas>& entry : blases.blases) {
		BlasPlacement placement;
		placement.nodeBase = (uint32_t)(geometry.nodes.size() / wordsPerNode);
		placement.triangleBase = (uint32_t)geometry.triangles.size();
		placement.attributeBase = (uint32_t)geometry.attributes.size();
		geometry.placements.push_back(placement);
		if (entry == nullptr || !entry->blas.usable()) {
			continue;
		}
		geometry.nodes.insert(geometry.nodes.end(), entry->blas.bvh.nodes.begin(), entry->blas.bvh.nodes.end());
		geometry.triangles.insert(geometry.triangles.end(), entry->blas.triangles.begin(), entry->blas.triangles.end());
		geometry.attributes.insert(geometry.attributes.end(), entry->attributes.begin(), entry->attributes.end());
	}
	return geometry;
}

std::shared_ptr<const RaytraceSceneAccel> buildSceneAccel(std::shared_ptr<const RaytraceBlasSet> blases, std::shared_ptr<const RaytraceGeometry> geometry,
	const RaytraceMeshData& meshData, const std::vector<SceneMeshObject>& objects, const std::vector<SceneSphere>& spheres, const RaytraceTextureArray& textures)
{
	auto accel = std::make_shared<RaytraceSceneAccel>();
	accel->blases = blases;
	accel->geometry = geometry;

	std::vector<SceneInstance> instances;
	//per instance, what its GPU record needs beyond the TLAS: a mesh's BLAS and material table base,
	//or a sphere's material
	std::vector<uint32_t> materialBase;

	//spheres first, so a sphere's material index is its index in the list, as the shade stage has
	//always assumed
	for (uint32_t i = 0; i < spheres.size(); i++) {
		accel->materials.push_back(RaytraceTriMaterial { spheres[i].material, -1 });
		SceneInstance instance;
		instance.kind = SceneInstanceKind::Sphere;
		instance.sphere = glm::vec4(spheres[i].center, spheres[i].radius);
		instances.push_back(instance);
		materialBase.push_back(i);
	}
	accel->sphereCount = (uint32_t)spheres.size();

	//one material per distinct glTF material, and one per object that overrides its own
	std::unordered_map<const GLTFMaterial*, uint32_t> gltfMaterials;
	for (const SceneMeshObject& object : objects) {
		if (!object.visible || blases == nullptr) {
			continue;
		}
		const RTMeshNode* node = meshData.findNode(object.modelKey, object.nodeIndex);
		if (node == nullptr || node->surfaces.empty()) {
			continue;
		}
		const size_t meshIndex = node->surfaces.front().meshIndex;
		if (meshIndex >= blases->blases.size() || blases->blases[meshIndex] == nullptr || !blases->blases[meshIndex]->blas.usable()) {
			continue;
		}
		const MeshAsset& mesh = *meshData.meshes[meshIndex];

		uint32_t overrideMaterial = 0;
		if (object.materialMode == MeshMaterialMode::Override) {
			//an override replaces the glTF material outright, texture included
			overrideMaterial = (uint32_t)accel->materials.size();
			accel->materials.push_back(RaytraceTriMaterial { object.material, -1 });
		}

		//the instance's material table: one entry per GeoSurface of its mesh
		materialBase.push_back((uint32_t)accel->instanceMaterials.size());
		for (const GeoSurface& surface : mesh.surfaces) {
			if (object.materialMode == MeshMaterialMode::Override) {
				accel->instanceMaterials.push_back(overrideMaterial);
				continue;
			}
			const GLTFMaterial* material = surface.material.get();
			auto [it, inserted] = gltfMaterials.try_emplace(material, (uint32_t)accel->materials.size());
			if (inserted) {
				//glTF pbr is not one of the four materials the tracer scatters with, so the base
				//colour becomes a diffuse albedo: the honest reading of a base-colour factor and
				//texture, and the metal/rough factors are deliberately unused
				RaytraceTriMaterial entry;
				entry.material.type = MaterialType::Lambertian;
				entry.material.albedo = material != nullptr ? glm::vec3(material->colorFactors) : glm::vec3(1.f);
				entry.albedoLayer = material != nullptr ? textures.layerOf(material->baseColorImage.image) : -1;
				accel->materials.push_back(entry);
			}
			accel->instanceMaterials.push_back(it->second);
		}

		SceneInstance instance;
		instance.kind = SceneInstanceKind::Mesh;
		instance.blas = (uint32_t)meshIndex;
		instance.objectToWorld = object.transform;
		instances.push_back(instance);
		accel->meshInstanceCount++;
		accel->placedTriangles += blases->blases[meshIndex]->blas.triangleCount;
	}

	std::vector<const Blas*> blasPointers;
	if (blases != nullptr) {
		for (const std::shared_ptr<const RaytraceBlas>& entry : blases->blases) {
			blasPointers.push_back(entry != nullptr ? &entry->blas : nullptr);
		}
	}
	accel->tlas = buildTlas(blasPointers, instances);

	//the GPU's instance records in TLAS slot order, so a TLAS leaf names its instance directly
	for (const uint32_t index : accel->tlas.bvh.primOrder) {
		const SceneInstance& instance = instances[index];
		GpuInstance gpu {};
		gpu.materialBase = materialBase[index];
		if (instance.kind == SceneInstanceKind::Sphere) {
			gpu.row0 = instance.sphere;
			gpu.nodeBase = GPU_INSTANCE_SPHERE;
		} else {
			const glm::mat4& m = accel->tlas.worldToObject[index];
			gpu.row0 = glm::vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
			gpu.row1 = glm::vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
			gpu.row2 = glm::vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
			const BlasPlacement& placement = geometry->placements[instance.blas];
			gpu.nodeBase = placement.nodeBase;
			gpu.triangleBase = placement.triangleBase;
			gpu.attributeBase = placement.attributeBase;
		}
		accel->instances.push_back(gpu);
	}

	if (!accel->tlas.bvh.error.empty()) {
		fmt::println("buildSceneAccel: the TLAS is not traced - {}", accel->tlas.bvh.error);
	}
	return accel;
}
