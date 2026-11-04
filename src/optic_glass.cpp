#include <optic_glass.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

#include <fmt/format.h>

double glassIndex(const OpticGlass& glass, double lambda)
{
	const double index = evalSpectrumRefRaw(glass.index, lambda);
	return glass.relativeToAir ? index * ciddorAirIndex(lambda) : index;
}

double indexRelativeToAir(const OpticGlass& glass, double lambda)
{
	const double index = evalSpectrumRefRaw(glass.index, lambda);
	return glass.relativeToAir ? index : index / ciddorAirIndex(lambda);
}

SpectrumRef absoluteIndexRef(const OpticGlass& glass)
{
	if (!glass.relativeToAir) {
		return glass.index;
	}
	std::vector<float> samples;
	samples.reserve(2 * 2301);
	for (int lambda = 200; lambda <= 2500; lambda++) {
		samples.push_back((float)lambda);
		samples.push_back((float)glassIndex(glass, (double)lambda));
	}
	return tabulatedRef(std::move(samples));
}

double glassAbsorption(const OpticGlass& glass, double lambda)
{
	return std::max(evalSpectrumRefRaw(glass.absorption, lambda), 0.0);
}

double abbeNumber(const OpticGlass& glass)
{
	// a catalog's, relative to air
	const double nF = indexRelativeToAir(glass, LAMBDA_F);
	const double nC = indexRelativeToAir(glass, LAMBDA_C);
	return nF != nC ? (indexRelativeToAir(glass, LAMBDA_D) - 1.0) / (nF - nC) : 0.0;
}

bool outsideRange(const OpticGlass& glass, double lambda)
{
	return glass.range.y > glass.range.x && (lambda < glass.range.x || lambda > glass.range.y);
}

double ciddorAirIndex(double lambda)
{
	// Ciddor 1996 equation (1), the standard-air term, sigma in inverse micrometers
	const double sigma = 1000.0 / lambda;
	const double s2 = sigma * sigma;
	return 1.0 + 1e-8 * (5792105.0 / (238.0185 - s2) + 167917.0 / (57.362 - s2));
}

OpticGlass airGlass()
{
	// tabulated every 5 nm from 200 nm to 2.5 um, so the GPU reads it as it reads any measured table; beyond the table
	// it holds the end values, which differ from the formula's by less than 1e-6
	std::vector<float> samples;
	for (int lambda = 200; lambda <= 2500; lambda += 5) {
		samples.push_back((float)lambda);
		samples.push_back((float)ciddorAirIndex((double)lambda));
	}
	OpticGlass air;
	air.name = GLASS_AIR;
	air.catalog = "builtin";
	air.relativeToAir = false;
	air.index = tabulatedRef(std::move(samples));
	air.range = glm::dvec2(230.0, 1690.0);
	air.catalogNd = ciddorAirIndex(LAMBDA_D);
	return air;
}

OpticGlass vacuumGlass()
{
	OpticGlass vacuum;
	vacuum.name = GLASS_VACUUM;
	vacuum.catalog = "builtin";
	vacuum.relativeToAir = false;
	vacuum.index = constantRef(1.f);
	vacuum.catalogNd = 1.0;
	return vacuum;
}

SpectrumRef absorptionFromTransmittance(const std::vector<glm::dvec3>& lines)
{
	std::map<double, std::pair<double, int>> byLambda;
	for (const glm::dvec3& line : lines) {
		if (!(line.y > 0.0) || !(line.z > 0.0)) {
			continue;
		}
		auto& entry = byLambda[line.x];
		entry.first += -std::log(std::min(line.y, 1.0)) / line.z;
		entry.second++;
	}
	if (byLambda.empty()) {
		return constantRef(0.f);
	}
	if (byLambda.size() == 1) {
		const auto& only = byLambda.begin()->second;
		return constantRef((float)(only.first / only.second));
	}
	std::vector<float> samples;
	for (const auto& [lambda, sum] : byLambda) {
		samples.push_back((float)lambda);
		samples.push_back((float)(sum.first / sum.second));
	}
	return tabulatedRef(std::move(samples));
}

namespace {

// an OpticStudio catalog is often UTF-16 little endian with a byte order mark; its glass names are ASCII
std::string toAscii(const std::string& bytes)
{
	const bool utf16le = bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE;
	const bool utf16be = bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFE && (unsigned char)bytes[1] == 0xFF;
	if (!utf16le && !utf16be) {
		return bytes;
	}
	std::string out;
	out.reserve(bytes.size() / 2);
	for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
		const unsigned char lo = (unsigned char)bytes[utf16le ? i : i + 1];
		const unsigned char hi = (unsigned char)bytes[utf16le ? i + 1 : i];
		out += hi == 0 ? (char)lo : '?';
	}
	return out;
}

bool knownFormula(int id)
{
	return (id >= 1 && id <= 13) || id == 100;
}

}

std::optional<GlassCatalog> parseAgf(const std::string& textIn, const std::string& catalogName, std::vector<std::string>& warnings, std::string* error)
{
	const std::string text = toAscii(textIn);
	GlassCatalog catalog;
	catalog.name = catalogName;

	// the glass being read, finished when the next NM line or the end arrives
	std::optional<OpticGlass> current;
	int formula = 0;
	std::vector<double> coefficients;
	bool hasCoefficients = false;
	auto finish = [&]() {
		if (!current) {
			return;
		}
		if (!knownFormula(formula)) {
			warnings.push_back(fmt::format("{}: dispersion formula {} is not one this reader knows; the glass is skipped", current->name, formula));
		} else if (!hasCoefficients) {
			warnings.push_back(fmt::format("{}: no CD line; the glass is skipped", current->name));
		} else {
			const int count = dispersionCoefficientCount((DispersionFormula)formula);
			coefficients.resize((size_t)std::max(count, 0), 0.0);
			current->index = dispersionRef((DispersionFormula)formula, coefficients, current->name);
			current->index.validRange = glm::vec2(current->range);
			current->absorption = absorptionFromTransmittance(current->transmittance);
			catalog.glasses.push_back(std::move(*current));
		}
		current.reset();
	};

	std::istringstream lines(text);
	std::string line;
	int lineNumber = 0;
	while (std::getline(lines, line)) {
		lineNumber++;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		std::istringstream fields(line);
		std::string tag;
		if (!(fields >> tag)) {
			continue;
		}
		if (tag == "NM") {
			finish();
			OpticGlass glass;
			glass.catalog = catalogName;
			std::string mil;
			double formulaValue = 0.0;
			if (!(fields >> glass.name >> formulaValue)) {
				warnings.push_back(fmt::format("line {}: an NM line without a name and formula", lineNumber));
				continue;
			}
			fields >> mil >> glass.catalogNd >> glass.catalogVd;
			formula = (int)formulaValue;
			coefficients.clear();
			hasCoefficients = false;
			current = std::move(glass);
		} else if (!current) {
			continue;
		} else if (tag == "CD") {
			double value;
			while (fields >> value) {
				coefficients.push_back(value);
			}
			hasCoefficients = true;
		} else if (tag == "LD") {
			double lo = 0.0;
			double hi = 0.0;
			if (fields >> lo >> hi) {
				current->range = glm::dvec2(lo * 1000.0, hi * 1000.0);
			}
		} else if (tag == "IT") {
			double lambda = 0.0;
			double transmittance = 0.0;
			double thickness = 0.0;
			if (fields >> lambda >> transmittance >> thickness) {
				current->transmittance.emplace_back(lambda * 1000.0, transmittance, thickness * 1e-3);
			}
		} else if (tag == "TD") {
			const size_t start = line.find("TD");
			current->thermal = line.substr(start + 2);
			current->thermal.erase(0, current->thermal.find_first_not_of(" \t"));
		}
		// CC, GC, ED, OD and the rest are not needed
	}
	finish();
	if (catalog.glasses.empty()) {
		if (error != nullptr) {
			*error = "no glass in the catalog could be read";
		}
		return std::nullopt;
	}
	return catalog;
}

std::optional<GlassCatalog> readAgfFile(const std::filesystem::path& path, std::vector<std::string>& warnings, std::string* error)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		if (error != nullptr) {
			*error = fmt::format("cannot open {}", path.string());
		}
		return std::nullopt;
	}
	std::ostringstream bytes;
	bytes << file.rdbuf();
	std::optional<GlassCatalog> catalog = parseAgf(bytes.str(), path.stem().string(), warnings, error);
	if (catalog) {
		catalog->path = path;
	}
	return catalog;
}
