#pragma once

// COATINGS (docs/plans/completed/physoptics-optical-materials.md), part of rt_optics: what replaces a surface's bare Fresnel
// reflection and transmission. A coating answers one question for a wavelength, an angle of incidence in the incident
// medium and a polarization: the complex amplitudes r and t, from which R = |r|^2, T and the absorptance A = 1 - R - T
// follow. The amplitudes are kept for the polarization plan and the wave backend; the renderer reads the powers.
//
// A coating sits on a surface of an optical body, between the surface's two media (src/optic_surface.h). Its layers are
// listed FROM THE PLUS SIDE (the side the surface's normal points to, the air side of a lens or a mirror) toward the
// minus side, and a ray arriving from the minus side meets them in reverse. The kinds:
//
//   Uncoated   exact Fresnel between the two media
//   Ideal      R and T per polarization, the same at every wavelength and angle (a perfect 50:50 splitter, a mirror)
//   Tabulated  measured R and T against wavelength, at one or more angles (a vendor's curve, from a CSV file)
//   Stack      thin films, the transfer-matrix method (Macleod, Thin-film optical filters, ch. 2)
//   Metal      a metal's eta and k (pbrt's named spectra) under an optional protective stack, opaque
//
// The transfer matrix runs here in double complex arithmetic; crt_tmm.glsl evaluates the same stacks in float for
// Exact coatings, and coatingTable() tabulates any coating for the GPU's interpolated lookup (crt_coating.glsl).

#include <array>
#include <complex>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <spectrum_ref.h>

// the values are saved by name (coatingKindName()); the GPU never sees them, only a coating's table
enum class CoatingKind : uint8_t {
	Uncoated,
	Ideal,
	Tabulated,
	Stack,
	Metal
};

struct CoatingLayer {
	// a glass of the library by name: the layer's index
	std::string material;
	// the physical thickness in meters, used when quarterWaves is 0
	double thickness { 0.0 };
	// the optical thickness in quarter waves at the coating's design wavelength, at normal incidence; 0 to use thickness
	double quarterWaves { 0.0 };
	bool operator==(const CoatingLayer&) const = default;
};

// one measured curve: at an angle of incidence, R and T against wavelength, for s and p (the average in both when the
// data has no polarization). A curve that gives only R has T = 1 - R, and one that gives only T has R = 1 - T
struct CoatingCurve {
	// radians, in the incident medium
	double angle { 0.0 };
	std::vector<double> lambda;
	std::vector<double> rs, rp, ts, tp;
	bool operator==(const CoatingCurve&) const = default;
};

struct OpticCoating {
	// unique within the library, saved in scene files; never renamed once a scene uses it
	std::string key;
	std::string name;
	CoatingKind kind { CoatingKind::Uncoated };
	// a bundled preset: read-only, duplicable
	bool builtin { false };
	// where the design comes from, for the panel: the published curve it approximates, or the file it was read from
	std::string source;
	// the splitter treats s and p differently: without polarization the renderer uses their average, and the panel warns
	bool polarizing { false };
	// Ideal
	double rs { 0.0 }, rp { 0.0 }, ts { 1.0 }, tp { 1.0 };
	// Tabulated: one curve per angle, increasing in angle
	std::vector<CoatingCurve> curves;
	// Stack and Metal: the layers from the plus side
	std::vector<CoatingLayer> layers;
	// nm: what quarterWaves are measured at
	double designWavelength { 550.0 };
	// Metal: the metal's symbol in pbrt's named spectra ("Ag", "Al", "Au", "Cu"): metal-<symbol>-eta and -k
	std::string metal;

	bool operator==(const OpticCoating&) const = default;
};

const char* coatingKindName(CoatingKind kind);
std::optional<CoatingKind> findCoatingKind(std::string_view name);

// The response at one wavelength and angle: amplitudes (Macleod's sign conventions, N = n - ik) and powers
struct CoatingResponse {
	std::complex<double> rs, rp, ts, tp;
	double Rs { 0.0 }, Rp { 0.0 }, Ts { 0.0 }, Tp { 0.0 };
	double reflectance() const { return 0.5 * (Rs + Rp); }
	double transmittance() const { return 0.5 * (Ts + Tp); }
};

// a medium's complex index n + ik at lambda nm, by glass name: what a coating's layers and its surface's media resolve to
using IndexLookup = std::function<std::complex<double>(std::string_view material, double lambda)>;

// The coating between the media `nPlus` and `nMinus` (n + ik), at lambda nm, for a ray arriving from the plus side
// (`fromPlus`) or the minus side at `theta` radians from the normal in the medium it arrives in. Uncoated is the exact
// Fresnel term; a total internal reflection gives R = 1
CoatingResponse evaluateCoating(const OpticCoating& coating, const IndexLookup& index, double lambda, double theta, std::complex<double> nPlus, std::complex<double> nMinus, bool fromPlus);

// The transfer-matrix method itself: a ray in the incident medium n0 (real part used) at theta, through `layers` (each
// an index n + ik and a physical thickness in meters), onto the substrate ns. Exposed for the tests and crt_tmm.glsl's
// mirror
struct ThinFilm {
	std::complex<double> index;
	double thickness;
};
CoatingResponse transferMatrix(std::complex<double> n0, const std::vector<ThinFilm>& layers, std::complex<double> ns, double lambda, double theta);

// the layers' physical thicknesses: a quarter-wave layer is quarterWaves lambda0 / (4 n(lambda0))
std::vector<ThinFilm> resolveLayers(const OpticCoating& coating, const IndexLookup& index, double lambda);

// THE BUNDLED PRESETS: Thorlabs' common coatings as designs that approximate their published curves (-E02, -P01, -G01,
// -F01, the -A, -B and -C broadband AR coatings, the BSW and BSN plate splitters), and the ideal coatings (a 50:50
// splitter, a polarizing splitter, a mirror, an absorber, a perfect AR)
std::vector<OpticCoating> presetCoatings();

// A tabulated coating from a CSV file of wavelength (nm) and one column per polarization (s, p) or one averaged
// column, in percent or as fractions (a column above 1.5 is percent). `transmission` says the values are T rather than R;
// `angle` is the angle of incidence, radians
std::optional<OpticCoating> readCoatingCsv(const std::string& text, bool transmission, double angle, std::string* error);

// THE GPU TABLE of one coating on one pair of media: R and T for s and p over a wavelength grid and 91 cosines, for each
// of the two directions (from the plus side, then from the minus side). Each direction samples the cosine IN THE LOWER-
// INDEX MEDIUM of the pair, u = 1 - cos(pi j / 180) for j = 0 to 90, dense near 0: the incident cosine for a ray entering a denser medium, the transmitted
// one for a ray leaving it. In both R and T are smooth in that cosine, where in the incident angle the reflectance from
// inside glass climbs with an infinite slope to total internal reflection, which no 1-degree grid follows. Past the
// critical angle the lookup returns total reflection. A metal is opaque: it is sampled by the incident cosine.
// `wavelengths` are the columns; in exact RGB coatingTableRgb() makes three, the response projected onto red, green and
// blue as a reflectance spectrum is
inline constexpr uint32_t COATING_TABLE_ANGLES = 91;
struct CoatingTable {
	uint32_t columns { 0 };
	// the wavelength of each column, nm; empty for an RGB table
	std::vector<float> wavelengths;
	// per direction: 1 when it samples the transmitted cosine (it leaves the denser medium), 0 the incident one
	std::array<uint32_t, 2> transmittedAxis { 0u, 0u };
	// [direction][column][cosine], (Rs, Rp, Ts, Tp)
	std::vector<glm::vec4> data;
};
CoatingTable coatingTable(const OpticCoating& coating, const IndexLookup& index, const std::string& plusMedium, const std::string& minusMedium,
	const std::vector<float>& wavelengths);
// the same over 360 to 830 nm, projected to exact RGB's three columns by the spectral plan's projection of a reflectance;
// the transmitted cosine is found at `designWavelength`
CoatingTable coatingTableRgb(const OpticCoating& coating, const IndexLookup& index, const std::string& plusMedium, const std::string& minusMedium,
	const SpectrumContext& context, double designWavelength);

// The GPU's lookup, crt_coating.glsl coatingLookup() line for line: the incident cosine and the ratio eta = n_incident /
// n_other at the column's wavelength give the table's cosine, then bilinear in it and in the fractional column
glm::vec4 lookupCoatingTable(const CoatingTable& table, uint32_t direction, float column, float cosIncident, float eta);
