#pragma once

// THE SPECTRAL REPRESENTATIONS (static library rt_spectrum, no Vulkan): what the values a path carries mean, how a
// SpectrumRef becomes those values, and how they become the film's linear sRGB. The line-for-line C++ mirror of
// shaders/rt/include/crt_spectrum.glsl, and the packing of SpectralData (binding 24) that file reads.
// docs/plans/completed/pbrt-spectral-rendering.md is the design.
//
// Three independent choices make a configuration:
//  - the build, by lane count: exact RGB (a vec3, today's shaders), one lane (monochromatic), or 4, 8, 16 or 32
//    lanes. laneFamily() says which a setting needs; GpuRenderer builds a family's pipelines the first time it runs
//  - the representation, below: what the lanes mean
//  - the scene's spectra, SpectrumRefs, which every representation evaluates or projects (spectrum_ref.h)
//
// Every function here that the GPU also runs works in float, in the order the shader does.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <color_space.h>
#include <spectrum_ref.h>

//---------------------------------------------------------------- settings

// the scene file's keys are spectralRepresentationKey(); the values are the GPU's CRT_REPRESENTATION_*
enum class SpectralRepresentation : uint32_t {
	// R, G and B, as the renderer always transported: the build without lanes
	ExactRgb = 0,
	// one wavelength, every path, no wavelength noise. The film holds the spectral radiance at it in all three channels
	Monochromatic = 1,
	// three bands with Gaussian sensitivities in the four-lane build
	RgbGaussians = 2,
	// N fixed bands over the range, box or Gaussian, every path carrying every band
	SpectralBands = 3,
	// N wavelengths drawn per path, pbrt-v4's SampledWavelengths, estimated against the matching functions
	HeroWavelengths = 4,
	Count
};

// how hero wavelengths are drawn: the GPU's CRT_SAMPLING_*
enum class WavelengthSampling : uint32_t {
	// pbrt's visible-importance density, sech^2(0.0072 (l - 538)), over 360 to 830 nm; uniform outside that range
	Visible = 0,
	Uniform = 1,
	// a mixture of the base density and the scene's emitters' spectra, so a narrow line is not mostly missed
	Lights = 2,
	Count
};

enum class BandBasis : uint32_t {
	Box = 0,
	Gaussian = 1,
	Count
};

// a Gaussian band: its center and full width at half maximum, in nm
struct GaussianBand {
	float center { 550.f };
	float fwhm { 100.f };
	bool operator==(const GaussianBand&) const = default;
};

// the RGB Gaussians' default bands, fitted by ./bin/spectrum_test --fit-gaussians: the three that bring a grid of sRGB
// colors, uplifted as reflectances under the white light, projected onto the bands and through the film, back closest
// to themselves (least squares, Nelder-Mead from the sRGB primaries' dominant wavelengths)
extern const std::array<GaussianBand, 3> DEFAULT_RGB_GAUSSIANS;

inline constexpr float DEFAULT_DESIGN_WAVELENGTH = 587.6f;
inline constexpr float SPECTRAL_LAMBDA_MIN = 360.f;
inline constexpr float SPECTRAL_LAMBDA_MAX = 830.f;
// the widest range a render may ask for: the XUV's 1 nm to the far infrared
inline constexpr float SPECTRAL_RANGE_FLOOR = 1.f;
inline constexpr float SPECTRAL_RANGE_CEILING = 20000.f;

struct SpectralSettings {
	SpectralRepresentation representation { SpectralRepresentation::ExactRgb };
	// bands and hero: 4, 8, 16 or 32. The two RGB-like modes fix their own
	uint32_t lanes { 4 };
	// bands and hero, and the spectral images: the range in nm
	float lambdaMin { SPECTRAL_LAMBDA_MIN };
	float lambdaMax { SPECTRAL_LAMBDA_MAX };
	float monochromaticWavelength { DEFAULT_DESIGN_WAVELENGTH };
	// what exact RGB reads a dispersive glass's index at, and what an optical design is specified at (the
	// helium d line by default)
	float designWavelength { DEFAULT_DESIGN_WAVELENGTH };
	BandBasis bandBasis { BandBasis::Box };
	std::array<GaussianBand, 3> gaussians { DEFAULT_RGB_GAUSSIANS };
	WavelengthSampling sampling { WavelengthSampling::Visible };
	// Lights sampling: the share of the mixture the emitters take; the rest is the base density
	float lightSamplingShare { 0.5f };
	// the space the scene's RGB values are in
	ColorSpaceId colorSpace { ColorSpaceId::Srgb };
	// THE SPECTRAL IMAGES (pbrt's SpectralFilm): keep the mean spectral radiance of each pixel in this many wavelength
	// buckets over the range, beside the picture, for the EXR export's spectral channels. 0 keeps none. Hero
	// wavelengths bin each lane into its bucket; the bands are their own buckets whatever the count
	uint32_t imageBuckets { 0 };

	bool operator==(const SpectralSettings&) const = default;
};

// the buckets a render keeps: 0 when it keeps no spectral images or carries no wavelengths to bin (exact RGB,
// monochromatic), the band count for a band representation, the setting for hero wavelengths (at most 256)
uint32_t spectralImageBuckets(const SpectralSettings& settings);
// a bucket's wavelength range and center, nm
glm::vec2 spectralBucketRange(const SpectralSettings& settings, uint32_t bucket);

// the settings as a render can use them: a lane count of 4, 8, 16 or 32, wavelengths inside 1 to 20000 nm, a range of
// at least 1 nm, Gaussian bands at least 5 nm wide, a share in [0, 0.99]
SpectralSettings clampSpectralSettings(SpectralSettings settings);

const char* spectralRepresentationKey(SpectralRepresentation representation);
const char* spectralRepresentationName(SpectralRepresentation representation);
const char* wavelengthSamplingKey(WavelengthSampling sampling);
const char* bandBasisKey(BandBasis basis);

// the build a setting runs: 0 for exact RGB, else the lane count (1 for monochromatic, 4 for RGB Gaussians)
uint32_t laneFamily(const SpectralSettings& settings);
// the lane families every lane-dependent kernel is built in, beside exact RGB: VKGUIDE_SPECTRAL_FAMILIES in CMake
inline constexpr uint32_t SPECTRAL_FAMILIES[] = { 1, 4, 8, 16, 32 };
// the .spv suffix of a family: "" for exact RGB, ".lanes4" and so on
std::string laneFamilySuffix(uint32_t family);
// the number of lanes a representation uses of its family's: 3 for RGB Gaussians, the band count or lanes otherwise
uint32_t activeLanes(const SpectralSettings& settings);
// the range a representation covers: [l, l] for monochromatic
glm::vec2 spectralRange(const SpectralSettings& settings);

// The display of a monochromatic render: it is a property of how the picture is shown, not of the render, so it
// changes nothing but the display image (the tonemap pass)
enum class MonochromeDisplay : uint32_t {
	Grayscale = 0,
	// the radiance times the wavelength's own color (wavelengthDisplayColor())
	WavelengthColor = 1,
	// the radiance through a colormap, linear or logarithmic
	Colormap = 2,
	Count
};
enum class Colormap : uint32_t {
	Viridis = 0,
	Inferno = 1,
	Turbo = 2,
	Count
};
struct SpectralDisplay {
	MonochromeDisplay mode { MonochromeDisplay::Grayscale };
	Colormap colormap { Colormap::Viridis };
	// the colormap's input: exposure x radiance clamped to [0, 1], or its log10 over `decades` below 1
	bool logarithmic { false };
	float decades { 4.f };
	// the colors wavelengths outside the visible range are shown in
	glm::vec3 infraredColor { 0.55f, 0.f, 0.f };
	glm::vec3 ultravioletColor { 0.45f, 0.f, 0.75f };
	// below 100 nm
	glm::vec3 xuvColor { 0.2f, 0.6f, 1.f };

	bool operator==(const SpectralDisplay&) const = default;
};
const char* monochromeDisplayKey(MonochromeDisplay mode);
const char* colormapKey(Colormap map);

// the linear sRGB color a single wavelength is shown in: the CIE 1931 color of a line at lambda, desaturated into
// the gamut and scaled so its largest channel is 1, from 380 to 780 nm; the display's fallback colors outside,
// blended over the last 20 nm so the edge of the visible range does not jump
glm::vec3 wavelengthDisplayColor(float lambda, const SpectralDisplay& display);
// the colormap at t in [0, 1], sRGB-encoded (the display image's values), from the polynomial fits the tonemap uses
glm::vec3 colormapColor(Colormap map, float t);

//---------------------------------------------------------------- wavelengths

// PBR 4ed 5.4.2: the visible-importance density and its inverse CDF
float sampleVisibleWavelength(float u);
float visibleWavelengthPdf(float lambda);

// the emitters' part of the Lights sampling mixture, as SpectralData holds it: a piecewise-constant density over the
// range for the smooth spectra, and truncated Cauchy densities (closed-form CDF) for up to MAX_SAMPLING_LINES lines
inline constexpr uint32_t WAVELENGTH_TABLE_BINS = 512;
inline constexpr uint32_t MAX_SAMPLING_LINES = 8;
struct SamplingLine {
	float center { 0.f };
	float halfWidth { 0.f };
	// the share of the emitters' part
	float weight { 0.f };
};
struct WavelengthDistribution {
	// share of the whole mixture the base density keeps (1 when the scene has no emitter spectra to follow)
	float baseShare { 1.f };
	// share of the emitters' part the table takes; the lines share the rest by their weights
	float tableShare { 0.f };
	// WAVELENGTH_TABLE_BINS + 1 entries, 0 to 1, and the density per bin (integrating to 1 over the range)
	std::vector<float> cdf;
	std::vector<float> density;
	std::vector<SamplingLine> lines;
};
// what the Lights sampling follows: each emitter's spectrum (in the Illuminant role) weighted by its power
struct EmitterSpectrum {
	SpectrumRef spectrum;
	float power { 0.f };
};
WavelengthDistribution buildWavelengthDistribution(std::span<const EmitterSpectrum> emitters, const SpectralSettings& settings, const SpectrumContext& context);

// one path's wavelengths and their densities: the hero mirror of pbrt's SampledWavelengths. Lane i is drawn from
// u + i / lanes, wrapped. Monochromatic and the bands have fixed wavelengths and densities of 1
struct SampledWavelengths {
	std::array<float, 32> lambda {};
	std::array<float, 32> pdf {};
	uint32_t count { 0 };
};
SampledWavelengths sampleWavelengths(float u, const SpectralSettings& settings, const WavelengthDistribution& distribution);
// the mixture's density at lambda (Lights sampling), or the base density's
float wavelengthPdf(float lambda, const SpectralSettings& settings, const WavelengthDistribution& distribution);

//---------------------------------------------------------------- bands

// One band of a band representation: its center (where a scalar such as an index is read), its basis, and the
// quadrature that projects a spectrum the GPU only knows at run time (a textured color) onto it
inline constexpr uint32_t BAND_QUADRATURE = 4;
struct Band {
	float center { 0.f };
	// box: [lo, hi]. Gaussian: center and sigma
	float lo { 0.f };
	float hi { 0.f };
	float sigma { 0.f };
	std::array<float, BAND_QUADRATURE> nodes {};
	std::array<float, BAND_QUADRATURE> weights {};
	// the band's column of the band-to-film matrix: its share of the reconstructed spectrum against the CIE
	// functions, in linear sRGB
	glm::vec3 toRgb { 0.f };
};
// the bands of a band representation (RGB Gaussians, spectral bands), in lane order
std::vector<Band> buildBands(const SpectralSettings& settings);
// a spectrum's value in a band: the average over it, weighted by the basis (a box or the Gaussian). Reconstructing
// from the averages uses the partition of unity sum(phi_b) = 1, so a constant spectrum comes back exactly
double projectOntoBand(const std::function<double(double)>& f, const Band& band);

//---------------------------------------------------------------- to the film

// the lanes of one contribution as linear sRGB, as filmAdd() in crt_spectrum.glsl adds them. `values` holds one per
// active lane
glm::vec3 lanesToRgb(std::span<const float> values, const SampledWavelengths& wavelengths, const SpectralSettings& settings, std::span<const Band> bands);

//---------------------------------------------------------------- SpectralData, binding 24

// The buffer crt_spectrum.glsl reads, in 32-bit words: a header of SPECTRAL_HEADER_WORDS, then the tables it points
// to, then the pool of packed SpectrumRefs. Offsets are word indices into the whole buffer
inline constexpr uint32_t SPECTRAL_HEADER_WORDS = 64;
inline constexpr uint32_t SPECTRAL_NONE = 0xFFFFFFFFu;
// the header's words, crt_spectrum.glsl CRT_SH_*
enum SpectralHeaderWord : uint32_t {
	SH_REPRESENTATION = 0,
	SH_LANES = 1,
	SH_SAMPLING = 2,
	SH_COLOR_SPACE = 3,
	SH_LAMBDA_MIN = 4,
	SH_LAMBDA_MAX = 5,
	SH_MONO_LAMBDA = 6,
	SH_BAND_COUNT = 7,
	SH_CIE = 8,
	SH_ILLUMINANT = 9,
	SH_BANDS = 10,
	SH_DISTRIBUTION = 11,
	SH_MATERIALS = 12,
	SH_BACKGROUND = 13,
	SH_UPLIFT_RES = 14,
	SH_IMAGE_BUCKETS = 15,
	// 9 floats each, column-major: linear sRGB to the working space (textures, the environment map, the sky), and
	// XYZ to the film's linear sRGB
	SH_SRGB_TO_WORKING = 16,
	SH_XYZ_TO_RGB = 25,
	SH_DESIGN_LAMBDA = 34,
	SH_POOL = 35,
	// the table of the scene's spectra: SceneSpectra index to offset
	SH_REFS = 36,
	SH_REF_COUNT = 37,
};
// a packed SpectrumRef's tag, its first word: crt_spectrum.glsl CRT_SPECTRUM_*, with the role in bits 8 to 15
enum SpectralTag : uint32_t {
	TAG_ZERO = 0,
	// rgb, c0, c1, c2, scale; then, for a band representation, the band values
	TAG_RGB = 1,
	TAG_CONSTANT = 2,
	// kelvin, factor (scale x normalization / peak)
	TAG_BLACKBODY = 3,
	// n, then n wavelengths, then n values (scale and normalization applied)
	TAG_TABULATED = 4,
	// formula, then DISPERSION_COEFFICIENTS coefficients
	TAG_DISPERSION = 5,
	// center, fwhm, profile, scale
	TAG_LINE = 6,
	// the band values alone: a band representation's projection, made on the CPU
	TAG_BANDS = 7,
};
// words of a material's spectral record: four reference slots and the emission's scale
inline constexpr uint32_t SPECTRAL_MATERIAL_WORDS = 8;
// what slot a material's reference goes in: rt_material.h PackedMaterial::spectra
enum SpectralSlot : uint32_t {
	SLOT_ALBEDO = 0,
	SLOT_EMISSION = 1,
	SLOT_AUX0 = 2,
	SLOT_AUX1 = 3,
	SLOT_COUNT = 4
};
inline constexpr uint32_t BAND_WORDS = 16;
inline constexpr uint32_t CIE_TABLE_SAMPLES = spectrum_tables::CIE_SAMPLES;

// THE SCENE'S SPECTRA: every (reference, role) the scene's materials and lights use, each once, in the order they were
// first added. Built with the scene's GPU records (rt_accel.h buildSceneAccel()), which name a spectrum by its index
// here, so the records do not change when the representation does; SpectralData maps each index to the reference
// packed for the render's representation
class SceneSpectra {
public:
	uint32_t add(const SpectrumRef& ref, SpectrumRole role);
	const std::vector<std::pair<SpectrumRef, SpectrumRole>>& list() const { return m_list; }
	bool operator==(const SceneSpectra& other) const { return m_list == other.m_list; }

private:
	std::vector<std::pair<SpectrumRef, SpectrumRole>> m_list;
	std::unordered_map<std::string, uint32_t> m_index;
};

// a material's spectral record as the scene holds it: SceneSpectra indices by slot (SPECTRAL_NONE for none), then the
// emission's scale as float bits, then padding
using MaterialSpectralRecord = std::array<uint32_t, SPECTRAL_MATERIAL_WORDS>;
MaterialSpectralRecord emptyMaterialSpectralRecord();

// Packs the SpectralData for one render: the settings' tables, the scene's spectra (a table of their offsets, by
// SceneSpectra index, then the packed references), the materials' records, and the background
class SpectralDataBuilder {
public:
	SpectralDataBuilder(const SpectralSettings& settings, const SpectrumContext& context, const WavelengthDistribution& distribution, uint32_t imageBuckets);
	// the reference's offset in the pool, packed for the representation; the same (reference, role) twice is packed once
	uint32_t add(const SpectrumRef& ref, SpectrumRole role);
	void setScene(const SceneSpectra& spectra, std::span<const MaterialSpectralRecord> materials);
	void setBackground(const SpectrumRef& ref);
	const std::vector<Band>& bands() const { return m_bands; }
	std::vector<uint32_t> finish();

private:
	void putFloat(float value) { m_words.push_back(floatBits(value)); }
	static uint32_t floatBits(float value);

	SpectralSettings m_settings;
	SpectrumContext m_context;
	std::vector<Band> m_bands;
	std::vector<uint32_t> m_words;
	std::vector<uint32_t> m_refTable;
	std::vector<MaterialSpectralRecord> m_materials;
	uint32_t m_background { SPECTRAL_NONE };
	std::map<std::pair<std::string, uint32_t>, uint32_t> m_interned;
	uint32_t m_poolStart { 0 };
};

// a reference's packed words as add() writes them, for the tests
std::vector<uint32_t> packSpectrumRef(const SpectrumRef& ref, SpectrumRole role, const SpectralSettings& settings, const SpectrumContext& context, std::span<const Band> bands);
// a packed reference evaluated at a lane, as crt_spectrum.glsl's spectrumLane() does: lambda for a pointwise
// representation, the band for a band one (where `tint` multiplies an RGB value, as a texture does)
float evalPackedLane(std::span<const uint32_t> words, uint32_t lane, float lambda, const SpectralSettings& settings, const SpectrumContext& context,
	std::span<const Band> bands, glm::vec3 tint = glm::vec3(1.f));
