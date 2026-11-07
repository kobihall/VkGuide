// CPU verification of the beams (rt_optics; docs/plans/physoptics-beam-axis.md, and later the Gaussian beam solver and
// the beam preview). No Vulkan, no GPU.
//
//  - ports: every kind's port names back to their ids, the source's single output, a probe plane passing the beam on
//  - the authored tree (src/beam_axis.h): appending, a splitter's children by port, inserting before and after, every
//    removal (a chain closes up, a splitter's lowest port takes its place and the rest dangle, a dangling root's
//    children dangle), every visit of an optic visited twice, moving and its refusals, renumbering, and 20000 random
//    edits that must leave a tree: unique ids, every parent there, one child per place, no cycle, every node listed once
//
// The scene file's round trip of the axes, through the editor's ids, is tools/verify/beam_axis_check.py.
//
// Run from the repository root:  ./bin/beam_test [--selfcheck]

#include <beam_axis.h>
#include <optic_catalog.h>

#include <algorithm>
#include <cstring>
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

} // namespace

int main(int argc, char** argv)
{
	bool selfcheck = false;
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--selfcheck") == 0) {
			selfcheck = true;
		} else {
			fmt::println("usage: beam_test [--selfcheck]");
			return 2;
		}
	}

	auto runAll = [&]() {
		testPorts();
		testAppendAndOrder();
		testInsert();
		testRemove();
		testMove();
		testRenumber();
		testRandomEdits(20000);
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
