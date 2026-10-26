#include <spectrum_ref.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

using namespace spectrum_tables;

namespace {

constexpr double PI = 3.14159265358979323846;

// Planck's law in W / (sr m^2 m) at lambda nm, as pbrt's Blackbody()
double planck(double lambda, double kelvin)
{
	if (kelvin <= 0.0 || lambda <= 0.0) {
		return 0.0;
	}
	const double c = 299792458.0;
	const double h = 6.62606957e-34;
	const double kb = 1.3806488e-23;
	const double l = lambda * 1e-9;
	return (2.0 * h * c * c) / (std::pow(l, 5.0) * (std::exp((h * c) / (l * kb * kelvin)) - 1.0));
}

// linear between the pairs, the end values held beyond them
double piecewiseLinear(const std::vector<float>& samples, double lambda)
{
	const size_t n = samples.size() / 2;
	if (n == 0) {
		return 0.0;
	}
	if (lambda <= samples[0]) {
		return samples[1];
	}
	if (lambda >= samples[2 * (n - 1)]) {
		return samples[2 * (n - 1) + 1];
	}
	size_t lo = 0;
	size_t hi = n - 1;
	while (hi - lo > 1) {
		const size_t middle = (lo + hi) / 2;
		if (samples[2 * middle] <= lambda) {
			lo = middle;
		} else {
			hi = middle;
		}
	}
	const double l0 = samples[2 * lo];
	const double l1 = samples[2 * hi];
	const double t = (lambda - l0) / (l1 - l0);
	return (1.0 - t) * samples[2 * lo + 1] + t * samples[2 * hi + 1];
}

double sellmeierTerms(std::span<const double> c, int terms, double l2)
{
	double sum = 0.0;
	for (int i = 0; i < terms; i++) {
		sum += c[2 * i] * l2 / (l2 - c[2 * i + 1]);
	}
	return sum;
}

}

SpectrumRef rgbSpectrum(glm::vec3 rgb)
{
	SpectrumRef ref;
	ref.kind = SpectrumKind::Rgb;
	ref.rgb = rgb;
	return ref;
}

SpectrumRef constantRef(float value)
{
	SpectrumRef ref;
	ref.kind = SpectrumKind::Constant;
	ref.value = value;
	return ref;
}

SpectrumRef blackbodyRef(float kelvin, float scale)
{
	SpectrumRef ref;
	ref.kind = SpectrumKind::Blackbody;
	ref.value = kelvin;
	ref.scale = scale;
	return ref;
}

std::optional<SpectrumRef> namedRef(std::string_view name, float scale)
{
	for (size_t i = 0; i < NAMED_SPECTRUM_COUNT; i++) {
		const NamedSpectrumData& data = NAMED_SPECTRA[i];
		if (name != data.name) {
			continue;
		}
		SpectrumRef ref;
		ref.kind = SpectrumKind::Named;
		ref.name = data.name;
		ref.scale = scale;
		ref.samples.assign(data.interleaved, data.interleaved + 2 * data.count);
		//pbrt scales its illuminants to a luminance of 1 when it loads them; the samples carry that factor
		if (data.normalized) {
			const double factor = CIE_Y_INTEGRAL / spectrumLuminance(piecewiseLinearSpectrum(std::span(data.interleaved, 2 * data.count), false));
			for (size_t j = 1; j < ref.samples.size(); j += 2) {
				ref.samples[j] = (float)(ref.samples[j] * factor);
			}
		}
		return ref;
	}
	return std::nullopt;
}

SpectrumRef tabulatedRef(std::vector<float> interleaved, float scale)
{
	SpectrumRef ref;
	ref.kind = SpectrumKind::Tabulated;
	ref.samples = std::move(interleaved);
	ref.scale = scale;
	return ref;
}

std::optional<SpectrumRef> fileRef(const std::filesystem::path& path, std::string* error, float scale)
{
	std::ifstream file(path);
	if (!file) {
		if (error != nullptr) {
			*error = "cannot open " + path.string();
		}
		return std::nullopt;
	}
	std::vector<float> values;
	std::string line;
	while (std::getline(file, line)) {
		line = line.substr(0, line.find('#'));
		std::istringstream words(line);
		float value = 0.f;
		while (words >> value) {
			values.push_back(value);
		}
	}
	bool increasing = values.size() >= 4 && values.size() % 2 == 0;
	for (size_t i = 2; increasing && i < values.size(); i += 2) {
		increasing = values[i] > values[i - 2];
	}
	if (!increasing) {
		if (error != nullptr) {
			*error = path.string() + " does not hold two or more (wavelength, value) pairs in increasing wavelength";
		}
		return std::nullopt;
	}
	SpectrumRef ref = tabulatedRef(std::move(values), scale);
	ref.kind = SpectrumKind::File;
	ref.name = path.string();
	return ref;
}

SpectrumRef dispersionRef(DispersionFormula formula, std::span<const double> coefficients, std::string name)
{
	SpectrumRef ref;
	ref.kind = SpectrumKind::Dispersion;
	ref.formula = formula;
	for (size_t i = 0; i < std::min(coefficients.size(), DISPERSION_COEFFICIENTS); i++) {
		ref.coefficients[i] = coefficients[i];
	}
	ref.name = std::move(name);
	return ref;
}

SpectrumRef lineRef(float center, float fwhm, LineProfile profile, float scale)
{
	SpectrumRef ref;
	ref.kind = SpectrumKind::Line;
	ref.value = center;
	ref.width = fwhm;
	ref.profile = profile;
	ref.scale = scale;
	return ref;
}

const char* spectrumKindName(SpectrumKind kind)
{
	switch (kind) {
	case SpectrumKind::Rgb: return "rgb";
	case SpectrumKind::Constant: return "constant";
	case SpectrumKind::Blackbody: return "blackbody";
	case SpectrumKind::Named: return "named";
	case SpectrumKind::Tabulated: return "tabulated";
	case SpectrumKind::File: return "spd";
	case SpectrumKind::Dispersion: return "dispersion";
	case SpectrumKind::Line: return "line";
	}
	return "rgb";
}

const char* dispersionFormulaName(DispersionFormula formula)
{
	switch (formula) {
	case DispersionFormula::Schott: return "Schott";
	case DispersionFormula::Sellmeier1: return "Sellmeier 1";
	case DispersionFormula::Herzberger: return "Herzberger";
	case DispersionFormula::Sellmeier2: return "Sellmeier 2";
	case DispersionFormula::Conrady: return "Conrady";
	case DispersionFormula::Sellmeier3: return "Sellmeier 3";
	case DispersionFormula::Handbook1: return "Handbook of Optics 1";
	case DispersionFormula::Handbook2: return "Handbook of Optics 2";
	case DispersionFormula::Sellmeier4: return "Sellmeier 4";
	case DispersionFormula::Extended: return "Extended";
	case DispersionFormula::Sellmeier5: return "Sellmeier 5";
	case DispersionFormula::Extended2: return "Extended 2";
	case DispersionFormula::Extended3: return "Extended 3";
	case DispersionFormula::Cauchy: return "Cauchy";
	}
	return "Sellmeier 1";
}

int dispersionCoefficientCount(DispersionFormula formula)
{
	switch (formula) {
	case DispersionFormula::Schott: return 6;
	case DispersionFormula::Sellmeier1: return 6;
	case DispersionFormula::Herzberger: return 6;
	case DispersionFormula::Sellmeier2: return 5;
	case DispersionFormula::Conrady: return 3;
	case DispersionFormula::Sellmeier3: return 8;
	case DispersionFormula::Handbook1: return 4;
	case DispersionFormula::Handbook2: return 4;
	case DispersionFormula::Sellmeier4: return 5;
	case DispersionFormula::Extended: return 8;
	case DispersionFormula::Sellmeier5: return 10;
	case DispersionFormula::Extended2: return 8;
	case DispersionFormula::Extended3: return 9;
	case DispersionFormula::Cauchy: return 3;
	}
	return 0;
}

double evalDispersion(DispersionFormula formula, std::span<const double> c, double lambda)
{
	const double l = lambda * 1e-3;
	const double l2 = l * l;
	const double li2 = 1.0 / l2;
	double n2 = 1.0;
	switch (formula) {
	case DispersionFormula::Schott:
		n2 = c[0] + c[1] * l2 + li2 * (c[2] + li2 * (c[3] + li2 * (c[4] + li2 * c[5])));
		break;
	case DispersionFormula::Sellmeier1:
		n2 = 1.0 + sellmeierTerms(c, 3, l2);
		break;
	case DispersionFormula::Herzberger: {
		const double L = 1.0 / (l2 - 0.028);
		return c[0] + c[1] * L + c[2] * L * L + c[3] * l2 + c[4] * l2 * l2 + c[5] * l2 * l2 * l2;
	}
	case DispersionFormula::Sellmeier2:
		n2 = 1.0 + c[0] + c[1] * l2 / (l2 - c[2] * c[2]) + c[3] / (l2 - c[4] * c[4]);
		break;
	case DispersionFormula::Conrady:
		return c[0] + c[1] / l + c[2] / std::pow(l, 3.5);
	case DispersionFormula::Sellmeier3:
		n2 = 1.0 + sellmeierTerms(c, 4, l2);
		break;
	case DispersionFormula::Handbook1:
		n2 = c[0] + c[1] / (l2 - c[2]) - c[3] * l2;
		break;
	case DispersionFormula::Handbook2:
		n2 = c[0] + c[1] * l2 / (l2 - c[2]) - c[3] * l2;
		break;
	case DispersionFormula::Sellmeier4:
		n2 = c[0] + c[1] * l2 / (l2 - c[2]) + c[3] * l2 / (l2 - c[4]);
		break;
	case DispersionFormula::Extended:
		n2 = c[0] + c[1] * l2 + li2 * (c[2] + li2 * (c[3] + li2 * (c[4] + li2 * (c[5] + li2 * (c[6] + li2 * c[7])))));
		break;
	case DispersionFormula::Sellmeier5:
		n2 = 1.0 + sellmeierTerms(c, 5, l2);
		break;
	case DispersionFormula::Extended2:
		n2 = c[0] + c[1] * l2 + li2 * (c[2] + li2 * (c[3] + li2 * (c[4] + li2 * c[5]))) + c[6] * l2 * l2 + c[7] * l2 * l2 * l2;
		break;
	case DispersionFormula::Extended3:
		n2 = c[0] + c[1] * l2 + c[2] * l2 * l2 + li2 * (c[3] + li2 * (c[4] + li2 * (c[5] + li2 * (c[6] + li2 * (c[7] + li2 * c[8])))));
		break;
	case DispersionFormula::Cauchy:
		return c[0] + c[1] * li2 + c[2] * li2 * li2;
	}
	return std::sqrt(std::max(n2, 0.0));
}

double evalLineProfile(LineProfile profile, double center, double fwhm, double lambda)
{
	if (!(fwhm > 0.0)) {
		return 0.0;
	}
	const double x = lambda - center;
	if (profile == LineProfile::Lorentzian) {
		const double gamma = 0.5 * fwhm;
		return gamma / (PI * (x * x + gamma * gamma));
	}
	//FWHM = 2 sqrt(2 ln 2) sigma
	const double sigma = fwhm / 2.3548200450309493;
	return std::exp(-0.5 * x * x / (sigma * sigma)) / (sigma * std::sqrt(2.0 * PI));
}

double evalSpectrumRefRaw(const SpectrumRef& ref, double lambda)
{
	switch (ref.kind) {
	case SpectrumKind::Rgb:
		//without a table the raw value of an RGB is its mean, which no caller with a table sees
		return (ref.rgb.r + ref.rgb.g + ref.rgb.b) / 3.0;
	case SpectrumKind::Constant:
		return ref.value;
	case SpectrumKind::Blackbody: {
		if (ref.value <= 0.f) {
			return 0.0;
		}
		//normalized to 1 at the peak, which Wien's displacement law places
		const double peak = planck(2.8977721e-3 / ref.value * 1e9, ref.value);
		return ref.scale * planck(lambda, ref.value) / peak;
	}
	case SpectrumKind::Named:
	case SpectrumKind::Tabulated:
	case SpectrumKind::File:
		return ref.scale * piecewiseLinear(ref.samples, lambda);
	case SpectrumKind::Dispersion:
		return evalDispersion(ref.formula, ref.coefficients, lambda);
	case SpectrumKind::Line:
		return ref.scale * evalLineProfile(ref.profile, ref.value, ref.width, lambda);
	}
	return 0.0;
}

double illuminantNormalization(const SpectrumRef& ref)
{
	if (ref.kind == SpectrumKind::Rgb || ref.kind == SpectrumKind::Line) {
		return 1.0;
	}
	//the luminance of the unscaled spectrum, so that scale stays a brightness
	SpectrumRef unit = ref;
	unit.scale = 1.f;
	DenseSpectrum s;
	for (int i = 0; i < CIE_SAMPLES; i++) {
		s.values[i] = evalSpectrumRefRaw(unit, LAMBDA_MIN + i);
	}
	const double luminance = spectrumLuminance(s);
	return luminance > 0.0 ? CIE_Y_INTEGRAL / luminance : 1.0;
}

double evalSpectrumRef(const SpectrumRef& ref, SpectrumRole role, const SpectrumContext& context, double lambda)
{
	if (ref.kind == SpectrumKind::Rgb) {
		if (role == SpectrumRole::Scalar) {
			return scalarAt(ref, lambda);
		}
		if (context.table == nullptr) {
			return 0.0;
		}
		return evalUpliftedRgb(upliftRgb(*context.table, ref.rgb, role), role, context.space, lambda);
	}
	const double value = evalSpectrumRefRaw(ref, lambda);
	return role == SpectrumRole::Illuminant ? value * illuminantNormalization(ref) : value;
}

DenseSpectrum denseSpectrum(const SpectrumRef& ref, SpectrumRole role, const SpectrumContext& context)
{
	DenseSpectrum s;
	if (ref.kind == SpectrumKind::Rgb && context.table != nullptr && role != SpectrumRole::Scalar) {
		const UpliftedRgb u = upliftRgb(*context.table, ref.rgb, role);
		for (int i = 0; i < CIE_SAMPLES; i++) {
			s.values[i] = evalUpliftedRgb(u, role, context.space, LAMBDA_MIN + i);
		}
		return s;
	}
	const double normalization = role == SpectrumRole::Illuminant ? illuminantNormalization(ref) : 1.0;
	for (int i = 0; i < CIE_SAMPLES; i++) {
		s.values[i] = normalization * (ref.kind == SpectrumKind::Rgb ? scalarAt(ref, LAMBDA_MIN + i) : evalSpectrumRefRaw(ref, LAMBDA_MIN + i));
	}
	return s;
}

glm::vec3 exactRgb(const SpectrumRef& ref, SpectrumRole role, const SpectrumContext& context)
{
	if (ref.kind == SpectrumKind::Rgb) {
		if (context.space == ColorSpaceId::Srgb) {
			return ref.rgb;
		}
		return glm::vec3(convertRgb(context.space, ColorSpaceId::Srgb) * glm::dvec3(ref.rgb));
	}
	switch (role) {
	case SpectrumRole::Albedo: {
		//under the working space's white, so a reflectance of 1 everywhere is (1, 1, 1)
		const DenseSpectrum s = denseSpectrum(ref, role, context) * *colorSpace(context.space).illuminant;
		return glm::vec3(xyzToLinearSrgb(spectrumToXyz(s)));
	}
	case SpectrumRole::Illuminant:
	case SpectrumRole::Unbounded:
		//as pbrt's film sees it: XYZ over the integral of y-bar, so an emitter normalized to luminance 1 has Y = 1
		return glm::vec3(xyzToLinearSrgb(spectrumToXyz(denseSpectrum(ref, role, context))));
	case SpectrumRole::Scalar:
		return glm::vec3((float)luminanceWeightedMean(denseSpectrum(ref, role, context)));
	}
	return glm::vec3(0.f);
}

double scalarAt(const SpectrumRef& ref, double lambda)
{
	if (ref.kind == SpectrumKind::Rgb) {
		return 0.2126 * ref.rgb.r + 0.7152 * ref.rgb.g + 0.0722 * ref.rgb.b;
	}
	return evalSpectrumRefRaw(ref, lambda);
}

float spectrumRadiometricPrice(const SpectrumRef& ref)
{
	if (ref.kind == SpectrumKind::Line) {
		return std::max(ref.scale, 0.f) / (float)CIE_Y_INTEGRAL;
	}
	if (ref.kind == SpectrumKind::Rgb || ref.kind == SpectrumKind::Dispersion) {
		return 0.f;
	}
	//the midpoint rule on a logarithmic grid: 4000 steps from 1 nm to 20 um
	double sum = 0.0;
	const int steps = 4000;
	const double lo = std::log(1.0), hi = std::log(20000.0);
	for (int i = 0; i < steps; i++) {
		const double a = std::exp(lo + (hi - lo) * i / steps);
		const double b = std::exp(lo + (hi - lo) * (i + 1) / steps);
		sum += std::max(0.0, evalSpectrumRefRaw(ref, 0.5 * (a + b))) * (b - a);
	}
	return (float)(sum / CIE_Y_INTEGRAL);
}

bool isConstantSpectrum(const SpectrumRef& ref)
{
	switch (ref.kind) {
	case SpectrumKind::Constant:
		return true;
	case SpectrumKind::Rgb:
		return ref.rgb.r == ref.rgb.g && ref.rgb.g == ref.rgb.b;
	case SpectrumKind::Named:
	case SpectrumKind::Tabulated:
	case SpectrumKind::File:
		for (size_t i = 3; i < ref.samples.size(); i += 2) {
			if (ref.samples[i] != ref.samples[1]) {
				return false;
			}
		}
		return true;
	default:
		return false;
	}
}
