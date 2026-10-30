#pragma once

// OPTICAL GLASSES (docs/plans/completed/physoptics-optical-materials.md), part of rt_optics: what an optical body's media are
// made of. A glass has a dispersion formula from the Zemax AGF catalog format, the range it is valid over, and its
// internal transmittance, from which the absorption coefficient alpha(lambda) = -ln(T) / d follows. The surrounding
// medium is a glass too: air by Ciddor's formula, or vacuum.
//
// The reader takes the AGF files the glass makers publish (Schott, Ohara, CDGM, Hoya, Sumita), in ASCII or in the
// UTF-16 OpticStudio writes. assets/glass/bundled.agf holds the glasses the bundled elements are sold in.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <spectrum_ref.h>

// the helium d line and the hydrogen F and C lines, in nm: where catalogs give nd and the Abbe number
inline constexpr double LAMBDA_D = 587.5618;
inline constexpr double LAMBDA_F = 486.1327;
inline constexpr double LAMBDA_C = 656.2725;

// the names the surrounding media go by
inline constexpr const char* GLASS_AIR = "AIR";
inline constexpr const char* GLASS_VACUUM = "VACUUM";

struct OpticGlass {
	// unique within the library: what an element and a scene file name it by
	std::string name;
	// the catalog it came from ("bundled", a file's stem, or "builtin" for air and vacuum)
	std::string catalog;
	// the index: a Dispersion reference for a catalog glass, tabulated for air, constant for vacuum
	SpectrumRef index { constantRef(1.f) };
	// the AGF LD line, in nm; 0 for unknown
	glm::dvec2 range { 0.0 };
	// the catalog's own nd and vd (the NM line), for the panel and the tests
	double catalogNd { 0.0 };
	double catalogVd { 0.0 };
	// the AGF IT lines: (lambda nm, internal transmittance, thickness m)
	std::vector<glm::dvec3> transmittance;
	// alpha(lambda) in 1/m: tabulated from the IT lines, or constant 0 for a glass without them
	SpectrumRef absorption { constantRef(0.f) };
	// the TD line, kept as text and saved back but not used: no plan models temperature
	std::string thermal;

	bool operator==(const OpticGlass&) const = default;
};

// the index at lambda nm
double glassIndex(const OpticGlass& glass, double lambda);
// the absorption coefficient at lambda nm, in 1/m
double glassAbsorption(const OpticGlass& glass, double lambda);
// (nd - 1) / (nF - nC) from the glass's own formula
double abbeNumber(const OpticGlass& glass);
// whether lambda nm is outside the glass's valid range (a known range only)
bool outsideRange(const OpticGlass& glass, double lambda);

// Ciddor 1996: standard air at 15 C, 101 325 Pa, 0 % humidity and 450 ppm CO2 (n = 1.000 276 at 633 nm)
double ciddorAirIndex(double lambda);
OpticGlass airGlass();
OpticGlass vacuumGlass();

struct GlassCatalog {
	std::string name;
	std::filesystem::path path;
	std::vector<OpticGlass> glasses;
};

// an AGF catalog from text, or from a file (ASCII or UTF-16). `catalogName` names its glasses' catalog. Lines this
// reader does not use are skipped; a glass without a CD line, or with a formula outside the set, is skipped and named
// in `warnings`
std::optional<GlassCatalog> parseAgf(const std::string& text, const std::string& catalogName, std::vector<std::string>& warnings, std::string* error = nullptr);
std::optional<GlassCatalog> readAgfFile(const std::filesystem::path& path, std::vector<std::string>& warnings, std::string* error = nullptr);
// the internal transmittance lines as alpha(lambda): the mean of -ln(T)/d over the lines at each wavelength
SpectrumRef absorptionFromTransmittance(const std::vector<glm::dvec3>& lines);
