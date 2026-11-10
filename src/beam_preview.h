#pragma once

// THE BEAM PREVIEW'S GEOMETRY (docs/plans/completed/physoptics-beam-preview.md), part of rt_optics so bin/beam_test can check it:
// the solver's segments as tube sections the raster preview draws in one instanced draw (shaders/optics/beam_preview.vert).
//
// Per segment and principal axis, the request's three sections: a cone from w(z1) to w(z2) when the segment holds no
// waist, else a cone to the waist's radius, a cylinder of that radius over the waist region (z_R long by default, centered
// on the waist, or the confocal parameter 2 z_R) and a cone out, the cones dropped where the cylinder reaches an end. An
// astigmatic segment rings at both axes' breaks, at most five sections, each axis's radius at a ring read off its own
// three-section profile, so each still follows the rule exactly. A ring is an ellipse on the beam's principal axes there,
// except a segment's first and last, which lie on the surface the segment starts or ends on: its footprint, the
// ellipse carried along the beam onto the surface's tangent plane, so the tubes meet a mirror at its face. A full beam also
// draws that footprint as a disk on every surface it lands on and at the laser's output, after all the tubes.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <beam_axis.h>
#include <beam_gaussian.h>
#include <spectral.h>

struct BeamSolution;
struct BeamTree;
class BeamSolver;

// the preview's settings (Optics -> Beam Preview), saved with the scene's viewport (payload version 33, the ghost threshold 34)
enum class BeamDisplay : uint8_t {
	FullBeam,
	// the footprint ellipse on every surface the beam meets, at the hit's obliquity, as an alignment card shows it
	SpotsOnly,
	// no beam: the beam axes' arrows instead, or each laser's output arrow when there is no axis
	Off
};
enum class BeamWaistRegion : uint8_t {
	// z_R long, centered on the waist: the request's
	RayleighRange,
	// the confocal parameter, -z_R to +z_R, where w <= sqrt(2) w0
	Confocal
};
struct BeamPreviewSettings {
	BeamDisplay display { BeamDisplay::FullBeam };
	// the radius drawn, in units of w: 1 (the 1/e^2 radius) or 1.5 (98.9 % of the power)
	float radiusScale { 1.5f };
	BeamWaistRegion waistRegion { BeamWaistRegion::RayleighRange };
	// the decades of power below the source's that fade from full opacity to nothing
	float brightnessDecades { 4.f };
	// draw the ghosts too: branches that left an optic by no port (a lens's back reflections, a plate's second surface).
	// Off, the preview draws each laser's main beam and what its ports send on
	bool ghosts { false };
	// the beam solver's power floor for a branch, relative to the source's power, unless a beam axis sets its own. Shown
	// beside Ghosts; the solver reads it with Ghosts off too
	float ghostThreshold { DEFAULT_GHOST_THRESHOLD };
	// the Viewport's annotations: each waist's w0 and z_R, the distance from the source to each optic (beamDistanceMarks()),
	// and where an axis's visit is missed
	bool waistLabels { false };
	bool distances { false };
	bool missMarkers { true };
	bool operator==(const BeamPreviewSettings&) const = default;
};

// one ring of a segment: where along it (m from its start), the radii drawn on its two axes, and the first axis's angle
// from the segment's frame
struct BeamRing {
	double s { 0.0 };
	glm::dvec2 radii { 0.0 };
	double angle { 0.0 };
};
// a segment's rings, in order: consecutive rings bound a section
std::vector<BeamRing> beamRings(const ComplexRays& rays, double index, double lambda0, double length, BeamWaistRegion region);

// THE GPU RECORD (shaders/optics/beam_preview.vert): a section's two rings, each a center and the two semi-axis vectors
// of its ellipse, and a color. center.w is the distance from the source along the beam (the dashes), axisA0.w the flags
struct BeamSection {
	glm::vec4 center0;
	glm::vec4 axisA0;
	glm::vec4 axisB0;
	glm::vec4 center1;
	glm::vec4 axisA1;
	glm::vec4 axisB1;
	// linear rgb and opacity
	glm::vec4 color;
};
static_assert(sizeof(BeamSection) == 112);
// BeamSection::axisA0.w: a segment cut off by the solver's caps (dashed), a flat spot rather than a tube, and the glow ends at
// the surface the section's first or last ring lies on, which no part of the beam carries on across
inline constexpr uint32_t BEAM_SECTION_DASHED = 1u;
inline constexpr uint32_t BEAM_SECTION_SPOT = 2u;
inline constexpr uint32_t BEAM_SECTION_STOP_AT_START = 4u;
inline constexpr uint32_t BEAM_SECTION_STOP_AT_END = 8u;

// every section of every laser's tree; a camera's chief ray draws none
std::vector<BeamSection> buildBeamSections(const BeamSolution& solution, const BeamPreviewSettings& settings, const SpectralDisplay& display = {});

// a distance label of the Viewport's annotations: its world point, and the path along the beam from the source to it, m
struct BeamDistanceMark {
	glm::dvec3 point { 0.0 };
	double distance { 0.0 };
};
// One mark per arrival of a main beam at an optic from outside it, in the tree's order. It sits where the beam lands,
// except on a cube beamsplitter, where it is the solver's hit on the diagonal, and on a spherical lens, where the optic's
// "originAt" chooses: a vertex names its surface, and the mark is the solver's hit on it, whichever way the beam goes
// through; a principal plane or the optical center is where the beam crosses that plane of the body, in the glass or on
// either side of it
std::vector<BeamDistanceMark> beamDistanceMarks(const BeamTree& tree, const BeamSolver& solver);
