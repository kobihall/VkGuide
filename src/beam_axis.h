#pragma once

// BEAM AXES (docs/plans/physoptics-beam-axis.md), part of rt_optics: the order a beam is meant to visit the optics in,
// as a scene object you author. An axis is a source and a tree of visits. The source is what the beam starts from: a
// laser, or a camera whose chief ray is traced outward from its sensor, or nothing yet, which lets you write the order
// down before attaching one. Each node visits one optic and hangs off one named port of its parent's optic (or off
// the source, for the first visit), so an element that splits the beam has a child per port and everything else is a
// chain. A node is a visit, not an optic: a double pass through a lens visits it twice.
//
// How a render traces along an axis (physoptics-guided-tracing.md) is a render setting, not the axis's: the renderer
// guides by one axis at a time (RaytraceRenderer::guideAxisId(), OpticsSettings::traceMode).
//
// The axis places nothing. You place the optics; the axis records the intent, the beam solver traces the real beam
// beside it (physoptics-gaussian-beam-solver.md) and gives each node a BeamNodeStatus, and the difference between
// the two is what the Scene panel and the Viewport show.
//
// Optics, cameras and the axes share the editor's id space. Every id here is the editor's and is never saved: the
// scene file names an optic by its index in "optics", a camera by its index in "cameras", and a port by its name.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <optic_catalog.h>

//---------------------------------------------------------------- ports

// a port of an optic kind: its index into opticTraits(kind).ports. The scene file saves the port's name
using OpticPortId = uint8_t;
inline constexpr OpticPortId NO_PORT = 0xff;

// the port's name, "" for one the kind does not have
std::string_view opticPortName(OpticKind kind, OpticPortId port);
std::optional<OpticPortId> findOpticPort(OpticKind kind, std::string_view name);
uint32_t opticPortCount(OpticKind kind);

//---------------------------------------------------------------- the authored graph

// what an axis starts from
enum class BeamSourceKind : uint8_t {
	// nothing attached yet: the nodes record an order and every one reads Unreached
	None,
	// a laser optic (OpticKind::Laser), by its id; the beam leaves by its Output port
	Laser,
	// a scene camera, by its id: its chief ray leaves the sensor's center along the view axis, outward through the
	// elements, as a realistic camera traces
	Camera
	// later: an assembly's input reference (a ray and a beam, or a field of view and an aperture)
};

struct BeamSource {
	BeamSourceKind kind { BeamSourceKind::None };
	// the optic's id for a laser, the camera's for a camera, 0 for none
	uint64_t id { 0 };
	bool operator==(const BeamSource&) const = default;
};

// the parent of the first visit: the source itself
inline constexpr uint32_t BEAM_SOURCE_NODE = 0xffffffffu;
// the parent of a dangling node: one a Remove cut loose, which keeps its subtree, reads Unreached, and waits for you to
// attach it somewhere (moveBeamNode()) or delete it
inline constexpr uint32_t BEAM_DANGLING_NODE = 0xfffffffeu;

struct BeamAxisNode {
	// unique within its axis and stable across edits; a load numbers the nodes from 0 in the file's order
	uint32_t id { 0 };
	// the SceneOptic this node visits
	uint64_t optic { 0 };
	// the node whose port leads here, BEAM_SOURCE_NODE for the first visit, BEAM_DANGLING_NODE for a dangling one
	uint32_t parent { BEAM_SOURCE_NODE };
	// which of the parent's optic's ports; NO_PORT under the source, which has one output, and for a dangling node
	OpticPortId port { NO_PORT };
	// set when an edit changed the beam that reaches this node (a Remove moved it up): the panel asks you to check it.
	// Saved, cleared when you confirm or edit the node
	bool review { false };
	bool operator==(const BeamAxisNode&) const = default;
};

inline constexpr float DEFAULT_GHOST_THRESHOLD = 1e-4f;

struct BeamAxis {
	// shares the editor's id space with the optics; never saved
	uint64_t id { 0 };
	std::string name;
	BeamSource source;
	// in the order they were added; the tree is the parent links. At most one node hangs off each port of each node,
	// and at most one off the source. Dangling nodes are roots of their own, any number of them
	std::vector<BeamAxisNode> nodes;
	// the beam solver's power floor for a branch, relative to the source's power
	float ghostThreshold { DEFAULT_GHOST_THRESHOLD };
	bool operator==(const BeamAxis&) const = default;
};

BeamAxis makeBeamAxis(std::string name);

//---------------------------------------------------------------- reading the tree

const BeamAxisNode* findBeamNode(const BeamAxis& axis, uint32_t node);
BeamAxisNode* findBeamNode(BeamAxis& axis, uint32_t node);
// the node hanging off `port` of `parent` (BEAM_SOURCE_NODE and NO_PORT for the first visit), or null
const BeamAxisNode* beamNodeAt(const BeamAxis& axis, uint32_t parent, OpticPortId port);
// a node's children, ordered by port
std::vector<uint32_t> beamNodeChildren(const BeamAxis& axis, uint32_t node);
// the nodes in depth-first order from the source, children by port: the order the panel lists and the solver walks.
// Dangling subtrees are not in it
std::vector<uint32_t> beamAxisOrder(const BeamAxis& axis);
// the dangling nodes' subtrees, each depth-first, in the order the nodes were cut loose
std::vector<uint32_t> beamDanglingOrder(const BeamAxis& axis);
// whether `node` is `ancestor` or lies below it
bool beamNodeBelow(const BeamAxis& axis, uint32_t node, uint32_t ancestor);
// whether `node` or an ancestor of it dangles
bool beamNodeDangling(const BeamAxis& axis, uint32_t node);

//---------------------------------------------------------------- editing the tree

// Each returns the new node's id, or nothing when the place is taken or does not exist. The editor checks that the
// optic and port exist; these keep the tree a tree.

// a visit of `optic` off `port` of `parent`; nothing when that port already has a child
std::optional<uint32_t> appendBeamNode(BeamAxis& axis, uint32_t parent, OpticPortId port, uint64_t optic);
// a visit of `optic` in front of `node`: the new node takes `node`'s place under its parent, and `node` hangs off the
// new node's `throughPort`
std::optional<uint32_t> insertBeamNodeBefore(BeamAxis& axis, uint32_t node, uint64_t optic, OpticPortId throughPort);
// a visit of `optic` off `port` of `node`; whatever hung off that port now hangs off the new node's `throughPort`
std::optional<uint32_t> insertBeamNodeAfter(BeamAxis& axis, uint32_t node, OpticPortId port, uint64_t optic, OpticPortId throughPort);
// removes a visit. The child on its lowest port moves up to the place it had and is marked for review; the others,
// which that place cannot also hold, dangle with their subtrees and are marked too. A dangling node's children
// dangle in its place
void removeBeamNode(BeamAxis& axis, uint32_t node);
// removes a visit and everything below it
void removeBeamSubtree(BeamAxis& axis, uint32_t node);
// removes every visit of an optic, as removeBeamNode does each
void removeBeamOptic(BeamAxis& axis, uint64_t optic);
// numbers the nodes by their places in `nodes`, 0 up, their parents with them: what the scene file stores
void renumberBeamNodes(BeamAxis& axis);
// moves a node and its subtree off `port` of `parent` (BEAM_SOURCE_NODE, or BEAM_DANGLING_NODE to cut it loose).
// False when that port already has a child, or `parent` lies below `node`
bool moveBeamNode(BeamAxis& axis, uint32_t node, uint32_t parent, OpticPortId port);

//---------------------------------------------------------------- the traced beam against the graph

// what the solver found for a node (the plan's status table). Never saved
enum class BeamNodeStatus : uint8_t {
	// the parent's port beam reaches this optic first, inside its clear aperture
	Hit,
	// it reaches it, but loses a fraction of its power on the aperture or on something on the way
	Clipped,
	// it passes the optic
	Missed,
	// it hits another object first
	Blocked,
	// the parent's port carries less than the axis's ghost threshold
	BelowThreshold,
	// an ancestor was missed or blocked, the axis has no source, or the node dangles
	Unreached
};

const char* beamNodeStatusLabel(BeamNodeStatus status);

struct BeamNodeReport {
	uint32_t node { 0 };
	BeamNodeStatus status { BeamNodeStatus::Unreached };
	// the power reaching the node, watts, and the fraction of the parent's port power lost before it (Clipped)
	double power { 0.0 };
	double lostFraction { 0.0 };
	// what took the power or blocked the beam: an optic's or a blocker's name and part
	std::string blocker;
	// Missed: the distance from the aperture's edge (m) and the world direction from the beam toward the aperture's
	// center, perpendicular to the beam
	double missDistance { 0.0 };
	glm::dvec3 steer { 0.0 };
};
