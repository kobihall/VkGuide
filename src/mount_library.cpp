#include <mount_library.h>

#include <algorithm>
#include <chrono>

#include <fmt/format.h>

#include <imported_model.h>
#include <rt_material.h>
#include <spectrum_ref.h>
#include <vk_loader.h>

namespace {

// The material each finish is traced with, and what the raster preview shows. Anodized aluminum is the plan's
// diffuse reflectance of 0.04; steel and brass are the glTF metal at roughness 0.3 (pbrt's gray reflectance of
// 0.55 for steel); a mirror blank is pbrt's glass at index 1.5. A textured surface's color is the decal
// texture's, which holds the finish's own color around the decals, so its albedo is white
ImportedMaterial finishMaterial(MountFinish finish, int texture)
{
	const MountFinishInfo& info = mountFinishInfo(finish);
	const glm::vec3 color = texture >= 0 ? glm::vec3(1.f) : glm::vec3(info.baseColor);
	ImportedMaterial material;
	material.baseColorImage = texture;
	material.previewColor = texture >= 0 ? glm::vec4(1.f) : info.baseColor;
	material.previewMetallic = info.metallic;
	material.previewRoughness = info.roughness;
	switch (finish) {
	case MountFinish::Stainless:
	case MountFinish::Brass:
		material.definition.type = MaterialType::Pbr;
		material.definition.params["albedo"] = rgbSpectrum(color);
		material.definition.params["metallic"] = info.metallic;
		material.definition.params["roughness"] = info.roughness;
		break;
	case MountFinish::Glass:
		material.definition = makeMaterial(MaterialType::Glass);
		break;
	default:
		material.definition = makeMaterial(MaterialType::Lambertian, color);
		break;
	}
	material.definition.name = texture >= 0 ? fmt::format("{} decal", info.name) : info.name;
	return material;
}

// The model as the importer's plain data: a material per finish it uses (and per finish it textures), a mesh per
// part with a surface per finish, and a node per part at the identity, since the parts are already in the
// model's canonical frame
ImportedModel toImported(const MountModel& model)
{
	ImportedModel imported;
	if (!model.decalPixels.empty()) {
		ImportedImage image;
		image.path = fmt::format("{}.decals.png", model.info->fileStem);
		image.rgba8 = model.decalPixels;
		image.width = model.decalWidth;
		image.height = model.decalHeight;
		imported.images.push_back(std::move(image));
	}
	int materialOf[MOUNT_FINISH_COUNT][2];
	std::fill(&materialOf[0][0], &materialOf[0][0] + 2 * MOUNT_FINISH_COUNT, -1);
	for (const MountPart& part : model.parts) {
		ImportedMesh mesh;
		mesh.name = fmt::format("{} {}", model.info->id, part.name);
		const bool textured = std::any_of(part.surfaces.begin(), part.surfaces.end(), [](const MountSurface& s) { return s.textured; });
		for (const MountSurface& surface : part.surfaces) {
			int& material = materialOf[(size_t)surface.finish][surface.textured ? 1 : 0];
			if (material < 0) {
				material = (int)imported.materials.size();
				imported.materials.push_back(finishMaterial(surface.finish, surface.textured && !imported.images.empty() ? 0 : -1));
			}
			const uint32_t base = (uint32_t)mesh.positions.size();
			ImportedSurface out;
			out.firstIndex = (uint32_t)mesh.indices.size();
			out.indexCount = (uint32_t)surface.indices.size();
			out.material = (uint32_t)material;
			mesh.positions.insert(mesh.positions.end(), surface.positions.begin(), surface.positions.end());
			mesh.normals.insert(mesh.normals.end(), surface.normals.begin(), surface.normals.end());
			// a mesh has uvs for every vertex or none: an untextured surface's are never read
			if (textured) {
				if (surface.textured) {
					mesh.uvs.insert(mesh.uvs.end(), surface.uvs.begin(), surface.uvs.end());
				} else {
					mesh.uvs.resize(mesh.positions.size(), glm::vec2(0.f));
				}
			}
			for (uint32_t index : surface.indices) {
				mesh.indices.push_back(base + index);
			}
			mesh.surfaces.push_back(out);
		}
		ImportedNode node;
		node.name = part.name;
		node.mesh = (uint32_t)imported.meshes.size();
		imported.meshes.push_back(std::move(mesh));
		imported.nodes.push_back(std::move(node));
	}
	return imported;
}

}

void MountLibrary::open(VulkanEngine* engine, const std::filesystem::path& directory)
{
	clear();
	m_engine = engine;
	m_directory = directory;
	for (const MountModelInfo& info : mountModels()) {
		Entry entry;
		entry.info = &info;
		m_entries.push_back(std::move(entry));
		if (isPlaceholderKind(info.kind)) {
			continue;
		}
		for (const char* extension : { ".glb", ".mates.json" }) {
			const std::filesystem::path file = directory / (std::string(info.fileStem) + extension);
			if (!std::filesystem::exists(file)) {
				m_warnings.push_back(fmt::format("{}: a placeholder box stands in (no {} in {})", info.id, file.filename().string(), directory.string()));
				fmt::println("optical models: {}", m_warnings.back());
				break;
			}
		}
	}
}

void MountLibrary::clear()
{
	m_entries.clear();
	m_warnings.clear();
}

MountLibrary::Entry* MountLibrary::find(std::string_view id)
{
	for (Entry& entry : m_entries) {
		if (id == entry.info->id) {
			return &entry;
		}
	}
	return nullptr;
}

const MountModel* MountLibrary::model(std::string_view id)
{
	Entry* entry = find(id);
	if (entry == nullptr) {
		return nullptr;
	}
	if (!entry->model) {
		const auto start = std::chrono::steady_clock::now();
		entry->model = std::make_unique<MountModel>(loadMountModel(m_directory, *entry->info));
		const std::string& warning = entry->model->warning;
		// a missing file was already reported by open(); an unreadable one is news
		if (!warning.empty() && std::filesystem::exists(m_directory / (std::string(entry->info->fileStem) + ".glb"))
			&& std::filesystem::exists(m_directory / (std::string(entry->info->fileStem) + ".mates.json"))) {
			m_warnings.push_back(fmt::format("{}: a placeholder box stands in ({})", entry->info->id, warning));
			fmt::println("optical models: {}", m_warnings.back());
		}
		fmt::println("optical models: read {} ({} triangles) in {:.3f} s", entry->info->id, entry->model->triangleCount(),
			std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
	}
	return entry->model.get();
}

std::shared_ptr<LoadedModel> MountLibrary::meshes(std::string_view id)
{
	const MountModel* read = model(id);
	if (read == nullptr) {
		return nullptr;
	}
	Entry* entry = find(id);
	if (!entry->meshes) {
		entry->meshes = loadImportedModel(m_engine, toImported(*read));
	}
	return entry->meshes;
}

size_t MountLibrary::readCount() const
{
	size_t count = 0;
	for (const Entry& entry : m_entries) {
		count += entry.model != nullptr;
	}
	return count;
}

size_t MountLibrary::uploadedCount() const
{
	size_t count = 0;
	for (const Entry& entry : m_entries) {
		count += entry.meshes != nullptr;
	}
	return count;
}
