// CPU verification of the beams (rt_optics; docs/plans/physoptics-beam-axis.md, physoptics-gaussian-beam-solver.md and
// physoptics-beam-preview.md). No Vulkan, no GPU.
//
//  - ports: every kind's port names back to their ids, the source's single output, a probe plane passing the beam on
//  - the authored tree (src/beam_axis.h): appending, a splitter's children by port, inserting before and after, every
//    removal (a chain closes up, a splitter's lowest port takes its place and the rest dangle, a dangling root's
//    children dangle), every visit of an optic visited twice, moving and its refusals, renumbering, and 20000 random
//    edits that must leave a tree: unique ids, every parent there, one child per place, no cycle, every node listed once
//
//  - the Gaussian beam (src/beam_gaussian.h): w(z), the waist and the Rayleigh range in free space against the closed
//    forms; each surface's ray-transfer matrix symplectic and against central differences of parabasal rays traced
//    through the real surface, at 0, 10, 45 and 70 degrees, reflected and refracted, on a sphere, an asphere off its axis
//    with its principal directions turned against the plane of incidence, and an off-axis paraboloid; a spherical
//    mirror's tangential and sagittal foci at 45 degrees; an off-axis parabola's stigmatic focus
//  - the solver (src/beam_solver.h) on a small bench: a plano-convex lens's focus against the thick-lens ABCD law, the
//    power budget adding up to the laser's, a meter's reading, the visits' statuses (Hit, Clipped by an iris, Missed by
//    the distance the geometry gives, Blocked), Follow beam, and 1000 random edits solved selectively against full solves
//  - the preview's sections (src/beam_preview.h): one cone without a waist, cone, cylinder and cone with one, clamped at
//    the ends, and an astigmatic segment's rings each on its own axis's profile; on a solved bench, ghosts (a lens's
//    internal reflections among them) drawn only when asked, and a tube's last ring lying on the mirror it meets
//
// The scene file's round trip of the axes, through the editor's ids, is tools/verify/beam_axis_check.py; the solver on
// the verification scenes is tools/verify/beam_solver_check.py.
//
// Run from the repository root:  ./bin/beam_test [--selfcheck] [--root dir]

#include <bit>
#include <beam_axis.h>
#include <beam_gaussian.h>
#include <beam_preview.h>
#include <beam_solver.h>
#include <optic_catalog.h>
#include <optic_detectors.h>
#include <optic_laser.h>
#include <optic_library.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <map>
#include <random>
#include <set>

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace {

int g_failures = 0;
// --selfcheck: errors planted in the references, which every check below must catch
bool g_plant = false;

std::mt19937_64 g_rng(20261107);

void check(bool ok, const std::string& what)
{
	if (!ok) {
		g_failures++;
		fmt::println("  FAIL {}", what);
	}
}

// what a well-formed axis is, or the first thing wrong with it
std::string treeProblem(const BeamAxis& axis)
{
	std::set<uint32_t> ids;
	std::set<std::pair<uint32_t, OpticPortId>> places;
	for (const BeamAxisNode& node : axis.nodes) {
		if (!ids.insert(node.id).second) {
			return fmt::format("node id {} twice", node.id);
		}
	}
	for (const BeamAxisNode& node : axis.nodes) {
		if (node.parent != BEAM_SOURCE_NODE && node.parent != BEAM_DANGLING_NODE && !ids.contains(node.parent)) {
			return fmt::format("node {}'s parent {} is not there", node.id, node.parent);
		}
		if ((node.parent == BEAM_SOURCE_NODE || node.parent == BEAM_DANGLING_NODE) && node.port != NO_PORT) {
			return fmt::format("node {} under the source or dangling has port {}", node.id, node.port);
		}
		if (node.parent != BEAM_DANGLING_NODE && !places.insert({ node.parent, node.port }).second) {
			return fmt::format("two nodes hang off port {} of {}", node.port, node.parent);
		}
		if (!beamNodeDangling(axis, node.id) && !beamNodeBelow(axis, node.id, BEAM_SOURCE_NODE)) {
			return fmt::format("node {} reaches neither the source nor a dangling root", node.id);
		}
	}
	std::vector<uint32_t> listed = beamAxisOrder(axis);
	const std::vector<uint32_t> dangling = beamDanglingOrder(axis);
	listed.insert(listed.end(), dangling.begin(), dangling.end());
	std::sort(listed.begin(), listed.end());
	if (listed.size() != axis.nodes.size() || std::adjacent_find(listed.begin(), listed.end()) != listed.end()) {
		return fmt::format("{} nodes listed for {}", listed.size(), axis.nodes.size());
	}
	return {};
}

void checkTree(BeamAxis axis, const std::string& what)
{
	// planted: a second child on the first node's place
	if (g_plant && !axis.nodes.empty()) {
		BeamAxisNode copy = axis.nodes.front();
		copy.id = 1000000;
		copy.parent = axis.nodes.front().parent == BEAM_DANGLING_NODE ? BEAM_SOURCE_NODE : axis.nodes.front().parent;
		copy.port = axis.nodes.front().port;
		axis.nodes.push_back(copy);
		if (copy.parent == BEAM_SOURCE_NODE && beamNodeAt(axis, BEAM_SOURCE_NODE, NO_PORT) == &axis.nodes.back()) {
			axis.nodes.push_back({ .id = 1000001, .optic = 1, .parent = BEAM_SOURCE_NODE });
		}
	}
	const std::string problem = treeProblem(axis);
	check(problem.empty(), fmt::format("{}: {}", what, problem));
}

// the optics along the depth-first order, the way the panel lists them
std::vector<uint64_t> opticOrder(const BeamAxis& axis, bool dangling = false)
{
	std::vector<uint64_t> optics;
	for (uint32_t id : dangling ? beamDanglingOrder(axis) : beamAxisOrder(axis)) {
		optics.push_back(findBeamNode(axis, id)->optic);
	}
	return optics;
}

void checkOrder(const BeamAxis& axis, std::vector<uint64_t> expected, const std::string& what, bool dangling = false)
{
	if (g_plant) {
		expected.push_back(99);
	}
	const std::vector<uint64_t> got = opticOrder(axis, dangling);
	check(got == expected, fmt::format("{}: order [{}], expected [{}]", what, fmt::join(got, " "), fmt::join(expected, " ")));
}

// the node of an axis that visits `optic` (the first, for an optic visited twice)
uint32_t nodeOf(const BeamAxis& axis, uint64_t optic)
{
	for (const BeamAxisNode& node : axis.nodes) {
		if (node.optic == optic) {
			return node.id;
		}
	}
	return BEAM_DANGLING_NODE - 1;
}

//---------------------------------------------------------------- ports

void testPorts()
{
	for (size_t k = 0; k < OPTIC_KIND_COUNT; k++) {
		const OpticKind kind = (OpticKind)k;
		for (uint32_t p = 0; p < opticPortCount(kind); p++) {
			const std::string_view name = opticPortName(kind, (OpticPortId)p);
			const std::optional<OpticPortId> back = findOpticPort(kind, g_plant ? std::string(name) + "x" : std::string(name));
			check(!name.empty() && back == (OpticPortId)p, fmt::format("{} port {} \"{}\" round-trips", opticTraits(kind).name, p, name));
		}
		check(opticPortName(kind, (OpticPortId)opticPortCount(kind)).empty() != g_plant, fmt::format("{}: a port past its last has no name", opticTraits(kind).name));
		check(!findOpticPort(kind, "Nowhere").has_value() != g_plant, fmt::format("{}: an unknown port name is not found", opticTraits(kind).name));
	}
	check(opticPortName(OpticKind::Laser, 0) == (g_plant ? "Input" : "Output"), "a laser leaves by Output");
	check(opticPortCount(OpticKind::ProbePlane) == (g_plant ? 0u : 1u), "a probe plane passes the beam on");
	check(opticPortCount(OpticKind::PowerMeter) == (g_plant ? 1u : 0u), "a power meter ends the beam");
	check(opticPortCount(OpticKind::PlateBeamsplitter) == (g_plant ? 1u : 2u), "a plate beamsplitter has two ports");
}

//---------------------------------------------------------------- the tree

// optics by number: 1 to 9 one-port elements, 20 a beamsplitter (ports 0 and 1)
void testAppendAndOrder()
{
	BeamAxis axis = makeBeamAxis("chain");
	const std::optional<uint32_t> a = appendBeamNode(axis, BEAM_SOURCE_NODE, 3, 1);
	check(a.has_value() && findBeamNode(axis, *a)->port == (g_plant ? 3 : NO_PORT), "the first visit's port is the source's one output");
	check(!appendBeamNode(axis, BEAM_SOURCE_NODE, NO_PORT, 2).has_value() != g_plant, "the source holds one visit");
	const std::optional<uint32_t> b = appendBeamNode(axis, *a, 0, 2);
	appendBeamNode(axis, *b, 0, 3);
	check(!appendBeamNode(axis, *a, 0, 4).has_value() != g_plant, "a taken port refuses another visit");
	check(!appendBeamNode(axis, 77, 0, 4).has_value() != g_plant, "a missing parent refuses a visit");
	checkOrder(axis, { 1, 2, 3 }, "a chain");
	checkTree(axis, "a chain");

	// a splitter's children list by port, whichever came first
	BeamAxis split = makeBeamAxis("split");
	const uint32_t bs = *appendBeamNode(split, BEAM_SOURCE_NODE, NO_PORT, 20);
	const uint32_t transmitted = *appendBeamNode(split, bs, 1, 6);
	appendBeamNode(split, transmitted, 0, 7);
	appendBeamNode(split, bs, 0, 5);
	checkOrder(split, { 20, 5, 6, 7 }, "a splitter's arms by port");
	checkTree(split, "a splitter");

	// a dangling root takes any number
	check(appendBeamNode(split, BEAM_DANGLING_NODE, 0, 8).has_value() && appendBeamNode(split, BEAM_DANGLING_NODE, 0, 9).has_value() != g_plant,
		"any number dangle");
	checkOrder(split, { 8, 9 }, "dangling roots in the order they came", true);
	checkTree(split, "a splitter with dangling roots");
}

void testInsert()
{
	BeamAxis axis = makeBeamAxis("insert");
	const uint32_t a = *appendBeamNode(axis, BEAM_SOURCE_NODE, NO_PORT, 1);
	const uint32_t b = *appendBeamNode(axis, a, 0, 2);
	// before b: the new visit takes b's place, b hangs off its port
	const std::optional<uint32_t> x = insertBeamNodeBefore(axis, b, 20, 1);
	check(x.has_value() && findBeamNode(axis, b)->parent == *x && findBeamNode(axis, b)->port == (g_plant ? 0 : 1), "insert before: the visit hangs off the chosen port");
	checkOrder(axis, { 1, 20, 2 }, "insert before");
	// after a's port 0, which x holds now: between them
	const std::optional<uint32_t> y = insertBeamNodeAfter(axis, a, 0, 3, 0);
	check(y.has_value() && findBeamNode(axis, *x)->parent == (g_plant ? a : *y), "insert after: what hung there moves below");
	checkOrder(axis, { 1, 3, 20, 2 }, "insert after a taken port");
	// after the source, in front of everything
	insertBeamNodeAfter(axis, BEAM_SOURCE_NODE, NO_PORT, 4, 0);
	checkOrder(axis, { 4, 1, 3, 20, 2 }, "insert after the source");
	// after a free port: an append
	insertBeamNodeAfter(axis, *x, 0, 5, 0);
	checkOrder(axis, { 4, 1, 3, 20, 5, 2 }, "insert after a free port");
	checkTree(axis, "after inserts");
	// in front of the first visit
	insertBeamNodeBefore(axis, nodeOf(axis, 4), 6, 0);
	checkOrder(axis, { 6, 4, 1, 3, 20, 5, 2 }, "insert before the first visit");
	checkTree(axis, "after inserting before the first visit");
}

void testRemove()
{
	// a chain closes up, the moved visit marked for review
	{
		BeamAxis axis = makeBeamAxis("chain");
		const uint32_t a = *appendBeamNode(axis, BEAM_SOURCE_NODE, NO_PORT, 1);
		const uint32_t b = *appendBeamNode(axis, a, 0, 2);
		const uint32_t c = *appendBeamNode(axis, b, 0, 3);
		removeBeamNode(axis, b);
		checkOrder(axis, { 1, 3 }, "removing a chain's middle");
		check(findBeamNode(axis, c)->review != g_plant, "the visit that moved up is marked for review");
		check(!findBeamNode(axis, a)->review != g_plant, "its parent is not");
		removeBeamNode(axis, a);
		checkOrder(axis, { 3 }, "removing the first visit");
		checkTree(axis, "a chain after removals");
	}
	// a splitter: its lowest port's arm takes its place, the other dangles with its subtree
	{
		BeamAxis axis = makeBeamAxis("split");
		const uint32_t a = *appendBeamNode(axis, BEAM_SOURCE_NODE, NO_PORT, 1);
		const uint32_t bs = *appendBeamNode(axis, a, 0, 20);
		const uint32_t t = *appendBeamNode(axis, bs, 1, 6);
		appendBeamNode(axis, t, 0, 7);
		const uint32_t r = *appendBeamNode(axis, bs, 0, 5);
		removeBeamNode(axis, bs);
		checkOrder(axis, { 1, 5 }, "the reflected arm takes the splitter's place");
		checkOrder(axis, { 6, 7 }, "the transmitted arm dangles with what follows it", true);
		check(findBeamNode(axis, r)->review && findBeamNode(axis, t)->review != g_plant, "both arms are marked for review");
		check(findBeamNode(axis, t)->parent == (g_plant ? bs : BEAM_DANGLING_NODE) && findBeamNode(axis, t)->port == NO_PORT, "a dangling node has no port");
		checkTree(axis, "a splitter removed");

		// a dangling root's children dangle in its place
		removeBeamNode(axis, t);
		checkOrder(axis, { 7 }, "a dangling root's child dangles", true);
		checkTree(axis, "a dangling root removed");
		// and a whole subtree goes at once
		removeBeamSubtree(axis, a);
		check(opticOrder(axis).empty() != g_plant && axis.nodes.size() == 1, "removing a subtree removes everything below it");
		checkTree(axis, "a subtree removed");
	}
	// an optic visited twice, as a lens in front of a retroreflector: both visits go
	{
		BeamAxis axis = makeBeamAxis("double pass");
		const uint32_t lens = *appendBeamNode(axis, BEAM_SOURCE_NODE, NO_PORT, 1);
		const uint32_t retro = *appendBeamNode(axis, lens, 0, 2);
		const uint32_t back = *appendBeamNode(axis, retro, 1, 1);
		appendBeamNode(axis, back, 0, 3);
		checkOrder(axis, { 1, 2, 1, 3 }, "a double pass");
		removeBeamOptic(axis, 1);
		checkOrder(axis, { 2, 3 }, "every visit of the twice-visited optic removed");
		checkTree(axis, "a double pass without its lens");
	}
}

void testMove()
{
	BeamAxis axis = makeBeamAxis("move");
	const uint32_t a = *appendBeamNode(axis, BEAM_SOURCE_NODE, NO_PORT, 1);
	const uint32_t bs = *appendBeamNode(axis, a, 0, 20);
	const uint32_t r = *appendBeamNode(axis, bs, 0, 5);
	const uint32_t t = *appendBeamNode(axis, bs, 1, 6);
	check(!moveBeamNode(axis, a, r, 0) != g_plant, "a node cannot move below itself");
	check(!moveBeamNode(axis, t, bs, 0) != g_plant, "a node cannot move onto a taken place");
	check(moveBeamNode(axis, t, BEAM_DANGLING_NODE, 0) != g_plant && findBeamNode(axis, t)->port == NO_PORT, "detaching cuts a node loose");
	check(moveBeamNode(axis, t, r, 0) != g_plant, "a dangling node attaches to a free place");
	checkOrder(axis, { 1, 20, 5, 6 }, "after moves");
	check(moveBeamNode(axis, t, bs, 1) != g_plant, "a node moves back");
	check(moveBeamNode(axis, t, bs, 1) != g_plant, "moving onto its own place is allowed");
	checkTree(axis, "after moves");
}

void testRenumber()
{
	BeamAxis axis = makeBeamAxis("renumber");
	const uint32_t a = *appendBeamNode(axis, BEAM_SOURCE_NODE, NO_PORT, 1);
	const uint32_t bs = *appendBeamNode(axis, a, 0, 20);
	appendBeamNode(axis, bs, 1, 6);
	appendBeamNode(axis, bs, 0, 5);
	removeBeamNode(axis, a);
	insertBeamNodeBefore(axis, bs, 2, 0);
	const std::vector<uint64_t> before = opticOrder(axis);
	renumberBeamNodes(axis);
	bool positions = true;
	for (size_t i = 0; i < axis.nodes.size(); i++) {
		positions &= axis.nodes[i].id == (uint32_t)i;
		positions &= axis.nodes[i].parent == BEAM_SOURCE_NODE || axis.nodes[i].parent == BEAM_DANGLING_NODE || axis.nodes[i].parent < axis.nodes.size();
	}
	check(positions != g_plant, "renumbered ids are positions");
	checkOrder(axis, before, "renumbering keeps the tree");
	checkTree(axis, "renumbered");
}

// random edits, each of which must leave a tree; nodes visit optics 1 to 6 (one port) and 20 (two ports)
void testRandomEdits(int edits)
{
	BeamAxis axis = makeBeamAxis("random");
	const uint64_t optics[] = { 1, 2, 3, 4, 5, 6, 20 };
	const auto ports = [](uint64_t optic) { return optic == 20 ? 2u : 1u; };
	const auto pick = [&](size_t n) { return (size_t)std::uniform_int_distribution<size_t>(0, n - 1)(g_rng); };
	const auto anyNode = [&]() -> uint32_t {
		if (axis.nodes.empty() || pick(8) == 0) {
			return BEAM_SOURCE_NODE;
		}
		return axis.nodes[pick(axis.nodes.size())].id;
	};
	const auto portOf = [&](uint32_t node) -> OpticPortId {
		const BeamAxisNode* n = findBeamNode(axis, node);
		return n == nullptr ? NO_PORT : (OpticPortId)pick(ports(n->optic));
	};
	std::string firstProblem;
	int firstEdit = -1;
	for (int e = 0; e < edits && firstEdit < 0; e++) {
		const uint64_t optic = optics[pick(std::size(optics))];
		switch (pick(8)) {
		case 0:
		case 1: {
			const uint32_t parent = anyNode();
			appendBeamNode(axis, parent, portOf(parent), optic);
			break;
		}
		case 2:
			if (!axis.nodes.empty()) {
				insertBeamNodeBefore(axis, anyNode(), optic, (OpticPortId)pick(ports(optic)));
			}
			break;
		case 3: {
			const uint32_t parent = anyNode();
			insertBeamNodeAfter(axis, parent, portOf(parent), optic, (OpticPortId)pick(ports(optic)));
			break;
		}
		case 4:
			if (!axis.nodes.empty()) {
				removeBeamNode(axis, anyNode());
			}
			break;
		case 5:
			if (!axis.nodes.empty() && pick(4) == 0) {
				removeBeamSubtree(axis, anyNode());
			}
			break;
		case 6:
			if (!axis.nodes.empty()) {
				const uint32_t node = anyNode();
				const uint32_t parent = pick(3) == 0 ? BEAM_DANGLING_NODE : anyNode();
				moveBeamNode(axis, node, parent, portOf(parent));
			}
			break;
		case 7:
			if (pick(10) == 0) {
				removeBeamOptic(axis, optic);
			} else if (pick(10) == 0) {
				renumberBeamNodes(axis);
			}
			break;
		}
		const std::string problem = treeProblem(axis);
		if (!problem.empty()) {
			firstProblem = problem;
			firstEdit = e;
		}
	}
	check(firstEdit < 0, fmt::format("random edits: edit {} left {}", firstEdit, firstProblem));
	checkTree(axis, fmt::format("after {} random edits", edits));
}


//---------------------------------------------------------------- the Gaussian beam

OpticalLibrary g_library;

// the planted error: references scaled by this under --selfcheck
double plant(double value)
{
	return g_plant ? value * 1.001 + 1e-9 : value;
}

bool close(double a, double b, double relative, double absolute = 0.0)
{
	return std::abs(a - b) <= relative * std::max(std::abs(a), std::abs(b)) + absolute;
}

void testFreeSpace()
{
	// an astigmatic laser's axes in free space, against w(z) = w0 sqrt(1 + ((z - zw) / zR)^2)
	const double lambda = 632.8e-9;
	const double n = 1.000276;
	const glm::dvec2 w0(0.405e-3, 0.7e-3);
	const glm::dvec2 m2(1.2, 2.5);
	const glm::dvec2 zw(0.05, -0.3);
	const glm::dvec2 zR = std::numbers::pi * w0 * w0 * n / (m2 * lambda);
	// the embedded M^2 the solver gives a laser, so its own w0 comes back
	const glm::dvec2 m2Embedded = std::numbers::pi * n * w0 * w0 / (lambda * zR);
	const ComplexRays rays = simpleAstigmaticBeam(zw, zR, n, m2Embedded);
	double worst = 0.0;
	for (double z : { 0.0, 0.05, 0.1, 0.7, 3.0 }) {
		const BeamCrossSection at = crossSection(rays, z, n, lambda);
		for (int i = 0; i < 2; i++) {
			const double expected = plant(w0[i] * std::sqrt(1.0 + std::pow((z - zw[i]) / zR[i], 2.0)));
			worst = std::max(worst, std::abs(at.radii[i] - expected) / expected);
		}
	}
	check(worst < 1e-12, fmt::format("free space: w(z) off the closed form by {:.2e} relative", worst));
	const BeamProfile profile = beamProfile(rays, n, lambda);
	for (int i = 0; i < 2; i++) {
		check(close(profile.axes[i].waist, plant(w0[i]), 1e-12), fmt::format("free space: axis {} waist {} against {}", i, profile.axes[i].waist, w0[i]));
		check(close(profile.axes[i].waistAt, plant(zw[i]), 1e-12, 1e-15), fmt::format("free space: axis {} waist at {} against {}", i, profile.axes[i].waistAt, zw[i]));
		check(close(profile.axes[i].rayleighRange, plant(zR[i]), 1e-12), fmt::format("free space: axis {} zR {} against {}", i, profile.axes[i].rayleighRange, zR[i]));
	}
	check(!profile.twisted, "free space: a simple astigmatic beam on its frame's axes reads as twisted");
}

// one case of a surface: the surface (its frame the world's), the chief ray's hit and direction, and the media
struct SurfaceCase {
	std::string name;
	OpticSurface surface;
	glm::dvec3 hit;
	glm::dvec3 direction;
	double n { 1.0 };
	double nOut { 1.0 };
	bool reflect { true };
};

// the outgoing ray of a neighbour, traced through the real surface: its offset and reduced direction in the outgoing frame
bool traceNeighbour(const SurfaceCase& c, const glm::dvec3& e1, const glm::dvec3& e2, const glm::dvec3& dOut, const glm::dvec3& f1, const glm::dvec3& f2,
	const glm::dvec4& in, glm::dvec4& out)
{
	const glm::dvec2 slope = glm::dvec2(in.z, in.w) / c.n;
	const glm::dvec3 d = glm::normalize(slope.x * e1 + slope.y * e2 + std::sqrt(1.0 - glm::dot(slope, slope)) * c.direction);
	const glm::dvec3 start = c.hit + in.x * e1 + in.y * e2 - 0.01 * d;
	double t;
	if (!intersectSurfaceLocal(c.surface, start, d, 0.0, 1.0, t)) {
		return false;
	}
	const glm::dvec3 q = start + t * d;
	const glm::dvec3 normal = surfaceNormalLocal(c.surface, q);
	glm::dvec3 leaving;
	if (c.reflect) {
		reflectDirection(d, normal, leaving);
	} else if (!refractDirection(d, normal, c.n / c.nOut, leaving)) {
		return false;
	}
	leaving = glm::normalize(leaving);
	const double tau = glm::dot(dOut, c.hit - q) / glm::dot(dOut, leaving);
	const glm::dvec3 p = q + tau * leaving - c.hit;
	out = glm::dvec4(glm::dot(f1, p), glm::dot(f2, p), c.nOut * glm::dot(f1, leaving), c.nOut * glm::dot(f2, leaving));
	return true;
}

void testSurfaceMatrices()
{
	std::vector<SurfaceCase> cases;
	// a sphere of radius 0.1 m, concave toward +z, hit at its vertex and off it
	OpticSurface sphere;
	sphere.curvature = 1.0 / 0.1;
	sphere.aperture.a = 0.04;
	finishSurface(sphere);
	// an even asphere, hit at (3, 4) mm: its principal directions radial and tangential there, 53 degrees off the xz plane
	OpticSurface asphere;
	asphere.shape = SurfaceShape::EvenAsphere;
	asphere.curvature = 1.0 / 0.08;
	asphere.conic = -0.6;
	asphere.asphere[0] = 400.0;
	asphere.asphere[1] = -2e5;
	asphere.aperture.a = 0.02;
	finishSurface(asphere);
	// a paraboloid of focal length 50 mm hit 50 mm off its axis: a 90-degree off-axis parabola
	OpticSurface parabola;
	parabola.curvature = 1.0 / 0.1;
	parabola.conic = -1.0;
	parabola.aperture.a = 0.1;
	finishSurface(parabola);

	const auto onSurface = [](const OpticSurface& s, double x, double y) { return glm::dvec3(x, y, *surfaceSag(s, glm::dvec2(x, y))); };
	for (double degrees : { 0.0, 10.0, 45.0, 70.0 }) {
		for (int which = 0; which < 3; which++) {
			for (bool reflect : { true, false }) {
				SurfaceCase c;
				c.surface = which == 0 ? sphere : which == 1 ? asphere : parabola;
				c.hit = which == 0 ? onSurface(sphere, 0.004, -0.002) : which == 1 ? onSurface(asphere, 0.003, 0.004) : onSurface(parabola, 0.0, 0.05);
				const glm::dvec3 normal = surfaceNormalLocal(c.surface, c.hit);
				// arriving against the normal, tilted in the plane of the normal and x
				const glm::dvec3 tangent = glm::normalize(glm::dvec3(1.0, 0.0, 0.0) - normal * normal.x);
				const double angle = degrees * std::numbers::pi / 180.0;
				c.direction = glm::normalize(-std::cos(angle) * normal + std::sin(angle) * tangent);
				c.reflect = reflect;
				c.n = 1.0;
				c.nOut = reflect ? 1.0 : 1.5168;
				c.name = fmt::format("{} {} at {} deg", which == 0 ? "sphere" : which == 1 ? "asphere" : "off-axis paraboloid", reflect ? "reflecting" : "refracting", degrees);
				cases.push_back(c);
			}
		}
	}

	double worstSymplectic = 0.0;
	double worstDifference = 0.0;
	std::string worstCase;
	for (const SurfaceCase& c : cases) {
		const glm::dvec3 normal = surfaceNormalLocal(c.surface, c.hit);
		const glm::dvec3 e1 = perpendicularTo(c.direction, glm::dvec3(1.0, 0.0, 0.0));
		const glm::dvec3 e2 = glm::cross(c.direction, e1);
		glm::dvec3 dOut;
		if (c.reflect) {
			reflectDirection(c.direction, normal, dOut);
		} else {
			refractDirection(c.direction, normal, c.n / c.nOut, dOut);
		}
		dOut = glm::normalize(dOut);
		glm::dvec3 f1, f2;
		transportFrame(c.direction, dOut, normal, c.reflect, e1, e2, f1, f2);
		const PrincipalCurvaturesT<double> k = principalCurvatures(c.surface, c.hit);
		SurfaceGeometry geometry { normal, k.k1, k.k2, k.d1, k.d2 };
		RayTransfer s = surfaceTransfer(geometry, c.direction, e1, e2, c.n, dOut, f1, f2, c.nOut, c.reflect);
		if (g_plant) {
			s.C[0][0] *= 1.001;
		}
		worstSymplectic = std::max(worstSymplectic, s.symplecticError());
		// central differences of four parabasal rays a side
		const double h = 1e-6;
		for (int j = 0; j < 4; j++) {
			glm::dvec4 plus(0.0), minus(0.0);
			glm::dvec4 delta(0.0);
			delta[j] = h;
			if (!traceNeighbour(c, e1, e2, dOut, f1, f2, delta, plus) || !traceNeighbour(c, e1, e2, dOut, f1, f2, -delta, minus)) {
				check(false, fmt::format("{}: a parabasal ray missed", c.name));
				continue;
			}
			const glm::dvec4 column = (plus - minus) / (2.0 * h);
			const glm::dvec4 analytic = j < 2 ? glm::dvec4(s.A[j][0], s.A[j][1], s.C[j][0], s.C[j][1]) : glm::dvec4(s.B[j - 2][0], s.B[j - 2][1], s.D[j - 2][0], s.D[j - 2][1]);
			// the position and direction rows each scaled by their own size
			for (int i = 0; i < 4; i++) {
				const double scale = i < 2 ? 1.0 : std::max(1.0, std::abs(c.nOut) * c.surface.curvature * 4.0);
				const double difference = std::abs(column[i] - analytic[i]) / scale;
				if (difference > worstDifference) {
					worstDifference = difference;
					worstCase = fmt::format("{}, entry ({}, {}): {} against {}", c.name, i, j, analytic[i], column[i]);
				}
			}
		}
	}
	check(worstSymplectic < 1e-12, fmt::format("surface matrices: S^T J S off J by {:.2e}", worstSymplectic));
	check(worstDifference < 1e-7, fmt::format("surface matrices: off central differences by {:.2e} ({})", worstDifference, worstCase));
	fmt::println("  surface matrices: {} cases, symplectic to {:.1e}, central differences to {:.1e}", cases.size(), worstSymplectic, worstDifference);
}

// a collimated beam onto one surface: each outgoing axis's waist distance
glm::dvec2 focusAfter(const SurfaceCase& c, double zR, double lambda)
{
	const glm::dvec3 normal = surfaceNormalLocal(c.surface, c.hit);
	// the frame's first vector in the plane of incidence: the tangential axis
	const glm::dvec3 inPlane = glm::cross(c.direction, normal);
	const glm::dvec3 e2 = glm::length(inPlane) > 1e-12 ? glm::normalize(inPlane) : glm::dvec3(0.0, 1.0, 0.0);
	const glm::dvec3 e1 = glm::cross(e2, c.direction);
	glm::dvec3 dOut;
	reflectDirection(c.direction, normal, dOut);
	glm::dvec3 f1, f2;
	transportFrame(c.direction, dOut, normal, true, e1, e2, f1, f2);
	const PrincipalCurvaturesT<double> k = principalCurvatures(c.surface, c.hit);
	const RayTransfer s = surfaceTransfer({ normal, k.k1, k.k2, k.d1, k.d2 }, c.direction, e1, e2, 1.0, dOut, f1, f2, 1.0, true);
	const ComplexRays out = s * simpleAstigmaticBeam(glm::dvec2(0.0), glm::dvec2(zR), 1.0, glm::dvec2(1.0));
	// read each frame axis's q off the diagonal
	const CMat2 q = out.q();
	return glm::dvec2(-q(0, 0).real(), -q(1, 1).real());
}

void testMirrorFoci()
{
	// a spherical mirror of R = 0.2 m at 45 degrees: tangential focus R cos / 2, sagittal R / (2 cos), for a beam whose
	// zR is finite: z = f / (1 + (f / zR)^2)
	const double R = 0.2;
	const double zR = 50.0;
	OpticSurface sphere;
	sphere.curvature = 1.0 / R;
	sphere.aperture.a = 0.05;
	finishSurface(sphere);
	SurfaceCase c;
	c.surface = sphere;
	c.hit = glm::dvec3(0.0);
	const double angle = std::numbers::pi / 4.0;
	c.direction = glm::dvec3(std::sin(angle), 0.0, -std::cos(angle));
	const glm::dvec2 foci = focusAfter(c, zR, 632.8e-9);
	const auto gaussianFocus = [&](double f) { return f / (1.0 + (f / zR) * (f / zR)); };
	check(close(foci.x, plant(gaussianFocus(R * std::cos(angle) / 2.0)), 1e-10), fmt::format("45-degree mirror: tangential focus {} against {}", foci.x, gaussianFocus(R * std::cos(angle) / 2.0)));
	check(close(foci.y, plant(gaussianFocus(R / (2.0 * std::cos(angle)))), 1e-10), fmt::format("45-degree mirror: sagittal focus {} against {}", foci.y, gaussianFocus(R / (2.0 * std::cos(angle)))));

	// a 90-degree off-axis parabola, f = 50 mm, the beam along its parent axis: one focus, f + y^2 / 4f from the hit
	OpticSurface parabola;
	parabola.curvature = 1.0 / 0.1;
	parabola.conic = -1.0;
	parabola.aperture.a = 0.1;
	finishSurface(parabola);
	c.surface = parabola;
	c.hit = glm::dvec3(0.0, 0.05, *surfaceSag(parabola, glm::dvec2(0.0, 0.05)));
	c.direction = glm::dvec3(0.0, 0.0, -1.0);
	const glm::dvec2 oap = focusAfter(c, zR, 632.8e-9);
	const double reflected = 0.05 + 0.05 * 0.05 / (4.0 * 0.05);
	check(close(oap.x, oap.y, g_plant ? 0.0 : 1e-10, g_plant ? -1.0 : 0.0), fmt::format("off-axis parabola: astigmatic, foci {} and {}", oap.x, oap.y));
	check(close(oap.x, plant(gaussianFocus(reflected)), 1e-10), fmt::format("off-axis parabola: focus {} against {}", oap.x, gaussianFocus(reflected)));
}

//---------------------------------------------------------------- the solver on a small bench

// a scene of optics placed by hand (no bench), their slot materials by id
struct TestBench {
	std::vector<SceneOptic> optics;
	std::map<uint64_t, MaterialDef> materials;
	std::vector<BeamAxis> axes;
	uint64_t nextId { 1 };

	SceneOptic& add(OpticKind kind, const glm::dmat4& world)
	{
		SceneOptic optic = makeOptic(kind);
		optic.id = nextId++;
		optic.name = fmt::format("{} {}", opticTraits(kind).label, optic.id);
		optic.world = world;
		optics.push_back(optic);
		SceneOptic& o = optics.back();
		for (size_t s = 0; s < opticTraits(kind).slots.size(); s++) {
			o.materials[s] = o.id * 16 + s + 1;
			materials[o.materials[s]] = defaultSlotMaterial(o, s);
		}
		return o;
	}
	SceneOptic* find(uint64_t id)
	{
		for (SceneOptic& o : optics) {
			if (o.id == id) {
				return &o;
			}
		}
		return nullptr;
	}
	BeamSolverInput input() const
	{
		BeamSolverInput in;
		in.optics = optics;
		in.material = [this](uint64_t id) -> const MaterialDef* {
			const auto found = materials.find(id);
			return found != materials.end() ? &found->second : nullptr;
		};
		in.build.library = &g_library;
		in.build.surrounding = GLASS_AIR;
		in.libraryRevision = g_library.revision();
		in.axes = axes;
		return in;
	}
};

glm::dmat4 at(double x, double y, double z)
{
	return glm::translate(glm::dmat4(1.0), glm::dvec3(x, y, z));
}

glm::dmat4 turned(const glm::dmat4& place, double degrees, const glm::dvec3& axis = glm::dvec3(0.0, 1.0, 0.0))
{
	return glm::rotate(place, degrees * std::numbers::pi / 180.0, axis);
}

void testLensFocus()
{
	// a laser's waist at its aperture, a plano-convex N-BK7 lens 0.1 m out, its curved face first: the waist after it
	// against the thick lens's ABCD law, with the indices the library gives at 632.8 nm
	TestBench bench;
	const uint64_t laserId = bench.add(OpticKind::Laser, glm::dmat4(1.0)).id;
	setOpticValue(*bench.find(laserId), "waistRadius", 1e-3);
	setOpticValue(*bench.find(laserId), "m2", 1.0);
	SceneOptic& lens = bench.add(OpticKind::SphericalLens, at(0.0, 0.0, 0.1));
	setOpticValue(lens, "r1", 0.0515);
	setOpticValue(lens, "centerThickness", 3.6e-3);
	const SceneOptic& laser = *bench.find(laserId);
	BeamSolver solver;
	solver.solve(bench.input());
	const BeamTree& tree = solver.solution().trees.front();

	// the lens's faces from its body, and the ABCD law in reduced coordinates
	const OpticPrescription* body = solver.body(lens.id);
	const double lambdaNm = 632.8;
	const double n0 = glassIndex(*g_library.findGlass(GLASS_AIR), lambdaNm);
	const double n = glassIndex(*g_library.findGlass("N-BK7"), lambdaNm);
	const OpticSurface& front = body->surfaces[0];
	const OpticSurface& back = body->surfaces[1];
	const double zFront = 0.1 + front.placement.origin.z;
	// each face's curvature with its center toward +z of the body: a face's own frame may look the other way
	const double cFront = front.curvature * front.placement.rotation[2][2];
	const double cBack = back.curvature * back.placement.rotation[2][2];
	const double thickness = back.placement.origin.z - front.placement.origin.z;
	// refraction by a face of curvature c (center toward +z) from n1 into n2: u' = u - (n2 - n1) c x
	const auto refraction = [](double c, double n1, double n2) { return glm::dmat2(1.0, -(n2 - n1) * c, 0.0, 1.0); };
	const glm::dmat2 drift = glm::dmat2(1.0, 0.0, thickness / n, 1.0);
	const glm::dmat2 abcd = refraction(cBack, n, n0) * drift * refraction(cFront, n0, n);
	const LaserBeam beam = laserBeam(laser);
	const double zR = beam.x.rayleighRange();
	const cdouble qIn = cdouble(zFront, zR) / n0;
	const cdouble qOut = (abcd[0][0] * qIn + abcd[1][0]) / (abcd[0][1] * qIn + abcd[1][1]);
	const double waistAfterBack = -n0 * qOut.real();
	// the segment that leaves the back face
	uint32_t after = NO_BEAM_SEGMENT;
	for (uint32_t i = 0; i < tree.segments.size(); i++) {
		const BeamSegment& s = tree.segments[i];
		if (s.start.fromOptic == lens.id && s.start.port == 0 && s.start.slot == 1 && !s.start.ghost) {
			after = i;
			break;
		}
	}
	check(after != NO_BEAM_SEGMENT, "lens focus: no beam leaves the lens by its Transmitted port");
	if (after == NO_BEAM_SEGMENT) {
		return;
	}
	const BeamProfile profile = tree.profile(after);
	const double startZ = tree.segments[after].start.origin.z;
	const double expected = plant(zFront + thickness + waistAfterBack);
	check(close(startZ + profile.axes[0].waistAt, expected, 1e-6), fmt::format("lens focus: waist at z = {} against the ABCD law's {}", startZ + profile.axes[0].waistAt, expected));
	const double w0 = std::sqrt(632.8e-9 * qOut.imag() / std::numbers::pi * (std::numbers::pi * n0 * beam.x.waist * beam.x.waist / (632.8e-9 * zR)));
	check(close(profile.axes[0].waist, plant(w0), 1e-6), fmt::format("lens focus: waist {} against the ABCD law's {}", profile.axes[0].waist, w0));
	fmt::println("  lens focus: waist {:.4f} um at z = {:.6f} m", profile.axes[0].waist * 1e6, startZ + profile.axes[0].waistAt);
}

// a laser along +z, a turning mirror, an iris, a plano-convex lens, a plate beamsplitter and two power meters, with an
// axis along it
TestBench setupBench(uint64_t& mirror, uint64_t& iris, uint64_t& lens, uint64_t& splitter, uint64_t& meterA, uint64_t& meterB)
{
	TestBench bench;
	bench.add(OpticKind::Laser, glm::dmat4(1.0));
	// a mirror at z = 0.15 facing back at 45 degrees: the beam turns to +x
	mirror = bench.add(OpticKind::PlanarMirror, turned(at(0.0, 0.0, 0.15), 180.0 - 45.0)).id;
	iris = bench.add(OpticKind::Iris, turned(at(0.1, 0.0, 0.15), 90.0)).id;
	lens = bench.add(OpticKind::SphericalLens, turned(at(0.2, 0.0, 0.15), 90.0)).id;
	// a plate's body +z runs along the beam, a mirror's toward it: the splitter's front turns the beam to -z
	splitter = bench.add(OpticKind::PlateBeamsplitter, turned(at(0.3, 0.0, 0.15), 45.0)).id;
	meterA = bench.add(OpticKind::PowerMeter, turned(at(0.45, 0.0, 0.15), 90.0)).id;
	meterB = bench.add(OpticKind::PowerMeter, turned(at(0.3, 0.0, 0.0), 180.0)).id;
	BeamAxis axis = makeBeamAxis("Test");
	axis.id = 1000;
	axis.source = { BeamSourceKind::Laser, bench.optics.front().id };
	bench.axes.push_back(axis);
	return bench;
}

void testSolverBench()
{
	uint64_t mirror, iris, lens, splitter, meterA, meterB;
	TestBench bench = setupBench(mirror, iris, lens, splitter, meterA, meterB);
	BeamSolver solver;
	solver.solve(bench.input());

	// Follow beam from the source builds the whole axis
	{
		BeamAxis& axis = bench.axes.front();
		const std::vector<BeamFollowStep> steps = followBeam(solver.solution(), axis, BEAM_SOURCE_NODE, bench.optics);
		std::vector<uint32_t> made;
		for (const BeamFollowStep& step : steps) {
			const uint32_t parent = (step.parent & BEAM_FOLLOW_STEP) != 0 && step.parent != BEAM_SOURCE_NODE ? made[step.parent & ~BEAM_FOLLOW_STEP] : step.parent;
			made.push_back(*appendBeamNode(axis, parent, step.port, step.optic));
		}
		std::vector<uint64_t> visited;
		for (const BeamAxisNode& node : axis.nodes) {
			visited.push_back(node.optic);
		}
		std::sort(visited.begin(), visited.end());
		std::vector<uint64_t> expected { mirror, iris, lens, splitter, meterA, meterB };
		std::sort(expected.begin(), expected.end());
		if (g_plant) {
			expected.pop_back();
		}
		check(visited == expected, fmt::format("follow beam: visits {} against {}", visited, expected));
	}
	solver.solve(bench.input());
	const BeamSolution& solution = solver.solution();
	const BeamTree& tree = solution.trees.front();
	if (std::getenv("BEAM_DUMP")) {
		for (uint32_t i = 0; i < tree.segments.size(); i++) {
			const BeamSegment& s = tree.segments[i];
			fmt::println("  seg {} parent {} start ({:.4f},{:.4f},{:.4f}) dir ({:.3f},{:.3f},{:.3f}) len {:.4f} end {} optic {} surf {} P {:.3e} port {} from {} ghost {} inside {}", i, (int)s.parent,
				s.start.origin.x, s.start.origin.y, s.start.origin.z, s.start.direction.x, s.start.direction.y, s.start.direction.z, s.length, beamEndLabel(s.end), s.endOptic,
				s.endSurface, s.start.power, (int)s.start.port, s.start.fromOptic, s.start.ghost, s.start.inside);
		}
	}
	const BeamAxis& axis = bench.axes.front();
	const auto status = [&](uint64_t optic) {
		for (const BeamAxisNode& node : axis.nodes) {
			if (node.optic == optic) {
				return solution.report(axis.id, node.id)->status;
			}
		}
		return BeamNodeStatus::Unreached;
	};
	for (uint64_t optic : { mirror, iris, lens, splitter, meterA, meterB }) {
		check(status(optic) == (g_plant ? BeamNodeStatus::Clipped : BeamNodeStatus::Hit),
			fmt::format("statuses: {} is {}", bench.find(optic)->name, beamNodeStatusLabel(status(optic))));
	}
	// the power adds up
	const double total = tree.budget.total();
	check(close(total, plant(tree.power), 1e-12), fmt::format("power: the budget adds up to {} of the laser's {}", total, tree.power));
	// each meter reads the beam's power on it, the two together near the laser's less the coatings' losses
	double meters = 0.0;
	for (const BeamDetectorReading& r : tree.readings) {
		meters += r.power;
		if (tree.segments[r.segment].start.ghost) {
			continue;
		}
		const BeamCrossSection at = tree.crossSection(r.segment, tree.segments[r.segment].length);
		// on a sensor the beam meets square to within a degree: its cross section to cos^2 of the tilt
		check(close(std::max(r.radii.x, r.radii.y), plant(std::max(at.radii.x, at.radii.y)), 1e-4),
			fmt::format("reading: radius {} against the beam's {}", std::max(r.radii.x, r.radii.y), std::max(at.radii.x, at.radii.y)));
	}
	check(meters > 0.8 * tree.power && meters < tree.power, fmt::format("readings: the meters read {} of {}", meters, tree.power));
	fmt::println("  bench: {} segments, meters {:.4f} mW of {:.1f}, solved in {:.3f} ms", tree.segments.size(), meters * 1e3, tree.power * 1e3, solver.lastSolveMs());

	// closing the iris to its smallest opening clips the beam there, and what follows still reaches its visits
	{
		TestBench closed = bench;
		setOpticValue(*closed.find(iris), "opening", 0.0);
		BeamSolver s;
		s.solve(closed.input());
		const BeamAxis& a = closed.axes.front();
		const BeamNodeReport* report = nullptr;
		for (const BeamAxisNode& node : a.nodes) {
			if (node.optic == iris) {
				report = s.solution().report(a.id, node.id);
			}
		}
		// the beam's radius at the iris, and a Gaussian's transmission through a centered circle: 1 - exp(-2 a^2 / w^2)
		const BeamTree& t = s.solution().trees.front();
		const BeamCrossSection beamThere = t.crossSection(report->segment, t.segments[report->segment].length);
		const double hole = s.body(iris)->surfaces[0].aperture.a;
		const double transmission = 1.0 - std::exp(-2.0 * hole * hole / (beamThere.radii.x * beamThere.radii.y));
		check(report != nullptr && report->status == BeamNodeStatus::Clipped, "iris closed: not Clipped");
		check(report != nullptr && close(report->lostFraction, plant(1.0 - transmission), 1e-6),
			fmt::format("iris closed to {} mm: loses {} against {}", 2e3 * hole, report ? report->lostFraction : -1.0, 1.0 - transmission));
		for (const BeamAxisNode& node : a.nodes) {
			const BeamNodeReport* r = s.solution().report(a.id, node.id);
			if (node.optic == lens || node.optic == meterA) {
				check(r->status == BeamNodeStatus::Hit, fmt::format("iris closed: {} is {}", closed.find(node.optic)->name, beamNodeStatusLabel(r->status)));
			}
		}
	}

	// moving the lens off the beam by its radius and 2 mm: Missed by 2 mm, the meters unreached
	{
		TestBench moved = bench;
		SceneOptic& l = *moved.find(lens);
		const double radius = 0.5 * opticNumber(l, "diameter");
		l.world = turned(at(0.2, 0.0, 0.15 + radius + 2e-3), 90.0);
		BeamSolver s;
		s.solve(moved.input());
		const BeamAxis& a = moved.axes.front();
		for (const BeamAxisNode& node : a.nodes) {
			const BeamNodeReport* r = s.solution().report(a.id, node.id);
			if (node.optic == lens) {
				check(r->status == BeamNodeStatus::Missed, fmt::format("lens moved: {}", beamNodeStatusLabel(r->status)));
				check(close(r->missDistance, plant(2e-3), 1e-6), fmt::format("lens moved: missed by {} against 2 mm", r->missDistance));
				check(r->steer.z > 0.999, fmt::format("lens moved: steer ({}, {}, {}) not toward +z", r->steer.x, r->steer.y, r->steer.z));
			}
			if (node.optic == meterA || node.optic == meterB) {
				check(r->status == BeamNodeStatus::Unreached, fmt::format("lens moved: a meter is {}", beamNodeStatusLabel(r->status)));
			}
		}
	}

	// a second mirror in front of the iris: the iris is Blocked by it
	{
		TestBench blocked = bench;
		blocked.add(OpticKind::PlanarMirror, turned(at(0.05, 0.0, 0.15), -90.0));
		BeamSolver s;
		s.solve(blocked.input());
		const BeamAxis& a = blocked.axes.front();
		for (const BeamAxisNode& node : a.nodes) {
			if (node.optic == iris) {
				const BeamNodeReport* r = s.solution().report(a.id, node.id);
				check(r->status == (g_plant ? BeamNodeStatus::Missed : BeamNodeStatus::Blocked), fmt::format("mirror in the way: the iris is {}", beamNodeStatusLabel(r->status)));
			}
		}
	}
}

void testSelectiveUpdating(int edits)
{
	uint64_t mirror, iris, lens, splitter, meterA, meterB;
	TestBench bench = setupBench(mirror, iris, lens, splitter, meterA, meterB);
	BeamSolver selective;
	selective.solve(bench.input());
	{
		BeamAxis& axis = bench.axes.front();
		std::vector<uint32_t> made;
		for (const BeamFollowStep& step : followBeam(selective.solution(), axis, BEAM_SOURCE_NODE, bench.optics)) {
			const uint32_t parent = (step.parent & BEAM_FOLLOW_STEP) != 0 && step.parent != BEAM_SOURCE_NODE ? made[step.parent & ~BEAM_FOLLOW_STEP] : step.parent;
			made.push_back(*appendBeamNode(axis, parent, step.port, step.optic));
		}
	}
	std::uniform_int_distribution<size_t> pick(1, bench.optics.size() - 1);
	std::normal_distribution<double> nudge(0.0, 1.0);
	size_t mismatches = 0;
	size_t partial = 0;
	for (int e = 0; e < edits; e++) {
		SceneOptic& o = bench.optics[pick(g_rng)];
		// a small move or turn, or an iris's opening
		if (o.kind == OpticKind::Iris && e % 3 == 0) {
			setOpticValue(o, "opening", std::clamp(opticNumber(o, "opening") + 1e-3 * nudge(g_rng), 0.5e-3, 10e-3));
		} else if (e % 2 == 0) {
			o.world = glm::translate(glm::dmat4(1.0), 2e-3 * glm::dvec3(nudge(g_rng), nudge(g_rng), nudge(g_rng))) * o.world;
		} else {
			o.world = o.world * glm::rotate(glm::dmat4(1.0), 0.01 * nudge(g_rng), glm::normalize(glm::dvec3(nudge(g_rng), nudge(g_rng), nudge(g_rng))));
		}
		selective.solve(bench.input());
		BeamSolver full;
		full.solve(bench.input());
		const std::vector<BeamTree>& a = selective.solution().trees;
		const std::vector<BeamTree>& b = full.solution().trees;
		bool same = a.size() == b.size() && selective.solution().reports == full.solution().reports;
		for (size_t t = 0; same && t < a.size(); t++) {
			same = a[t].segments == b[t].segments && a[t].budget == b[t].budget && a[t].readings == b[t].readings;
		}
		if (g_plant && e == 0) {
			same = false;
		}
		mismatches += same ? 0 : 1;
		partial += selective.lastTraced() < full.lastTraced() ? 1 : 0;
	}
	check(mismatches == 0, fmt::format("selective updating: {} of {} edits differ from a full solve", mismatches, edits));
	check(partial > 0 || g_plant, "selective updating: no edit re-traced less than the whole tree");
	fmt::println("  selective updating: {} edits, {} re-traced only part of the tree", edits, partial);
}

//---------------------------------------------------------------- the preview's sections

void testSections()
{
	const double lambda = 632.8e-9;
	// stigmatic, waist 0.1 m before the segment: one cone
	{
		const double zR = 0.05;
		const ComplexRays rays = simpleAstigmaticBeam(glm::dvec2(-0.1), glm::dvec2(zR), 1.0, glm::dvec2(1.0));
		const std::vector<BeamRing> rings = beamRings(rays, 1.0, lambda, 0.2, BeamWaistRegion::RayleighRange);
		const BeamAxisProfile p = beamProfile(rays, 1.0, lambda).axes[0];
		check(rings.size() == (g_plant ? 3u : 2u), fmt::format("sections: no waist gives {} rings, not 2", rings.size()));
		if (rings.size() == 2) {
			check(close(rings[0].radii.x, p.radius(0.0), 1e-12) && close(rings[1].radii.x, plant(p.radius(0.2)), 1e-12), "sections: a cone's ends are not w(z1) and w(z2)");
		}
	}
	// stigmatic, waist at 0.1 m inside a 0.3 m segment, zR 0.04: cone, cylinder 0.08 to 0.12, cone
	{
		const double zR = 0.04;
		const ComplexRays rays = simpleAstigmaticBeam(glm::dvec2(0.1), glm::dvec2(zR), 1.0, glm::dvec2(1.0));
		const BeamAxisProfile p = beamProfile(rays, 1.0, lambda).axes[0];
		const std::vector<BeamRing> rings = beamRings(rays, 1.0, lambda, 0.3, BeamWaistRegion::RayleighRange);
		check(rings.size() == 4, fmt::format("sections: a waist gives {} rings, not 4", rings.size()));
		if (rings.size() == 4) {
			check(close(rings[1].s, plant(0.08), 1e-12) && close(rings[2].s, 0.12, 1e-12), fmt::format("sections: cylinder from {} to {}", rings[1].s, rings[2].s));
			check(close(rings[1].radii.x, p.waist, 1e-12) && close(rings[2].radii.x, p.waist, 1e-12), "sections: the cylinder is not at w0");
		}
		// clamped: the waist at 0.01 m, so the cylinder starts at the segment's start and the first cone goes
		const ComplexRays early = simpleAstigmaticBeam(glm::dvec2(0.01), glm::dvec2(zR), 1.0, glm::dvec2(1.0));
		const std::vector<BeamRing> clamped = beamRings(early, 1.0, lambda, 0.3, BeamWaistRegion::RayleighRange);
		check(clamped.size() == (g_plant ? 4u : 3u) && close(clamped[1].s, 0.03, 1e-12), fmt::format("sections: a cylinder at the start gives {} rings", clamped.size()));
	}
	// astigmatic: waists at 0.1 and 0.2 m: at most six rings, each axis on its own profile
	{
		const glm::dvec2 zR(0.03, 0.05);
		const ComplexRays rays = simpleAstigmaticBeam(glm::dvec2(0.1, 0.2), zR, 1.0, glm::dvec2(1.0));
		const std::vector<BeamRing> rings = beamRings(rays, 1.0, lambda, 0.4, BeamWaistRegion::RayleighRange);
		check(rings.size() <= 6 && rings.size() >= 5, fmt::format("sections: astigmatic gives {} rings", rings.size()));
		const BeamProfile profile = beamProfile(rays, 1.0, lambda);
		for (const BeamRing& ring : rings) {
			for (int i = 0; i < 2; i++) {
				const BeamAxisProfile& p = profile.axes[i];
				const double lo = std::max(0.0, p.waistAt - 0.5 * p.rayleighRange);
				const double hi = std::min(0.4, p.waistAt + 0.5 * p.rayleighRange);
				double expected;
				if (ring.s >= lo && ring.s <= hi) {
					expected = p.waist;
				} else if (ring.s < lo) {
					expected = p.radius(0.0) + (p.waist - p.radius(0.0)) * ring.s / lo;
				} else {
					expected = p.waist + (p.radius(0.4) - p.waist) * (ring.s - hi) / (0.4 - hi);
				}
				check(close(ring.radii[i], plant(expected), 1e-12), fmt::format("sections: astigmatic axis {} at {} is {}, its profile {}", i, ring.s, ring.radii[i], expected));
			}
		}
	}
}


// a lens's one distance label, for every "origin at" and both ways round: on its own surface for a vertex, on its plane
// otherwise, at the path along the beam from the source
void testDistanceMarks()
{
	uint64_t mirror, iris, lens, splitter, meterA, meterB;
	TestBench bench = setupBench(mirror, iris, lens, splitter, meterA, meterB);
	BeamSolver solver;
	for (const double degrees : { 90.0, 270.0 }) {
		bench.find(lens)->world = turned(at(0.2, 0.0, 0.15), degrees);
		for (int originAt = 0; originAt <= 4; originAt++) {
			setOpticValue(*bench.find(lens), "originAt", originAt);
			solver.solve(bench.input());
			const BeamTree& tree = solver.solution().trees.front();
			const std::vector<BeamDistanceMark> marks = beamDistanceMarks(tree, solver);
			size_t arrivals = 0, lensMark = marks.size();
			const BeamSegment* arrival = nullptr;
			for (const BeamSegment& seg : tree.segments) {
				if (seg.start.ghost || seg.length <= 0.0 || seg.start.inside != 0 || seg.end == BeamEnd::Escaped) {
					continue;
				}
				if (seg.endOptic == lens && arrival == nullptr) {
					arrival = &seg;
					lensMark = arrivals;
				}
				arrivals++;
			}
			const std::string where = fmt::format("distances: origin {} at {} degrees", originAt, degrees);
			check(marks.size() == arrivals && arrival != nullptr, fmt::format("{}: {} marks for {} arrivals", where, marks.size(), arrivals));
			if (marks.size() != arrivals || arrival == nullptr) {
				continue;
			}
			const OpticPrescription& body = *solver.body(lens);
			const double z = originAt == 0 ? 0.0 : originAt == 4 ? body.surfaces[1].placement.origin.z : body.originZ;
			const glm::dvec3 expected = glm::dvec3(bench.find(lens)->world * glm::dvec4(0.0, 0.0, z, 1.0));
			const glm::dvec3 got = marks[lensMark].point + (g_plant ? glm::dvec3(1e-3, 0.0, 0.0) : glm::dvec3(0.0));
			check(glm::length(got - expected) < 1e-9, fmt::format("{}: the mark is {:.3e} m off", where, glm::length(got - expected)));
			const double path = arrival->start.distance + expected.x - arrival->start.origin.x;
			check(close(marks[lensMark].distance, path, 1e-9), fmt::format("{}: the mark reads {}, the path is {}", where, marks[lensMark].distance, path));
		}
	}
}

// a cube's one distance label is on its diagonal, whichever face the beam enters by
void testCubeDistanceMark()
{
	for (const double degrees : { 0.0, 180.0 }) {
		TestBench bench;
		bench.add(OpticKind::Laser, glm::dmat4(1.0));
		const uint64_t cube = bench.add(OpticKind::CubeBeamsplitter, turned(at(0.0, 0.0, 0.2), degrees)).id;
		BeamSolver solver;
		solver.solve(bench.input());
		const BeamTree& tree = solver.solution().trees.front();
		const std::vector<BeamDistanceMark> marks = beamDistanceMarks(tree, solver);
		const std::string where = fmt::format("distances: cube at {} degrees", degrees);
		check(!marks.empty(), fmt::format("{}: no marks", where));
		if (marks.empty()) {
			continue;
		}
		const BeamSegment& arrival = tree.segments.front();
		check(arrival.endOptic == cube && arrival.endSurface != CUBE_DIAGONAL_SURFACE, fmt::format("{}: the beam does not arrive on a face", where));
		const glm::dvec3 center(0.0, 0.0, 0.2);
		const glm::dvec3 got = marks.front().point + (g_plant ? glm::dvec3(0.0, 0.0, 1e-3) : glm::dvec3(0.0));
		check(glm::length(got - center) < 1e-9, fmt::format("{}: the mark is {:.3e} m from the diagonal's center", where, glm::length(got - center)));
		check(close(marks.front().distance, arrival.start.distance + center.z - arrival.start.origin.z, 1e-9), fmt::format("{}: the mark reads {}", where, marks.front().distance));
	}
}

// an axis with no threshold of its own follows the scene's, and one with its own keeps it
void testThresholdFollowsScene()
{
	uint64_t mirror, iris, lens, splitter, meterA, meterB;
	TestBench bench = setupBench(mirror, iris, lens, splitter, meterA, meterB);
	const auto solved = [&](float scene, std::optional<float> own) {
		bench.axes.front().ghostThreshold = own;
		BeamSolverInput input = bench.input();
		input.ghostThreshold = scene;
		BeamSolver solver;
		solver.solve(input);
		return solver.solution().trees.front().threshold;
	};
	check(close(solved(1e-7f, std::nullopt), 1e-7f, 1e-12), "ghost threshold: an axis does not follow the scene's");
	check(close(solved(1e-7f, 1e-2f), g_plant ? 1e-7f : 1e-2f, 1e-12), "ghost threshold: an axis's own is not used");
	bench.axes.clear();
	BeamSolverInput input = bench.input();
	input.ghostThreshold = 1e-6f;
	BeamSolver solver;
	solver.solve(input);
	check(close(solver.solution().trees.front().threshold, 1e-6f, 1e-12), "ghost threshold: a laser with no axis does not use the scene's");
}

// the plan's 30-element setup: a laser, 28 turning mirrors in a zigzag and a meter. A full solve, and a drag of one
// mirror's tilt re-solved selectively, against the 2 ms a frame of dragging may spend
void testTiming()
{
	TestBench bench;
	bench.add(OpticKind::Laser, glm::dmat4(1.0));
	// mirrors alternating between x = 0 and x = 0.08, 0.05 m apart along z, each turning the beam across to the next
	const int mirrors = 28;
	std::vector<uint64_t> ids;
	for (int i = 0; i < mirrors; i++) {
		const double x = (i % 2 == 0) ? 0.0 : 0.08;
		const double z = 0.1 + 0.05 * i;
		// the incoming and outgoing directions, and the normal between them facing back along the incoming beam
		const glm::dvec3 previous = i == 0 ? glm::dvec3(0.0, 0.0, 0.0) : glm::dvec3((i % 2 == 0) ? 0.08 : 0.0, 0.0, 0.1 + 0.05 * (i - 1));
		const glm::dvec3 here(x, 0.0, z);
		const glm::dvec3 next((i % 2 == 0) ? 0.08 : 0.0, 0.0, z + 0.05);
		const glm::dvec3 in = glm::normalize(here - previous);
		const glm::dvec3 out = glm::normalize(next - here);
		const glm::dvec3 normal = glm::normalize(out - in);
		// a body whose +z is the normal
		const glm::dvec3 xAxis = glm::normalize(glm::cross(glm::dvec3(0.0, 1.0, 0.0), normal));
		glm::dmat4 world(glm::dvec4(xAxis, 0.0), glm::dvec4(glm::cross(normal, xAxis), 0.0), glm::dvec4(normal, 0.0), glm::dvec4(here, 1.0));
		SceneOptic& m = bench.add(OpticKind::PlanarMirror, world);
		setOpticSize(m, 0);
		ids.push_back(m.id);
	}
	const glm::dvec3 last((mirrors % 2 == 0) ? 0.0 : 0.08, 0.0, 0.1 + 0.05 * mirrors);
	bench.add(OpticKind::PowerMeter, turned(at(last.x, last.y, last.z), (mirrors % 2 == 0) ? -58.0 : 58.0));
	BeamSolver solver;
	solver.solve(bench.input());
	const double full = solver.lastSolveMs();
	const size_t segments = solver.solution().trees.front().segments.size();
	// the beam reaches the last mirror
	bool reached = false;
	for (const BeamSegment& seg : solver.solution().trees.front().segments) {
		reached |= seg.endOptic == ids.back() && !seg.start.ghost;
	}
	check(g_plant ? !reached : reached, fmt::format("timing setup: the beam {} the last of {} mirrors", reached ? "reaches" : "does not reach", mirrors));
	// dragging the 20th mirror's tilt: 30 frames, each re-solved
	double worst = 0.0;
	double sum = 0.0;
	for (int frame = 0; frame < 30; frame++) {
		SceneOptic& m = *bench.find(ids[19]);
		m.world = m.world * glm::rotate(glm::dmat4(1.0), 2e-4, glm::dvec3(0.0, 1.0, 0.0));
		solver.solve(bench.input());
		worst = std::max(worst, solver.lastSolveMs());
		sum += solver.lastSolveMs();
	}
	check(worst < (g_plant ? 1e-6 : 2.0), fmt::format("timing: a drag's slowest solve took {:.3f} ms", worst));
	fmt::println("  timing: {} segments; a full solve {:.3f} ms, a drag of the 20th mirror {:.3f} ms on average, {:.3f} ms at worst", segments, full, sum / 30.0, worst);
}


// the preview on a solved bench: ghosts drawn only when asked, and a tube's end ring lying on the mirror it meets
void testPreviewOnBench()
{
	uint64_t mirror, iris, lens, splitter, meterA, meterB;
	TestBench bench = setupBench(mirror, iris, lens, splitter, meterA, meterB);
	BeamSolver solver;
	solver.solve(bench.input());
	const BeamTree& tree = solver.solution().trees.front();
	BeamPreviewSettings settings;
	settings.brightnessDecades = 10.f;
	settings.ghosts = false;
	const std::vector<BeamSection> main = buildBeamSections(solver.solution(), settings);
	settings.ghosts = true;
	const std::vector<BeamSection> all = buildBeamSections(solver.solution(), settings);
	size_t ghosts = 0;
	for (const BeamSegment& seg : tree.segments) {
		ghosts += seg.start.ghost ? 1 : 0;
	}
	check(ghosts > 0 && (g_plant ? main.size() == all.size() : main.size() < all.size()), fmt::format("preview: {} sections without ghosts, {} with ({} ghost segments)", main.size(), all.size(), ghosts));
	// a lens's internal reflection is a ghost before it leaves the lens
	bool internalGhost = false;
	for (const BeamSegment& seg : tree.segments) {
		internalGhost |= seg.start.inside == lens && seg.start.ghost;
	}
	check(g_plant ? !internalGhost : internalGhost, "preview: a reflection inside the lens is not a ghost until it leaves");
	// the first segment ends on the 45-degree mirror: its last ring's points lie on the mirror's plane
	settings.ghosts = false;
	const BeamSegment& first = tree.segments[0];
	const std::vector<BeamSection> sections = buildBeamSections(solver.solution(), settings);
	const BeamSection& last = sections[beamRings(first.start.rays, first.start.index, tree.wavelength, first.length, settings.waistRegion).size() - 2];
	double off = 0.0;
	for (int k = 0; k < 8; k++) {
		const double angle = 2.0 * std::numbers::pi * k / 8.0;
		const glm::dvec3 p = glm::dvec3(last.center1) + std::cos(angle) * glm::dvec3(last.axisA1) + std::sin(angle) * glm::dvec3(last.axisB1);
		off = std::max(off, std::abs(glm::dot(first.endNormal, p - first.endPoint())));
	}
	check(off < (g_plant ? -1.0 : 1e-6), fmt::format("preview: the ring at the mirror lies {:.2e} m off its face", off));

	// after the tubes, a disk at the laser's output and one on each surface the beam lands on, none at the iris's opening
	const auto flagsOf = [](const BeamSection& section) { return std::bit_cast<uint32_t>(section.axisA0.w); };
	size_t expectedDisks = 1;
	for (const BeamSegment& seg : tree.segments) {
		const bool lands = seg.end == BeamEnd::Surface || seg.end == BeamEnd::Detector || seg.end == BeamEnd::Absorbed || seg.end == BeamEnd::Scattered || seg.end == BeamEnd::Laser;
		expectedDisks += !seg.start.ghost && seg.length > 0.0 && lands && std::abs(glm::dot(seg.endNormal, seg.start.direction)) > 0.05 ? 1 : 0;
	}
	size_t disks = 0;
	bool disksLast = true;
	double nearestToOpening = 1e30;
	for (const BeamSection& section : sections) {
		const bool disk = (flagsOf(section) & BEAM_SECTION_SPOT) != 0u;
		disks += disk ? 1 : 0;
		disksLast &= disk || disks == 0;
		for (const BeamSegment& seg : tree.segments) {
			if (disk && seg.end == BeamEnd::Pass && !seg.start.ghost) {
				nearestToOpening = std::min(nearestToOpening, glm::distance(glm::dvec3(section.center0), seg.endPoint()));
			}
		}
	}
	check(disksLast && (g_plant ? disks != expectedDisks : disks == expectedDisks), fmt::format("preview: {} disks, {} expected, after the tubes", disks, expectedDisks));
	check(nearestToOpening > (g_plant ? 1e30 : 1e-6), fmt::format("preview: the nearest disk to an opening the beam passes is {:.2e} m from it", nearestToOpening));

	// the glow stops at a mirror on both legs, and goes on through a lens's face
	std::vector<size_t> firstSection(tree.segments.size(), 0);
	size_t next = 0;
	for (uint32_t i = 0; i < tree.segments.size(); i++) {
		const BeamSegment& seg = tree.segments[i];
		firstSection[i] = next;
		if (!seg.start.ghost && seg.length > 0.0) {
			next += beamRings(seg.start.rays, seg.start.index, tree.wavelength, seg.length, settings.waistRegion).size() - 1;
		}
	}
	const uint32_t mirrorHit = 0;
	bool reflectedStops = false;
	bool lensCarriesOn = false;
	for (uint32_t i = 0; i < tree.segments.size(); i++) {
		const BeamSegment& seg = tree.segments[i];
		if (seg.start.ghost || seg.length <= 0.0) {
			continue;
		}
		if (seg.parent == mirrorHit) {
			reflectedStops = (flagsOf(sections[firstSection[i]]) & BEAM_SECTION_STOP_AT_START) != 0u;
		}
		if (seg.end == BeamEnd::Surface && seg.endOptic == lens && seg.start.inside == 0) {
			const size_t rings = beamRings(seg.start.rays, seg.start.index, tree.wavelength, seg.length, settings.waistRegion).size();
			lensCarriesOn = (flagsOf(sections[firstSection[i] + rings - 2]) & BEAM_SECTION_STOP_AT_END) == 0u;
		}
	}
	const bool mirrorStops = (flagsOf(last) & BEAM_SECTION_STOP_AT_END) != 0u;
	check(g_plant ? !(mirrorStops && reflectedStops) : mirrorStops && reflectedStops, "preview: the glow stops at a mirror on the beam's way in and out");
	check(g_plant ? !lensCarriesOn : lensCarriesOn, "preview: the glow goes on through a lens's face");
}

} // namespace

int main(int argc, char** argv)
{
	bool selfcheck = false;
	std::string root = ".";
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--selfcheck") == 0) {
			selfcheck = true;
		} else if (std::strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
			root = argv[++i];
		} else {
			fmt::println("usage: beam_test [--selfcheck] [--root dir]");
			return 2;
		}
	}
	std::vector<std::string> warnings;
	g_library.loadBundled(root, warnings);
	for (const std::string& warning : warnings) {
		fmt::println("  warning: {}", warning);
	}

	auto runAll = [&]() {
		testPorts();
		testAppendAndOrder();
		testInsert();
		testRemove();
		testMove();
		testRenumber();
		testRandomEdits(20000);
		testFreeSpace();
		testSurfaceMatrices();
		testMirrorFoci();
		testLensFocus();
		testSolverBench();
		testSelectiveUpdating(1000);
		testSections();
		testPreviewOnBench();
		testDistanceMarks();
		testCubeDistanceMark();
		testThresholdFollowsScene();
		testTiming();
	};
	runAll();
	const int failures = g_failures;
	if (selfcheck) {
		// every check above plants an error under g_plant; each must fail
		g_failures = 0;
		g_plant = true;
		fmt::println("selfcheck: planting errors");
		runAll();
		const int caught = g_failures;
		g_plant = false;
		g_failures = failures;
		if (caught < 20) {
			fmt::println("beam_test: --selfcheck FAILED, only {} planted errors were caught", caught);
			return 1;
		}
		fmt::println("selfcheck: {} planted errors caught", caught);
	}
	if (g_failures == 0) {
		fmt::println("beam_test: all checks passed");
		return 0;
	}
	fmt::println("beam_test: {} check(s) FAILED", g_failures);
	return 1;
}
