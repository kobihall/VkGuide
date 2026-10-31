#include <optics_ui.h>

#include <cmath>
#include <cstring>
#include <optional>

#include <imgui.h>

#include <length_unit.h>
#include <optic_library.h>
#include <vk_engine.h>

namespace {

constexpr double PI = 3.14159265358979323846;

OpticalLibrary* s_library = nullptr;

// what the Glasses and Coatings folders selected; shown while the Materials window has no material selected
enum class EntryKind {
	None,
	Glass,
	Coating
};
EntryKind s_entryKind = EntryKind::None;
std::string s_entryKey;

void selectEntry(VulkanEngine* engine, EntryKind kind, const std::string& key)
{
	s_entryKind = kind;
	s_entryKey = key;
	engine->m_raytraceScene.selectMaterial(0);
}

// a coating's response against wavelength at an angle, for the panel's plot
void plotCoating(const OpticCoating& coating, const char* plus, const char* minus, double degrees)
{
	std::vector<float> reflectance;
	std::vector<float> transmittance;
	const IndexLookup index = s_library->indexLookup();
	for (int lambda = 380; lambda <= 1100; lambda += 5) {
		const CoatingResponse r = evaluateCoating(coating, index, lambda, degrees * PI / 180.0, index(plus, lambda), index(minus, lambda), true);
		reflectance.push_back((float)r.reflectance());
		transmittance.push_back((float)r.transmittance());
	}
	const std::string labelR = fmt::format("R at {:.0f} deg", degrees);
	const std::string labelT = fmt::format("T at {:.0f} deg", degrees);
	ImGui::PlotLines(labelR.c_str(), reflectance.data(), (int)reflectance.size(), 0, "380 to 1100 nm", 0.f, 1.f, ImVec2(0.f, 48.f));
	ImGui::PlotLines(labelT.c_str(), transmittance.data(), (int)transmittance.size(), 0, nullptr, 0.f, 1.f, ImVec2(0.f, 48.f));
}

void drawGlassDetails(VulkanEngine* engine, const OpticGlass& glass)
{
	ImGui::SeparatorText(glass.name.c_str());
	ImGui::TextDisabled("catalog: %s", glass.catalog.c_str());
	if (glass.index.kind == SpectrumKind::Dispersion) {
		ImGui::TextDisabled("%s, %d coefficients", dispersionFormulaName(glass.index.formula), dispersionCoefficientCount(glass.index.formula));
	}
	ImGui::Text("n at F, d, C: %.6f  %.6f  %.6f", glassIndex(glass, LAMBDA_F), glassIndex(glass, LAMBDA_D), glassIndex(glass, LAMBDA_C));
	ImGui::Text("Abbe number: %.2f (catalog %.2f)", abbeNumber(glass), glass.catalogVd);
	if (glass.range.y > glass.range.x) {
		ImGui::TextDisabled("valid from %.0f to %.0f nm; outside it the formula is extrapolated", glass.range.x, glass.range.y);
	}
	const double alpha = glassAbsorption(glass, 550.0);
	ImGui::Text("absorption at 550 nm: %.4g per m (%.4f through 10 mm)", alpha, std::exp(-alpha * 0.01));
	if (!glass.transmittance.empty()) {
		ImGui::TextDisabled("%zu internal transmittance lines", glass.transmittance.size());
	}
	if (ImGui::Button("Duplicate")) {
		OpticGlass copy = glass;
		copy.name = s_library->uniqueGlassName(glass.name + "_copy");
		s_library->setUserGlass(copy);
		selectEntry(engine, EntryKind::Glass, copy.name);
	}
	if (glass.catalog == CATALOG_USER) {
		ImGui::SameLine();
		if (ImGui::Button("Delete")) {
			s_library->removeUserGlass(glass.name);
			s_entryKind = EntryKind::None;
			return;
		}
		// a user glass's absorption, the simplest thing a filter needs: an internal transmittance through a thickness
		static float transmittance = 0.1f;
		static float thickness = 2.f;
		ImGui::SliderFloat("transmittance", &transmittance, 1e-6f, 1.f, "%.4f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("through (mm)", &thickness, 0.1f, 50.f);
		if (ImGui::Button("Set flat absorption")) {
			OpticGlass edited = glass;
			edited.transmittance = { glm::dvec3(300.0, transmittance, thickness * 1e-3), glm::dvec3(2500.0, transmittance, thickness * 1e-3) };
			edited.absorption = absorptionFromTransmittance(edited.transmittance);
			s_library->setUserGlass(edited);
		}
	}
}

void drawCoatingDetails(VulkanEngine* engine, OpticCoating& coating)
{
	ImGui::SeparatorText(coating.name.c_str());
	ImGui::TextDisabled("key %s, %s%s", coating.key.c_str(), coatingKindName(coating.kind), coating.builtin ? ", bundled" : "");
	if (!coating.source.empty()) {
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextDisabled("%s", coating.source.c_str());
		ImGui::PopTextWrapPos();
	}
	if (coating.polarizing) {
		ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f), "Polarizing: the renderer uses the average of s and p until polarization exists");
	}
	bool changed = false;
	if (!coating.builtin) {
		char name[128];
		std::snprintf(name, sizeof(name), "%s", coating.name.c_str());
		if (ImGui::InputText("name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0] != '\0') {
			coating.name = name;
			changed = true;
		}
		switch (coating.kind) {
		case CoatingKind::Ideal: {
			float values[4] = { (float)coating.rs, (float)coating.rp, (float)coating.ts, (float)coating.tp };
			const char* labels[4] = { "Rs", "Rp", "Ts", "Tp" };
			for (int i = 0; i < 4; i++) {
				changed |= ImGui::SliderFloat(labels[i], &values[i], 0.f, 1.f);
			}
			coating.rs = values[0];
			coating.rp = values[1];
			coating.ts = std::min((double)values[2], 1.0 - coating.rs);
			coating.tp = std::min((double)values[3], 1.0 - coating.rp);
			break;
		}
		case CoatingKind::Stack:
		case CoatingKind::Metal: {
			float design = (float)coating.designWavelength;
			if (ImGui::SliderFloat("design wavelength (nm)", &design, 200.f, 2000.f)) {
				coating.designWavelength = design;
				changed = true;
			}
			if (coating.kind == CoatingKind::Metal) {
				static const char* const METALS[] = { "Ag", "Al", "Au", "Cu" };
				if (ImGui::BeginCombo("metal", coating.metal.c_str())) {
					for (const char* metal : METALS) {
						if (ImGui::Selectable(metal, coating.metal == metal)) {
							coating.metal = metal;
							changed = true;
						}
					}
					ImGui::EndCombo();
				}
			}
			ImGui::TextDisabled("layers, from the air side:");
			for (size_t i = 0; i < coating.layers.size(); i++) {
				ImGui::PushID((int)i);
				CoatingLayer& layer = coating.layers[i];
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
				changed |= drawGlassCombo("##material", layer.material, false);
				ImGui::SameLine();
				float waves = (float)layer.quarterWaves;
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
				if (ImGui::DragFloat("quarter waves", &waves, 0.01f, 0.f, 20.f)) {
					layer.quarterWaves = std::max(waves, 0.f);
					changed = true;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("x")) {
					coating.layers.erase(coating.layers.begin() + (ptrdiff_t)i);
					changed = true;
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			if (ImGui::SmallButton("+ layer")) {
				coating.layers.push_back(CoatingLayer { "SIO2", 0.0, 1.0 });
				changed = true;
			}
			break;
		}
		case CoatingKind::Tabulated:
			ImGui::TextDisabled("%zu measured curve(s)", coating.curves.size());
			break;
		case CoatingKind::Uncoated:
			break;
		}
	}
	plotCoating(coating, GLASS_AIR, "N-BK7", 0.0);
	plotCoating(coating, GLASS_AIR, "N-BK7", 45.0);
	ImGui::TextDisabled("plotted on N-BK7 in air, from the air side");
	if (ImGui::Button("Duplicate")) {
		OpticCoating copy = coating;
		copy.key = s_library->uniqueCoatingKey(coating.key + "_copy");
		copy.name = coating.name + " (copy)";
		// an editable copy of a preset is a stack, a metal or an ideal coating of the user's own
		s_library->setUserCoating(copy);
		selectEntry(engine, EntryKind::Coating, copy.key);
		return;
	}
	if (!coating.builtin) {
		ImGui::SameLine();
		if (ImGui::Button("Delete")) {
			s_library->removeUserCoating(coating.key);
			s_entryKind = EntryKind::None;
			return;
		}
	}
	if (changed) {
		s_library->setUserCoating(coating);
	}
}

}

void setOpticsUiLibrary(OpticalLibrary* library)
{
	s_library = library;
}

const char* surroundingGlass(SurroundingMedium medium)
{
	return surroundingGlassName(medium);
}

bool drawCoatingCombo(const char* label, std::string& key)
{
	if (s_library == nullptr) {
		ImGui::TextDisabled("%s: %s", label, key.c_str());
		return false;
	}
	const OpticCoating* current = s_library->findCoating(key);
	const std::string preview = current != nullptr ? current->name : (key.empty() ? std::string("Uncoated") : key + " (missing)");
	bool changed = false;
	if (ImGui::BeginCombo(label, preview.c_str())) {
		for (const OpticCoating& coating : s_library->coatings()) {
			if (ImGui::Selectable(coating.name.c_str(), coating.key == key) && coating.key != key) {
				key = coating.key;
				changed = true;
			}
			if (!coating.source.empty() && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", coating.source.c_str());
			}
		}
		ImGui::EndCombo();
	}
	return changed;
}

bool drawGlassCombo(const char* label, std::string& name, bool allowDefault)
{
	if (s_library == nullptr) {
		ImGui::TextDisabled("%s: %s", label, name.c_str());
		return false;
	}
	const std::string preview = name.empty() ? std::string("(default)") : (s_library->findGlass(name) != nullptr ? name : name + " (missing)");
	bool changed = false;
	if (ImGui::BeginCombo(label, preview.c_str())) {
		if (allowDefault && ImGui::Selectable("(default)", name.empty()) && !name.empty()) {
			name.clear();
			changed = true;
		}
		for (const GlassCatalog& catalog : s_library->catalogs()) {
			if (catalog.glasses.empty()) {
				continue;
			}
			ImGui::SeparatorText(catalog.name.c_str());
			for (const OpticGlass& glass : catalog.glasses) {
				if (ImGui::Selectable(glass.name.c_str(), glass.name == name) && glass.name != name) {
					name = glass.name;
					changed = true;
				}
			}
		}
		ImGui::EndCombo();
	}
	return changed;
}

void drawOpticalLibraryFolders(VulkanEngine* engine)
{
	if (s_library == nullptr) {
		return;
	}
	// the elements' own slots first: they are what a bench edits most
	engine->m_raytraceScene.drawOpticMaterialRows();
	if (ImGui::TreeNodeEx("Glasses")) {
		for (const GlassCatalog& catalog : s_library->catalogs()) {
			if (catalog.glasses.empty() || !ImGui::TreeNodeEx(catalog.name.c_str())) {
				continue;
			}
			for (const OpticGlass& glass : catalog.glasses) {
				const bool selected = s_entryKind == EntryKind::Glass && s_entryKey == glass.name && engine->m_raytraceScene.selectedMaterial() == 0;
				if (ImGui::Selectable(glass.name.c_str(), selected)) {
					selectEntry(engine, EntryKind::Glass, glass.name);
				}
			}
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("Coatings")) {
		for (const OpticCoating& coating : s_library->coatings()) {
			const bool selected = s_entryKind == EntryKind::Coating && s_entryKey == coating.key && engine->m_raytraceScene.selectedMaterial() == 0;
			const std::string label = coating.builtin ? coating.name : coating.name + "  (user)";
			if (ImGui::Selectable(label.c_str(), selected)) {
				selectEntry(engine, EntryKind::Coating, coating.key);
			}
		}
		ImGui::TreePop();
	}
}

bool drawOpticalLibraryDetails(VulkanEngine* engine)
{
	if (s_library == nullptr || s_entryKind == EntryKind::None) {
		return false;
	}
	if (s_entryKind == EntryKind::Glass) {
		const OpticGlass* glass = s_library->findGlass(s_entryKey);
		if (glass == nullptr) {
			s_entryKind = EntryKind::None;
			return false;
		}
		const OpticGlass copy = *glass;
		drawGlassDetails(engine, copy);
		return true;
	}
	OpticCoating* coating = s_library->findCoating(s_entryKey);
	if (coating == nullptr) {
		s_entryKind = EntryKind::None;
		return false;
	}
	OpticCoating copy = *coating;
	drawCoatingDetails(engine, copy);
	return true;
}

void drawOpticsRenderSettings(VulkanEngine* engine)
{
	if (!ImGui::CollapsingHeader("Optics")) {
		return;
	}
	RenderSettings settings = engine->m_raytracer.settings();
	bool changed = false;
	const char* const MEDIA[] = { "Air (Ciddor 1996)", "Vacuum" };
	int medium = (int)settings.optics.surrounding;
	if (ImGui::Combo("surrounding medium", &medium, MEDIA, 2)) {
		settings.optics.surrounding = (SurroundingMedium)medium;
		changed = true;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Medium 0 of every optical body: standard air at 15 C, 101 325 Pa and 450 ppm CO2, or vacuum for an XUV setup");
	}
	float design = settings.spectral.designWavelength;
	if (ImGui::SliderFloat("design wavelength (nm)", &design, 200.f, 2000.f, "%.1f")) {
		settings.spectral.designWavelength = design;
		changed = true;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Exact RGB reads every glass's index here (the helium d line, 587.6 nm, by default); the element readouts and wedge angles too");
	}
	changed |= ImGui::Checkbox("exact coatings", &settings.optics.exactCoatings);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Evaluate each thin-film coating's transfer matrix at every hit, rather than interpolating its table. Slower, and exact");
	}
	if (settings.spectral.representation == SpectralRepresentation::ExactRgb) {
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextDisabled("Exact RGB: indices at the design wavelength, absorption and coatings at red, green and blue (630, 532 and 465 nm, "
			"and coatings projected per channel)");
		ImGui::PopTextWrapPos();
	}
	if (changed) {
		engine->m_raytracer.setSettings(settings);
	}
	if (s_library != nullptr) {
		size_t glasses = 0;
		for (const GlassCatalog& catalog : s_library->catalogs()) {
			glasses += catalog.glasses.size();
		}
		ImGui::TextDisabled("%zu glasses in %zu catalogs, %zu coatings", glasses, s_library->catalogs().size(), s_library->coatings().size());
	}
}

void drawOpticsMenu(VulkanEngine* engine)
{
	if (!ImGui::BeginMenu("Optics")) {
		return;
	}
	if (ImGui::MenuItem("Bench Settings...")) {
		*engine->m_raytraceScene.benchSettingsVisibilityFlag() = true;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("The breadboard, its size, the beam height new mounts are set to, and snapping to holes");
	}
	ImGui::Separator();
	if (ImGui::MenuItem("Load Glass Catalog...")) {
		engine->m_pendingFileAction = VulkanEngine::FileAction::LoadGlassCatalog;
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("An AGF catalog (Schott, Ohara, CDGM, Hoya, Sumita): its glasses join the library, saved with the scene by path");
	}
	if (ImGui::BeginMenu("Import Coating Curve")) {
		if (ImGui::MenuItem("Reflectance (CSV)...")) {
			engine->m_pendingFileAction = VulkanEngine::FileAction::ImportCoatingReflectance;
		}
		if (ImGui::MenuItem("Transmittance (CSV)...")) {
			engine->m_pendingFileAction = VulkanEngine::FileAction::ImportCoatingTransmittance;
		}
		ImGui::EndMenu();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Wavelength in nm, then one column per polarization or one averaged, in percent or as fractions: what Thorlabs' data exports to");
	}
	ImGui::EndMenu();
}
