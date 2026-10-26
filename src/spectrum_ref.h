#pragma once

// SPECTRUMREF: A QUANTITY THAT VARIES WITH WAVELENGTH, AUTHORED ONCE (static library rt_spectrum, no Vulkan).
//
// Every spectrally varying parameter of a material or a light is a SpectrumRef (docs/plans/completed/pbrt-spectral-rendering.md).
// What it holds is one of the kinds below; what it MEANS is the parameter's SpectrumRole (color_space.h): the same
// RGB triple is a reflectance on an albedo and an emitter's radiance on an emission. Each spectral representation
// evaluates or projects the reference its own way, and exact RGB reads `exactRgb()`: the authored RGB untouched for an
// RGB reference, so a scene that uses no spectral kind renders as it always did.
//
// Wavelengths are in nanometers throughout. Evaluation is in double here; the GPU packs the same data into
// SpectralData (binding 24) and crt_spectrum.glsl evaluates it in float, line for line with evalSpectrumRefFloat().

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <color_space.h>
#include <spectrum.h>

// the kinds a reference can be. The values are saved in nothing (the scene file names them) but are the GPU's
// CRT_SPECTRUM_* tags for the kinds it evaluates directly
enum class SpectrumKind : uint8_t {
	// an RGB triple in the scene's working space, uplifted by the parameter's role
	Rgb,
	Constant,
	// Planck's law at a temperature, normalized to 1 at its peak (pbrt's BlackbodySpectrum)
	Blackbody,
	// one of pbrt-v4's named spectra ("metal-Au-eta", "glass-BK7", "stdillum-F11", ...)
	Named,
	// (wavelength, value) pairs given in the scene, linear between them
	Tabulated,
	// an .spd file of (wavelength, value) pairs, read when the reference is made
	File,
	// an index of refraction from one of the AGF catalog format's formulas, or Cauchy's
	Dispersion,
	// an emission line: a profile of a width around a center, integrating to `scale`
	Line
};

// The AGF format's dispersion formulas, numbered as the OpticStudio manual numbers them, with lambda in micrometers.
// Coefficients are in the order the AGF "CD" line lists them:
//   1  Schott        n^2 = a0 + a1 l^2 + a2 l^-2 + a3 l^-4 + a4 l^-6 + a5 l^-8
//   2  Sellmeier 1   n^2 - 1 = K1 l^2 / (l^2 - L1) + K2 l^2 / (l^2 - L2) + K3 l^2 / (l^2 - L3)     (K1 L1 K2 L2 K3 L3)
//   3  Herzberger    n = A + B L + C L^2 + D l^2 + E l^4 + F l^6,  L = 1 / (l^2 - 0.028)
//   4  Sellmeier 2   n^2 - 1 = A + B1 l^2 / (l^2 - l1^2) + B2 / (l^2 - l2^2)                       (A B1 l1 B2 l2)
//   5  Conrady       n = n0 + A / l + B / l^3.5
//   6  Sellmeier 3   n^2 - 1 = four terms K l^2 / (l^2 - L)                                         (K1 L1 ... K4 L4)
//   7  Handbook 1    n^2 = A + B / (l^2 - C) - D l^2
//   8  Handbook 2    n^2 = A + B l^2 / (l^2 - C) - D l^2
//   9  Sellmeier 4   n^2 = A + B l^2 / (l^2 - C) + D l^2 / (l^2 - E)
//   10 Extended      n^2 = a0 + a1 l^2 + a2 l^-2 + a3 l^-4 + a4 l^-6 + a5 l^-8 + a6 l^-10 + a7 l^-12
//   11 Sellmeier 5   n^2 - 1 = five terms K l^2 / (l^2 - L)                                         (K1 L1 ... K5 L5)
//   12 Extended 2    n^2 = a0 + a1 l^2 + a2 l^-2 + a3 l^-4 + a4 l^-6 + a5 l^-8 + a6 l^4 + a7 l^6
//   13 Extended 3    n^2 = a0 + a1 l^2 + a2 l^4 + a3 l^-2 + a4 l^-4 + a5 l^-6 + a6 l^-8 + a7 l^-10 + a8 l^-12
// and one outside the AGF set:
//   100 Cauchy       n = A + B / l^2 + C / l^4
// Schott, Sellmeier 1, 3 and 5, Herzberger, Conrady and the Extended formula are checked against Schott's data in
// bin/spectrum_test; formulas 4, 7, 8, 12 and 13 follow the manual's forms and wait for a catalog glass to check them
// (docs/plans/physoptics-optical-materials.md)
enum class DispersionFormula : uint8_t {
	Schott = 1,
	Sellmeier1 = 2,
	Herzberger = 3,
	Sellmeier2 = 4,
	Conrady = 5,
	Sellmeier3 = 6,
	Handbook1 = 7,
	Handbook2 = 8,
	Sellmeier4 = 9,
	Extended = 10,
	Sellmeier5 = 11,
	Extended2 = 12,
	Extended3 = 13,
	Cauchy = 100
};

inline constexpr size_t DISPERSION_COEFFICIENTS = 10;

enum class LineProfile : uint8_t {
	Gaussian,
	Lorentzian
};

struct SpectrumRef {
	SpectrumKind kind { SpectrumKind::Rgb };
	// Rgb: the color, in the scene's working space
	glm::vec3 rgb { 0.f };
	// Constant: the value. Blackbody: the temperature in kelvin. Line: the center wavelength
	float value { 0.f };
	// multiplies every kind but Rgb and Constant: for a Line it is the integral over wavelength
	float scale { 1.f };
	// Line: the full width at half maximum, in nm
	float width { 0.f };
	LineProfile profile { LineProfile::Gaussian };
	DispersionFormula formula { DispersionFormula::Sellmeier1 };
	std::array<double, DISPERSION_COEFFICIENTS> coefficients {};
	// Named: pbrt's name. File: the path, as given. Dispersion: the glass's name, for the panel
	std::string name;
	// Tabulated and File: (wavelength, value) pairs, increasing in wavelength. Named: filled from the name
	std::vector<float> samples;
	// Dispersion: the range the formula was fitted over (0 for unknown); evaluated outside it all the same
	glm::vec2 validRange { 0.f };

	bool operator==(const SpectrumRef&) const = default;
};

// constructors for the kinds
SpectrumRef rgbSpectrum(glm::vec3 rgb);
SpectrumRef constantRef(float value);
SpectrumRef blackbodyRef(float kelvin, float scale = 1.f);
// nullopt for a name pbrt does not have
std::optional<SpectrumRef> namedRef(std::string_view name, float scale = 1.f);
SpectrumRef tabulatedRef(std::vector<float> interleaved, float scale = 1.f);
std::optional<SpectrumRef> fileRef(const std::filesystem::path& path, std::string* error = nullptr, float scale = 1.f);
SpectrumRef dispersionRef(DispersionFormula formula, std::span<const double> coefficients, std::string name = {});
SpectrumRef lineRef(float center, float fwhm, LineProfile profile, float scale);

// the scene file's and the panel's name of a kind and a formula
const char* spectrumKindName(SpectrumKind kind);
const char* dispersionFormulaName(DispersionFormula formula);
// how many coefficients the formula reads
int dispersionCoefficientCount(DispersionFormula formula);

// the index a dispersion formula gives at lambda nm
double evalDispersion(DispersionFormula formula, std::span<const double> coefficients, double lambda);
// a line's density at lambda nm, integrating to 1 over wavelength
double evalLineProfile(LineProfile profile, double center, double fwhm, double lambda);

// What evaluation needs beyond the reference itself: the uplift table of the working space, for an RGB reference
struct SpectrumContext {
	const UpliftTable* table { nullptr };
	ColorSpaceId space { ColorSpaceId::Srgb };
};

// The reference's value at lambda nm, in its role. Rgb follows its role's uplift (and is 0 without a table); every
// other kind is the same in every role except Illuminant, where it is normalized as the role requires
// (illuminantNormalization())
double evalSpectrumRef(const SpectrumRef& ref, SpectrumRole role, const SpectrumContext& context, double lambda);
// the reference's own value at lambda nm, before any role
double evalSpectrumRefRaw(const SpectrumRef& ref, double lambda);

// AS AN EMITTER. pbrt divides every light's spectrum by its luminance, so that its color and its brightness are
// separate: a blackbody, a named illuminant, a tabulated or file spectrum in the Illuminant role is normalized to a
// luminance of 1, like the RGB white (1, 1, 1), and then multiplied by `scale`. A Line is the exception: its scale is
// already a radiance integrated over wavelength, as a laser's power is, and a line outside the visible range has no
// luminance to normalize by. The factor evalSpectrumRef() applies, 1 for an Rgb or Line reference
double illuminantNormalization(const SpectrumRef& ref);

// the reference as a dense spectrum from 360 to 830 nm, in its role
DenseSpectrum denseSpectrum(const SpectrumRef& ref, SpectrumRole role, const SpectrumContext& context);

// WHAT EXACT RGB USES. An Rgb reference is its color converted from the working space to linear sRGB (identity for an
// sRGB scene, so it is the authored triple bit for bit). Another kind is projected: an albedo under the space's
// illuminant, an emitter as pbrt's film would see it, a scalar by its luminance-weighted mean
glm::vec3 exactRgb(const SpectrumRef& ref, SpectrumRole role, const SpectrumContext& context);
// a scalar quantity (an index of refraction) read at one wavelength, as exact RGB reads a glass at the design
// wavelength. An Rgb reference gives its luminance-weighted mean
double scalarAt(const SpectrumRef& ref, double lambda);

// What the light list prices an emitter whose spectrum has no luminance by (an infrared or ultraviolet line): its
// raw spectrum integrated over wavelength from 1 to 20000 nm, over CIE_Y_INTEGRAL, so it compares with a luminance.
// Any positive price keeps light selection unbiased; this one keeps such an emitter in the list at all
float spectrumRadiometricPrice(const SpectrumRef& ref);

// true when the reference is the same at every wavelength, so a dielectric made of it does not disperse
bool isConstantSpectrum(const SpectrumRef& ref);
