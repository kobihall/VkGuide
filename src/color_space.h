#pragma once

// RGB COLOR SPACES AND THE RGB UPLIFT (static library rt_spectrum, no Vulkan), as pbrt-v4 defines them
// (util/colorspace.h, util/color.h; PBR 4ed 4.6.3 to 4.6.6).
//
// A scene's RGB values (material colors, light colors) are in its working space, sRGB unless the scene says
// otherwise. Exact RGB transports them as they are after converting to linear sRGB, the film's space. The spectral
// modes turn each one into a spectrum first: a sigmoid of a quadratic in wavelength, s(c0 l^2 + c1 l + c2), whose
// three coefficients come from a table indexed by the color (Jakob and Hanika 2019). The tables are generated per
// color space by tools/rgb2spec/rgb2spec_opt.cpp, pbrt's own optimizer, into assets/spectra/uplift_<space>.bin.
//
// The lookup here is the line-for-line mirror of crt_spectrum.glsl's rgbToCoefficients(), in float, as pbrt's
// RGBToSpectrumTable::operator() computes it.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <spectrum.h>

// the values are saved in scene files by key, never by number, and are the GPU's CRT_COLOR_SPACE_*
enum class ColorSpaceId : uint32_t {
	Srgb,
	DciP3,
	Rec2020,
	Aces2065_1,
	Count
};

struct ColorSpace {
	ColorSpaceId id { ColorSpaceId::Srgb };
	// the scene file's name, and pbrt's ColorSpace directive's
	const char* key { "" };
	const char* name { "" };
	// what tools/rgb2spec/rgb2spec_opt calls the gamut, and the table file it writes
	const char* gamut { "" };
	const char* tableFile { "" };
	// the chromaticities of the primaries
	glm::dvec2 r { 0.0 };
	glm::dvec2 g { 0.0 };
	glm::dvec2 b { 0.0 };
	// its white: pbrt's named illuminant, normalized to a luminance of 1
	const DenseSpectrum* illuminant { nullptr };
	glm::dmat3 xyzFromRgb { 1.0 };
	glm::dmat3 rgbFromXyz { 1.0 };
};

const ColorSpace& colorSpace(ColorSpaceId id);
// by key ("srgb", "dci-p3", "rec2020", "aces2065-1"), or nullopt
std::optional<ColorSpaceId> findColorSpace(std::string_view key);
// the matrix that takes a color of `from` to the same color in `to`: identity for the same space
glm::dmat3 convertRgb(ColorSpaceId from, ColorSpaceId to);

//---------------------------------------------------------------- the uplift

// the sigmoid polynomial of one RGB color: c0 l^2 + c1 l + c2 with l in nanometers
struct SigmoidCoefficients {
	float c0 { 0.f };
	float c1 { 0.f };
	float c2 { 0.f };
};

// s(x) = 1/2 + x / (2 sqrt(1 + x^2)), and its value at lambda nm (pbrt's RGBSigmoidPolynomial)
float sigmoid(float x);
float evalSigmoidPolynomial(const SigmoidCoefficients& c, float lambda);

// One color space's table: res x res x res cells of three coefficients for each of the three components that can be
// largest, and the res z nodes (a smoothstep of a smoothstep, denser near 0 and 1) the largest component is placed by
struct UpliftTable {
	ColorSpaceId space { ColorSpaceId::Srgb };
	int res { 0 };
	std::vector<float> zNodes;
	// [maxc][z][y][x][3], as rgb2spec_opt writes it
	std::vector<float> data;

	bool valid() const { return res > 1 && data.size() == (size_t)9 * res * res * res; }
};

// the table file's path under the repository root
std::filesystem::path upliftTablePath(const std::filesystem::path& root, ColorSpaceId space);
std::optional<UpliftTable> loadUpliftTable(const std::filesystem::path& path, ColorSpaceId space, std::string* error = nullptr);
// the table of `space` under `root`, loaded once and kept; null when the file is missing or malformed
const UpliftTable* upliftTable(const std::filesystem::path& root, ColorSpaceId space, std::string* error = nullptr);

// pbrt's RGBToSpectrumTable::operator(): the coefficients of an RGB in [0, 1], trilinearly interpolated. A gray
// gets the exact constant-spectrum coefficients
SigmoidCoefficients rgbToCoefficients(const UpliftTable& table, glm::vec3 rgb);

// The three ways an RGB value becomes a spectrum, by what it means (pbrt's SpectrumType):
//   Albedo      a reflectance, clamped to [0, 1]: the sigmoid alone (RGBAlbedoSpectrum)
//   Unbounded   any non-negative magnitude: 2 max(rgb) times the sigmoid of rgb over that (RGBUnboundedSpectrum)
//   Illuminant  an emitter: the unbounded form times the space's illuminant, so that a white light lights a white
//               surface white (RGBIlluminantSpectrum)
enum class SpectrumRole : uint8_t {
	Albedo,
	Unbounded,
	Illuminant,
	// not a color at all: an index of refraction, an absorption coefficient. An RGB value in this role is read as
	// its luminance-weighted mean
	Scalar
};

// an RGB value's uplift: coefficients and a scale (1 for an albedo)
struct UpliftedRgb {
	SigmoidCoefficients c;
	float scale { 1.f };
};
UpliftedRgb upliftRgb(const UpliftTable& table, glm::vec3 rgb, SpectrumRole role);
// its value at lambda nm, the illuminant included for that role. RGB data is defined only from 360 to 830 nm, and
// a wavelength outside is read at the nearer end (docs/plans/spectral-out-of-range.md)
double evalUpliftedRgb(const UpliftedRgb& u, SpectrumRole role, ColorSpaceId space, double lambda);
