#include <rt_accel.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <future>
#include <thread>

#include <fmt/format.h>
#include <glm/gtc/packing.hpp>

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

// a unit vector as the octahedral map's two snorm16s in one word (Cigolle et al. 2014, "A Survey of
// Efficient Representations for Independent Unit Vectors"). crt_intersect.glsl unpackTangent()
uint32_t packUnitVector(glm::vec3 v)
{
	v /= std::max(std::abs(v.x) + std::abs(v.y) + std::abs(v.z), 1e-20f);
	glm::vec2 e(v.x, v.y);
	if (v.z < 0.f) {
		const glm::vec2 signs(e.x >= 0.f ? 1.f : -1.f, e.y >= 0.f ? 1.f : -1.f);
		e = (1.f - glm::abs(glm::vec2(e.y, e.x))) * signs;
	}
	return glm::packSnorm2x16(e);
}

// Every triangle of a mesh in the order its BLAS numbers them - GeoSurface by GeoSurface - as
// visit(surface, triangle, ia, ib, ic) with the vertex indices. The one definition of a triangle's
// index, which its attribute record, its BvhTriangle's v0.w and the light list all go by
template<typename Visit>
void forEachMeshTriangle(const MeshAsset& mesh, Visit&& visit)
{
	uint32_t triangle = 0;
	for (uint32_t surfaceIndex = 0; surfaceIndex < mesh.surfaces.size(); surfaceIndex++) {
		const GeoSurface& surface = mesh.surfaces[surfaceIndex];
		const size_t end = (size_t)surface.startIndex + surface.count;
		if (end > mesh.cpuIndices.size()) {
			continue;
		}
		for (size_t i = surface.startIndex; i + 2 < end; i += 3) {
			visit(surfaceIndex, triangle++, mesh.cpuIndices[i], mesh.cpuIndices[i + 1], mesh.cpuIndices[i + 2]);
		}
	}
}

// one mesh's triangles in object space and their shading attributes, in GeoSurface order
std::shared_ptr<const RaytraceBlas> buildMeshBlas(const MeshAsset& mesh, const AccelSettings& settings)
{
	auto out = std::make_shared<RaytraceBlas>();
	out->surfaceCount = (uint32_t)mesh.surfaces.size();

	const bool hasTangents = mesh.cpuTangents.size() == mesh.cpuVertices.size();
	auto tangentOf = [&](uint32_t vertex) { return hasTangents ? mesh.cpuTangents[vertex] : glm::vec4(0.f, 0.f, 0.f, 1.f); };

	std::vector<glm::vec3> vertices;
	//per triangle, whether its material can cut it out: marked on the BLAS's triangles below
	std::vector<uint8_t> cutout;
	forEachMeshTriangle(mesh, [&](uint32_t surfaceIndex, uint32_t, uint32_t ia, uint32_t ib, uint32_t ic) {
		const GeoSurface& surface = mesh.surfaces[surfaceIndex];
		const bool masked = surface.material != nullptr && surface.material->alphaMode == GltfAlphaMode::Mask;
		const Vertex& a = mesh.cpuVertices[ia];
		const Vertex& b = mesh.cpuVertices[ib];
		const Vertex& c = mesh.cpuVertices[ic];
		vertices.push_back(a.position);
		vertices.push_back(b.position);
		vertices.push_back(c.position);
		cutout.push_back(masked ? 1 : 0);

		const glm::vec4 ta = tangentOf(ia);
		const glm::vec4 tb = tangentOf(ib);
		const glm::vec4 tc = tangentOf(ic);

		GpuTriangleAttributes attributes;
		attributes.n0 = glm::vec4(a.normal, a.uv_x);
		attributes.n1 = glm::vec4(b.normal, b.uv_x);
		attributes.n2 = glm::vec4(c.normal, c.uv_x);
		attributes.vAndSurface = glm::vec4(a.uv_y, b.uv_y, c.uv_y, std::bit_cast<float>(surfaceIndex));
		auto missing = [](const glm::vec4& t) { return glm::dot(glm::vec3(t), glm::vec3(t)) < 1e-12f; };
		attributes.tangents = glm::uvec4(packUnitVector(ta), packUnitVector(tb), packUnitVector(tc),
			(ta.w < 0.f ? 1u : 0u) | (tb.w < 0.f ? 2u : 0u) | (tc.w < 0.f ? 4u : 0u)
				| (missing(ta) ? 8u : 0u) | (missing(tb) ? 16u : 0u) | (missing(tc) ? 32u : 0u));
		out->attributes.push_back(attributes);
	});

	out->blas = buildBlas(vertices, effectiveBlasOptions(settings), settings.layout);
	if (!out->blas.error.empty()) {
		fmt::println("RaytraceBlasCache: mesh '{}' is not traced - {}", mesh.name, out->blas.error);
	}
	for (BvhTriangle& triangle : out->blas.triangles) {
		const uint32_t index = std::bit_cast<uint32_t>(triangle.v0.w);
		if (index < cutout.size() && cutout[index] != 0) {
			triangle.v0.w = std::bit_cast<float>(index | BVH_TRIANGLE_CUTOUT);
		}
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
		placement.triangleCount = (entry != nullptr && entry->blas.usable()) ? (uint32_t)entry->blas.triangles.size() : 0;
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

glm::vec3 materialEmission(const SceneMaterial& material)
{
	switch (material.type) {
	case MaterialType::Emissive:
		return material.albedo * material.strength;
	case MaterialType::Pbr:
		return glm::max(material.emission, glm::vec3(0.f)) * std::max(material.strength, 0.f);
	case MaterialType::Lambertian:
	case MaterialType::Metal:
	case MaterialType::Phong:
	case MaterialType::Dielectric:
		break;
	}
	return glm::vec3(0.f);
}

RaytraceTriMaterial gltfTriMaterial(const GLTFMaterial& material, const RaytraceTextureArray& textures)
{
	RaytraceTriMaterial entry;
	entry.material.type = MaterialType::Pbr;
	entry.material.albedo = glm::vec3(material.colorFactors);
	entry.material.metallic = material.metalRoughFactors.x;
	entry.material.roughness = material.metalRoughFactors.y;
	entry.material.emission = material.emissiveFactor;
	entry.material.strength = 1.f;
	entry.albedoLayer = textures.layerOf(material.baseColorImage.image);
	entry.normalLayer = textures.layerOf(material.normalImage.image);
	entry.metalRoughLayer = textures.layerOf(material.metalRoughImage.image);
	entry.emissiveLayer = textures.layerOf(material.emissiveImage.image);
	entry.normalScale = material.normalScale;
	if (material.alphaMode == GltfAlphaMode::Mask) {
		//coverage is texel alpha x factor alpha; folding the factor into the threshold leaves the
		//traversal one compare. A zero factor cuts the whole surface away, which is what glTF says
		const float factor = material.colorFactors.a;
		entry.alphaCutoff = factor > 0.f ? material.alphaCutoff / factor : 2.f;
	}
	return entry;
}

std::shared_ptr<const RaytraceSceneAccel> buildSceneAccel(std::shared_ptr<const RaytraceBlasSet> blases, std::shared_ptr<const RaytraceGeometry> geometry,
	const RaytraceMeshData& meshData, const std::vector<SceneMeshObject>& objects, const std::vector<SceneShape>& shapes, const std::vector<SceneLight>& lights,
	const EnvironmentLight& environment, const RaytraceTextureArray& textures)
{
	auto accel = std::make_shared<RaytraceSceneAccel>();
	accel->blases = blases;
	accel->geometry = geometry;

	std::vector<SceneInstance> instances;
	//per instance, what its GPU record needs beyond the TLAS: a mesh's BLAS and material table base,
	//or a shape's material, and where its lights are
	std::vector<uint32_t> materialBase;
	std::vector<uint32_t> lightBase;

	//the punctual lights first: the delta lights are the front of the list
	LightListBuilder lightList;
	for (const SceneLight& light : lights) {
		lightList.addPunctual(light);
	}

	//shapes first, so a shape's material index is its index in the list
	for (uint32_t i = 0; i < shapes.size(); i++) {
		accel->materials.push_back(RaytraceTriMaterial { shapes[i].material, -1 });
		SceneInstance instance;
		instance.kind = SceneInstanceKind::Shape;
		instance.shape = shapes[i].kind;
		instance.objectToWorld = shapes[i].objectToWorld();
		instances.push_back(instance);
		materialBase.push_back(i);
		lightBase.push_back(lightList.addShape(shapes[i].kind, instance.objectToWorld, materialEmission(shapes[i].material)));
	}
	accel->shapeCount = (uint32_t)shapes.size();

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
		const uint32_t tableBase = (uint32_t)accel->instanceMaterials.size();
		materialBase.push_back(tableBase);
		bool anyEmits = false;
		for (const GeoSurface& surface : mesh.surfaces) {
			if (object.materialMode == MeshMaterialMode::Override) {
				accel->instanceMaterials.push_back(overrideMaterial);
			} else {
				const GLTFMaterial* material = surface.material.get();
				auto [it, inserted] = gltfMaterials.try_emplace(material, (uint32_t)accel->materials.size());
				if (inserted) {
					accel->materials.push_back(material != nullptr ? gltfTriMaterial(*material, textures) : RaytraceTriMaterial { makeSceneMaterial(MaterialType::Lambertian, glm::vec3(1.f)) });
				}
				accel->instanceMaterials.push_back(it->second);
			}
			const glm::vec3 emission = materialEmission(accel->materials[accel->instanceMaterials.back()].material);
			anyEmits |= emission.r > 0.f || emission.g > 0.f || emission.b > 0.f;
		}

		//an emitting mesh puts each emitting triangle in the light list, in world space, and a run of
		//triangleLights entries that lets the intersect kernel name the record of the triangle it hit
		uint32_t meshLightBase = GPU_NO_LIGHT;
		if (anyEmits) {
			const RaytraceBlas& blas = *blases->blases[meshIndex];
			const uint32_t attributeBase = geometry->placements[meshIndex].attributeBase;
			meshLightBase = lightList.beginTriangleRun((uint32_t)blas.attributes.size());
			forEachMeshTriangle(mesh, [&](uint32_t surfaceIndex, uint32_t triangle, uint32_t ia, uint32_t ib, uint32_t ic) {
				const RaytraceTriMaterial& material = accel->materials[accel->instanceMaterials[tableBase + surfaceIndex]];
				const glm::vec3 emission = materialEmission(material.material);
				if (!(emission.r > 0.f || emission.g > 0.f || emission.b > 0.f)) {
					return;
				}
				auto world = [&](uint32_t vertex) { return glm::vec3(object.transform * glm::vec4(mesh.cpuVertices[vertex].position, 1.f)); };
				auto uv = [&](uint32_t vertex) { return glm::vec2(mesh.cpuVertices[vertex].uv_x, mesh.cpuVertices[vertex].uv_y); };
				//a textured emitter is as bright as its texture is over this triangle; the GPU applies
				//the texel itself, this only prices the triangle for the light selection
				const float textureMean = material.emissiveLayer >= 0 ? textures.meanLuminance(material.emissiveLayer, uv(ia), uv(ib), uv(ic)) : 1.f;
				const uint32_t light = lightList.addTriangle(world(ia), world(ib), world(ic), emission, attributeBase + triangle, accel->instanceMaterials[tableBase + surfaceIndex], material.emissiveLayer, textureMean);
				lightList.setTriangleLight(meshLightBase, triangle, light);
			});
		}
		lightBase.push_back(meshLightBase);

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

	//the GPU's instance records: the unbounded shapes, which every ray tests directly, then the TLAS
	//slot order, so a TLAS leaf names its instance by slot + unboundedCount
	auto gpuInstance = [&](uint32_t index) {
		const SceneInstance& instance = instances[index];
		const glm::mat4& m = accel->tlas.worldToObject[index];
		GpuInstance gpu {};
		gpu.row0 = glm::vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
		gpu.row1 = glm::vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
		gpu.row2 = glm::vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
		gpu.materialBase = materialBase[index];
		gpu.lightBase = lightBase[index];
		if (instance.kind == SceneInstanceKind::Shape) {
			gpu.nodeBase = GPU_INSTANCE_SHAPE;
			gpu.triangleBase = (uint32_t)instance.shape;
		} else {
			const BlasPlacement& placement = geometry->placements[instance.blas];
			gpu.nodeBase = placement.nodeBase;
			gpu.triangleBase = placement.triangleBase;
			gpu.attributeBase = placement.attributeBase;
			gpu.triangleCount = placement.triangleCount;
		}
		return gpu;
	};
	for (const uint32_t index : accel->tlas.unbounded) {
		accel->instances.push_back(gpuInstance(index));
	}
	accel->unboundedCount = (uint32_t)accel->tlas.unbounded.size();
	for (const uint32_t index : accel->tlas.bvh.primOrder) {
		accel->instances.push_back(gpuInstance(index));
	}

	if (!accel->tlas.bvh.error.empty()) {
		fmt::println("buildSceneAccel: the TLAS is not traced - {}", accel->tlas.bvh.error);
	}

	//the environment last, then every light priced: a directional light and the environment by the
	//scene's size, a sphere around its bounded geometry (1 for a scene with none)
	if (environment.distribution != nullptr) {
		lightList.addEnvironment(environment.distribution->luminanceIntegral, environment.intensity);
	}
	const Aabb& bounds = accel->tlas.bounds;
	const bool hasBounds = bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z;
	const float sceneRadius = hasBounds ? std::max(0.5f * glm::length(bounds.max - bounds.min), 1e-3f) : 1.f;
	accel->lights = lightList.finish(sceneRadius);
	return accel;
}
