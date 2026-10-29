#pragma once

// The optomechanical models: the Thorlabs mounts, holders, irises and mirror blanks that
// tools/optical_models/convert.py made from their STEP files, and procedural placeholders for the
// lasers and detectors that have no file yet (docs/plans/completed/physoptics-optomechanical-models.md).
//
// Plain C++ with no Vulkan, so bin/optic_models_test can check every model without a GPU. The engine's
// MountLibrary (mount_library.h) uploads what this loads.
//
// Every model is in its canonical frame, in meters: +y up, +z along the optical axis toward the incoming
// light, the origin at the model's primary mate (the optic seat of a mount, the base of a holder, the output
// of a laser). A mate is a named frame on the model where it meets another: physoptics-bench-and-mounts.md
// stacks fork, holder, post, mount and optic by them.
//
// Lettering, scales, springs and threads are not geometry: the converter bakes them into one texture per model
// (MountModel::decalPixels), which the surfaces marked `textured` carry with their uvs.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

// The surface finishes a model's parts carry, which the converter maps from the STEP file's colors.
enum class MountFinish : uint8_t {
	AnodizedBlack,
	Stainless,
	Brass,
	Glass,
	// the white of engraved lettering and graduations
	Engraving,
	// a placeholder's body, a mid gray so that it reads as a stand-in
	Placeholder,
	Count
};

inline constexpr size_t MOUNT_FINISH_COUNT = (size_t)MountFinish::Count;

struct MountFinishInfo {
	// the glTF material name the converter writes
	const char* name;
	glm::vec4 baseColor;
	float metallic;
	float roughness;
};

const MountFinishInfo& mountFinishInfo(MountFinish finish);
std::optional<MountFinish> mountFinishByName(std::string_view name);

// A named frame on a model, in its canonical frame: an origin and three orthonormal axes. Its z is the
// direction of the mating (out of a seat, along a screw), and `part` the part it moves with. `params` holds
// the numbers the converter's manifest gave it, in SI units: an adjuster's thread pitch, a bore's diameter,
// a bore's depth
struct MountMate {
	glm::dvec3 origin { 0.0 };
	glm::dvec3 x { 1.0, 0.0, 0.0 };
	glm::dvec3 y { 0.0, 1.0, 0.0 };
	glm::dvec3 z { 0.0, 0.0, 1.0 };
	std::string part;
	std::map<std::string, double> params;
};

// How a part moves: a kinematic plate tilts about a mate's x and y, an adjuster screws along a mate's z, a
// rotor or lever turns about it, a jaw or a retaining ring slides along it. `range` is in meters or radians.
// A motion with a `drive` does not move alone: the drive's value sets it (an iris's lever and every blade
// follow its aperture)
enum class MountMotionKind : uint8_t {
	Tilt,
	Screw,
	Rotate,
	Slide
};

struct MountMotion {
	std::string part;
	MountMotionKind kind { MountMotionKind::Slide };
	std::string mate;
	double rangeMin { 0.0 };
	double rangeMax { 0.0 };
	// the MountDrive that sets it, empty for a motion of its own
	std::string drive;
};

// One value that moves several parts together: an iris's aperture, which turns its lever and every blade.
// `values` ascend; each motion's entry holds its value (radians or meters) at each of them, linear between
struct MountDrive {
	std::string name;
	std::vector<double> values;
	// the drive's value as the model is modeled, every part at rest
	double rest { 0.0 };
	std::map<std::string, std::vector<double>> motionValues;
};

// a driven motion's value at a drive value, clamped to the drive's range; 0 for a motion the drive does not set
double driveMotionValue(const MountDrive& drive, std::string_view motion, double value);

// One finish of one part: an indexed triangle list with per-vertex normals. A textured surface takes its color
// from the model's decal texture through `uvs` (v down the image, as glTF's) instead of from its finish, whose
// metal and roughness it keeps
struct MountSurface {
	MountFinish finish { MountFinish::AnodizedBlack };
	bool textured { false };
	std::vector<glm::vec3> positions;
	std::vector<glm::vec3> normals;
	std::vector<glm::vec2> uvs;
	std::vector<uint32_t> indices;
};

// One part of a model: what moves as one (a kinematic mount's base, its plate, each adjuster)
struct MountPart {
	std::string name;
	std::vector<MountSurface> surfaces;
};

// What a model is for, which decides the mates it must carry
enum class MountModelKind : uint8_t {
	KinematicMount,
	FixedMount,
	RotationMount,
	PrismMount,
	PlateHolder,
	Iris,
	PostHolder,
	ClampingFork,
	MirrorBlank,
	// the placeholders
	LaserHead,
	DiodeModule,
	FiberCollimator,
	PowerMeterHead,
	Camera,
	BeamProfiler,
	AlignmentCard,
	BeamDump,
	Count
};

const char* mountModelKindName(MountModelKind kind);

// the mates every model of a kind carries (the plan's table). Each name is one MountModel::mates key
std::span<const char* const> requiredMates(MountModelKind kind);

// One entry of the model library. `fileStem` names assets/optical_models/meshes/<stem>.glb and its
// <stem>.mates.json. A converted model whose files are missing falls back to a box of `nominalSize`; a
// placeholder kind (laser, detectors, accessories) is built procedurally until its file exists, and
// `suggestedPart` is the Thorlabs part whose STEP file would replace it
struct MountModelInfo {
	const char* id;
	const char* fileStem;
	MountModelKind kind;
	// meters, along x, y and z of the canonical frame
	glm::vec3 nominalSize;
	const char* suggestedPart;
};

// Every model, each named once. physoptics-bench-and-mounts.md maps optic kinds and sizes to these ids
std::span<const MountModelInfo> mountModels();
const MountModelInfo* findMountModel(std::string_view id);

bool isPlaceholderKind(MountModelKind kind);

struct MountModel {
	const MountModelInfo* info { nullptr };
	std::vector<MountPart> parts;
	std::map<std::string, MountMate> mates;
	std::vector<MountMotion> motions;
	std::vector<MountDrive> drives;
	// the decal texture the .glb embeds, decoded: rgba8 rows, sRGB, row 0 at the top. Empty for a model
	// without decals
	std::vector<uint8_t> decalPixels;
	uint32_t decalWidth { 0 };
	uint32_t decalHeight { 0 };
	glm::vec3 boundsMin { 0.f };
	glm::vec3 boundsMax { 0.f };
	// the converter's budget for it (tools/optical_models/manifest.json); 0 for a procedural model
	uint32_t triangleBudget { 0 };
	// built procedurally: a placeholder kind, or a converted model whose file could not be read
	bool procedural { false };
	// why a converted model fell back to its box; empty otherwise
	std::string warning;

	size_t triangleCount() const;
	const MountMate* findMate(std::string_view name) const;
	const MountDrive* findDrive(std::string_view name) const;
};

// Reads <directory>/<stem>.glb and <stem>.mates.json. On failure returns nullopt and says why in `error`
std::optional<MountModel> readMountModel(const std::filesystem::path& directory, const MountModelInfo& info, std::string& error);

// The procedural model of an entry: a placeholder kind's shape with its mates, or for a converted kind a box
// of the nominal size with the kind's required mates at plausible places
MountModel buildProceduralModel(const MountModelInfo& info);

// One entry of mountModels(): read from `directory`, or procedural, with the warning saying why when a converted
// model's files are missing or unreadable. Never fails
MountModel loadMountModel(const std::filesystem::path& directory, const MountModelInfo& info);
// every entry, as loadMountModel() gives each
std::vector<MountModel> loadMountModels(const std::filesystem::path& directory);

// a mate's frame as a matrix: model space from the mate's local space (columns x, y, z, origin)
glm::dmat4 mateTransform(const MountMate& mate);
