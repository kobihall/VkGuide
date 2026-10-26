#pragma once

// SPECTRA ON THE CPU (static library rt_spectrum, no Vulkan): spectral distributions and their projection to the
// renderer's linear sRGB, the way pbrt-v4 defines both.
//
// These functions turn a spectrum a scene gives (a blackbody temperature, a named metal or glass, a .spd file) into
// RGB: exact RGB's projection of it, and what the pbrt importer (docs/plans/completed/pbrt-scene-import.md) reports.
// The spectral modes (docs/plans/completed/pbrt-spectral-rendering.md) build on them: spectrum_ref.h holds a
// spectrum as a scene authors it, color_space.h the RGB uplift, and spectral.h the representations the kernels carry
// (crt_spectrum.glsl).
//
// A spectrum is held densely at every nanometer from 360 to 830, which is what pbrt's DenselySampledSpectrum holds
// and what its InnerProduct() sums over, so every integral here is pbrt's to float rounding. The tables
// (src/spectrum_tables.cpp) are generated from pbrt-v4's src/pbrt/util/spectrum.cpp by
// tools/spectrum_tables/extract_pbrt_spectra.py.

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

namespace spectrum_tables {

inline constexpr int LAMBDA_MIN = 360;
inline constexpr int LAMBDA_MAX = 830;
inline constexpr int CIE_SAMPLES = LAMBDA_MAX - LAMBDA_MIN + 1;
// the sum of the Y matching function at every nanometer (pbrt's CIE_Y_integral)
inline constexpr double CIE_Y_INTEGRAL = 106.856895;

// the CIE 1931 2-degree matching functions at every nanometer from LAMBDA_MIN
extern const float CIE_X[CIE_SAMPLES];
extern const float CIE_Y[CIE_SAMPLES];
extern const float CIE_Z[CIE_SAMPLES];

// one of pbrt's named spectra: `count` (wavelength in nm, value) pairs, increasing in wavelength. `normalized`
// for the illuminants, which pbrt scales to a luminance of 1
struct NamedSpectrumData {
	const char* name;
	const float* interleaved;
	size_t count;
	bool normalized;
};

extern const NamedSpectrumData NAMED_SPECTRA[];
extern const size_t NAMED_SPECTRUM_COUNT;

}

// A spectral distribution at every nanometer from 360 to 830: pbrt's DenselySampledSpectrum
struct DenseSpectrum {
	std::array<double, spectrum_tables::CIE_SAMPLES> values {};

	// the value at a whole wavelength in nm; 0 outside 360 to 830
	double operator()(int lambda) const;
	DenseSpectrum operator*(const DenseSpectrum& other) const;
	DenseSpectrum operator*(double scale) const;
};

DenseSpectrum constantSpectrum(double value);
// pbrt's BlackbodySpectrum: Planck's law at `kelvin`, normalized to 1 at its peak
DenseSpectrum blackbodySpectrum(double kelvin);
// pbrt's PiecewiseLinearSpectrum::FromInterleaved: linear between the given (nm, value) pairs, the end values held to
// 360 and 830, zero past the data. `normalize` scales it to a luminance of 1 (pbrt's illuminants)
DenseSpectrum piecewiseLinearSpectrum(std::span<const float> interleaved, bool normalize);
// one of pbrt-v4's named spectra ("metal-Au-eta", "glass-BK7", "stdillum-D65", ...), or nullopt
std::optional<DenseSpectrum> namedSpectrum(std::string_view name);
// an .spd file as pbrt reads one: whitespace-separated (nm, value) pairs, '#' comments. Unnormalized
std::optional<DenseSpectrum> readSpectrumFile(const std::filesystem::path& path, std::string* error = nullptr);

// the sum over every nanometer of a times b (pbrt's InnerProduct)
double innerProduct(const DenseSpectrum& a, const DenseSpectrum& b);
// the inner product with the Y matching function (pbrt's SpectrumToPhotometric for a spectrum that is not RGB)
double spectrumLuminance(const DenseSpectrum& s);
// CIE XYZ, Y of 1 for a spectrum whose luminance is CIE_Y_INTEGRAL (pbrt's SpectrumToXYZ)
glm::dvec3 spectrumToXyz(const DenseSpectrum& s);
// pbrt's sRGB color space: XYZ to linear sRGB, its white the chromaticity of pbrt's D65 spectrum
glm::dvec3 xyzToLinearSrgb(const glm::dvec3& xyz);
// the D65 illuminant normalized to a luminance of 1, the white of the renderer's RGB
const DenseSpectrum& d65Spectrum();

// AS THE RENDERER SEES THEM. The linear sRGB radiance a light of this spectrum has once pbrt scales it to a luminance
// of 1 nit, as every pbrt-v4 light does (`scale /= SpectrumToPhotometric(L)`): multiply by the light's "scale"
glm::vec3 emissionToRgb(const DenseSpectrum& s);
// the linear sRGB albedo of a reflectance spectrum lit by D65, so a reflectance of 1 everywhere is (1, 1, 1)
glm::vec3 reflectanceToRgb(const DenseSpectrum& s);
// the spectrum's mean weighted by the Y matching function: the one value an RGB renderer gives a quantity that varies
// with wavelength and cannot be a color, such as an index of refraction
double luminanceWeightedMean(const DenseSpectrum& s);
