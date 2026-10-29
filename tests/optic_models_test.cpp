// CPU verification of the optomechanical model library (src/optic_models.h): the converter's output
// in assets/optical_models/meshes/ (tools/optical_models/convert.py) and the procedural placeholders.
// No Vulkan, no GPU.
//
// For every entry of mountModels():
//  - a converted model reads from its .glb and .mates.json, and carries every mate its kind requires;
//    a placeholder kind builds procedurally with its mates
//  - every mate is a right-handed orthonormal frame, and lies within the model's bounds grown by a
//    centimeter (a mate far outside its model means a frame or a reference went wrong)
//  - every surface's indices are in range and its normals unit length
//  - a converted model's triangle count is within the budget its mates file records
//  - every motion names a part the model has and a mate it carries, and a driven motion a drive that sets it
//  - a drive's values ascend, its rest lies among them, and each motion it sets has a value for each and stays
//    within the motion's range
//  - a textured surface has a uv in [0, 1] for every vertex, and the model a decoded texture for them to read
// And for every converted kind, the fallback box a missing file gets carries the same mates, so a scene
// still assembles. The whole library loads in under a second.
//
// Run from the repository root:  ./bin/optic_models_test [--models assets/optical_models/meshes]

#include <optic_models.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <fmt/format.h>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
	if (!ok) {
		g_failures++;
		fmt::println("    FAIL: {}", what);
	}
}

void checkFrame(const MountModel& model, const std::string& name, const MountMate& mate)
{
	const double tolerance = 1e-6;
	const std::string label = fmt::format("{} {}", model.info->id, name);
	check(std::abs(glm::length(mate.x) - 1.0) < tolerance && std::abs(glm::length(mate.y) - 1.0) < tolerance
			&& std::abs(glm::length(mate.z) - 1.0) < tolerance,
		label + ": axes are unit length");
	check(std::abs(glm::dot(mate.x, mate.y)) < tolerance && std::abs(glm::dot(mate.y, mate.z)) < tolerance
			&& std::abs(glm::dot(mate.z, mate.x)) < tolerance,
		label + ": axes are orthogonal");
	check(glm::length(glm::cross(mate.x, mate.y) - mate.z) < tolerance, label + ": axes are right-handed");
	const glm::dvec3 lo = glm::dvec3(model.boundsMin) - 0.01, hi = glm::dvec3(model.boundsMax) + 0.01;
	check(glm::all(glm::greaterThanEqual(mate.origin, lo)) && glm::all(glm::lessThanEqual(mate.origin, hi)),
		fmt::format("{}: origin ({:.4f}, {:.4f}, {:.4f}) lies within a centimeter of the model", label, mate.origin.x,
			mate.origin.y, mate.origin.z));
}

void checkModel(const MountModel& model)
{
	const MountModelInfo& info = *model.info;
	for (const char* name : requiredMates(info.kind)) {
		const MountMate* mate = model.findMate(name);
		check(mate != nullptr, fmt::format("{} carries mate {}", info.id, name));
	}
	for (const auto& [name, mate] : model.mates) {
		checkFrame(model, name, mate);
		bool known = false;
		for (const MountPart& part : model.parts) {
			known |= part.name == mate.part;
		}
		check(known, fmt::format("{} {}: moves with part \"{}\", which the model has", info.id, name, mate.part));
	}
	for (const MountMotion& motion : model.motions) {
		bool known = false;
		for (const MountPart& part : model.parts) {
			known |= part.name == motion.part;
		}
		check(known, fmt::format("{}: motion of part \"{}\", which the model has", info.id, motion.part));
		check(model.findMate(motion.mate) != nullptr, fmt::format("{}: motion about mate \"{}\", which the model carries", info.id, motion.mate));
		check(motion.rangeMin <= motion.rangeMax, fmt::format("{}: motion of {} has an ordered range", info.id, motion.part));
		if (!motion.drive.empty()) {
			const MountDrive* drive = model.findDrive(motion.drive);
			check(drive != nullptr && drive->motionValues.count(motion.part) == 1,
				fmt::format("{}: drive \"{}\" sets the motion of {}", info.id, motion.drive, motion.part));
		}
	}
	for (const MountDrive& drive : model.drives) {
		check(drive.values.size() >= 2 && std::is_sorted(drive.values.begin(), drive.values.end()),
			fmt::format("{}: drive {}'s values ascend", info.id, drive.name));
		check(!drive.values.empty() && drive.rest >= drive.values.front() && drive.rest <= drive.values.back(),
			fmt::format("{}: drive {}'s rest lies within its values", info.id, drive.name));
		for (const auto& [part, values] : drive.motionValues) {
			const MountMotion* motion = nullptr;
			for (const MountMotion& m : model.motions) {
				motion = m.part == part ? &m : motion;
			}
			check(motion != nullptr && motion->drive == drive.name, fmt::format("{}: drive {} sets a motion of {}", info.id, drive.name, part));
			bool within = motion != nullptr;
			for (double v : values) {
				within &= motion != nullptr && v >= motion->rangeMin - 1e-9 && v <= motion->rangeMax + 1e-9;
			}
			check(within, fmt::format("{}: drive {} keeps {} within its range", info.id, drive.name, part));
		}
	}
	if (!model.decalPixels.empty()) {
		check(model.decalWidth > 0 && model.decalHeight > 0 && model.decalPixels.size() == (size_t)model.decalWidth * model.decalHeight * 4,
			fmt::format("{}: the decal texture decodes to {} x {} texels", info.id, model.decalWidth, model.decalHeight));
	}
	for (const MountPart& part : model.parts) {
		for (const MountSurface& surface : part.surfaces) {
			check(surface.positions.size() == surface.normals.size(), fmt::format("{} {}: a normal per vertex", info.id, part.name));
			check(surface.indices.size() % 3 == 0, fmt::format("{} {}: whole triangles", info.id, part.name));
			bool inRange = true, unit = true;
			for (uint32_t index : surface.indices) {
				inRange &= index < surface.positions.size();
			}
			for (const glm::vec3& n : surface.normals) {
				unit &= std::abs(glm::length(n) - 1.f) < 1e-3f;
			}
			check(inRange, fmt::format("{} {}: indices in range", info.id, part.name));
			check(unit, fmt::format("{} {}: unit normals", info.id, part.name));
			if (surface.textured) {
				bool inside = surface.uvs.size() == surface.positions.size() && !model.decalPixels.empty();
				for (const glm::vec2& uv : surface.uvs) {
					inside &= uv.x >= 0.f && uv.x <= 1.f && uv.y >= 0.f && uv.y <= 1.f;
				}
				check(inside, fmt::format("{} {}: a uv in the texture for every vertex", info.id, part.name));
			}
		}
	}
	if (!model.procedural) {
		check(model.triangleBudget > 0 && model.triangleCount() <= model.triangleBudget,
			fmt::format("{}: {} triangles within the budget of {}", info.id, model.triangleCount(), model.triangleBudget));
	}
}

}

int main(int argc, char** argv)
{
	std::filesystem::path directory = "assets/optical_models/meshes";
	for (int i = 1; i + 1 < argc; i++) {
		if (std::strcmp(argv[i], "--models") == 0) {
			directory = argv[++i];
		}
	}

	const auto start = std::chrono::steady_clock::now();
	const std::vector<MountModel> models = loadMountModels(directory);
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

	fmt::println("{:<18} {:<18} {:>9} {:>7} {:>6}  {}", "model", "kind", "triangles", "budget", "parts", "source");
	size_t triangles = 0;
	for (const MountModel& model : models) {
		const MountModelInfo& info = *model.info;
		fmt::println("{:<18} {:<18} {:>9} {:>7} {:>6}  {}", info.id, mountModelKindName(info.kind), model.triangleCount(),
			model.triangleBudget, model.parts.size(), model.procedural ? (isPlaceholderKind(info.kind) ? "placeholder" : "FALLBACK BOX") : "converted");
		triangles += model.triangleCount();
		check(model.warning.empty(), fmt::format("{} loads: {}", info.id, model.warning));
		check(!model.procedural || isPlaceholderKind(info.kind), fmt::format("{} is converted, not a fallback", info.id));
		checkModel(model);
	}

	// the fallback a converted model gets when its files are missing must still assemble
	for (const MountModelInfo& info : mountModels()) {
		if (!isPlaceholderKind(info.kind)) {
			const MountModel box = buildProceduralModel(info);
			for (const char* name : requiredMates(info.kind)) {
				check(box.findMate(name) != nullptr, fmt::format("{}'s fallback box carries mate {}", info.id, name));
			}
		}
	}

	fmt::println("{} models, {} triangles, loaded in {:.3f} s", models.size(), triangles, seconds);
	check(seconds < 1.0, fmt::format("the library loads in under a second ({:.3f} s)", seconds));
	if (g_failures) {
		fmt::println("{} failures", g_failures);
		return 1;
	}
	fmt::println("all checks passed");
	return 0;
}
