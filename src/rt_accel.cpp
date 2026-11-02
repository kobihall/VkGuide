#include <rt_accel.h>

#include <rt_material_record.h>
#include <texture_library.h>

#include <algorithm>
#include <functional>
#include <bit>
#include <chrono>
#include <future>
#include <map>
#include <thread>

#include <fmt/format.h>
#include <glm/gtc/packing.hpp>

#include <optic_library.h>
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
	//per triangle, whether its material can cut it out and which surface it belongs to: both marked
	//on the BLAS's triangles below
	std::vector<uint8_t> cutout;
	std::vector<uint32_t> surfaceOf;
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
		surfaceOf.push_back(surfaceIndex);

		const glm::vec4 ta = tangentOf(ia);
		const glm::vec4 tb = tangentOf(ib);
		const glm::vec4 tc = tangentOf(ic);

		GpuTriangleAttributes attributes;
		attributes.n0 = glm::vec4(a.normal, a.uv_x);
		attributes.n1 = glm::vec4(b.normal, b.uv_x);
		attributes.n2 = glm::vec4(c.normal, c.uv_x);
		if (mesh.cpuNormalsMissing) {
			//no vertex normals in the file: the triangle shades flat, with its own normal at every corner
			const glm::vec3 face = glm::cross(b.position - a.position, c.position - a.position);
			const glm::vec3 n = glm::dot(face, face) > 0.f ? glm::normalize(face) : glm::vec3(0.f, 0.f, 1.f);
			attributes.n0 = glm::vec4(n, a.uv_x);
			attributes.n1 = glm::vec4(n, b.uv_x);
			attributes.n2 = glm::vec4(n, c.uv_x);
		}
		attributes.vAndSurface = glm::vec4(a.uv_y, b.uv_y, c.uv_y, std::bit_cast<float>(surfaceIndex));
		auto missing = [](const glm::vec4& t) { return glm::dot(glm::vec3(t), glm::vec3(t)) < 1e-12f; };
		attributes.tangents = glm::uvec4(packUnitVector(ta), packUnitVector(tb), packUnitVector(tc),
			(ta.w < 0.f ? 1u : 0u) | (tb.w < 0.f ? 2u : 0u) | (tc.w < 0.f ? 4u : 0u)
				| (missing(ta) ? 8u : 0u) | (missing(tb) ? 16u : 0u) | (missing(tc) ? 32u : 0u));
		out->attributes.push_back(attributes);
		if (mesh.cpuUv1.size() == mesh.cpuVertices.size()) {
			out->uv1.insert(out->uv1.end(), { mesh.cpuUv1[ia], mesh.cpuUv1[ib], mesh.cpuUv1[ic] });
		}
	});

	out->blas = buildBlas(vertices, effectiveBlasOptions(settings), settings.layout);
	if (!out->blas.error.empty()) {
		fmt::println("RaytraceBlasCache: mesh '{}' is not traced - {}", mesh.name, out->blas.error);
	}
	for (BvhTriangle& triangle : out->blas.triangles) {
		const uint32_t index = std::bit_cast<uint32_t>(triangle.v0.w);
		if (index >= cutout.size()) {
			continue;
		}
		if (cutout[index] != 0) {
			triangle.v0.w = std::bit_cast<float>(index | BVH_TRIANGLE_CUTOUT);
		}
		triangle.v1.w = std::bit_cast<float>(surfaceOf[index]);
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
	//the second uv sets, once any mesh has one: a mesh without one reads its first set there too
	const bool anyUv1 = std::any_of(blases.blases.begin(), blases.blases.end(), [](const std::shared_ptr<const RaytraceBlas>& entry) {
		return entry != nullptr && entry->blas.usable() && !entry->uv1.empty();
	});
	if (anyUv1) {
		for (const std::shared_ptr<const RaytraceBlas>& entry : blases.blases) {
			if (entry == nullptr || !entry->blas.usable()) {
				continue;
			}
			if (entry->uv1.size() == entry->attributes.size() * 3) {
				geometry.uv1.insert(geometry.uv1.end(), entry->uv1.begin(), entry->uv1.end());
				continue;
			}
			for (const GpuTriangleAttributes& a : entry->attributes) {
				geometry.uv1.insert(geometry.uv1.end(), { glm::vec2(a.n0.w, a.vAndSurface.x), glm::vec2(a.n1.w, a.vAndSurface.y), glm::vec2(a.n2.w, a.vAndSurface.z) });
			}
		}
	}
	return geometry;
}

namespace {

// the glTF material's texture layers and alpha cutoff: what its library entry takes from the file
// and never lets the user edit
void setModelReadOnlyParams(MaterialDef& entry, const GLTFMaterial& material, const RaytraceTextureArray& textures)
{
	entry.params["baseColorTexture"] = TexRef { textures.layerOf(material.baseColorImage.image) };
	entry.params["normalTexture"] = TexRef { textures.layerOf(material.normalImage.image) };
	entry.params["metallicRoughnessTexture"] = TexRef { textures.layerOf(material.metalRoughImage.image) };
	entry.params["emissiveTexture"] = TexRef { textures.layerOf(material.emissiveImage.image) };
	float alphaCutoff = 0.f;
	if (material.alphaMode == GltfAlphaMode::Mask && !material.alphaFromLibrary) {
		//coverage is texel alpha x factor alpha; folding the factor into the threshold leaves the
		//traversal one compare. A zero factor cuts the whole surface away, which is what glTF says
		const float factor = material.colorFactors.a;
		alphaCutoff = factor > 0.f ? material.alphaCutoff / factor : 2.f;
	}
	entry.params["alphaCutoff"] = alphaCutoff;
}

}

namespace {

//---------------------------------------------------------------- optics

constexpr uint32_t NO_COATING = 0xFFFFFFFFu;
constexpr double RGB_LAMBDAS[3] = { 630.0, 532.0, 465.0 };

float floatOf(uint32_t bits)
{
	return std::bit_cast<float>(bits);
}

// A coating's table on one pair of media for one grid, kept across scene builds: a thin-film stack's table takes tens of
// milliseconds, and a gizmo drag rebuilds the scene every frame. The key names everything the table depends on
struct CoatingTableCache {
	std::map<std::string, std::shared_ptr<const CoatingTable>> tables;
	std::shared_ptr<const CoatingTable> get(const std::string& key, const std::function<CoatingTable()>& make)
	{
		const auto found = tables.find(key);
		if (found != tables.end()) {
			return found->second;
		}
		// a bound on what stale entries can hold
		if (tables.size() > 256) {
			tables.clear();
		}
		auto table = std::make_shared<const CoatingTable>(make());
		tables[key] = table;
		return table;
	}
};
CoatingTableCache s_coatingTables;

// What packing the optics needs beyond the scene: the library, the design wavelength and grid, and the binding-56 tables
// already written in this build, by (coating, media)
struct OpticPacker {
	const OpticalLibrary& library;
	const SpectralSceneContext& spectral;
	RaytraceSceneAccel& accel;
	std::map<std::string, uint32_t> tables;

	std::string glassName(const std::string& name) const
	{
		return name.empty() ? std::string(surroundingGlassName(spectral.surrounding)) : name;
	}

	// two vec4 per medium (crt_optic_material.glsl): n at the design wavelength and alpha at red, green and blue; the
	// index's and the absorption's SceneSpectra entries, and whether the index varies
	void packMedia(const OpticPrescription& body)
	{
		for (const OpticMedium& medium : body.media) {
			const OpticGlass* glass = library.findGlass(glassName(medium.glass));
			const OpticGlass fallback = vacuumGlass();
			const OpticGlass& g = glass != nullptr ? *glass : fallback;
			accel.opticData.push_back(glm::vec4((float)glassIndex(g, spectral.designWavelength), (float)glassAbsorption(g, RGB_LAMBDAS[0]),
				(float)glassAbsorption(g, RGB_LAMBDAS[1]), (float)glassAbsorption(g, RGB_LAMBDAS[2])));
			const uint32_t index = accel.spectra.add(g.index, SpectrumRole::Scalar);
			const uint32_t absorption = accel.spectra.add(g.absorption, SpectrumRole::Scalar);
			accel.opticData.push_back(glm::vec4(floatOf(index), floatOf(absorption), floatOf(isConstantSpectrum(g.index) ? 0u : 1u), 0.f));
		}
	}

	// the table header's offset in binding 56 for a coating between two media, NO_COATING for an uncoated surface
	uint32_t coatingTable(const std::string& key, const std::string& plusGlass, const std::string& minusGlass)
	{
		const OpticCoating* coating = library.findCoating(key);
		if (coating == nullptr || coating->kind == CoatingKind::Uncoated) {
			return NO_COATING;
		}
		const std::string plus = glassName(plusGlass);
		const std::string minus = glassName(minusGlass);
		const std::string tableKey = fmt::format("{}|{}|{}", key, plus, minus);
		if (const auto found = tables.find(tableKey); found != tables.end()) {
			return found->second;
		}
		const IndexLookup index = library.indexLookup();
		// the grid: three projected columns in exact RGB, one wavelength for a monochromatic render, else 1 nm over the range
		std::vector<float> wavelengths;
		float lambdaMin = 0.f;
		float lambdaStep = 0.f;
		if (!spectral.exactRgb) {
			if (spectral.monochromatic > 0.f) {
				wavelengths.push_back(spectral.monochromatic);
				lambdaMin = spectral.monochromatic;
			} else {
				lambdaMin = std::floor(spectral.lambdaMin);
				const int count = std::clamp((int)std::ceil(spectral.lambdaMax) - (int)lambdaMin + 1, 2, 4096);
				for (int i = 0; i < count; i++) {
					wavelengths.push_back(lambdaMin + (float)i);
				}
				lambdaStep = 1.f;
			}
		}
		const std::string cacheKey = fmt::format("{}|{}|{}|{}|{}|{}|{}|{}", tableKey, library.revision(), spectral.exactRgb, lambdaMin, wavelengths.size(),
			spectral.designWavelength, (int)spectral.spectra.space, (const void*)spectral.spectra.table);
		const std::shared_ptr<const CoatingTable> table = s_coatingTables.get(cacheKey, [&]() {
			return spectral.exactRgb ? coatingTableRgb(*coating, index, plus, minus, spectral.spectra, spectral.designWavelength)
									 : ::coatingTable(*coating, index, plus, minus, wavelengths);
		});

		const uint32_t header = library.packCoatingRecord(*coating, *table, lambdaMin, lambdaStep, spectral.exactRgb,
			[this](const SpectrumRef& ref) { return accel.spectra.add(ref, SpectrumRole::Scalar); }, accel.coatingData);
		tables[tableKey] = header;
		return header;
	}
};

}

MaterialDef gltfMaterialDef(const GLTFMaterial& material, const RaytraceTextureArray& textures)
{
	MaterialDef entry;
	//KHR_materials_transmission at 1, with no texture and nothing emitted, is a window: pbrt's dielectric with the glTF
	//index and the roughness glTF's alpha (the square of the factor) makes. A partial or textured transmission, which
	//would need two materials mixed or a texture on glass, stays the opaque material it was
	const bool emits = glm::any(glm::greaterThan(material.emissiveFactor, glm::vec3(0.f)));
	if (material.transmissionFactor >= 1.f && !material.transmissionTextured && !emits) {
		entry.type = MaterialType::Glass;
		entry.params["eta"] = constantRef(material.ior);
		const float alpha = material.metalRoughFactors.y * material.metalRoughFactors.y;
		entry.params["uRoughness"] = alpha;
		entry.params["vRoughness"] = alpha;
		entry.params["remapRoughness"] = false;
		setModelReadOnlyParams(entry, material, textures);
		return entry;
	}
	entry.type = MaterialType::Pbr;
	entry.params["f0"] = material.dielectricF0;
	entry.params["albedo"] = rgbSpectrum(glm::vec3(material.colorFactors));
	entry.params["metallic"] = material.metalRoughFactors.x;
	entry.params["roughness"] = material.metalRoughFactors.y;
	entry.params["emission"] = rgbSpectrum(material.emissiveFactor);
	entry.params["strength"] = 1.f;
	entry.params["normalScale"] = material.normalScale;
	setModelReadOnlyParams(entry, material, textures);
	return entry;
}

MaterialDef modelMaterialDef(const LoadedModel& model, uint32_t index, const RaytraceTextureArray& textures)
{
	const GLTFMaterial& material = *model.materialsByIndex[index];
	if (!model.imported.has_value() || index >= model.imported->materials.size()) {
		return gltfMaterialDef(material, textures);
	}
	MaterialDef entry = model.imported->materials[index].definition;
	setModelReadOnlyParams(entry, material, textures);
	return entry;
}

void refreshModelMaterialDef(MaterialDef& entry, const GLTFMaterial& material, const RaytraceTextureArray& textures)
{
	setModelReadOnlyParams(entry, material, textures);
}

std::shared_ptr<const RaytraceSceneAccel> buildSceneAccel(std::shared_ptr<const RaytraceBlasSet> blases, std::shared_ptr<const RaytraceGeometry> geometry,
	const RaytraceMeshData& meshData, const std::vector<SceneMeshObject>& objects, const std::vector<SceneShape>& shapes, const std::vector<SceneLight>& lights,
	const std::vector<MaterialDef>& materials, const ModelMaterialIds& modelMaterialIds, const EnvironmentLight& environment, const RaytraceTextureArray& textures,
	const SpectralSceneContext& spectral, const std::vector<SceneOptic>& optics, const OpticalLibrary* library, const BenchTrace* bench,
	const SceneTextureContext& textureContext)
{
	auto accel = std::make_shared<RaytraceSceneAccel>();
	//the bench's finishes are not library entries: an id from BENCH_MATERIAL_ID up is one of them
	auto findMaterialDef = [&](uint64_t id) -> const MaterialDef* {
		const auto found = std::find_if(materials.begin(), materials.end(), [id](const MaterialDef& m) { return m.id == id; });
		if (found != materials.end()) {
			return &*found;
		}
		if (bench != nullptr && id >= BENCH_MATERIAL_ID) {
			const auto finish = std::find_if(bench->materials.begin(), bench->materials.end(), [id](const MaterialDef& m) { return m.id == id; });
			return finish != bench->materials.end() ? &*finish : nullptr;
		}
		return nullptr;
	};

	//every library entry an instance references, packed once per way it is packed: with texture layers for a mesh,
	//without them for a shape, and for a shape with Reverse orientation once more, emitting from its back. Id 0 is a glTF
	//surface with no material at all, white lambertian as the tracer has always shaded it
	std::map<std::tuple<uint64_t, bool, bool>, uint32_t> packedIndex;
	//the light group of each packed material, for the light records of what it emits
	std::vector<uint32_t> materialGroups;
	//the texture library's descriptors, for the records' texture tables
	std::function<std::optional<glm::uvec2>(uint64_t)> textureSlot;
	if (textureContext.library != nullptr) {
		textureSlot = [library = textureContext.library](uint64_t id) -> std::optional<glm::uvec2> {
			const TextureEntry* entry = library->find(id);
			if (entry == nullptr || entry->view == VK_NULL_HANDLE) {
				return std::nullopt;
			}
			return glm::uvec2(entry->descriptor, entry->sampler);
		};
	}
	//a std::function, since a mix's pack function asks for its children's indices: the entry is reserved before it is
	//packed, so the children land after it and a cycle of mixes finds itself already there
	std::function<uint32_t(uint64_t, bool, bool)> materialIndex = [&](uint64_t id, bool textures, bool reversed) -> uint32_t {
		const auto [it, inserted] = packedIndex.try_emplace({ id, textures, reversed }, (uint32_t)accel->materials.size());
		if (inserted) {
			const uint32_t index = it->second;
			accel->materials.emplace_back();
			accel->materialSpectra.push_back(emptyMaterialSpectralRecord());
			materialGroups.push_back(0u);
			const MaterialDef* found = findMaterialDef(id);
			const MaterialDef fallback = makeMaterial(MaterialType::Lambertian, id == 0 ? glm::vec3(1.f) : glm::vec3(0.7f));
			PackedMaterial packed = packMaterial(found != nullptr ? *found : fallback,
				//native storage reads a model's textures from the library alone, so its texture layers are left out
				PackContext { .textures = textures && !textureContext.modelTexturesFromLibrary,
					.spectra = spectral.spectra,
					.designWavelength = spectral.designWavelength,
					.textureSlot = textureSlot,
					.modelTexturesFromLibrary = textureContext.modelTexturesFromLibrary,
					.reversed = reversed,
					.materialIndex = [&materialIndex, textures, reversed](uint64_t child) { return materialIndex(child, textures, reversed); } });
			materialGroups[index] = found != nullptr ? std::min(found->lightGroup, MAX_LIGHT_GROUPS - 1u) : 0u;
			//its spectra, by slot, as indices into the scene's list
			MaterialSpectralRecord record = emptyMaterialSpectralRecord();
			for (size_t slot = 0; slot < MATERIAL_SPECTRUM_SLOTS; slot++) {
				if (packed.spectra[slot].has_value()) {
					record[slot] = accel->spectra.add(packed.spectra[slot]->first, packed.spectra[slot]->second);
				}
			}
			record[SLOT_COUNT] = std::bit_cast<uint32_t>(packed.emissionScale);
			accel->materialSpectra[index] = record;
			accel->textured |= packed.textured;
			accel->materials[index] = std::move(packed);
		}
		return it->second;
	};
	//the flags an emitter's light records carry: one-sided, and from the back
	auto emitterFlags = [&](uint32_t material) {
		const uint32_t flags = accel->materials[material].flags;
		return ((flags & CRT_MATERIAL_FLAG_ONE_SIDED) != 0 ? GPU_LIGHT_FLAG_ONE_SIDED : 0u) | ((flags & CRT_MATERIAL_FLAG_REVERSED) != 0 ? GPU_LIGHT_FLAG_REVERSED : 0u);
	};
	//the mean of an emitter's library texture: its share of the light selection
	auto emissionMean = [&](uint32_t material) {
		const uint64_t texture = accel->materials[material].emissionTexture;
		return texture != 0 && textureContext.library != nullptr ? textureContext.library->meanLuminance(texture) : 1.f;
	};
	accel->blases = blases;
	accel->geometry = geometry;

	std::vector<SceneInstance> instances;
	//per instance, what its GPU record needs beyond the TLAS: a mesh's BLAS and material table base,
	//or a shape's material, and where its lights are
	std::vector<uint32_t> materialBase;
	std::vector<uint32_t> lightBase;
	//per instance, its bench tag or -1
	std::vector<int32_t> benchTag;

	//the punctual lights first: the delta lights are the front of the list
	LightListBuilder lightList(&accel->spectra, spectral.spectra);
	//what an emitting material's records carry for the spectral builds
	auto emitterSpectrum = [&](uint32_t material) {
		return LightSpectrum { accel->materialSpectra[material][SLOT_EMISSION], accel->materials[material].emissionScale, accel->materials[material].emissionPrice };
	};
	for (const SceneLight& light : lights) {
		//a projection or goniometric light's image, from the library, measured for its power
		LightImage image;
		if ((light.kind == LightKind::Projection || light.kind == LightKind::Goniometric) && !light.image.empty() && textureContext.library != nullptr) {
			const LightImageRequest request = lightImageRequest(light.image);
			const TextureEntry* entry = textureContext.library->find(request.source, request.colorSpace, request.sampling);
			if (entry != nullptr && entry->view != VK_NULL_HANDLE) {
				image = lightImageMeasure(light.kind == LightKind::Projection, light.fovDegrees, entry->luminanceSize.x, entry->luminanceSize.y, entry->luminance.data());
				image.descriptor = entry->descriptor;
				image.sampler = entry->sampler;
				//the frustum follows the stored image's shape, which the luminance copy shares
				image.aspect = (float)entry->size.x / (float)std::max(entry->size.y, 1u);
				image.luminance = entry->luminance.data();
				image.luminanceSize = entry->luminanceSize;
				accel->textured = true;
			}
		}
		lightList.addPunctual(light, image);
	}

	for (uint32_t i = 0; i < shapes.size(); i++) {
		const uint32_t material = materialIndex(shapes[i].materialId, false, shapes[i].reverseOrientation);
		SceneInstance instance;
		instance.kind = SceneInstanceKind::Shape;
		instance.shape = shapes[i].kind;
		instance.objectToWorld = shapes[i].objectToWorld();
		//a shape's light records name its instance, whose material a textured emitter's samples are looked up in
		ShapeEmitter emitter;
		emitter.flags = emitterFlags(material) | (accel->materials[material].emissionTexture != 0 ? GPU_LIGHT_FLAG_TEXTURED : 0u);
		emitter.instance = (uint32_t)instances.size();
		emitter.textureMean = emissionMean(material);
		instances.push_back(instance);
		materialBase.push_back(material);
		lightBase.push_back(lightList.addShape(shapes[i].kind, instance.objectToWorld, accel->materials[material].emission, materialGroups[material], emitterSpectrum(material), emitter));
		benchTag.push_back(-1);
	}
	accel->shapeCount = (uint32_t)shapes.size();

	//THE OPTICAL BODIES: each element's prescription, built once per distinct body, its records and media table in
	//binding 55, and one instance under the element's transform. Its table in instanceMaterials is its slots'
	//materials, then each surface's coating table (crt_optic.glsl)
	struct OpticInstanceRecord {
		uint32_t base;
		uint32_t surfaces;
		uint32_t media;
		uint32_t slots;
	};
	std::map<size_t, OpticInstanceRecord> opticRecords;
	if (library != nullptr && !optics.empty()) {
		OpticPacker packer { *library, spectral, *accel, {} };
		OpticBuildContext context;
		context.library = library;
		context.designWavelength = spectral.designWavelength;
		context.surrounding = surroundingGlassName(spectral.surrounding);
		struct Body {
			OpticPrescription prescription;
			std::shared_ptr<const OpticPrescriptionF> floats;
			uint32_t base;
			uint32_t media;
		};
		std::map<std::string, Body> bodies;
		for (const SceneOptic& optic : optics) {
			const std::string key = opticBodyKey(optic);
			auto found = bodies.find(key);
			if (found == bodies.end()) {
				OpticReadout readout;
				Body body;
				body.prescription = buildOptic(optic, context, readout);
				if (body.prescription.surfaces.empty()) {
					continue;
				}
				body.floats = std::make_shared<const OpticPrescriptionF>(toFloat(body.prescription));
				body.base = packOpticBody(*body.floats, accel->opticData);
				body.media = (uint32_t)accel->opticData.size();
				packer.packMedia(body.prescription);
				found = bodies.emplace(key, std::move(body)).first;
			}
			const Body& body = found->second;
			const OpticTraits& traits = opticTraits(optic.kind);
			const uint32_t tableBase = (uint32_t)accel->instanceMaterials.size();
			for (size_t slot = 0; slot < traits.slots.size(); slot++) {
				accel->instanceMaterials.push_back(materialIndex(optic.materials[slot], false, false));
			}
			for (const OpticSurface& surface : body.prescription.surfaces) {
				const auto material = std::find_if(materials.begin(), materials.end(), [&](const MaterialDef& m) { return m.id == optic.materials[surface.materialSlot]; });
				uint32_t coating = NO_COATING;
				if (material != materials.end() && material->type == MaterialType::OpticalInterface && std::get<int>(materialValue(*material, "finish")) == 0) {
					coating = packer.coatingTable(materialCoating(*material, "coating"), body.prescription.media[surface.mediumPlus].glass, body.prescription.media[surface.mediumMinus].glass);
				}
				accel->instanceMaterials.push_back(coating);
			}
			SceneInstance instance;
			instance.kind = SceneInstanceKind::Optic;
			instance.objectToWorld = glm::mat4(optic.bodyToWorld());
			instance.optic = body.floats;
			opticRecords[instances.size()] = OpticInstanceRecord { body.base, (uint32_t)body.prescription.surfaces.size(), body.media, (uint32_t)traits.slots.size() };
			instances.push_back(instance);
			materialBase.push_back(tableBase);
			lightBase.push_back(GPU_NO_LIGHT);
			benchTag.push_back(-1);
			accel->opticCount++;
		}
	}

	//THE BENCH's posts: exact capped cylinders, which emit nothing
	if (bench != nullptr) {
		accel->benchTags = bench->tags;
		for (const BenchPostInstance& post : bench->posts) {
			SceneInstance instance;
			instance.kind = SceneInstanceKind::Shape;
			instance.shape = ShapeKind::Cylinder;
			instance.objectToWorld = post.objectToWorld;
			instances.push_back(instance);
			materialBase.push_back(materialIndex(post.material, false, false));
			lightBase.push_back(GPU_NO_LIGHT);
			benchTag.push_back((int32_t)post.tag);
		}
	}

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

		//an override replaces every surface's glTF material with one library entry
		const uint32_t overrideMaterial = object.materialMode == MeshMaterialMode::Override ? materialIndex(object.materialId, true, false) : 0;

		//the instance's material table: one entry per GeoSurface of its mesh
		const uint32_t tableBase = (uint32_t)accel->instanceMaterials.size();
		materialBase.push_back(tableBase);
		bool anyEmits = false;
		for (const GeoSurface& surface : mesh.surfaces) {
			if (object.materialMode == MeshMaterialMode::Override) {
				accel->instanceMaterials.push_back(overrideMaterial);
			} else {
				//the library entry the glTF material's import made
				const auto entry = modelMaterialIds.find(surface.material.get());
				accel->instanceMaterials.push_back(materialIndex(entry != modelMaterialIds.end() ? entry->second : 0, true, false));
			}
			const PackedMaterial& packed = accel->materials[accel->instanceMaterials.back()];
			anyEmits |= packed.emission.r > 0.f || packed.emission.g > 0.f || packed.emission.b > 0.f || packed.emissionPrice > 0.f;
		}

		//an emitting mesh puts each emitting triangle in the light list, in world space, and a run of
		//triangleLights entries that lets the intersect kernel name the record of the triangle it hit
		uint32_t meshLightBase = GPU_NO_LIGHT;
		if (anyEmits) {
			const RaytraceBlas& blas = *blases->blases[meshIndex];
			const uint32_t attributeBase = geometry->placements[meshIndex].attributeBase;
			meshLightBase = lightList.beginTriangleRun((uint32_t)blas.attributes.size());
			forEachMeshTriangle(mesh, [&](uint32_t surfaceIndex, uint32_t triangle, uint32_t ia, uint32_t ib, uint32_t ic) {
				const PackedMaterial& material = accel->materials[accel->instanceMaterials[tableBase + surfaceIndex]];
				const glm::vec3 emission = material.emission;
				if (!(emission.r > 0.f || emission.g > 0.f || emission.b > 0.f || material.emissionPrice > 0.f)) {
					return;
				}
				auto world = [&](uint32_t vertex) { return glm::vec3(object.transform * glm::vec4(mesh.cpuVertices[vertex].position, 1.f)); };
				auto uv = [&](uint32_t vertex) { return glm::vec2(mesh.cpuVertices[vertex].uv_x, mesh.cpuVertices[vertex].uv_y); };
				//a textured emitter is as bright as its texture is over this triangle; the GPU applies
				//the texel itself, this only prices the triangle for the light selection
				float textureMean = material.emissiveLayer >= 0 ? textures.meanLuminance(material.emissiveLayer, uv(ia), uv(ib), uv(ic)) : 1.f;
				const uint32_t packedIndexOf = accel->instanceMaterials[tableBase + surfaceIndex];
				uint32_t flags = emitterFlags(packedIndexOf);
				if (material.emissionTexture != 0 && textureContext.library != nullptr) {
					textureMean *= textureContext.library->meanLuminance(material.emissionTexture, uv(ia), uv(ib), uv(ic));
					flags |= GPU_LIGHT_FLAG_TEXTURED;
				}
				//a one-sided triangle emits on the side of its normal as the hit sees it: the winding's, turned to its
				//vertex normals (crt_interaction.glsl), so the record says when that is against the winding
				//The hit's normal is the object-space winding normal, turned to the vertex normals' side, carried to world
				//space as a normal; the sampler's is the winding of the world-space vertices, which a mirroring transform
				//(a pbrt import's) turns the other way
				if ((flags & GPU_LIGHT_FLAG_ONE_SIDED) != 0) {
					const glm::vec3 a = world(ia);
					const glm::vec3 winding = glm::cross(world(ib) - a, world(ic) - a);
					const glm::vec3& oa = mesh.cpuVertices[ia].position;
					glm::vec3 objectNormal = glm::cross(mesh.cpuVertices[ib].position - oa, mesh.cpuVertices[ic].position - oa);
					const glm::vec3 vertexNormal = mesh.cpuVertices[ia].normal + mesh.cpuVertices[ib].normal + mesh.cpuVertices[ic].normal;
					if (!mesh.cpuNormalsMissing && glm::dot(objectNormal, vertexNormal) < 0.f) {
						objectNormal = -objectNormal;
					}
					const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(object.transform)));
					if (glm::dot(winding, normalMatrix * objectNormal) < 0.f) {
						flags ^= GPU_LIGHT_FLAG_REVERSED;
					}
				}
				const uint32_t light = lightList.addTriangle(world(ia), world(ib), world(ic), emission, attributeBase + triangle, packedIndexOf, material.emissiveLayer, textureMean,
					materialGroups[packedIndexOf], emitterSpectrum(packedIndexOf), flags);
				lightList.setTriangleLight(meshLightBase, triangle, light);
			});
		}
		lightBase.push_back(meshLightBase);
		benchTag.push_back(-1);

		SceneInstance instance;
		instance.kind = SceneInstanceKind::Mesh;
		instance.blas = (uint32_t)meshIndex;
		instance.objectToWorld = object.transform;
		instances.push_back(instance);
		accel->meshInstanceCount++;
		accel->placedTriangles += blases->blases[meshIndex]->blas.triangleCount;
	}

	//THE BENCH's meshes: the breadboard and every part of every stack, each surface in its finish, none emitting
	if (bench != nullptr && blases != nullptr) {
		for (const BenchMeshInstance& part : bench->meshes) {
			if (part.meshIndex >= blases->blases.size() || blases->blases[part.meshIndex] == nullptr || !blases->blases[part.meshIndex]->blas.usable()) {
				continue;
			}
			materialBase.push_back((uint32_t)accel->instanceMaterials.size());
			for (const uint64_t material : part.surfaceMaterials) {
				accel->instanceMaterials.push_back(materialIndex(material, true, false));
			}
			lightBase.push_back(GPU_NO_LIGHT);
			benchTag.push_back((int32_t)part.tag);
			SceneInstance instance;
			instance.kind = SceneInstanceKind::Mesh;
			instance.blas = (uint32_t)part.meshIndex;
			instance.objectToWorld = part.objectToWorld;
			instances.push_back(instance);
			accel->meshInstanceCount++;
			accel->placedTriangles += blases->blases[part.meshIndex]->blas.triangleCount;
		}
	}

	std::vector<const Blas*> blasPointers;
	if (blases != nullptr) {
		for (const std::shared_ptr<const RaytraceBlas>& entry : blases->blases) {
			blasPointers.push_back(entry != nullptr ? &entry->blas : nullptr);
		}
	}
	accel->tlas = buildTlas(blasPointers, instances);
	accel->sceneInstances = instances;

	//the GPU's instance records: the unbounded shapes, which every ray tests directly, then the TLAS
	//slot order, so a TLAS leaf names its instance by slot + unboundedCount
	auto gpuInstance = [&](uint32_t index) {
		const SceneInstance& instance = instances[index];
		const glm::mat4& m = accel->tlas.worldToObject[index];
		const glm::mat4& w = instance.objectToWorld;
		GpuInstance gpu {};
		for (int row = 0; row < 3; row++) {
			gpu.toObject[row] = glm::vec4(m[0][row], m[1][row], m[2][row], m[3][row]);
			gpu.toWorld[row] = glm::vec4(w[0][row], w[1][row], w[2][row], w[3][row]);
		}
		gpu.materialBase = materialBase[index];
		gpu.lightBase = lightBase[index];
		if (instance.kind == SceneInstanceKind::Optic) {
			const OpticInstanceRecord& record = opticRecords.at(index);
			gpu.nodeBase = GPU_INSTANCE_SHAPE;
			gpu.triangleBase = GPU_SHAPE_OPTIC;
			gpu.attributeBase = record.base;
			gpu.triangleCount = record.surfaces;
			gpu.opticMedia = record.media;
			gpu.opticSlots = record.slots;
		} else if (instance.kind == SceneInstanceKind::Shape) {
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
		accel->instanceBenchTag.push_back(benchTag[index]);
	}
	accel->unboundedCount = (uint32_t)accel->tlas.unbounded.size();
	for (const uint32_t index : accel->tlas.bvh.primOrder) {
		accel->instances.push_back(gpuInstance(index));
		accel->instanceBenchTag.push_back(benchTag[index]);
	}

	if (!accel->tlas.bvh.error.empty()) {
		fmt::println("buildSceneAccel: the TLAS is not traced - {}", accel->tlas.bvh.error);
	}


	//the environment last, then every light priced: a directional light and the environment by the
	//scene's size, a sphere around its bounded geometry (1 for a scene with none)
	if (environment.distribution != nullptr) {
		lightList.addEnvironment(environment.distribution->luminanceIntegral, environment.intensity, std::min(environment.group, MAX_LIGHT_GROUPS - 1u));
	}
	const Aabb& bounds = accel->tlas.bounds;
	const bool hasBounds = bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z;
	const float sceneRadius = hasBounds ? std::max(0.5f * glm::length(bounds.max - bounds.min), 1e-3f) : 1.f;
	accel->lights = lightList.finish(sceneRadius);
	accel->sceneSphere = glm::vec4(hasBounds ? 0.5f * (bounds.min + bounds.max) : glm::vec3(0.f), sceneRadius);

	//a textured shape emitter's records name its instance as it was made; the GPU's records are in another order, so
	//each is given its instance's slot there
	std::vector<uint32_t> gpuSlot(instances.size(), 0);
	for (size_t slot = 0; slot < accel->tlas.unbounded.size(); slot++) {
		gpuSlot[accel->tlas.unbounded[slot]] = (uint32_t)slot;
	}
	for (size_t slot = 0; slot < accel->tlas.bvh.primOrder.size(); slot++) {
		gpuSlot[accel->tlas.bvh.primOrder[slot]] = (uint32_t)(accel->unboundedCount + slot);
	}
	for (GpuLight& light : accel->lights.lights) {
		if ((light.kind & GPU_LIGHT_FLAG_TEXTURED) == 0) {
			continue;
		}
		const GpuLightKind kind = (GpuLightKind)(light.kind & GPU_LIGHT_KIND_MASK);
		float* field = kind == GpuLightKind::Sphere ? &light.b.w : kind == GpuLightKind::Rectangle ? &light.a.w : kind == GpuLightKind::Cylinder ? &light.c.w : nullptr;
		if (field != nullptr) {
			const uint32_t made = std::bit_cast<uint32_t>(*field);
			*field = std::bit_cast<float>(made < gpuSlot.size() ? gpuSlot[made] : 0u);
		}
	}
	return accel;
}

