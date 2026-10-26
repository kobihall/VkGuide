#pragma once

// THE SPECTRUM WIDGET: how the panels edit a SpectrumRef (docs/plans/completed/pbrt-spectral-rendering.md). An RGB value
// is a color picker, as it always was, with a Spectral menu beside it that turns the parameter into another kind: a
// color temperature, one of pbrt's named spectra, a constant, an emission line, a dispersion formula, or an .spd file.
// Any other kind shows its own parameters and the color exact RGB sees, and the menu's Color item turns it back into
// that color. Used by the Materials window and the Scene panel's lights.

#include <spectrum_ref.h>

// What the widget offers, by the parameter's role: a reflectance gets no lines, a scalar (an index) gets the formulas
// and the glasses and no colors
struct SpectrumEditOptions {
	SpectrumRole role { SpectrumRole::Albedo };
	// the wavelength a scalar's value is shown at (the design wavelength)
	float designWavelength { 587.6f };
	// for an RGB value: the color picker's flags (ImGuiColorEditFlags)
	int colorFlags { 0 };
};

// Draws the widget; true when the reference changed
bool spectrumEdit(const char* label, SpectrumRef& ref, const SpectrumEditOptions& options);
// one line naming the reference, for read-only lists
std::string spectrumSummary(const SpectrumRef& ref);
