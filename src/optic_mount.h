#pragma once

// How an optical element stands on the bench (docs/plans/completed/physoptics-bench-and-mounts.md): the kind of mount it sits in,
// where the post under that mount stands, how the clamping fork holds the post holder, and the mount's fine adjustment.
// Plain data, part of SceneOptic and saved with it. optic_bench.h composes it, through the mount models' mates, into
// the element's transform; nothing else derives a position from it.
//
// On the table an optic has the degrees of freedom the stack gives it: the post slides in x and z, turns about its own
// axis (yaw) and rides up and down in its holder (height). Tip, tilt and roll come only from the mount's adjusters or
// rotor. The pose is held at the optic's origin (opticOrigin(), optic_bench.h), the point its optics are defined by: a
// mirror's front face, the center of anything light goes through. The post stands wherever that puts it, and moves when
// the mount or the optic changes, so the optical surface stays where it was placed. Free placement, and an optic with no mount, have all six: a position and an orientation, which a file stores as
// they are, so a save and a reopen give the same bits.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// the kinds of mount an optic may sit in; OpticTraits::mounts lists a kind's, the first its default. Saved by key
// (mountKindKey()): append only
enum class MountKind : uint8_t {
	// KM05T/M, KM100 or KM200 by the optic's diameter
	Kinematic,
	// LMR05/M, LMR1/M or LMR2/M
	Fixed,
	// FP01
	PlateHolder,
	// RSP05/M, RSP1/M or RSP2/M
	Rotation,
	// KM200PM/M, every size
	PrismMount,
	// the iris's own ID-series housing
	IrisAssembly,
	// the optic alone, placed freely: no mount, post, holder or fork
	None
};
inline constexpr size_t MOUNT_KIND_COUNT = 7;

const char* mountKindKey(MountKind kind);
const char* mountKindLabel(MountKind kind);
std::optional<MountKind> findMountKind(std::string_view key);

// The fine adjustment a mount kind has. Only the fields of the optic's mount kind are read: a kinematic or prism mount's
// two adjusters, a rotation mount's rotor; a fixed mount, a plate holder and an iris have none
struct FineAdjust {
	// revolutions of each adjuster from where the model has it; positive advances the screw along its tip's z
	double adjusterX { 0.0 };
	double adjusterY { 0.0 };
	// radians about the rotor's axis from where the model has it (the scale's zero is not identified)
	double rotor { 0.0 };

	bool operator==(const FineAdjust&) const = default;
};

struct MountPose {
	MountKind kind { MountKind::None };
	// the optic's origin on the table, x and z, with the adjusters at zero
	glm::dvec2 origin { 0.0 };
	// about a vertical axis through the origin: 0 points the mount's front (+z, toward the incoming light) along world +z
	double yaw { 0.0 };
	// the origin above the table's top face, on the optical axis; the bench's beam height when the optic is made
	double height { 0.1 };
	// the fork's slot direction about the post (0 along world +z, as yaw), and the bolt's distance from the post's axis
	double forkAngle { 0.0 };
	double forkReach { 0.05 };
	FineAdjust fine;
	// six degrees of freedom, with no post, holder or fork under the mount
	bool freePlacement { false };
	// where the element's body frame is while it is free or unmounted
	glm::dvec3 freePosition { 0.0, 0.1, 0.0 };
	glm::dquat freeOrientation { 1.0, 0.0, 0.0, 0.0 };

	// placed by its free position and orientation rather than by the stack
	bool isFree() const { return freePlacement || kind == MountKind::None; }
	// the body frame -> world they give
	glm::dmat4 freeTransform() const
	{
		glm::dmat4 m = glm::mat4_cast(freeOrientation);
		m[3] = glm::dvec4(freePosition, 1.0);
		return m;
	}
	// from a rigid transform: its translation and its rotation's unit quaternion
	void setFreeTransform(const glm::dmat4& m)
	{
		freePosition = glm::dvec3(m[3]);
		freeOrientation = glm::normalize(glm::quat_cast(glm::dmat3(m)));
	}
	bool operator==(const MountPose&) const = default;
};
