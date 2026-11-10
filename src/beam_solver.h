#pragma once

// THE GAUSSIAN BEAM SOLVER (docs/plans/completed/physoptics-gaussian-beam-solver.md), part of rt_optics: every laser's beam, and the
// chief ray of every camera a beam axis starts from, traced through the optics' real surfaces in double on the CPU, each
// segment carrying a general astigmatic Gaussian beam (src/beam_gaussian.h), its power, and what it ended on. From the
// traced segments it gives each beam axis's visits their statuses (BeamNodeReport, src/beam_axis.h).
//
// WHAT IT SEES. Only the optics' own bodies: their surfaces, apertures and materials. Mounts, posts, the breadboard, meshes
// and shapes are left to the path tracer (the user, 2026-11-07), so a beam that misses an optic, or meets a lens's rim from
// outside, carries on past it. The part of a beam outside a surface's clear aperture is counted as clipped and dropped.
//
// HOW A BEAM SPLITS. At a polished surface the coating's reflectance and transmittance at the beam's wavelength and angle
// (evaluateCoating(), the average of s and p: no polarization yet) divide the power between a reflected and a refracted
// branch; each is followed while it carries at least the ghost threshold times the source's power, up to
// BEAM_MAX_INTERACTIONS along one path and BEAM_MAX_SEGMENTS in one tree. A ground surface scatters the beam, a black or
// diffuse one absorbs it, a sensor detects it, a laser's aperture takes it back (a back reflection). An iris's opening and
// a probe plane are crossed, not hit: the segment ends there and one child carries on.
//
// PORTS. The events (surface, reflect or refract) a branch went through from entering an optic to leaving it are matched
// against the kind's ports (OPTIC_TRAITS, PortEvent), forward or reversed, so the double pass through a lens matches its
// Transmitted port both ways. A branch that matches none is a ghost.
//
// SELECTIVE UPDATING. The solver keeps its trees and, on each solve, re-traces only from the first segment of a branch
// that ends on or lies inside a changed optic, or whose envelope (three times the beam's largest radius around the chief
// ray) overlaps a changed optic's old or new bounds. A changed laser, library, surrounding medium or wavelength re-solves
// its tree. The result is the same, bit for bit, as a full solve.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <beam_axis.h>
#include <beam_gaussian.h>
#include <optic_catalog.h>
#include <rt_material.h>

class OpticalLibrary;

inline constexpr uint32_t NO_BEAM_SEGMENT = 0xffffffffu;
inline constexpr uint32_t BEAM_MAX_INTERACTIONS = 64;
inline constexpr uint32_t BEAM_MAX_SEGMENTS = 4096;
// how far a segment that leaves everything is drawn and its envelope reaches, m
inline constexpr double BEAM_ESCAPE_LENGTH = 2.0;
// the fraction of a beam an aperture must take before a visit reads Clipped
inline constexpr double BEAM_CLIP_REPORT = 1e-4;

// what a segment ends on
enum class BeamEnd : uint8_t {
	// a polished surface of an optic, where it splits into its children
	Surface,
	// an iris's opening or a probe plane, crossed: one child carries on
	Pass,
	// a detector's sensor
	Detector,
	// a black or diffuse surface: an iris's blade, a card's face
	Absorbed,
	// a ground surface
	Scattered,
	// a laser's output aperture: a back reflection
	Laser,
	// nothing ahead
	Escaped,
	// BEAM_MAX_INTERACTIONS along its path, or BEAM_MAX_SEGMENTS in its tree
	Truncated
};
const char* beamEndLabel(BeamEnd end);

// everything a segment starts with: what a re-trace from it needs
struct BeamBranchState {
	glm::dvec3 origin { 0.0 };
	glm::dvec3 direction { 0.0, 0.0, 1.0 };
	// the transverse frame the beam is carried in
	glm::dvec3 e1 { 1.0, 0.0, 0.0 };
	glm::dvec3 e2 { 0.0, 1.0, 0.0 };
	ComplexRays rays;
	double power { 0.0 };
	// the optic the segment runs inside (0 outside every body), its medium there and the medium's index
	uint64_t inside { 0 };
	uint8_t medium { 0 };
	double index { 1.0 };
	// the interactions since the source, the optical path n L summed from it and the distance travelled
	uint32_t depth { 0 };
	double opticalPath { 0.0 };
	double distance { 0.0 };
	// the fraction of the power apertures took upstream: 1 - P / (P had nothing been clipped)
	double clippedUpstream { 0.0 };
	// inside an optic: the events since entering it, and the segment that entered it (by key)
	std::vector<PortEvent> events;
	uint64_t entry { 0 };
	// a branch that has just left an optic: the optic, the segment that entered it (by key) and the port it left by
	// (NO_PORT for a ghost); 0 and NO_PORT otherwise
	uint64_t fromOptic { 0 };
	uint64_t passage { 0 };
	OpticPortId port { NO_PORT };
	// it or an ancestor left an optic by no port, or, inside an optic, its events there can no longer make a port
	bool ghost { false };
	// 0 for a reflected branch, 1 for a refracted or passing one: the order children are kept in
	uint8_t slot { 0 };
	bool operator==(const BeamBranchState&) const = default;
};

// where the power went, watts; adds up to the source's
struct BeamPowerBudget {
	double detected { 0.0 };
	// by coatings, black and diffuse surfaces
	double absorbed { 0.0 };
	// by the glass along the way
	double glass { 0.0 };
	double clipped { 0.0 };
	double scattered { 0.0 };
	double escaped { 0.0 };
	double backReflected { 0.0 };
	double belowThreshold { 0.0 };
	double truncated { 0.0 };
	double total() const { return detected + absorbed + glass + clipped + scattered + escaped + backReflected + belowThreshold + truncated; }
	BeamPowerBudget& operator+=(const BeamPowerBudget& other);
	bool operator==(const BeamPowerBudget&) const = default;
};

// the solver's beam where it meets a detector's sensor or crosses a probe plane, in the detector's own frame (its body's
// x and y): power on the sensor, the centroid, the two 1/e^2 radii and the first's angle from x
struct BeamDetectorReading {
	uint64_t optic { 0 };
	uint32_t segment { 0 };
	double power { 0.0 };
	glm::dvec2 centroid { 0.0 };
	glm::dvec2 radii { 0.0 };
	double angle { 0.0 };
	bool operator==(const BeamDetectorReading&) const = default;
};

struct BeamSegment {
	// a key unique within the solver's life, and the parent's and the passage's indices in the tree (NO_BEAM_SEGMENT)
	uint64_t key { 0 };
	uint32_t parent { NO_BEAM_SEGMENT };
	uint32_t passage { NO_BEAM_SEGMENT };
	BeamBranchState start;
	// the physical length, BEAM_ESCAPE_LENGTH for an escaped segment
	double length { 0.0 };
	BeamEnd end { BeamEnd::Escaped };
	uint64_t endOptic { 0 };
	uint32_t endSurface { 0 };
	glm::dvec3 endNormal { 0.0 };
	// the power arriving at the end, after the medium absorbed its share; and the fraction of it the end's aperture took
	double endPower { 0.0 };
	double endClipped { 0.0 };
	// a branch left at the end fell below the ghost threshold
	bool dropped { false };
	// what the end did with the power, and the beam the solver read on a detector or a probe plane there
	BeamPowerBudget spent;
	std::optional<BeamDetectorReading> reading;
	glm::dvec3 endPoint() const { return start.origin + length * start.direction; }
	bool operator==(const BeamSegment& other) const;
};

// warnings: a branch back into a laser, and a ghost coming to a focus on or inside an optic
struct BeamWarning {
	enum Kind : uint8_t { BackReflection, GhostFocus } kind { BackReflection };
	uint32_t segment { 0 };
	uint64_t optic { 0 };
	double power { 0.0 };
	// BackReflection: the angle between the returning beam and the output axis, radians. GhostFocus: the peak intensity,
	// 2 P / (pi wx wy), W/m^2
	double value { 0.0 };
};

// one source's traced beam
struct BeamTree {
	BeamSourceKind sourceKind { BeamSourceKind::Laser };
	uint64_t source { 0 };
	double wavelength { 632.8e-9 };
	double power { 0.0 };
	// the source's ghost threshold, relative: the smallest of its axes', the scene's for a source no axis starts from
	double threshold { DEFAULT_GHOST_THRESHOLD };
	// a camera's chief ray, which carries a notional beam the preview does not draw
	bool chiefRayOnly { false };
	// the laser's own M^2, for the readouts
	glm::dvec2 m2 { 1.0 };
	// depth first from the root, children reflected before refracted
	std::vector<BeamSegment> segments;
	BeamPowerBudget budget;
	std::vector<BeamDetectorReading> readings;
	std::vector<BeamWarning> warnings;
	bool truncated { false };

	// the beam at the start of a segment, and the profile and cross section along it
	BeamProfile profile(uint32_t segment) const;
	BeamCrossSection crossSection(uint32_t segment, double s) const;
};

struct BeamSolution {
	std::vector<BeamTree> trees;
	// per beam axis by id, a report per node in beamAxisOrder() order then the dangling ones
	std::map<uint64_t, std::vector<BeamNodeReport>> reports;
	// the tree each axis's source traced, by axis id
	std::map<uint64_t, uint32_t> axisTrees;

	const BeamNodeReport* report(uint64_t axis, uint32_t node) const;
	const BeamTree* treeOf(uint64_t axis) const;
};

// a camera a beam axis starts from: its chief ray leaves the sensor's center along the view axis
struct BeamCameraSource {
	uint64_t id { 0 };
	glm::dvec3 origin { 0.0 };
	glm::dvec3 direction { 0.0, 0.0, -1.0 };
	glm::dvec3 up { 0.0, 1.0, 0.0 };
	bool operator==(const BeamCameraSource&) const = default;
};

struct BeamSolverInput {
	std::span<const SceneOptic> optics;
	// the material library entry an optic's slot names
	std::function<const MaterialDef*(uint64_t)> material;
	// the library, the surrounding medium and the design wavelength the bodies are built at
	OpticBuildContext build;
	uint64_t libraryRevision { 0 };
	std::span<const BeamAxis> axes;
	// the ghost threshold of an axis with none of its own, and of a laser no axis starts from
	float ghostThreshold { DEFAULT_GHOST_THRESHOLD };
	std::vector<BeamCameraSource> cameras;
};

class BeamSolver {
public:
	// Brings the solution up to date with `input`: re-traces what changed and re-reads every axis. Returns whether the
	// solution changed
	bool solve(const BeamSolverInput& input);
	// forgets every tree, so the next solve is a full one
	void reset();

	const BeamSolution& solution() const { return m_solution; }
	// moves whenever the solution does
	uint64_t revision() const { return m_revision; }
	// the last solve's time and how many segments it traced
	double lastSolveMs() const { return m_lastSolveMs; }
	size_t lastTraced() const { return m_lastTraced; }

	// an optic's body as the solver traced it, or null
	const OpticPrescription* body(uint64_t optic) const;
	const SceneOptic* optic(uint64_t optic) const;

private:
	struct OpticEntry {
		SceneOptic optic;
		std::array<std::optional<MaterialDef>, MAX_OPTIC_MATERIAL_SLOTS> slots;
		std::shared_ptr<const OpticPrescription> body;
		glm::dmat4 worldToBody { 1.0 };
		// world bounds, and the media's indices and absorption at each tree's wavelength, by wavelength
		glm::dvec3 boundsMin { 0.0 };
		glm::dvec3 boundsMax { 0.0 };
		bool operator==(const OpticEntry& other) const { return optic == other.optic && slots == other.slots; }
	};
	friend struct BeamTracer;

	std::vector<OpticEntry> m_optics;
	std::map<uint64_t, size_t> m_opticIndex;
	std::map<std::string, std::shared_ptr<const OpticPrescription>> m_bodies;
	std::string m_contextKey;
	std::vector<BeamCameraSource> m_cameras;
	std::vector<BeamAxis> m_axes;
	BeamSolution m_solution;
	uint64_t m_revision { 1 };
	uint64_t m_nextKey { 1 };
	double m_lastSolveMs { 0.0 };
	size_t m_lastTraced { 0 };
	bool m_solved { false };
};

// which port of `kind` a passage's events make, forward or reversed; NO_PORT for none
OpticPortId matchOpticPort(OpticKind kind, std::span<const PortEvent> events);
// whether the events so far inside an optic can still become one of its kind's ports, forward or reversed: a branch
// inside a body that cannot is a ghost before it leaves
bool opticPortPrefix(OpticKind kind, std::span<const PortEvent> events);

// FOLLOW BEAM: the visits that extend `node` (BEAM_SOURCE_NODE for the source) of `axis` along the traced beam, port by
// port, as far as it goes: each step a parent (a node id, or the index of an earlier step plus BEAM_FOLLOW_STEP), a port and
// an optic. Ports that already have a child are left alone; at most `limit` visits
inline constexpr uint32_t BEAM_FOLLOW_STEP = 0x80000000u;
struct BeamFollowStep {
	uint32_t parent { 0 };
	OpticPortId port { NO_PORT };
	uint64_t optic { 0 };
};
std::vector<BeamFollowStep> followBeam(const BeamSolution& solution, const BeamAxis& axis, uint32_t node, std::span<const SceneOptic> optics, size_t limit = 64);
