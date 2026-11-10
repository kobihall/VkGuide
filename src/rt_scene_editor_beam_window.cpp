// The Beam window (docs/plans/completed/physoptics-gaussian-beam-solver.md, "The Beam window"; physoptics-beam-preview.md), part of
// RaytraceSceneEditor: one traced beam's segments as a table, a probe dragged along a segment with the beam there, Copy as
// CSV. Registered under the Optical bench capability (Windows -> Beam). The raster preview's settings are a submenu of the
// Optics menu (drawBeamPreviewMenu()), since they are display, not results.

#include <rt_scene_editor.h>

#include <length_unit.h>
#include <optics_ui.h>
#include <vk_engine.h>

#include <imgui.h>

#include <fmt/format.h>

#include <cmath>
#include <numbers>

namespace {


std::string radiusOrInfinity(double r)
{
	return std::isfinite(r) && std::abs(r) < 1e6 ? formatLength(r) : std::string("flat");
}

} // namespace

void RaytraceSceneEditor::drawBeamWindow(VulkanEngine*)
{
	if (!m_showBeamWindow) {
		return;
	}
	if (!ImGui::Begin("Beam", &m_showBeamWindow)) {
		ImGui::End();
		return;
	}
	const BeamSolution& solution = m_beamSolver.solution();

	//---- which beam: the selected axis's, else the one picked here
	if (m_selectionKind == SelectionKind::BeamAxis) {
		if (const auto found = solution.axisTrees.find(m_selectionId); found != solution.axisTrees.end() && found->second != m_beamWindowTree) {
			m_beamWindowTree = found->second;
			m_beamWindowSegment = 0;
		}
	}
	const auto treeName = [&](const BeamTree& tree) {
		std::string source = "?";
		if (tree.sourceKind == BeamSourceKind::Laser) {
			if (const SceneOptic* laser = findOptic(tree.source)) {
				source = laser->name;
			}
		} else if (const SceneCamera* camera = findCamera(tree.source)) {
			source = fmt::format("{} (chief ray)", camera->name);
		}
		std::string axes;
		for (const BeamAxis& axis : m_beamAxes) {
			if (axis.source.kind == tree.sourceKind && axis.source.id == tree.source) {
				axes += (axes.empty() ? " - " : ", ") + axis.name;
			}
		}
		return source + axes;
	};
	if (solution.trees.empty()) {
		ImGui::TextDisabled("No beam: add a laser, or a beam axis from a camera");
	} else {
		m_beamWindowTree = std::min<uint32_t>(m_beamWindowTree, (uint32_t)solution.trees.size() - 1);
		if (ImGui::BeginCombo("beam", treeName(solution.trees[m_beamWindowTree]).c_str())) {
			for (uint32_t t = 0; t < solution.trees.size(); t++) {
				if (ImGui::Selectable(treeName(solution.trees[t]).c_str(), t == m_beamWindowTree)) {
					m_beamWindowTree = t;
					m_beamWindowSegment = 0;
				}
			}
			ImGui::EndCombo();
		}
	}

	if (!solution.trees.empty()) {
		const BeamTree& tree = solution.trees[m_beamWindowTree];
		m_beamWindowSegment = std::min<uint32_t>(m_beamWindowSegment, (uint32_t)tree.segments.size() - 1);
		const auto opticName = [&](uint64_t id) {
			const SceneOptic* optic = findOptic(id);
			return optic != nullptr ? optic->name : std::string("-");
		};
		const auto medium = [&](const BeamSegment& s) {
			if (s.start.inside == 0) {
				return std::string("air");
			}
			const OpticPrescription* body = m_beamSolver.body(s.start.inside);
			const std::string glass = body != nullptr && s.start.medium < body->media.size() ? body->media[s.start.medium].glass : std::string();
			return fmt::format("{} in {}", glass.empty() ? "?" : glass, opticName(s.start.inside));
		};
		const auto endName = [&](const BeamSegment& s) {
			return s.end == BeamEnd::Escaped || s.end == BeamEnd::Truncated ? std::string(beamEndLabel(s.end)) : fmt::format("{} ({})", opticName(s.endOptic), beamEndLabel(s.end));
		};

		//---- the segments
		static bool showGhosts = false;
		ImGui::Checkbox("ghosts", &showGhosts);
		ImGui::SetItemTooltip("Branches that left an optic by no port: second-surface reflections, an etalon's pairs, a mirror's leak");
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy as CSV")) {
			std::string csv = "segment,parent,start x (m),start y (m),start z (m),end x (m),end y (m),end z (m),length (m),medium,power (W),clipped at end,ends on,port,ghost\n";
			for (uint32_t i = 0; i < tree.segments.size(); i++) {
				const BeamSegment& s = tree.segments[i];
				const glm::dvec3 e = s.endPoint();
				const SceneOptic* from = findOptic(s.start.fromOptic);
				csv += fmt::format("{},{},{:.9g},{:.9g},{:.9g},{:.9g},{:.9g},{:.9g},{:.9g},\"{}\",{:.9g},{:.6g},\"{}\",\"{}\",{}\n", i, s.parent == NO_BEAM_SEGMENT ? -1 : (int)s.parent,
					s.start.origin.x, s.start.origin.y, s.start.origin.z, e.x, e.y, e.z, s.length, medium(s), s.start.power, s.endClipped, endName(s),
					from != nullptr ? std::string(opticPortName(from->kind, s.start.port)) : std::string(), s.start.ghost ? 1 : 0);
			}
			ImGui::SetClipboardText(csv.c_str());
		}
		ImGui::SetItemTooltip("Every segment of this beam, ghosts included, in meters and watts");
		ImGui::SameLine();
		ImGui::TextDisabled("%zu segments, %.3g mW", tree.segments.size(), tree.power * 1e3);

		const float height = ImGui::GetTextLineHeightWithSpacing() * 9.f;
		if (ImGui::BeginTable("##segments", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
				ImVec2(0.f, height))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("length");
			ImGui::TableSetupColumn("medium");
			ImGui::TableSetupColumn("power");
			ImGui::TableSetupColumn("clipped");
			ImGui::TableSetupColumn("ends on");
			ImGui::TableHeadersRow();
			for (uint32_t i = 0; i < tree.segments.size(); i++) {
				const BeamSegment& s = tree.segments[i];
				if (s.start.ghost && !showGhosts) {
					continue;
				}
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (ImGui::Selectable(fmt::format("{}", i).c_str(), i == m_beamWindowSegment, ImGuiSelectableFlags_SpanAllColumns)) {
					m_beamWindowSegment = i;
					m_beamProbe = 0.0;
				}
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(s.end == BeamEnd::Escaped ? "-" : formatLength(s.length).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(medium(s).c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%.4g mW", s.start.power * 1e3);
				ImGui::TableNextColumn();
				if (s.endClipped > 0.0) {
					ImGui::Text("%.3g %%", s.endClipped * 100.0);
				}
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(endName(s).c_str());
			}
			ImGui::EndTable();
		}

		//---- the probe
		const BeamSegment& s = tree.segments[m_beamWindowSegment];
		ImGui::SeparatorText(fmt::format("Segment {}", m_beamWindowSegment).c_str());
		double probe = std::clamp(m_beamProbe, 0.0, s.length);
		if (dragLength("along it", probe, std::max(s.length, 1e-6) / 400.0, 0.0, s.length)) {
			m_beamProbe = probe;
		}
		ImGui::SetItemTooltip("Where along the segment the readouts below are, from its start");
		const BeamCrossSection at = tree.crossSection(m_beamWindowSegment, probe);
		const BeamProfile profile = tree.profile(m_beamWindowSegment);
		const glm::dvec3 p = s.start.origin + probe * s.start.direction;
		if (ImGui::BeginTable("##probe", 2, ImGuiTableFlags_SizingStretchProp)) {
			const auto row = [](const char* label, const std::string& value) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", label);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(value.c_str());
			};
			row("position", fmt::format("{}, {}, {}", formatLength(p.x), formatLength(p.y), formatLength(p.z)));
			row("direction", fmt::format("{:.5f}, {:.5f}, {:.5f}", s.start.direction.x, s.start.direction.y, s.start.direction.z));
			row("from the source", formatLength(s.start.distance + probe));
			if (tree.chiefRayOnly) {
				row("beam", "a camera's chief ray carries no beam");
			} else {
				row("1/e^2 radii", fmt::format("{}, {}", formatLength(at.radii.x), formatLength(at.radii.y)));
				row("axes", fmt::format("{:.2f} deg from the frame", at.angle * 180.0 / std::numbers::pi));
				row("wavefront radii", fmt::format("{}, {}", radiusOrInfinity(at.curvatureRadii.x), radiusOrInfinity(at.curvatureRadii.y)));
				for (int a = 0; a < 2; a++) {
					const BeamAxisProfile& axis = profile.axes[a];
					row(a == 0 ? "first axis" : "second axis",
						fmt::format("w0 {} at {} from the start, z_R {}, Gouy {:.3f} rad", formatLength(axis.waist), formatLength(axis.waistAt), formatLength(axis.rayleighRange),
							axis.gouy(probe)));
				}
				if (profile.twisted) {
					row("note", "the beam's axes turn along it, or unequal M^2 meet turned axes: the profiles are approximate");
				}
				row("power", fmt::format("{:.6g} mW{}", s.start.power * 1e3, s.start.clippedUpstream > 0.0 ? fmt::format(", {:.3g} % clipped upstream", s.start.clippedUpstream * 100.0) : ""));
				row("polarization", "waits for polarization (physoptics-polarization-optics.md)");
				row("optical path", formatLength(s.start.opticalPath + s.start.index * probe));
			}
			ImGui::EndTable();
		}
	}

	ImGui::Spacing();
	ImGui::TextDisabled("How the beams are drawn: Optics > Beam Preview");
	ImGui::End();
}

void RaytraceSceneEditor::drawBeamPreviewMenu()
{
	if (!ImGui::BeginMenu("Beam Preview")) {
		return;
	}
	BeamPreviewSettings settings = m_beamPreview;
	// one of the three at a time: the beam, else the axes' arrows, else the lasers' output arrows
	if (ImGui::RadioButton("Full beam", settings.display == BeamDisplay::FullBeam)) {
		settings.display = BeamDisplay::FullBeam;
	}
	ImGui::SetItemTooltip("A tube along every segment of each laser's solved beam, as wide as the beam");
	if (ImGui::RadioButton("Spots only", settings.display == BeamDisplay::SpotsOnly)) {
		settings.display = BeamDisplay::SpotsOnly;
	}
	ImGui::SetItemTooltip("The beam's footprint on every surface it meets, as an alignment card shows it");
	if (ImGui::RadioButton("Off", settings.display == BeamDisplay::Off)) {
		settings.display = BeamDisplay::Off;
	}
	ImGui::SetItemTooltip("No beam: the beam axes' arrows instead, or each laser's output arrow when the scene has no beam axis");
	ImGui::Separator();
	ImGui::BeginDisabled(settings.display == BeamDisplay::Off);
	ImGui::Checkbox("Ghosts", &settings.ghosts);
	ImGui::SetItemTooltip("Also the branches that leave an optic by no port: a lens's back reflections, a plate's second surface. A clipped "
						  "ghost is drawn at its Gaussian size, wider than the aperture that clipped it");
	bool wide = settings.radiusScale > 1.25f;
	if (ImGui::Checkbox("1.5 w radius", &wide)) {
		settings.radiusScale = wide ? 1.5f : 1.f;
	}
	ImGui::SetItemTooltip("The 1/e^2 radius w holds 86.5 %% of the power; 1.5 w holds 98.9 %%, the radius to judge an aperture's clearance by");
	bool confocal = settings.waistRegion == BeamWaistRegion::Confocal;
	if (ImGui::Checkbox("Confocal waist region", &confocal)) {
		settings.waistRegion = confocal ? BeamWaistRegion::Confocal : BeamWaistRegion::RayleighRange;
	}
	ImGui::SetItemTooltip("The cylinder drawn at a waist: z_R long and centered on it, or, checked, from -z_R to +z_R, where the radius stays "
						  "under sqrt(2) w0");
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.f);
	ImGui::SliderFloat("brightness range", &settings.brightnessDecades, 1.f, 10.f, "%.1f decades");
	ImGui::SetItemTooltip("The laser's power is fully opaque and this many decades below it is nothing");
	if (settings.ghosts) {
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.f);
		dragPowersOfTen("ghost threshold", settings.ghostThreshold, GHOST_THRESHOLD_MIN_DECADE, 0);
		ImGui::SetItemTooltip("The beam solver follows a branch while it carries at least this fraction of the source's power. Drag to change it by "
							  "powers of ten, or double-click and type any number from 1e-12 to 1. A beam axis can set its own");
	}
	ImGui::SeparatorText("Annotations");
	ImGui::Checkbox("Waist labels", &settings.waistLabels);
	ImGui::Checkbox("Distances", &settings.distances);
	ImGui::SetItemTooltip("The distance along the beam from the source to each optic. A spherical lens's label sits at its origin at setting");
	ImGui::EndDisabled();
	ImGui::Checkbox("Misses", &settings.missMarkers);
	ImGui::SetItemTooltip("A cross where a beam axis's visit is missed, with the distance and the way to steer");
	if (!(settings == m_beamPreview)) {
		m_beamPreview = settings;
		m_beamPreviewRevision++;
	}
	ImGui::EndMenu();
}
