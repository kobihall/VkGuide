#include <spectrum_edit.h>

#include <algorithm>
#include <cstring>

#include <fmt/format.h>
#include <imgui.h>

namespace {

// pbrt's named spectra by what they are, from their names
enum class NamedGroup {
	Illuminant,
	Metal,
	Glass,
	Sensor
};

NamedGroup namedGroup(std::string_view name)
{
	if (name.starts_with("metal-")) {
		return NamedGroup::Metal;
	}
	if (name.starts_with("glass-")) {
		return NamedGroup::Glass;
	}
	if (name.starts_with("stdillum-") || name.starts_with("illum-")) {
		return NamedGroup::Illuminant;
	}
	return NamedGroup::Sensor;
}

const DispersionFormula FORMULAS[] = {
	DispersionFormula::Sellmeier1, DispersionFormula::Cauchy, DispersionFormula::Schott, DispersionFormula::Herzberger, DispersionFormula::Sellmeier2,
	DispersionFormula::Conrady, DispersionFormula::Sellmeier3, DispersionFormula::Handbook1, DispersionFormula::Handbook2, DispersionFormula::Sellmeier4,
	DispersionFormula::Extended, DispersionFormula::Sellmeier5, DispersionFormula::Extended2, DispersionFormula::Extended3,
};

// N-BK7 from Schott's catalog: what a new dispersion formula starts from
const double BK7_SELLMEIER[] = { 1.03961212, 0.00600069867, 0.231792344, 0.0200179144, 1.01046945, 103.560653 };

// the Spectral menu's items: each replaces the reference with a new kind, starting from a value that looks like the
// old one where it can. Returns true when it replaced it
bool spectralMenu(SpectrumRef& ref, const SpectrumEditOptions& options)
{
	bool changed = false;
	const bool scalar = options.role == SpectrumRole::Scalar;
	if (ref.kind != SpectrumKind::Rgb && !scalar && ImGui::MenuItem("Color")) {
		//the color exact RGB sees, so the picker starts where the spectrum looked
		glm::vec3 rgb = glm::max(exactRgb(ref, options.role, {}), glm::vec3(0.f));
		if (options.role == SpectrumRole::Albedo) {
			rgb = glm::min(rgb, glm::vec3(1.f));
		}
		ref = rgbSpectrum(rgb);
		changed = true;
	}
	if (!scalar && ImGui::MenuItem("Color temperature")) {
		ref = blackbodyRef(6500.f);
		changed = true;
	}
	if (ImGui::MenuItem("Constant")) {
		ref = constantRef(scalar ? 1.5f : 1.f);
		changed = true;
	}
	if (ImGui::BeginMenu("Named spectrum")) {
		const char* groups[] = { "Illuminants", "Metals", "Glasses", "Camera sensors" };
		for (int g = 0; g < 4; g++) {
			if (ImGui::BeginMenu(groups[g])) {
				for (size_t i = 0; i < spectrum_tables::NAMED_SPECTRUM_COUNT; i++) {
					const char* name = spectrum_tables::NAMED_SPECTRA[i].name;
					if ((int)namedGroup(name) == g && ImGui::MenuItem(name)) {
						ref = *namedRef(name);
						changed = true;
					}
				}
				ImGui::EndMenu();
			}
		}
		ImGui::EndMenu();
	}
	if (options.role == SpectrumRole::Illuminant && ImGui::MenuItem("Emission line")) {
		ref = lineRef(632.8f, 1.f, LineProfile::Gaussian, 100.f);
		changed = true;
	}
	if (scalar && ImGui::MenuItem("Dispersion formula")) {
		ref = dispersionRef(DispersionFormula::Sellmeier1, BK7_SELLMEIER, "N-BK7");
		ref.validRange = glm::vec2(300.f, 2500.f);
		changed = true;
	}
	if (ImGui::MenuItem(".spd file...")) {
		ref = tabulatedRef({}, 1.f);
		ref.kind = SpectrumKind::File;
		changed = true;
	}
	return changed;
}

}

std::string spectrumSummary(const SpectrumRef& ref)
{
	switch (ref.kind) {
	case SpectrumKind::Rgb:
		return fmt::format("RGB ({:.3f}, {:.3f}, {:.3f})", ref.rgb.r, ref.rgb.g, ref.rgb.b);
	case SpectrumKind::Constant:
		return fmt::format("constant {:.4f}", ref.value);
	case SpectrumKind::Blackbody:
		return fmt::format("blackbody {:.0f} K", ref.value);
	case SpectrumKind::Named:
		return ref.name;
	case SpectrumKind::Tabulated:
		return fmt::format("tabulated, {} samples", ref.samples.size() / 2);
	case SpectrumKind::File:
		return ref.name.empty() ? std::string(".spd file") : ref.name;
	case SpectrumKind::Dispersion:
		return fmt::format("{} ({})", ref.name.empty() ? "glass" : ref.name, dispersionFormulaName(ref.formula));
	case SpectrumKind::Line:
		return fmt::format("line {:.2f} nm", ref.value);
	}
	return "";
}

bool spectrumEdit(const char* label, SpectrumRef& ref, const SpectrumEditOptions& options)
{
	ImGui::PushID(label);
	bool changed = false;
	const bool scalar = options.role == SpectrumRole::Scalar;

	if (ref.kind == SpectrumKind::Rgb && !scalar) {
		changed |= ImGui::ColorEdit3("##color", &ref.rgb.x, options.colorFlags);
	} else {
		//the color exact RGB sees, or the value at the design wavelength for a scalar
		if (!scalar) {
			const glm::vec3 rgb = glm::clamp(exactRgb(ref, options.role, {}), glm::vec3(0.f), glm::vec3(1.f));
			ImGui::ColorButton("##preview", ImVec4(rgb.r, rgb.g, rgb.b, 1.f), ImGuiColorEditFlags_NoTooltip);
			ImGui::SameLine();
			ImGui::TextUnformatted(spectrumSummary(ref).c_str());
		} else {
			ImGui::Text("%s: %.5f at %.1f nm", spectrumSummary(ref).c_str(), scalarAt(ref, options.designWavelength), options.designWavelength);
		}
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Spectral")) {
		ImGui::OpenPopup("spectral");
	}
	if (ImGui::BeginPopup("spectral")) {
		changed |= spectralMenu(ref, options);
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(label);

	//the kind's own parameters, indented under it
	ImGui::Indent();
	switch (ref.kind) {
	case SpectrumKind::Rgb:
		break;
	case SpectrumKind::Constant:
		changed |= ImGui::DragFloat("value", &ref.value, 0.001f, 0.f, 0.f, "%.5f");
		break;
	case SpectrumKind::Blackbody:
		changed |= ImGui::SliderFloat("temperature (K)", &ref.value, 1000.f, 20000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
		break;
	case SpectrumKind::Named:
		if (ImGui::BeginCombo("spectrum", ref.name.c_str())) {
			for (size_t i = 0; i < spectrum_tables::NAMED_SPECTRUM_COUNT; i++) {
				const char* name = spectrum_tables::NAMED_SPECTRA[i].name;
				if (ImGui::Selectable(name, ref.name == name)) {
					const float scale = ref.scale;
					ref = *namedRef(name, scale);
					changed = true;
				}
			}
			ImGui::EndCombo();
		}
		break;
	case SpectrumKind::Tabulated:
		ImGui::TextDisabled("%zu samples from %.1f to %.1f nm", ref.samples.size() / 2, ref.samples.empty() ? 0.f : ref.samples.front(),
			ref.samples.empty() ? 0.f : ref.samples[ref.samples.size() - 2]);
		break;
	case SpectrumKind::File: {
		char path[1024];
		std::strncpy(path, ref.name.c_str(), sizeof(path) - 1);
		path[sizeof(path) - 1] = '\0';
		ImGui::InputText("path", path, sizeof(path));
		ImGui::SameLine();
		if (ImGui::Button("Load")) {
			std::string error;
			if (std::optional<SpectrumRef> loaded = fileRef(path, &error, ref.scale)) {
				ref = *loaded;
				changed = true;
			} else {
				ref.name = path;
				ref.samples.clear();
				changed = true;
			}
		}
		if (ref.samples.empty()) {
			ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "no samples: the parameter is zero until a file loads");
		}
		break;
	}
	case SpectrumKind::Dispersion: {
		if (ImGui::BeginCombo("formula", dispersionFormulaName(ref.formula))) {
			for (const DispersionFormula formula : FORMULAS) {
				if (ImGui::Selectable(dispersionFormulaName(formula), ref.formula == formula)) {
					ref.formula = formula;
					changed = true;
				}
			}
			ImGui::EndCombo();
		}
		for (int i = 0; i < dispersionCoefficientCount(ref.formula); i++) {
			changed |= ImGui::InputDouble(fmt::format("c{}", i).c_str(), &ref.coefficients[(size_t)i], 0.0, 0.0, "%.9g");
		}
		if (ref.validRange.y > ref.validRange.x && (options.designWavelength < ref.validRange.x || options.designWavelength > ref.validRange.y)) {
			ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "outside the formula's range, %.0f to %.0f nm", ref.validRange.x, ref.validRange.y);
		}
		break;
	}
	case SpectrumKind::Line: {
		changed |= ImGui::DragFloat("center (nm)", &ref.value, 0.1f, 1.f, 20000.f, "%.3f");
		changed |= ImGui::DragFloat("FWHM (nm)", &ref.width, 0.01f, 1e-4f, 1000.f, "%.4f", ImGuiSliderFlags_Logarithmic);
		int profile = (int)ref.profile;
		if (ImGui::Combo("profile", &profile, "Gaussian\0Lorentzian\0")) {
			ref.profile = (LineProfile)profile;
			changed = true;
		}
		break;
	}
	}
	//every kind but a color and a constant has a scale; a line's is its radiance integrated over wavelength, an
	//emitter's otherwise a brightness on top of its luminance of 1
	if (ref.kind != SpectrumKind::Rgb && ref.kind != SpectrumKind::Constant && ref.kind != SpectrumKind::Dispersion) {
		const char* scaleLabel = ref.kind == SpectrumKind::Line ? "integral (W/(sr m^2))" : "scale";
		changed |= ImGui::DragFloat(scaleLabel, &ref.scale, 0.01f, 0.f, 1e6f, "%.4f");
	}
	ImGui::Unindent();
	ImGui::PopID();
	return changed;
}
