#include <optic_models.h>

#include <algorithm>
#include <cmath>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fmt/format.h>
#include <simdjson.h>

namespace {

constexpr double PI = 3.14159265358979323846;

// Kept in step with FINISHES in tools/optical_models/convert.py, which writes these names and colors into
// each .glb. A model's materials are looked up here by name, so the .glb's own factors are not read
constexpr MountFinishInfo FINISHES[MOUNT_FINISH_COUNT] = {
	{ "anodized_black", { 0.04f, 0.04f, 0.04f, 1.f }, 0.f, 0.5f },
	{ "stainless", { 0.55f, 0.55f, 0.55f, 1.f }, 1.f, 0.3f },
	{ "brass", { 0.78f, 0.60f, 0.30f, 1.f }, 1.f, 0.3f },
	{ "glass", { 0.90f, 0.95f, 1.00f, 0.25f }, 0.f, 0.05f },
	{ "engraving", { 0.80f, 0.80f, 0.80f, 1.f }, 0.f, 0.6f },
	{ "placeholder", { 0.35f, 0.35f, 0.38f, 1.f }, 0.f, 0.6f },
};

constexpr const char* KIND_NAMES[(size_t)MountModelKind::Count] = {
	"kinematic mount", "fixed mount", "rotation mount", "prism mount", "plate holder", "iris", "post holder",
	"clamping fork", "mirror blank", "laser head", "diode module", "fiber collimator", "power meter head", "camera",
	"beam profiler", "alignment card", "beam dump",
};

constexpr const char* KINEMATIC_MATES[] = { "optic_seat", "post_tap", "pivot", "adjuster_x_tip", "adjuster_y_tip" };
constexpr const char* FIXED_MATES[] = { "optic_seat", "post_tap", "retaining_ring_seat" };
constexpr const char* ROTATION_MATES[] = { "optic_seat", "post_tap", "rotor_axis" };
constexpr const char* PRISM_MATES[] = { "prism_seat", "post_tap", "pivot", "adjuster_x_tip", "adjuster_y_tip" };
constexpr const char* PLATE_HOLDER_MATES[] = { "plate_seat", "post_tap" };
constexpr const char* IRIS_MATES[] = { "aperture_center", "post_tap", "lever_axis" };
constexpr const char* POST_HOLDER_MATES[] = { "post_entry", "base_contact", "thumbscrew" };
constexpr const char* FORK_MATES[] = { "holder_seat", "slot", "bolt" };
constexpr const char* BLANK_MATES[] = { "front_center", "back_center" };
constexpr const char* SOURCE_MATES[] = { "output", "post_tap" };
constexpr const char* COLLIMATOR_MATES[] = { "output", "back_center" };
constexpr const char* DETECTOR_MATES[] = { "input", "post_tap" };
constexpr const char* CARD_MATES[] = { "face_center", "post_tap" };

// Nominal sizes are the converted models' bounds, rounded up; a placeholder's are its shape's
constexpr MountModelInfo MODELS[] = {
	{ "KM05T/M", "KM05T_M", MountModelKind::KinematicMount, { 0.030f, 0.030f, 0.019f }, nullptr },
	{ "KM100", "KM100", MountModelKind::KinematicMount, { 0.050f, 0.050f, 0.040f }, nullptr },
	{ "KM200", "KM200", MountModelKind::KinematicMount, { 0.077f, 0.077f, 0.043f }, nullptr },
	{ "KM200PM/M", "KM200PM_M", MountModelKind::PrismMount, { 0.077f, 0.071f, 0.077f }, nullptr },
	{ "LMR05/M", "LMR05_M", MountModelKind::FixedMount, { 0.018f, 0.025f, 0.008f }, nullptr },
	{ "LMR1/M", "LMR1_M", MountModelKind::FixedMount, { 0.031f, 0.038f, 0.011f }, nullptr },
	{ "LMR2/M", "LMR2_M", MountModelKind::FixedMount, { 0.056f, 0.063f, 0.013f }, nullptr },
	{ "RSP05/M", "RSP05_M", MountModelKind::RotationMount, { 0.024f, 0.026f, 0.012f }, nullptr },
	{ "RSP1/M", "RSP1_M", MountModelKind::RotationMount, { 0.057f, 0.057f, 0.016f }, nullptr },
	{ "RSP2/M", "RSP2_M", MountModelKind::RotationMount, { 0.084f, 0.084f, 0.016f }, nullptr },
	{ "FP01", "FP01", MountModelKind::PlateHolder, { 0.038f, 0.038f, 0.021f }, nullptr },
	{ "ID8/M", "ID8_M", MountModelKind::Iris, { 0.022f, 0.031f, 0.007f }, nullptr },
	{ "ID12/M", "ID12_M", MountModelKind::Iris, { 0.027f, 0.037f, 0.007f }, nullptr },
	{ "ID15/M", "ID15_M", MountModelKind::Iris, { 0.031f, 0.040f, 0.007f }, nullptr },
	{ "ID20/M", "ID20_M", MountModelKind::Iris, { 0.037f, 0.045f, 0.008f }, nullptr },
	{ "ID25/M", "ID25_M", MountModelKind::Iris, { 0.044f, 0.052f, 0.007f }, nullptr },
	{ "ID36/M", "ID36_M", MountModelKind::Iris, { 0.057f, 0.065f, 0.008f }, nullptr },
	{ "ID50/M", "ID50_M", MountModelKind::Iris, { 0.080f, 0.085f, 0.009f }, nullptr },
	{ "PH50E/M", "PH50E_M", MountModelKind::PostHolder, { 0.032f, 0.055f, 0.038f }, nullptr },
	{ "CF125C/M", "CF125C_M", MountModelKind::ClampingFork, { 0.037f, 0.016f, 0.074f }, nullptr },
	{ "PF05-03", "PF05-03", MountModelKind::MirrorBlank, { 0.0127f, 0.0127f, 0.006f }, nullptr },
	{ "PF10-03", "PF10-03", MountModelKind::MirrorBlank, { 0.0254f, 0.0254f, 0.006f }, nullptr },
	{ "PF20-03", "PF20-03", MountModelKind::MirrorBlank, { 0.0508f, 0.0508f, 0.012f }, nullptr },
	{ "laser_head", "laser_head", MountModelKind::LaserHead, { 0.045f, 0.045f, 0.300f }, "HNL050LB" },
	{ "diode_module", "diode_module", MountModelKind::DiodeModule, { 0.011f, 0.011f, 0.050f }, "CPS635R" },
	{ "fiber_collimator", "fiber_collimator", MountModelKind::FiberCollimator, { 0.012f, 0.012f, 0.030f }, "F220APC-633 in AD11F" },
	{ "power_meter_head", "power_meter_head", MountModelKind::PowerMeterHead, { 0.050f, 0.080f, 0.012f }, "S120C" },
	{ "camera", "camera", MountModelKind::Camera, { 0.045f, 0.045f, 0.050f }, "CS165MU" },
	{ "beam_profiler", "beam_profiler", MountModelKind::BeamProfiler, { 0.050f, 0.050f, 0.050f }, "BC207VIS/M" },
	{ "alignment_card", "alignment_card", MountModelKind::AlignmentCard, { 0.0856f, 0.054f, 0.0008f }, "VRC2 or VRC4" },
	{ "beam_dump", "beam_dump", MountModelKind::BeamDump, { 0.030f, 0.030f, 0.050f }, "BT610/M or LB1/M" },
};

//---------------------------------------------------------------- reading

bool readNumber(simdjson::dom::element element, double& out)
{
	if (element.get_double().get(out) == simdjson::SUCCESS) {
		return true;
	}
	int64_t integer = 0;
	if (element.get_int64().get(integer) == simdjson::SUCCESS) {
		out = (double)integer;
		return true;
	}
	return false;
}

bool readDvec3(simdjson::dom::element object, const char* key, glm::dvec3& out)
{
	simdjson::dom::array array;
	if (object[key].get_array().get(array) != simdjson::SUCCESS || array.size() != 3) {
		return false;
	}
	size_t i = 0;
	for (simdjson::dom::element component : array) {
		if (!readNumber(component, out[(glm::length_t)i++])) {
			return false;
		}
	}
	return true;
}

bool readMates(const std::filesystem::path& path, MountModel& model, std::string& error)
{
	simdjson::dom::parser parser;
	simdjson::dom::element root;
	if (parser.load(path.string()).get(root) != simdjson::SUCCESS) {
		error = fmt::format("cannot read {}", path.filename().string());
		return false;
	}
	simdjson::dom::element budgetElement;
	double budget = 0.0;
	if (root["budget"].get(budgetElement) == simdjson::SUCCESS && readNumber(budgetElement, budget)) {
		model.triangleBudget = (uint32_t)budget;
	}
	simdjson::dom::object mates;
	if (root["mates"].get_object().get(mates) != simdjson::SUCCESS) {
		error = fmt::format("{} has no mates", path.filename().string());
		return false;
	}
	for (auto [key, value] : mates) {
		MountMate mate;
		if (!readDvec3(value, "origin", mate.origin) || !readDvec3(value, "x", mate.x) || !readDvec3(value, "y", mate.y)
			|| !readDvec3(value, "z", mate.z)) {
			error = fmt::format("{}: mate {} is not a frame", path.filename().string(), std::string(key));
			return false;
		}
		simdjson::dom::object fields;
		if (value.get_object().get(fields) != simdjson::SUCCESS) {
			continue;
		}
		for (auto [name, field] : fields) {
			double number = 0.0;
			std::string_view text;
			if (name == "part" && field.get_string().get(text) == simdjson::SUCCESS) {
				mate.part = std::string(text);
			} else if (name != "origin" && name != "x" && name != "y" && name != "z" && readNumber(field, number)) {
				mate.params[std::string(name)] = number;
			}
		}
		model.mates[std::string(key)] = std::move(mate);
	}
	simdjson::dom::object motions;
	if (root["motions"].get_object().get(motions) == simdjson::SUCCESS) {
		for (auto [part, value] : motions) {
			MountMotion motion;
			motion.part = std::string(part);
			std::string_view kind, mate;
			if (value["kind"].get_string().get(kind) != simdjson::SUCCESS || value["mate"].get_string().get(mate) != simdjson::SUCCESS) {
				error = fmt::format("{}: the motion of {} has no kind or mate", path.filename().string(), std::string(part));
				return false;
			}
			motion.mate = std::string(mate);
			std::string_view drive;
			if (value["drive"].get_string().get(drive) == simdjson::SUCCESS) {
				motion.drive = std::string(drive);
			}
			motion.kind = kind == "tilt" ? MountMotionKind::Tilt
				: kind == "screw"        ? MountMotionKind::Screw
				: kind == "rotate"       ? MountMotionKind::Rotate
										 : MountMotionKind::Slide;
			simdjson::dom::element rangeElement;
			simdjson::dom::array range;
			double number = 0.0;
			if (value["range"].get(rangeElement) != simdjson::SUCCESS) {
				// no range: the part does not move
			} else if (rangeElement.get_array().get(range) == simdjson::SUCCESS) {
				std::vector<double> values;
				for (simdjson::dom::element v : range) {
					if (readNumber(v, number)) {
						values.push_back(number);
					}
				}
				if (!values.empty()) {
					motion.rangeMin = *std::min_element(values.begin(), values.end());
					motion.rangeMax = *std::max_element(values.begin(), values.end());
				}
			} else if (readNumber(rangeElement, number)) {
				// a tilt's range is one angle, either way about both axes
				motion.rangeMin = -number;
				motion.rangeMax = number;
			}
			model.motions.push_back(std::move(motion));
		}
	}
	simdjson::dom::object drives;
	if (root["drives"].get_object().get(drives) == simdjson::SUCCESS) {
		for (auto [name, value] : drives) {
			MountDrive drive;
			drive.name = std::string(name);
			auto readList = [](simdjson::dom::element element, std::vector<double>& out) {
				simdjson::dom::array array;
				if (element.get_array().get(array) != simdjson::SUCCESS) {
					return false;
				}
				for (simdjson::dom::element v : array) {
					double number = 0.0;
					if (!readNumber(v, number)) {
						return false;
					}
					out.push_back(number);
				}
				return true;
			};
			simdjson::dom::element values, rest;
			simdjson::dom::object driven;
			if (value["values"].get(values) != simdjson::SUCCESS || !readList(values, drive.values) || value["rest"].get(rest) != simdjson::SUCCESS
				|| !readNumber(rest, drive.rest) || value["motions"].get_object().get(driven) != simdjson::SUCCESS) {
				error = fmt::format("{}: drive {} has no values, rest or motions", path.filename().string(), drive.name);
				return false;
			}
			for (auto [motion, list] : driven) {
				std::vector<double>& out = drive.motionValues[std::string(motion)];
				if (!readList(list, out) || out.size() != drive.values.size()) {
					error = fmt::format("{}: drive {} gives {} the wrong number of values", path.filename().string(), drive.name, std::string(motion));
					return false;
				}
			}
			model.drives.push_back(std::move(drive));
		}
	}
	return true;
}

bool readMeshes(const std::filesystem::path& path, MountModel& model, std::string& error)
{
	auto data = fastgltf::GltfDataBuffer::FromPath(path);
	if (data.error() != fastgltf::Error::None) {
		error = fmt::format("cannot read {}", path.filename().string());
		return false;
	}
	fastgltf::Parser parser;
	auto asset = parser.loadGltfBinary(data.get(), path.parent_path(), fastgltf::Options::None);
	if (asset.error() != fastgltf::Error::None) {
		error = fmt::format("cannot parse {}: {}", path.filename().string(), fastgltf::getErrorMessage(asset.error()));
		return false;
	}
	// The decal texture, the one image the converter embeds
	if (!asset->images.empty()) {
		std::vector<uint8_t> encoded;
		const auto* view = std::get_if<fastgltf::sources::BufferView>(&asset->images[0].data);
		if (view != nullptr) {
			const fastgltf::BufferView& bufferView = asset->bufferViews[view->bufferViewIndex];
			std::visit(fastgltf::visitor {
						   [](const auto&) {},
						   [&](const fastgltf::sources::Array& array) {
							   const auto* bytes = reinterpret_cast<const uint8_t*>(array.bytes.data()) + bufferView.byteOffset;
							   encoded.assign(bytes, bytes + bufferView.byteLength);
						   },
						   [&](const fastgltf::sources::Vector& vector) {
							   const auto* bytes = reinterpret_cast<const uint8_t*>(vector.bytes.data()) + bufferView.byteOffset;
							   encoded.assign(bytes, bytes + bufferView.byteLength);
						   },
						   [&](const fastgltf::sources::ByteView& bytesView) {
							   const auto* bytes = reinterpret_cast<const uint8_t*>(bytesView.bytes.data()) + bufferView.byteOffset;
							   encoded.assign(bytes, bytes + bufferView.byteLength);
						   },
					   },
				asset->buffers[bufferView.bufferIndex].data);
		}
		int width = 0, height = 0, channels = 0;
		stbi_uc* pixels = encoded.empty() ? nullptr
										  : stbi_load_from_memory(encoded.data(), (int)encoded.size(), &width, &height, &channels, 4);
		if (pixels == nullptr) {
			error = fmt::format("{}: its texture is not a PNG in its binary chunk", path.filename().string());
			return false;
		}
		model.decalPixels.assign(pixels, pixels + (size_t)width * height * 4);
		model.decalWidth = (uint32_t)width;
		model.decalHeight = (uint32_t)height;
		stbi_image_free(pixels);
	}
	// The converter writes one node per part, each with its own mesh and an identity transform, under one root
	for (const fastgltf::Node& node : asset->nodes) {
		if (!node.meshIndex.has_value()) {
			continue;
		}
		MountPart part;
		part.name = std::string(node.name);
		for (const fastgltf::Primitive& primitive : asset->meshes[*node.meshIndex].primitives) {
			const auto* position = primitive.findAttribute("POSITION");
			const auto* normal = primitive.findAttribute("NORMAL");
			if (position == primitive.attributes.end() || normal == primitive.attributes.end() || !primitive.indicesAccessor) {
				error = fmt::format("{}: part {} has a primitive without positions, normals or indices", path.filename().string(), part.name);
				return false;
			}
			MountSurface surface;
			std::string materialName = primitive.materialIndex ? std::string(asset->materials[*primitive.materialIndex].name) : "";
			// a textured primitive's material is its finish's name and ".decal"
			if (primitive.materialIndex && asset->materials[*primitive.materialIndex].pbrData.baseColorTexture.has_value()) {
				surface.textured = true;
				materialName = materialName.substr(0, materialName.find('.'));
			}
			const std::optional<MountFinish> finish = mountFinishByName(materialName);
			if (!finish) {
				error = fmt::format("{}: unknown finish \"{}\"", path.filename().string(), materialName);
				return false;
			}
			surface.finish = *finish;
			const auto& positions = asset->accessors[position->accessorIndex];
			surface.positions.resize(positions.count);
			fastgltf::iterateAccessorWithIndex<glm::vec3>(asset.get(), positions, [&](glm::vec3 v, size_t i) { surface.positions[i] = v; });
			const auto& normals = asset->accessors[normal->accessorIndex];
			surface.normals.resize(normals.count);
			fastgltf::iterateAccessorWithIndex<glm::vec3>(asset.get(), normals, [&](glm::vec3 v, size_t i) { surface.normals[i] = v; });
			if (surface.textured) {
			const auto* uv = primitive.findAttribute("TEXCOORD_0");
			if (uv == primitive.attributes.end() || model.decalPixels.empty()) {
				error = fmt::format("{}: part {} has a textured primitive without uvs or a texture", path.filename().string(), part.name);
				return false;
			}
			const auto& uvs = asset->accessors[uv->accessorIndex];
			surface.uvs.resize(uvs.count);
			fastgltf::iterateAccessorWithIndex<glm::vec2>(asset.get(), uvs, [&](glm::vec2 v, size_t i) { surface.uvs[i] = v; });
		}
		const auto& indices = asset->accessors[*primitive.indicesAccessor];
			surface.indices.reserve(indices.count);
			fastgltf::iterateAccessor<uint32_t>(asset.get(), indices, [&](uint32_t index) { surface.indices.push_back(index); });
			part.surfaces.push_back(std::move(surface));
		}
		model.parts.push_back(std::move(part));
	}
	if (model.parts.empty()) {
		error = fmt::format("{} has no parts", path.filename().string());
		return false;
	}
	return true;
}

void computeBounds(MountModel& model)
{
	glm::vec3 lo(1e30f), hi(-1e30f);
	for (const MountPart& part : model.parts) {
		for (const MountSurface& surface : part.surfaces) {
			for (const glm::vec3& p : surface.positions) {
				lo = glm::min(lo, p);
				hi = glm::max(hi, p);
			}
		}
	}
	model.boundsMin = lo.x <= hi.x ? lo : glm::vec3(0.f);
	model.boundsMax = lo.x <= hi.x ? hi : glm::vec3(0.f);
}

//---------------------------------------------------------------- procedural shapes

// Flat-shaded quads and smooth cylinder walls, appended to one surface
struct MeshBuilder {
	MountSurface surface;

	void quad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d)
	{
		const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
		const uint32_t base = (uint32_t)surface.positions.size();
		for (const glm::vec3& p : { a, b, c, d }) {
			surface.positions.push_back(p);
			surface.normals.push_back(n);
		}
		for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u }) {
			surface.indices.push_back(base + i);
		}
	}

	// an axis-aligned box from lo to hi, faces outward
	void box(const glm::vec3& lo, const glm::vec3& hi)
	{
		const glm::vec3 p[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
			{ lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
		quad(p[0], p[3], p[2], p[1]); // -z
		quad(p[4], p[5], p[6], p[7]); // +z
		quad(p[0], p[1], p[5], p[4]); // -y
		quad(p[3], p[7], p[6], p[2]); // +y
		quad(p[0], p[4], p[7], p[3]); // -x
		quad(p[1], p[2], p[6], p[5]); // +x
	}

	// a closed cylinder along `axis` (a unit vector along x, y or z) from `start` for `length`
	void cylinder(const glm::vec3& start, const glm::vec3& axis, float radius, float length, int segments = 32)
	{
		const glm::vec3 u = std::abs(axis.y) < 0.9f ? glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0))) : glm::vec3(1, 0, 0);
		const glm::vec3 v = glm::cross(axis, u);
		const glm::vec3 end = start + axis * length;
		const uint32_t wall = (uint32_t)surface.positions.size();
		for (int i = 0; i <= segments; i++) {
			const double a = 2.0 * PI * i / segments;
			const glm::vec3 r = u * (float)std::cos(a) + v * (float)std::sin(a);
			surface.positions.push_back(start + r * radius);
			surface.normals.push_back(r);
			surface.positions.push_back(end + r * radius);
			surface.normals.push_back(r);
		}
		for (int i = 0; i < segments; i++) {
			const uint32_t k = wall + 2 * i;
			for (uint32_t j : { k, k + 2, k + 1, k + 1, k + 2, k + 3 }) {
				surface.indices.push_back(j);
			}
		}
		for (int cap = 0; cap < 2; cap++) {
			const glm::vec3 n = cap ? axis : -axis;
			const glm::vec3 c = cap ? end : start;
			const uint32_t center = (uint32_t)surface.positions.size();
			surface.positions.push_back(c);
			surface.normals.push_back(n);
			for (int i = 0; i <= segments; i++) {
				const double a = 2.0 * PI * i / segments;
				surface.positions.push_back(c + (u * (float)std::cos(a) + v * (float)std::sin(a)) * radius);
				surface.normals.push_back(n);
			}
			for (int i = 0; i < segments; i++) {
				const uint32_t a = center + 1 + i, b = center + 2 + i;
				for (uint32_t j : cap ? std::initializer_list<uint32_t> { center, a, b } : std::initializer_list<uint32_t> { center, b, a }) {
					surface.indices.push_back(j);
				}
			}
		}
	}
};

MountMate mateAt(const glm::dvec3& origin, const glm::dvec3& z, const glm::dvec3& y, const char* part)
{
	MountMate mate;
	mate.origin = origin;
	mate.z = glm::normalize(z);
	mate.y = glm::normalize(y - mate.z * glm::dot(y, mate.z));
	mate.x = glm::cross(mate.y, mate.z);
	mate.part = part;
	return mate;
}

// The mates of a converted kind on its fallback box, so that a scene still assembles: the primary mate at
// the origin (the box's front-face center for a mount, its bottom center for a holder), the rest on its faces
void boxMates(const MountModelInfo& info, MountModel& model, const glm::vec3& lo, const glm::vec3& hi)
{
	const glm::dvec3 up(0, 1, 0), forward(0, 0, 1), down(0, -1, 0);
	const glm::dvec3 bottom(0.0, lo.y, (lo.z + hi.z) * 0.5);
	for (const char* name : requiredMates(info.kind)) {
		const std::string n = name;
		if (n == "post_tap") {
			model.mates[n] = mateAt(bottom, down, forward, "body");
		} else if (n == "base_contact" || n == "holder_seat") {
			model.mates[n] = mateAt(glm::dvec3(0.0), down, forward, "body");
		} else if (n == "post_entry") {
			model.mates[n] = mateAt(glm::dvec3(0.0, hi.y, 0.0), up, -forward, "body");
		} else if (n == "back_center") {
			model.mates[n] = mateAt(glm::dvec3(0.0, 0.0, lo.z), forward, up, "body");
		} else if (n == "adjuster_x_tip") {
			model.mates[n] = mateAt(glm::dvec3(lo.x * 0.75, lo.y * 0.75, lo.z * 0.5), forward, up, "body");
		} else if (n == "adjuster_y_tip") {
			model.mates[n] = mateAt(glm::dvec3(hi.x * 0.75, hi.y * 0.75, lo.z * 0.5), forward, up, "body");
		} else if (n == "pivot") {
			model.mates[n] = mateAt(glm::dvec3(hi.x * 0.75, lo.y * 0.75, lo.z * 0.5), forward, up, "body");
		} else {
			model.mates[n] = mateAt(glm::dvec3(0.0), forward, up, "body");
		}
	}
}

}

//---------------------------------------------------------------- the table

const MountFinishInfo& mountFinishInfo(MountFinish finish) { return FINISHES[(size_t)finish]; }

std::optional<MountFinish> mountFinishByName(std::string_view name)
{
	for (size_t i = 0; i < MOUNT_FINISH_COUNT; i++) {
		if (name == FINISHES[i].name) {
			return (MountFinish)i;
		}
	}
	return std::nullopt;
}

const char* mountModelKindName(MountModelKind kind) { return KIND_NAMES[(size_t)kind]; }

std::span<const char* const> requiredMates(MountModelKind kind)
{
	switch (kind) {
	case MountModelKind::KinematicMount: return KINEMATIC_MATES;
	case MountModelKind::FixedMount: return FIXED_MATES;
	case MountModelKind::RotationMount: return ROTATION_MATES;
	case MountModelKind::PrismMount: return PRISM_MATES;
	case MountModelKind::PlateHolder: return PLATE_HOLDER_MATES;
	case MountModelKind::Iris: return IRIS_MATES;
	case MountModelKind::PostHolder: return POST_HOLDER_MATES;
	case MountModelKind::ClampingFork: return FORK_MATES;
	case MountModelKind::MirrorBlank: return BLANK_MATES;
	case MountModelKind::LaserHead:
	case MountModelKind::DiodeModule: return SOURCE_MATES;
	case MountModelKind::FiberCollimator: return COLLIMATOR_MATES;
	case MountModelKind::PowerMeterHead:
	case MountModelKind::Camera:
	case MountModelKind::BeamProfiler:
	case MountModelKind::BeamDump: return DETECTOR_MATES;
	case MountModelKind::AlignmentCard: return CARD_MATES;
	case MountModelKind::Count: break;
	}
	return {};
}

std::span<const MountModelInfo> mountModels() { return MODELS; }

const MountModelInfo* findMountModel(std::string_view id)
{
	for (const MountModelInfo& info : MODELS) {
		if (id == info.id) {
			return &info;
		}
	}
	return nullptr;
}

bool isPlaceholderKind(MountModelKind kind) { return kind >= MountModelKind::LaserHead; }

size_t MountModel::triangleCount() const
{
	size_t count = 0;
	for (const MountPart& part : parts) {
		for (const MountSurface& surface : part.surfaces) {
			count += surface.indices.size() / 3;
		}
	}
	return count;
}

const MountMate* MountModel::findMate(std::string_view name) const
{
	auto it = mates.find(std::string(name));
	return it == mates.end() ? nullptr : &it->second;
}

const MountDrive* MountModel::findDrive(std::string_view name) const
{
	for (const MountDrive& drive : drives) {
		if (drive.name == name) {
			return &drive;
		}
	}
	return nullptr;
}

double driveMotionValue(const MountDrive& drive, std::string_view motion, double value)
{
	auto it = drive.motionValues.find(std::string(motion));
	if (it == drive.motionValues.end() || drive.values.empty()) {
		return 0.0;
	}
	const std::vector<double>& x = drive.values;
	const std::vector<double>& y = it->second;
	if (value <= x.front()) {
		return y.front();
	}
	if (value >= x.back()) {
		return y.back();
	}
	const size_t i = size_t(std::upper_bound(x.begin(), x.end(), value) - x.begin());
	const double t = (value - x[i - 1]) / (x[i] - x[i - 1]);
	return y[i - 1] + (y[i] - y[i - 1]) * t;
}

glm::dmat4 mateTransform(const MountMate& mate)
{
	return glm::dmat4(glm::dvec4(mate.x, 0.0), glm::dvec4(mate.y, 0.0), glm::dvec4(mate.z, 0.0), glm::dvec4(mate.origin, 1.0));
}

std::optional<MountModel> readMountModel(const std::filesystem::path& directory, const MountModelInfo& info, std::string& error)
{
	MountModel model;
	model.info = &info;
	const std::filesystem::path glb = directory / (std::string(info.fileStem) + ".glb");
	const std::filesystem::path mates = directory / (std::string(info.fileStem) + ".mates.json");
	if (!std::filesystem::exists(glb) || !std::filesystem::exists(mates)) {
		error = fmt::format("{}: no {} in {}", info.id, glb.filename().string(), directory.string());
		return std::nullopt;
	}
	if (!readMeshes(glb, model, error) || !readMates(mates, model, error)) {
		return std::nullopt;
	}
	for (const char* name : requiredMates(info.kind)) {
		if (!model.findMate(name)) {
			error = fmt::format("{}: {} has no mate {}", info.id, mates.filename().string(), name);
			return std::nullopt;
		}
	}
	computeBounds(model);
	return model;
}

MountModel buildProceduralModel(const MountModelInfo& info)
{
	MountModel model;
	model.info = &info;
	model.procedural = true;
	const glm::vec3 s = info.nominalSize;
	const glm::dvec3 up(0, 1, 0), forward(0, 0, 1), down(0, -1, 0);
	MeshBuilder body;
	body.surface.finish = MountFinish::Placeholder;

	switch (info.kind) {
	case MountModelKind::LaserHead:
		// a housing behind its output aperture, which is the origin; the post under its middle
		body.box({ -s.x * 0.5f, -s.y * 0.5f, -s.z }, { s.x * 0.5f, s.y * 0.5f, 0.f });
		model.mates["output"] = mateAt(glm::dvec3(0.0), forward, up, "body");
		model.mates["post_tap"] = mateAt(glm::dvec3(0.0, -s.y * 0.5, -s.z * 0.5), down, forward, "body");
		break;
	case MountModelKind::DiodeModule:
	case MountModelKind::FiberCollimator:
		body.cylinder({ 0.f, 0.f, -s.z }, { 0.f, 0.f, 1.f }, s.x * 0.5f, s.z);
		model.mates["output"] = mateAt(glm::dvec3(0.0), forward, up, "body");
		if (info.kind == MountModelKind::DiodeModule) {
			model.mates["post_tap"] = mateAt(glm::dvec3(0.0, -s.y * 0.5, -s.z * 0.5), down, forward, "body");
		} else {
			// it rides in a KM100 like an optic: its back on the mount's optic seat
			model.mates["back_center"] = mateAt(glm::dvec3(0.0, 0.0, -s.z), forward, up, "body");
		}
		break;
	case MountModelKind::PowerMeterHead: {
		// a sensor disk facing +z on a stem down to the post
		const float radius = s.x * 0.5f;
		body.cylinder({ 0.f, 0.f, -s.z }, { 0.f, 0.f, 1.f }, radius, s.z);
		const float stem = s.y - s.x;
		body.cylinder({ 0.f, -radius - stem, -s.z * 0.5f }, { 0.f, 1.f, 0.f }, 0.00635f, stem);
		model.mates["input"] = mateAt(glm::dvec3(0.0), forward, up, "body");
		model.mates["post_tap"] = mateAt(glm::dvec3(0.0, -radius - stem, -s.z * 0.5), down, forward, "body");
		break;
	}
	case MountModelKind::Camera: {
		// a body behind a C-mount flange; the origin is the sensor plane, 17.526 mm behind the flange
		constexpr float FLANGE_FOCAL = 0.017526f;
		constexpr float MOUNT_LENGTH = 0.006f;
		const float front = FLANGE_FOCAL;
		body.box({ -s.x * 0.5f, -s.y * 0.5f, front - MOUNT_LENGTH - s.z }, { s.x * 0.5f, s.y * 0.5f, front - MOUNT_LENGTH });
		body.cylinder({ 0.f, 0.f, front - MOUNT_LENGTH }, { 0.f, 0.f, 1.f }, 0.0127f, MOUNT_LENGTH);
		model.mates["input"] = mateAt(glm::dvec3(0.0), forward, up, "body");
		model.mates["post_tap"] = mateAt(glm::dvec3(0.0, -s.y * 0.5, front - MOUNT_LENGTH - s.z * 0.5), down, forward, "body");
		break;
	}
	case MountModelKind::BeamProfiler:
		body.box({ -s.x * 0.5f, -s.y * 0.5f, -s.z }, { s.x * 0.5f, s.y * 0.5f, 0.f });
		model.mates["input"] = mateAt(glm::dvec3(0.0), forward, up, "body");
		model.mates["post_tap"] = mateAt(glm::dvec3(0.0, -s.y * 0.5, -s.z * 0.5), down, forward, "body");
		break;
	case MountModelKind::AlignmentCard:
		// a card facing +z, its face at the origin, held from its bottom edge
		body.box({ -s.x * 0.5f, -s.y * 0.5f, -s.z }, { s.x * 0.5f, s.y * 0.5f, 0.f });
		body.surface.finish = MountFinish::Engraving;
		model.mates["face_center"] = mateAt(glm::dvec3(0.0), forward, up, "body");
		model.mates["post_tap"] = mateAt(glm::dvec3(0.0, -s.y * 0.5, -s.z * 0.5), down, forward, "body");
		break;
	case MountModelKind::BeamDump:
		body.cylinder({ 0.f, 0.f, -s.z }, { 0.f, 0.f, 1.f }, s.x * 0.5f, s.z);
		model.mates["input"] = mateAt(glm::dvec3(0.0), forward, up, "body");
		model.mates["post_tap"] = mateAt(glm::dvec3(0.0, -s.y * 0.5, -s.z * 0.5), down, forward, "body");
		break;
	default: {
		// a converted model without its files: a box of its nominal size, the mount's front face at the
		// origin (a holder's bottom), with the kind's mates
		const bool holder = info.kind == MountModelKind::PostHolder || info.kind == MountModelKind::ClampingFork;
		const glm::vec3 lo = holder ? glm::vec3(-s.x * 0.5f, 0.f, -s.z * 0.5f) : glm::vec3(-s.x * 0.5f, -s.y * 0.5f, -s.z);
		const glm::vec3 hi = holder ? glm::vec3(s.x * 0.5f, s.y, s.z * 0.5f) : glm::vec3(s.x * 0.5f, s.y * 0.5f, 0.f);
		body.box(lo, hi);
		boxMates(info, model, lo, hi);
		break;
	}
	}

	MountPart part;
	part.name = "body";
	part.surfaces.push_back(std::move(body.surface));
	model.parts.push_back(std::move(part));
	computeBounds(model);
	return model;
}

MountModel loadMountModel(const std::filesystem::path& directory, const MountModelInfo& info)
{
	std::string error;
	std::optional<MountModel> model = readMountModel(directory, info, error);
	if (model) {
		return std::move(*model);
	}
	MountModel box = buildProceduralModel(info);
	// a placeholder kind without its file is expected; a converted model without its file is not
	if (!isPlaceholderKind(info.kind) || std::filesystem::exists(directory / (std::string(info.fileStem) + ".glb"))) {
		box.warning = error;
	}
	return box;
}

std::vector<MountModel> loadMountModels(const std::filesystem::path& directory)
{
	std::vector<MountModel> models;
	for (const MountModelInfo& info : MODELS) {
		models.push_back(loadMountModel(directory, info));
	}
	return models;
}
