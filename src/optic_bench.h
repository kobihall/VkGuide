#pragma once

// THE BENCH AND THE MOUNTS (docs/plans/completed/physoptics-bench-and-mounts.md), part of rt_optics: the breadboard, the stack
// every mounted optic stands on, and the rules that move it.
//
// From the table up, a stack is a CF125C/M clamping fork bolted into a hole, a PH50E/M post holder in the fork, a
// Ø12.7 mm post in the holder, the optic's mount on the post, and the optic in the mount through the mount's fine
// adjustment, each placed by the mates of the models in src/optic_models.h. opticWorldTransform() is the one function
// that composes it into the optic's body frame in world space, in double; the preview, the tracer and (later) the beam
// solver all read the transform it gives, so they cannot disagree about where an optic is.
//
// The table's top face is the plane y = 0, with a hole at the origin and the others on a 25 mm grid. Every length is in
// meters and every angle in radians.

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <optic_catalog.h>
#include <optic_models.h>

//---------------------------------------------------------------- the bench and its breadboard

// the scene's bench, saved with it
struct BenchSettings {
	// the breadboard: a scene object (the Optical bench capability lists it), absent until it is added
	bool table { false };
	// along x and z, in steps of the hole pitch
	glm::dvec2 size { 0.3, 0.3 };
	// the optical axis height every new mount is set to
	double beamHeight { 0.1 };
	// whether a drag puts a post on a hole center; holding Alt inverts it for one drag
	bool snap { true };

	bool operator==(const BenchSettings&) const = default;
};

// Thorlabs' metric breadboard as Breadboard-B3030A.step models it: M6 holes (the 5 mm tap drill, unthreaded) 5 mm deep
// through a 5 mm top skin, on a 25 mm pitch with a 12.5 mm border, a 5 mm bottom skin 60 mm down, corners rounded to
// the border's radius about the corner holes, and side walls between the skins, set back 3 mm, with corner bumpers set
// back 2 mm
inline constexpr double BREADBOARD_PITCH = 0.025;
inline constexpr double BREADBOARD_BORDER = 0.0125;
inline constexpr double BREADBOARD_THICKNESS = 0.060;
inline constexpr double BREADBOARD_SKIN = 0.005;
inline constexpr double BREADBOARD_HOLE_DIAMETER = 0.005;
inline constexpr double BREADBOARD_PANEL_INSET = 0.003;
inline constexpr double BREADBOARD_BUMPER_INSET = 0.002;
// each corner bumper's straight run along a side, from the corner hole's center
inline constexpr double BREADBOARD_BUMPER_RUN = 0.0395;
inline constexpr double BREADBOARD_MIN_SIZE = 2.0 * BREADBOARD_PITCH;
inline constexpr double BREADBOARD_MAX_SIZE = 3.6;
// the procedural post, a Thorlabs TR-series post's diameter
inline constexpr double POST_DIAMETER = 0.0127;

// a size rounded to whole steps of the pitch, within the limits above
glm::dvec2 snapBenchSize(glm::dvec2 size);

// The holes along x and z, and the table's extent. The origin is a hole: the one just below and to the left of the
// plate's center when a side has an even count, the center hole when it has an odd one
struct BenchGrid {
	glm::ivec2 count { 0 };
	// the first hole's center, (25 i, 25 j) mm with i, j <= 0
	glm::dvec2 first { 0.0 };
	// the plate's corners in x and z
	glm::dvec2 min { 0.0 };
	glm::dvec2 max { 0.0 };
};
BenchGrid benchGrid(const BenchSettings& bench);
// the hole center nearest a point, among the table's holes
glm::dvec2 nearestHole(const BenchSettings& bench, glm::dvec2 point);
// whether a point is a hole center of the table, within `tolerance`
bool isHole(const BenchSettings& bench, glm::dvec2 point, double tolerance = 1e-9);

// The plate as a triangle mesh in world space, its top face at y = 0: the top skin with its holes, the bottom skin and
// the side walls between them. The core inside is never seen and not built. Three surfaces: the two stainless skins,
// brushed along x, with the holes' walls; the black side walls between them; and the holes' floors, darker. About 60
// triangles a hole
struct BenchMesh {
	std::vector<glm::vec3> positions;
	std::vector<glm::vec3> normals;
	// x and z in centimeters: what sets a triangle's dp/du along x, the skins' brushing direction
	std::vector<glm::vec2> uvs;
	std::vector<uint32_t> indices;
	// the skins' triangles first, then the side walls', then the floors'
	uint32_t skinIndexCount { 0 };
	uint32_t wallIndexCount { 0 };
};
BenchMesh buildBreadboardMesh(const BenchSettings& bench);

//---------------------------------------------------------------- the stack

// what the bench needs from outside: the settings, the models' mates (MountLibrary::model() in the app,
// loadMountModel() in a test), and what an optic's body is built with
using MountModelLookup = std::function<const MountModel*(std::string_view id)>;
struct BenchContext {
	BenchSettings settings;
	MountModelLookup models;
	OpticBuildContext build;
};

inline constexpr const char* POST_HOLDER_MODEL = "PH50E/M";
inline constexpr const char* CLAMPING_FORK_MODEL = "CF125C/M";

// the model an optic's mount kind and size call for: by diameter for the round mounts (the smallest that holds it), by
// opening for an iris, KM200PM/M and FP01 for every size. Null for MountKind::None, or a round optic larger than every
// mount of its kind
const MountModelInfo* opticMountModel(const SceneOptic& optic);
// the mount kinds an optic may choose: its kind's, then None
std::vector<MountKind> allowedMounts(const SceneOptic& optic);

// what one part of a stack is
enum class StackRole : uint8_t {
	Mount,
	PostHolder,
	Fork
};

// one placed part of a model
struct StackPart {
	const MountModelInfo* model { nullptr };
	std::string part;
	StackRole role { StackRole::Mount };
	// the part's mesh (in its model's canonical frame) -> world, with the part's motion applied
	glm::dmat4 toWorld { 1.0 };
	// false for what the element itself stands for in the tracer: an iris's blades, whose sheet the optic is
	bool traced { true };
};

// what a kinematic or prism mount's plate does at the optic's adjuster turns
struct KinematicReadout {
	// the plate's rotations about the axis through the pivot and each other contact, which the adjuster turns
	// alone: X's turns the plate about the pivot and Y's contact (a horizontal steer as the mounts stand), and Y's about the
	// pivot and X's
	double angleX { 0.0 };
	double angleY { 0.0 };
	// the optic's datum's travel from where it is with the adjusters at zero, in world space: a mount walks the beam as
	// it steers it
	glm::dvec3 datumShift { 0.0 };
	// each adjuster's range of turns, where the plate reaches the mount's tilt range
	glm::dvec2 turnLimit { 0.0 };
	// effective thread pitch per adjuster: Thorlabs' angle per revolution over the modeled lever; and that angle
	glm::dvec2 pitch { 0.0 };
	glm::dvec2 anglePerRev { 0.0 };
	// the plate's tilt range either way
	double tiltRange { 0.0 };
	bool converged { true };
};

// Everything an optic's stack is, at its pose. Free placement keeps the mount around the optic, without post, holder
// or fork; MountKind::None has no parts at all
struct OpticStack {
	// = opticWorldTransform()
	glm::dmat4 opticToWorld { 1.0 };
	const MountModelInfo* mount { nullptr };
	std::vector<StackPart> parts;
	// the optic's origin in world space (the gizmo's point), with the adjusters at zero
	glm::dvec3 origin { 0.0 };
	// a post exists for a stack on the table: from the holder's mid-depth up to the mount, its axis at postAxis
	bool post { false };
	glm::dvec2 postAxis { 0.0 };
	glm::dvec3 postBottom { 0.0 };
	glm::dvec3 postTop { 0.0 };
	// the holder's top face, where the post enters it
	double holderTop { 0.0 };
	// the lowest optical axis the stack reaches: the post's top at the holder's top face
	double minimumHeight { 0.0 };
	// the fork's outline on the table and the holder's base, for the fork rule (chooseFork())
	std::array<glm::dvec2, 4> forkOutline {};
	// the bolt's axis on the table
	glm::dvec2 bolt { 0.0 };
	glm::dvec2 holderCenter { 0.0 };
	double holderRadius { 0.0 };
	// set by markForkClashes(): the fork overlaps another fork or holder, or its bolt is in no hole
	bool forkBlocked { false };
	std::optional<KinematicReadout> kinematic;
	// what the panel says about the stack: a prism overhanging its platform, an optic no mount of its kind holds
	std::vector<std::string> notes;
};

// the optic's body frame -> world: the stack's composition, or the free transform
glm::dmat4 opticWorldTransform(const SceneOptic& optic, const BenchContext& context);
// the whole stack: its parts in world space, the post, the outlines, the readouts
OpticStack assembleStack(const SceneOptic& optic, const BenchContext& context);
// every optic's stack, in order, with the forks that clash with another stack or have no hole marked
std::vector<OpticStack> assembleBench(std::span<const SceneOptic> optics, const BenchContext& context);
void markForkClashes(std::vector<OpticStack>& stacks, const BenchSettings& bench);

// the lowest height an optic's stack reaches; 0 for a free or unmounted one
double minimumHeight(const SceneOptic& optic, const BenchContext& context);

// The point an optic is placed by, in its body frame: the center of a mirror's front face (its datum), and the center of
// anything light goes through: a lens's, a filter's, a plate's or a wedge's middle on its axis, a cube's or a right-angle
// prism's box center, an equilateral prism's centroid, an iris's blades
glm::dvec3 opticOrigin(const SceneOptic& optic, const OpticPrescription& body);
// the same, from the optic's parameters
glm::dvec3 opticOrigin(const SceneOptic& optic, const BenchContext& context);

// The fork's hole for a post, among the holes its slot reaches. Axis-aligned directions first, then the nearest hole,
// then the smallest angle from an axis, then the direction nearest `previousAngle`. The first that clears every other
// stack's fork and holder wins; if none does, the first, `blocked`. `found` is false when no hole is in reach at all
struct ForkChoice {
	double angle { 0.0 };
	double reach { 0.0 };
	bool blocked { false };
	bool found { false };
};
ForkChoice chooseFork(glm::dvec2 post, double previousAngle, const BenchContext& context, std::span<const OpticStack> others);

// Moves an optic's origin to `origin` at `height` and turned by `yaw`, as a gizmo drag does: the origin snapped to the
// nearest hole when `snap` (the post then stands wherever that puts it), the height kept at or above the stack's lowest,
// and the fork rechosen against `others`
void placeOnBench(SceneOptic& optic, glm::dvec2 origin, double height, double yaw, bool snap, const BenchContext& context,
	std::span<const OpticStack> others);
// Rechooses the fork when its bolt is in no hole, as it is once the post has moved under a fixed origin (a thicker mirror,
// another mount), or when it overlaps another stack's fork or holder, as it does once another stack is put on it. Returns
// whether the pose changed
bool refitFork(SceneOptic& optic, const BenchContext& context, std::span<const OpticStack> others);
// Free placement off: the origin where it is, the yaw from the optic's axis's horizontal component, the adjusters at
// zero. Not snapped: the optic stays where it was as nearly as a stack allows
void dropOntoBench(SceneOptic& optic, const BenchContext& context, std::span<const OpticStack> others);

// A freely placed optic that snaps (OpticTraits::snapsFree, a laser) put on the bench: its origin's x and z on the nearest
// hole, its height kept, and its axis level and turned to the nearest of the grid's four directions
void snapFreeToBench(SceneOptic& optic, const BenchContext& context);

//---------------------------------------------------------------- the adjusters

// A kinematic mount's plate rests on the pivot ball and the two adjusters' tips. The plate's orientation is
// R = Rot(a_X, alpha) Rot(a_Y, beta) about the pivot, where a_X runs from the pivot to Y's contact and a_Y from the pivot
// to X's, with alpha and beta solved by Newton's method in double so that each contact sits on its tip, advanced along
// the screw by turns times the pitch. A single adjuster's turn is then exactly a rotation about the axis through the
// pivot and the other contact
struct KinematicGeometry {
	glm::dvec3 pivot { 0.0 };
	glm::dvec3 contactX { 0.0 };
	glm::dvec3 contactY { 0.0 };
	// each screw's axis, the way it advances
	glm::dvec3 screwX { 0.0, 0.0, 1.0 };
	glm::dvec3 screwY { 0.0, 0.0, 1.0 };
	// the contact's travel along its screw per radian of plate rotation about the axis through the pivot and the other
	double leverX { 0.0 };
	double leverY { 0.0 };
	// effective pitch: the mates' angle_per_rev times the lever, so one revolution is Thorlabs' angle
	double pitchX { 0.0 };
	double pitchY { 0.0 };
	// the plate's tilt range (the "plate" motion's range)
	double tiltRange { 0.0 };
};
std::optional<KinematicGeometry> kinematicGeometry(const MountModel& model);

struct KinematicPose {
	double alpha { 0.0 };
	double beta { 0.0 };
	// the plate's motion in the model frame: about the pivot
	glm::dmat4 plate { 1.0 };
	bool converged { true };
};
// from (alpha0, beta0), the previous pose's angles
KinematicPose solveKinematic(const KinematicGeometry& geometry, double turnsX, double turnsY, glm::dvec2 start = glm::dvec2(0.0));
// the turns at which the plate reaches its tilt range, per adjuster alone
glm::dvec2 adjusterTurnLimits(const KinematicGeometry& geometry);
