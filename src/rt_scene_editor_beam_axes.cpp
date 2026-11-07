// The beam axes' UI (docs/plans/physoptics-beam-axis.md), part of RaytraceSceneEditor: the Beam axes folder of the
// Scene panel, + Add -> Beam axis, an axis's panel, and the authored order drawn over the Viewport. Every change goes
// through editBeamAxis(), so it moves the axis's revision and never the render's.

#include <rt_scene_editor.h>

#include <optic_laser.h>
#include <viewport_view.h>
#include <vk_engine.h>

#include <imgui.h>

#include <fmt/format.h>

namespace {

constexpr ImVec4 REVIEW_COLOR { 1.f, 0.7f, 0.2f, 1.f };

// what an axis can visit: an optic of the file's own, not a laser (a source, not a visit) and not a stack's member,
// which the file cannot name
bool visitable(const SceneOptic& optic)
{
	return optic.stack == 0 && optic.kind != OpticKind::Laser;
}

// how many visits lie between a node and the source (0 for the first visit) or its dangling root
int beamNodeDepth(const BeamAxis& axis, uint32_t node)
{
	int depth = 0;
	for (const BeamAxisNode* current = findBeamNode(axis, node); current != nullptr && depth <= (int)axis.nodes.size(); depth++) {
		current = findBeamNode(axis, current->parent);
		if (current == nullptr) {
			break;
		}
	}
	return depth;
}

} // namespace

//---------------------------------------------------------------- the folder

void RaytraceSceneEditor::drawBeamAxisRows(VulkanEngine*)
{
	if (!ImGui::TreeNodeEx("Beam axes", ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}
	if (m_beamAxes.empty()) {
		ImGui::TextDisabled("none - Add > Beam axis");
	}
	bool alive = true;
	for (size_t a = 0; a < m_beamAxes.size() && alive; a++) {
		// the axis's own row, which renames, duplicates and deletes it, beside the arrow that opens its visits
		const uint64_t id = m_beamAxes[a].id;
		const std::string name = m_beamAxes[a].name;
		ImGui::PushID((int)id);
		const bool open = ImGui::TreeNodeEx("##visits", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowOverlap);
		ImGui::SameLine();
		alive = drawObjectRow(SelectionKind::BeamAxis, id, name);
		if (open) {
			if (alive) {
				// a deleting row invalidates the list, and a menu that changed the tree its walk: either stops here
				const BeamAxis& axis = *findBeamAxis(id);
				const std::vector<uint32_t> order = beamAxisOrder(axis);
				const std::vector<uint32_t> dangling = beamDanglingOrder(axis);
				drawBeamNodeRow(id, BEAM_SOURCE_NODE, 0);
				bool walking = true;
				for (size_t i = 0; walking && i < order.size(); i++) {
					walking = drawBeamNodeRow(id, order[i], 1 + beamNodeDepth(*findBeamAxis(id), order[i]));
				}
				if (walking && !dangling.empty()) {
					ImGui::TextColored(REVIEW_COLOR, "dangling");
					for (size_t i = 0; walking && i < dangling.size(); i++) {
						walking = drawBeamNodeRow(id, dangling[i], 1 + beamNodeDepth(*findBeamAxis(id), dangling[i]));
					}
				}
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	ImGui::TreePop();
}

void RaytraceSceneEditor::drawBeamAxisAddItems(VulkanEngine*)
{
	if (!ImGui::MenuItem("Beam axis")) {
		ImGui::SetItemTooltip("The order a beam is meant to visit the optics in, from a laser or a camera");
		return;
	}
	// from the selected laser or camera, else the scene's one laser; none to choose between otherwise
	BeamAxis axis = makeBeamAxis({});
	const SceneOptic* selectedOptic = m_selectionKind == SelectionKind::Optic ? findOptic(m_selectionId) : nullptr;
	if (selectedOptic != nullptr && selectedOptic->kind == OpticKind::Laser) {
		axis.source = { BeamSourceKind::Laser, selectedOptic->id };
	} else if (m_selectionKind == SelectionKind::Camera && findCamera(m_selectionId) != nullptr) {
		axis.source = { BeamSourceKind::Camera, m_selectionId };
	} else if (const SceneOptic* laser = singleLaser(m_optics)) {
		axis.source = { BeamSourceKind::Laser, laser->id };
	}
	select(SelectionKind::BeamAxis, addBeamAxis(std::move(axis)));
}

//---------------------------------------------------------------- one visit

bool RaytraceSceneEditor::drawBeamNodeRow(uint64_t axisId, uint32_t node, int depth)
{
	const BeamAxis* axis = findBeamAxis(axisId);
	if (axis == nullptr) {
		return false;
	}
	std::string label;
	bool review = false;
	bool danglingRoot = false;
	if (node == BEAM_SOURCE_NODE) {
		if (axis->source.kind == BeamSourceKind::Laser) {
			const SceneOptic* laser = findOptic(axis->source.id);
			label = fmt::format("from {}", laser != nullptr ? laser->name : "?");
		} else if (axis->source.kind == BeamSourceKind::Camera) {
			const SceneCamera* camera = findCamera(axis->source.id);
			label = fmt::format("from {} (camera)", camera != nullptr ? camera->name : "?");
		} else {
			label = "no source";
		}
	} else {
		const BeamAxisNode* visit = findBeamNode(*axis, node);
		if (visit == nullptr) {
			return false;
		}
		const SceneOptic* optic = findOptic(visit->optic);
		const BeamAxisNode* parent = findBeamNode(*axis, visit->parent);
		const SceneOptic* parentOptic = parent != nullptr ? findOptic(parent->optic) : nullptr;
		const std::string_view port = parentOptic != nullptr ? opticPortName(parentOptic->kind, visit->port) : std::string_view();
		label = fmt::format("{}{}{}", port, port.empty() ? "" : " > ", optic != nullptr ? optic->name : "?");
		review = visit->review;
		danglingRoot = visit->parent == BEAM_DANGLING_NODE;
	}

	ImGui::PushID((int)node);
	const float indent = (float)depth * ImGui::GetStyle().IndentSpacing * 0.6f;
	if (indent > 0.f) {
		ImGui::Indent(indent);
	}
	const bool selected = m_selectionKind == SelectionKind::BeamAxis && m_selectionId == axisId && m_selectedBeamNode == node;
	if (ImGui::Selectable(label.c_str(), selected)) {
		select(SelectionKind::BeamAxis, axisId);
		m_selectedBeamNode = node;
	}
	if (review) {
		ImGui::SameLine();
		ImGui::TextColored(REVIEW_COLOR, "review");
		ImGui::SetItemTooltip("An edit changed the beam that reaches this visit: check it, then Mark reviewed");
	}

	const uint64_t revision = beamAxisRevision(axisId);
	if (ImGui::BeginPopupContextItem("##visit")) {
		if (ImGui::BeginMenu("Add after")) {
			drawBeamAppendMenu(axisId, node);
			ImGui::EndMenu();
		}
		if (node != BEAM_SOURCE_NODE) {
			if (ImGui::BeginMenu("Insert before")) {
				drawBeamInsertBeforeMenu(axisId, node);
				ImGui::EndMenu();
			}
			if (danglingRoot) {
				// back into the tree, on any free place of a visit that is not below it
				if (ImGui::BeginMenu("Attach to")) {
					const BeamAxis& current = *findBeamAxis(axisId);
					bool any = false;
					std::vector<uint32_t> parents { BEAM_SOURCE_NODE };
					const std::vector<uint32_t> order = beamAxisOrder(current);
					parents.insert(parents.end(), order.begin(), order.end());
					for (uint32_t parent : parents) {
						const BeamAxisNode* parentNode = findBeamNode(current, parent);
						const SceneOptic* parentOptic = parentNode != nullptr ? findOptic(parentNode->optic) : nullptr;
						const uint32_t ports = parent == BEAM_SOURCE_NODE ? 1 : parentOptic != nullptr ? opticPortCount(parentOptic->kind) : 0;
						for (uint32_t p = 0; p < ports; p++) {
							const OpticPortId port = parent == BEAM_SOURCE_NODE ? NO_PORT : (OpticPortId)p;
							if (beamNodeAt(current, parent, port) != nullptr) {
								continue;
							}
							any = true;
							const std::string place = parent == BEAM_SOURCE_NODE ? std::string("the source")
																				 : fmt::format("{} > {}", parentOptic->name, opticPortName(parentOptic->kind, port));
							if (ImGui::MenuItem(place.c_str())) {
								editBeamAxis(axisId, [&](BeamAxis& edited) {
									moveBeamNode(edited, node, parent, port);
									findBeamNode(edited, node)->review = true;
								});
							}
						}
					}
					if (!any) {
						ImGui::MenuItem("no free place", nullptr, false, false);
					}
					ImGui::EndMenu();
				}
			} else {
				if (ImGui::MenuItem("Detach")) {
					editBeamAxis(axisId, [&](BeamAxis& edited) {
						moveBeamNode(edited, node, BEAM_DANGLING_NODE, NO_PORT);
						findBeamNode(edited, node)->review = true;
					});
				}
				ImGui::SetItemTooltip("Cuts the visit and what follows it loose from the tree, to attach elsewhere");
			}
			if (review && ImGui::MenuItem("Mark reviewed")) {
				editBeamAxis(axisId, [&](BeamAxis& edited) { findBeamNode(edited, node)->review = false; });
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Remove")) {
				editBeamAxis(axisId, [&](BeamAxis& edited) { removeBeamNode(edited, node); });
			}
			ImGui::SetItemTooltip("What followed it moves up: the visit on its first port takes its place, the others dangle");
			if (ImGui::MenuItem("Remove with what follows")) {
				editBeamAxis(axisId, [&](BeamAxis& edited) { removeBeamSubtree(edited, node); });
			}
		}
		ImGui::EndPopup();
	}

	if (indent > 0.f) {
		ImGui::Unindent(indent);
	}
	ImGui::PopID();
	// a visit gone from the selection is the source again
	if (const BeamAxis* after = findBeamAxis(axisId); after != nullptr && m_selectedBeamNode != BEAM_SOURCE_NODE && findBeamNode(*after, m_selectedBeamNode) == nullptr) {
		m_selectedBeamNode = BEAM_SOURCE_NODE;
	}
	return beamAxisRevision(axisId) == revision;
}

void RaytraceSceneEditor::drawBeamAppendMenu(uint64_t axisId, uint32_t parent)
{
	const BeamAxis* axis = findBeamAxis(axisId);
	if (axis == nullptr) {
		return;
	}
	// the ports a visit leaves by: the source's one output, or the parent optic's
	const BeamAxisNode* parentNode = findBeamNode(*axis, parent);
	const SceneOptic* parentOptic = parentNode != nullptr ? findOptic(parentNode->optic) : nullptr;
	const uint32_t ports = parent == BEAM_SOURCE_NODE ? 1 : parentOptic != nullptr ? opticPortCount(parentOptic->kind) : 0;
	if (ports == 0) {
		ImGui::MenuItem(parentOptic != nullptr ? fmt::format("a {} has no output", opticTraits(parentOptic->kind).label).c_str() : "no output", nullptr, false, false);
		return;
	}

	// a free port takes the new visit; a taken one puts it between the port and what hung there, which then leaves
	// the new optic by the port chosen for it
	const auto opticItems = [&](OpticPortId port, bool taken) {
		bool any = false;
		for (const SceneOptic& optic : m_optics) {
			if (!visitable(optic)) {
				continue;
			}
			any = true;
			const uint64_t opticId = optic.id;
			const uint32_t through = opticPortCount(optic.kind);
			if (!taken) {
				if (ImGui::MenuItem(optic.name.c_str(), opticTraits(optic.kind).label)) {
					editBeamAxis(axisId, [&](BeamAxis& edited) {
						if (const std::optional<uint32_t> added = appendBeamNode(edited, parent, port, opticId)) {
							m_selectedBeamNode = *added;
						}
					});
				}
			} else if (through == 1) {
				if (ImGui::MenuItem(optic.name.c_str(), opticTraits(optic.kind).label)) {
					editBeamAxis(axisId, [&](BeamAxis& edited) {
						if (const std::optional<uint32_t> added = insertBeamNodeAfter(edited, parent, port, opticId, 0)) {
							m_selectedBeamNode = *added;
						}
					});
				}
			} else if (through > 1 && ImGui::BeginMenu(optic.name.c_str())) {
				for (uint32_t p = 0; p < through; p++) {
					if (ImGui::MenuItem(fmt::format("then on by {}", opticPortName(optic.kind, (OpticPortId)p)).c_str())) {
						editBeamAxis(axisId, [&](BeamAxis& edited) {
							if (const std::optional<uint32_t> added = insertBeamNodeAfter(edited, parent, port, opticId, (OpticPortId)p)) {
								m_selectedBeamNode = *added;
							}
						});
					}
				}
				ImGui::EndMenu();
			}
		}
		if (!any) {
			ImGui::MenuItem("no optic to visit", nullptr, false, false);
		}
	};

	if (ports == 1) {
		const OpticPortId port = parent == BEAM_SOURCE_NODE ? NO_PORT : 0;
		opticItems(port, beamNodeAt(*axis, parent, port) != nullptr);
		return;
	}
	for (uint32_t p = 0; p < ports; p++) {
		const bool taken = beamNodeAt(*axis, parent, (OpticPortId)p) != nullptr;
		const std::string portLabel = fmt::format("{}{}", opticPortName(parentOptic->kind, (OpticPortId)p), taken ? " (insert)" : "");
		if (ImGui::BeginMenu(portLabel.c_str())) {
			opticItems((OpticPortId)p, taken);
			ImGui::EndMenu();
		}
	}
}

void RaytraceSceneEditor::drawBeamInsertBeforeMenu(uint64_t axisId, uint32_t node)
{
	// the new optic takes the visit's place, and the visit hangs off the port chosen for the new optic: an optic the
	// beam cannot leave (a detector) cannot go in front of anything
	bool any = false;
	for (const SceneOptic& optic : m_optics) {
		const uint32_t through = opticPortCount(optic.kind);
		if (!visitable(optic) || through == 0) {
			continue;
		}
		any = true;
		const uint64_t opticId = optic.id;
		const auto insert = [&](OpticPortId port) {
			editBeamAxis(axisId, [&](BeamAxis& edited) {
				if (const std::optional<uint32_t> added = insertBeamNodeBefore(edited, node, opticId, port)) {
					m_selectedBeamNode = *added;
				}
			});
		};
		if (through == 1) {
			if (ImGui::MenuItem(optic.name.c_str(), opticTraits(optic.kind).label)) {
				insert(0);
			}
		} else if (ImGui::BeginMenu(optic.name.c_str())) {
			for (uint32_t p = 0; p < through; p++) {
				if (ImGui::MenuItem(fmt::format("then on by {}", opticPortName(optic.kind, (OpticPortId)p)).c_str())) {
					insert((OpticPortId)p);
				}
			}
			ImGui::EndMenu();
		}
	}
	if (!any) {
		ImGui::MenuItem("no optic with an output", nullptr, false, false);
	}
}

//---------------------------------------------------------------- the panel

void RaytraceSceneEditor::drawSelectedBeamAxis(VulkanEngine* engine, BeamAxis& axis)
{
	ImGui::SeparatorText(axis.name.c_str());
	ImGui::TextDisabled("Beam axis");
	const uint64_t id = axis.id;
	ImGui::PushID((int)id);

	// the source: a laser, a camera, or none yet
	std::string preview = "none";
	if (axis.source.kind == BeamSourceKind::Laser) {
		if (const SceneOptic* laser = findOptic(axis.source.id)) {
			preview = laser->name;
		}
	} else if (axis.source.kind == BeamSourceKind::Camera) {
		if (const SceneCamera* camera = findCamera(axis.source.id)) {
			preview = fmt::format("{} (camera)", camera->name);
		}
	}
	if (ImGui::BeginCombo("source", preview.c_str())) {
		std::optional<BeamSource> chosen;
		if (ImGui::Selectable("none", axis.source.kind == BeamSourceKind::None)) {
			chosen = BeamSource {};
		}
		for (const SceneOptic& optic : m_optics) {
			if (optic.kind == OpticKind::Laser && ImGui::Selectable(optic.name.c_str(), axis.source.kind == BeamSourceKind::Laser && axis.source.id == optic.id)) {
				chosen = BeamSource { BeamSourceKind::Laser, optic.id };
			}
		}
		for (const SceneCamera& camera : m_cameras) {
			if (ImGui::Selectable(fmt::format("{} (camera)", camera.name).c_str(), axis.source.kind == BeamSourceKind::Camera && axis.source.id == camera.id)) {
				chosen = BeamSource { BeamSourceKind::Camera, camera.id };
			}
		}
		ImGui::EndCombo();
		if (chosen) {
			editBeamAxis(id, [&](BeamAxis& edited) { edited.source = *chosen; });
		}
	}
	ImGui::SetItemTooltip("Where the beam starts: a laser's output, or a camera's chief ray leaving its sensor outward through the optics");

	ImGui::SeparatorText("Visits");
	ImGui::TextDisabled("Right-click a visit to add, insert, detach or remove one");
	// the walk stops at a menu that changed the tree; the next frame draws it again
	if (drawBeamNodeRow(id, BEAM_SOURCE_NODE, 0)) {
		const BeamAxis* current = findBeamAxis(id);
		const std::vector<uint32_t> order = beamAxisOrder(*current);
		const std::vector<uint32_t> dangling = beamDanglingOrder(*current);
		bool walking = true;
		for (size_t i = 0; walking && i < order.size(); i++) {
			walking = drawBeamNodeRow(id, order[i], 1 + beamNodeDepth(*findBeamAxis(id), order[i]));
		}
		if (walking && !dangling.empty()) {
			ImGui::TextColored(REVIEW_COLOR, "Dangling");
			ImGui::SetItemTooltip("Visits a Remove or a Detach cut loose. They keep their order and read Unreached; right-click one to attach it");
			for (size_t i = 0; walking && i < dangling.size(); i++) {
				walking = drawBeamNodeRow(id, dangling[i], 1 + beamNodeDepth(*findBeamAxis(id), dangling[i]));
			}
		}
	}

	// the same edits as the rows' menus, on the selected visit
	ImGui::Spacing();
	if (ImGui::Button("Add after")) {
		ImGui::OpenPopup("##addAfter");
	}
	if (ImGui::BeginPopup("##addAfter")) {
		drawBeamAppendMenu(id, m_selectedBeamNode);
		ImGui::EndPopup();
	}
	if (m_selectedBeamNode != BEAM_SOURCE_NODE) {
		ImGui::SameLine();
		if (ImGui::Button("Insert before")) {
			ImGui::OpenPopup("##insertBefore");
		}
		if (ImGui::BeginPopup("##insertBefore")) {
			drawBeamInsertBeforeMenu(id, m_selectedBeamNode);
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Remove")) {
			const uint32_t node = m_selectedBeamNode;
			editBeamAxis(id, [&](BeamAxis& edited) { removeBeamNode(edited, node); });
			m_selectedBeamNode = BEAM_SOURCE_NODE;
		}
	}

	// what the beam solver reads, which is not built yet
	ImGui::SeparatorText("Beam solver");
	const BeamAxis* current = findBeamAxis(id);
	float threshold = current->ghostThreshold;
	if (ImGui::SliderFloat("ghost threshold", &threshold, 1e-8f, 1.f, "%.1e", ImGuiSliderFlags_Logarithmic)) {
		editBeamAxis(id, [&](BeamAxis& edited) { edited.ghostThreshold = std::clamp(threshold, 0.f, 1.f); });
	}
	ImGui::SetItemTooltip("The beam solver follows a branch while it carries at least this fraction of the source's power");

	// how renders use it is a render setting (Render Settings > Optics > Guided tracing); this says whether they do
	ImGui::SeparatorText("Guided tracing");
	RaytraceRenderer& renderer = engine->m_raytracer;
	if (renderer.guideAxisId() == id) {
		ImGui::TextDisabled("Renders trace along this axis: %s", BEAM_TRACE_MODE_LABELS[(size_t)renderer.settings().optics.traceMode]);
		if (ImGui::SmallButton("Stop guiding by this axis")) {
			renderer.setGuideAxis(0);
		}
	} else if (ImGui::SmallButton("Guide renders by this axis")) {
		renderer.setGuideAxis(id);
	}
	ImGui::SetItemTooltip("Render Settings > Optics > Guided tracing chooses the mode. Not built yet: every render traces fully");
	ImGui::PopID();
}

//---------------------------------------------------------------- the overlay

void RaytraceSceneEditor::drawBeamAxisOverlays(const ViewportView& view) const
{
	if (view.drawList == nullptr) {
		return;
	}
	for (const BeamAxis& axis : m_beamAxes) {
		const bool selected = m_selectionKind == SelectionKind::BeamAxis && m_selectionId == axis.id;
		const ImU32 followed = selected ? IM_COL32(255, 200, 60, 235) : IM_COL32(255, 200, 60, 120);
		const ImU32 loose = IM_COL32(150, 150, 150, selected ? 200 : 100);
		const float thickness = selected ? 2.f : 1.f;

		// where an arrow starts and ends: the source's output, or an optic's body origin
		std::optional<glm::vec3> sourcePoint;
		if (axis.source.kind == BeamSourceKind::Laser) {
			if (const SceneOptic* laser = findOptic(axis.source.id)) {
				sourcePoint = glm::vec3(laser->world * glm::dvec4(laserBeam(*laser).center, 1.0));
			}
		} else if (axis.source.kind == BeamSourceKind::Camera) {
			if (const SceneCamera* camera = findCamera(axis.source.id)) {
				sourcePoint = camera->position;
			}
		}
		const auto pointOf = [&](uint32_t node) -> std::optional<glm::vec3> {
			if (node == BEAM_SOURCE_NODE) {
				return sourcePoint;
			}
			const BeamAxisNode* visit = findBeamNode(axis, node);
			const SceneOptic* optic = visit != nullptr ? findOptic(visit->optic) : nullptr;
			return optic != nullptr ? std::optional<glm::vec3>(glm::vec3(optic->world[3])) : std::nullopt;
		};

		for (const BeamAxisNode& node : axis.nodes) {
			if (node.parent == BEAM_DANGLING_NODE) {
				continue;
			}
			const std::optional<glm::vec3> from = pointOf(node.parent);
			const std::optional<glm::vec3> to = pointOf(node.id);
			ImVec2 a, b;
			if (!from || !to || !view.project(*from, a) || !view.project(*to, b)) {
				continue;
			}
			const ImU32 color = beamNodeDangling(axis, node.id) ? loose : followed;
			view.drawList->AddLine(a, b, color, thickness);
			// an arrowhead two thirds of the way, pointing along the beam. The point is two thirds of the way in the world:
			// perspective does not keep fractions along a line, so the screen's two thirds would slide as the view moves.
			// A line projects to a line, so the screen direction from a to b holds anywhere along it
			const ImVec2 d(b.x - a.x, b.y - a.y);
			const float length = std::sqrt(d.x * d.x + d.y * d.y);
			ImVec2 tip;
			if (length < 12.f || !view.project(glm::mix(*from, *to, 0.66f), tip)) {
				continue;
			}
			const ImVec2 u(d.x / length, d.y / length);
			const float size = selected ? 8.f : 6.f;
			view.drawList->AddTriangleFilled(tip, ImVec2(tip.x - u.x * size - u.y * size * 0.5f, tip.y - u.y * size + u.x * size * 0.5f),
				ImVec2(tip.x - u.x * size + u.y * size * 0.5f, tip.y - u.y * size - u.x * size * 0.5f), color);
			// the port it leaves by, beside the arrow of a selected axis
			if (selected && node.parent != BEAM_SOURCE_NODE) {
				const BeamAxisNode* parent = findBeamNode(axis, node.parent);
				const SceneOptic* parentOptic = parent != nullptr ? findOptic(parent->optic) : nullptr;
				if (parentOptic != nullptr && opticPortCount(parentOptic->kind) > 1) {
					const std::string port(opticPortName(parentOptic->kind, node.port));
					view.drawList->AddText(ImVec2(tip.x + u.y * 6.f, tip.y - u.x * 6.f), color, port.c_str());
				}
			}
		}
	}
}
