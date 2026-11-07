#include <beam_axis.h>

#include <algorithm>
#include <utility>

namespace {

uint32_t nextNodeId(const BeamAxis& axis)
{
	uint32_t next = 0;
	for (const BeamAxisNode& node : axis.nodes) {
		next = std::max(next, node.id + 1);
	}
	return next;
}

// the place a node hangs in: its parent and port, NO_PORT under the source and when dangling
OpticPortId placePort(uint32_t parent, OpticPortId port)
{
	return parent == BEAM_SOURCE_NODE || parent == BEAM_DANGLING_NODE ? NO_PORT : port;
}

// whether `parent` names the source, a dangling root's sentinel or a node of the axis
bool parentExists(const BeamAxis& axis, uint32_t parent)
{
	return parent == BEAM_SOURCE_NODE || parent == BEAM_DANGLING_NODE || findBeamNode(axis, parent) != nullptr;
}

// whether a node may hang off `port` of `parent` without displacing another than `except`; any number dangle
bool placeFree(const BeamAxis& axis, uint32_t parent, OpticPortId port, uint32_t except)
{
	if (parent == BEAM_DANGLING_NODE) {
		return true;
	}
	const BeamAxisNode* occupant = beamNodeAt(axis, parent, port);
	return occupant == nullptr || occupant->id == except;
}

void appendSubtree(const BeamAxis& axis, uint32_t node, std::vector<uint32_t>& order)
{
	order.push_back(node);
	for (uint32_t child : beamNodeChildren(axis, node)) {
		appendSubtree(axis, child, order);
	}
}

void eraseNodes(BeamAxis& axis, const std::vector<uint32_t>& ids)
{
	std::erase_if(axis.nodes, [&](const BeamAxisNode& node) { return std::find(ids.begin(), ids.end(), node.id) != ids.end(); });
}

} // namespace

//---------------------------------------------------------------- ports

std::string_view opticPortName(OpticKind kind, OpticPortId port)
{
	const std::span<const OpticPort> ports = opticTraits(kind).ports;
	return port < ports.size() ? std::string_view(ports[port].name) : std::string_view();
}

std::optional<OpticPortId> findOpticPort(OpticKind kind, std::string_view name)
{
	const std::span<const OpticPort> ports = opticTraits(kind).ports;
	for (size_t i = 0; i < ports.size(); ++i) {
		if (name == ports[i].name) {
			return (OpticPortId)i;
		}
	}
	return std::nullopt;
}

uint32_t opticPortCount(OpticKind kind)
{
	return (uint32_t)opticTraits(kind).ports.size();
}

//---------------------------------------------------------------- the authored graph

BeamAxis makeBeamAxis(std::string name)
{
	BeamAxis axis;
	axis.name = std::move(name);
	return axis;
}

//---------------------------------------------------------------- reading the tree

const BeamAxisNode* findBeamNode(const BeamAxis& axis, uint32_t node)
{
	for (const BeamAxisNode& candidate : axis.nodes) {
		if (candidate.id == node) {
			return &candidate;
		}
	}
	return nullptr;
}

BeamAxisNode* findBeamNode(BeamAxis& axis, uint32_t node)
{
	return const_cast<BeamAxisNode*>(findBeamNode(std::as_const(axis), node));
}

const BeamAxisNode* beamNodeAt(const BeamAxis& axis, uint32_t parent, OpticPortId port)
{
	if (parent == BEAM_DANGLING_NODE) {
		return nullptr;
	}
	port = placePort(parent, port);
	for (const BeamAxisNode& node : axis.nodes) {
		if (node.parent == parent && node.port == port) {
			return &node;
		}
	}
	return nullptr;
}

std::vector<uint32_t> beamNodeChildren(const BeamAxis& axis, uint32_t node)
{
	std::vector<const BeamAxisNode*> children;
	for (const BeamAxisNode& candidate : axis.nodes) {
		if (candidate.parent == node) {
			children.push_back(&candidate);
		}
	}
	// by port, and the dangling roots in the order they were cut loose
	std::stable_sort(children.begin(), children.end(), [](const BeamAxisNode* a, const BeamAxisNode* b) { return a->port < b->port; });
	std::vector<uint32_t> ids;
	ids.reserve(children.size());
	for (const BeamAxisNode* child : children) {
		ids.push_back(child->id);
	}
	return ids;
}

std::vector<uint32_t> beamAxisOrder(const BeamAxis& axis)
{
	std::vector<uint32_t> order;
	for (uint32_t root : beamNodeChildren(axis, BEAM_SOURCE_NODE)) {
		appendSubtree(axis, root, order);
	}
	return order;
}

std::vector<uint32_t> beamDanglingOrder(const BeamAxis& axis)
{
	std::vector<uint32_t> order;
	for (uint32_t root : beamNodeChildren(axis, BEAM_DANGLING_NODE)) {
		appendSubtree(axis, root, order);
	}
	return order;
}

bool beamNodeBelow(const BeamAxis& axis, uint32_t node, uint32_t ancestor)
{
	// a tree's walk up ends within as many steps as it has nodes; a longer one is a cycle, which no edit makes
	for (size_t step = 0; step <= axis.nodes.size(); ++step) {
		if (node == ancestor) {
			return true;
		}
		const BeamAxisNode* current = findBeamNode(axis, node);
		if (current == nullptr) {
			return false;
		}
		node = current->parent;
	}
	return false;
}

bool beamNodeDangling(const BeamAxis& axis, uint32_t node)
{
	for (size_t step = 0; step <= axis.nodes.size(); ++step) {
		if (node == BEAM_DANGLING_NODE) {
			return true;
		}
		const BeamAxisNode* current = findBeamNode(axis, node);
		if (current == nullptr) {
			return false;
		}
		node = current->parent;
	}
	return false;
}

//---------------------------------------------------------------- editing the tree

std::optional<uint32_t> appendBeamNode(BeamAxis& axis, uint32_t parent, OpticPortId port, uint64_t optic)
{
	port = placePort(parent, port);
	if (!parentExists(axis, parent) || !placeFree(axis, parent, port, BEAM_SOURCE_NODE)) {
		return std::nullopt;
	}
	const uint32_t id = nextNodeId(axis);
	axis.nodes.push_back({ .id = id, .optic = optic, .parent = parent, .port = port });
	return id;
}

std::optional<uint32_t> insertBeamNodeBefore(BeamAxis& axis, uint32_t node, uint64_t optic, OpticPortId throughPort)
{
	BeamAxisNode* existing = findBeamNode(axis, node);
	if (existing == nullptr) {
		return std::nullopt;
	}
	const uint32_t id = nextNodeId(axis);
	const BeamAxisNode inserted { .id = id, .optic = optic, .parent = existing->parent, .port = existing->port };
	existing->parent = id;
	existing->port = throughPort;
	axis.nodes.push_back(inserted);
	return id;
}

std::optional<uint32_t> insertBeamNodeAfter(BeamAxis& axis, uint32_t node, OpticPortId port, uint64_t optic, OpticPortId throughPort)
{
	if (node == BEAM_DANGLING_NODE || !parentExists(axis, node)) {
		return std::nullopt;
	}
	port = placePort(node, port);
	const uint32_t id = nextNodeId(axis);
	if (const BeamAxisNode* occupant = beamNodeAt(axis, node, port)) {
		BeamAxisNode* moved = findBeamNode(axis, occupant->id);
		moved->parent = id;
		moved->port = throughPort;
	}
	axis.nodes.push_back({ .id = id, .optic = optic, .parent = node, .port = port });
	return id;
}

void removeBeamNode(BeamAxis& axis, uint32_t node)
{
	const BeamAxisNode* removed = findBeamNode(axis, node);
	if (removed == nullptr) {
		return;
	}
	const uint32_t parent = removed->parent;
	const OpticPortId port = removed->port;
	// a dangling node's place holds any number, so every child dangles in it; any other place holds one
	bool placeTaken = parent == BEAM_DANGLING_NODE;
	for (uint32_t childId : beamNodeChildren(axis, node)) {
		BeamAxisNode* child = findBeamNode(axis, childId);
		child->review = true;
		if (!placeTaken) {
			child->parent = parent;
			child->port = port;
			placeTaken = true;
		} else {
			child->parent = BEAM_DANGLING_NODE;
			child->port = NO_PORT;
		}
	}
	eraseNodes(axis, { node });
}

void removeBeamSubtree(BeamAxis& axis, uint32_t node)
{
	if (findBeamNode(axis, node) == nullptr) {
		return;
	}
	std::vector<uint32_t> subtree;
	appendSubtree(axis, node, subtree);
	eraseNodes(axis, subtree);
}

void removeBeamOptic(BeamAxis& axis, uint64_t optic)
{
	for (;;) {
		const auto visit = std::find_if(axis.nodes.begin(), axis.nodes.end(), [&](const BeamAxisNode& node) { return node.optic == optic; });
		if (visit == axis.nodes.end()) {
			return;
		}
		removeBeamNode(axis, visit->id);
	}
}

void renumberBeamNodes(BeamAxis& axis)
{
	std::vector<std::pair<uint32_t, uint32_t>> places;
	places.reserve(axis.nodes.size());
	for (size_t i = 0; i < axis.nodes.size(); ++i) {
		places.emplace_back(axis.nodes[i].id, (uint32_t)i);
	}
	const auto placeOf = [&](uint32_t id) {
		for (const auto& [old, place] : places) {
			if (old == id) {
				return place;
			}
		}
		return BEAM_DANGLING_NODE;
	};
	for (BeamAxisNode& node : axis.nodes) {
		node.id = placeOf(node.id);
		if (node.parent != BEAM_SOURCE_NODE && node.parent != BEAM_DANGLING_NODE) {
			node.parent = placeOf(node.parent);
		}
	}
}

bool moveBeamNode(BeamAxis& axis, uint32_t node, uint32_t parent, OpticPortId port)
{
	BeamAxisNode* moved = findBeamNode(axis, node);
	port = placePort(parent, port);
	if (moved == nullptr || !parentExists(axis, parent) || beamNodeBelow(axis, parent, node) || !placeFree(axis, parent, port, node)) {
		return false;
	}
	moved->parent = parent;
	moved->port = port;
	return true;
}

//---------------------------------------------------------------- the traced beam against the graph

const char* beamNodeStatusLabel(BeamNodeStatus status)
{
	switch (status) {
	case BeamNodeStatus::Hit:
		return "Hit";
	case BeamNodeStatus::Clipped:
		return "Clipped";
	case BeamNodeStatus::Missed:
		return "Missed";
	case BeamNodeStatus::Blocked:
		return "Blocked";
	case BeamNodeStatus::BelowThreshold:
		return "Below threshold";
	case BeamNodeStatus::Unreached:
		return "Unreached";
	}
	return "Unreached";
}
