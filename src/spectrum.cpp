#include <spectrum.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

using namespace spectrum_tables;

namespace {

const DenseSpectrum& matching(int axis)
{
	static const std::array<DenseSpectrum, 3> functions = [] {
		std::array<DenseSpectrum, 3> result;
		for (int i = 0; i < CIE_SAMPLES; i++) {
			result[0].values[i] = CIE_X[i];
			result[1].values[i] = CIE_Y[i];
			result[2].values[i] = CIE_Z[i];
		}
		return result;
	}();
	return functions[axis];
}

// Planck's law in W / (sr m^2 m) at lambda nm, as pbrt's Blackbody()
double planck(double lambda, double kelvin)
{
	if (kelvin <= 0.0) {
		return 0.0;
	}
	const double c = 299792458.0;
	const double h = 6.62606957e-34;
	const double kb = 1.3806488e-23;
	const double l = lambda * 1e-9;
	return (2.0 * h * c * c) / (std::pow(l, 5.0) * (std::exp((h * c) / (l * kb * kelvin)) - 1.0));
}

// pbrt's RGBColorSpace constructor for sRGB: the primaries' chromaticities and the white of its D65 spectrum
glm::dmat3 srgbFromXyz()
{
	static const glm::dmat3 matrix = [] {
		//Y is 1: d65Spectrum() is normalized
		const glm::dvec3 w = spectrumToXyz(d65Spectrum());
		auto fromxyY = [](double x, double y) { return glm::dvec3(x / y, 1.0, (1.0 - x - y) / y); };
		const glm::dvec3 r = fromxyY(0.64, 0.33);
		const glm::dvec3 g = fromxyY(0.30, 0.60);
		const glm::dvec3 b = fromxyY(0.15, 0.06);
		// glm is column-major: the columns are the primaries' XYZ
		const glm::dmat3 rgb(r, g, b);
		const glm::dvec3 c = glm::inverse(rgb) * w;
		const glm::dmat3 xyzFromRgb(r * c.x, g * c.y, b * c.z);
		return glm::inverse(xyzFromRgb);
	}();
	return matrix;
}

}

double DenseSpectrum::operator()(int lambda) const
{
	if (lambda < LAMBDA_MIN || lambda > LAMBDA_MAX) {
		return 0.0;
	}
	return values[lambda - LAMBDA_MIN];
}

DenseSpectrum DenseSpectrum::operator*(const DenseSpectrum& other) const
{
	DenseSpectrum result;
	for (int i = 0; i < CIE_SAMPLES; i++) {
		result.values[i] = values[i] * other.values[i];
	}
	return result;
}

DenseSpectrum DenseSpectrum::operator*(double scale) const
{
	DenseSpectrum result;
	for (int i = 0; i < CIE_SAMPLES; i++) {
		result.values[i] = values[i] * scale;
	}
	return result;
}

DenseSpectrum constantSpectrum(double value)
{
	DenseSpectrum s;
	s.values.fill(value);
	return s;
}

DenseSpectrum blackbodySpectrum(double kelvin)
{
	DenseSpectrum s;
	if (kelvin <= 0.0) {
		return s;
	}
	//Wien's displacement law gives the peak pbrt normalizes by
	const double peak = planck(2.8977721e-3 / kelvin * 1e9, kelvin);
	for (int i = 0; i < CIE_SAMPLES; i++) {
		s.values[i] = planck((double)(LAMBDA_MIN + i), kelvin) / peak;
	}
	return s;
}

DenseSpectrum piecewiseLinearSpectrum(std::span<const float> interleaved, bool normalize)
{
	DenseSpectrum s;
	const size_t n = interleaved.size() / 2;
	if (n == 0) {
		return s;
	}
	//FromInterleaved: the first and last values extend to just past 360 and 830
	std::vector<double> lambdas;
	std::vector<double> values;
	if (interleaved[0] > (float)LAMBDA_MIN) {
		lambdas.push_back(LAMBDA_MIN - 1);
		values.push_back(interleaved[1]);
	}
	for (size_t i = 0; i < n; i++) {
		lambdas.push_back(interleaved[2 * i]);
		values.push_back(interleaved[2 * i + 1]);
	}
	if (lambdas.back() < (double)LAMBDA_MAX) {
		lambdas.push_back(LAMBDA_MAX + 1);
		values.push_back(values.back());
	}
	for (int i = 0; i < CIE_SAMPLES; i++) {
		const double lambda = LAMBDA_MIN + i;
		if (lambda < lambdas.front() || lambda > lambdas.back()) {
			continue;
		}
		const size_t o = std::min<size_t>((size_t)(std::upper_bound(lambdas.begin(), lambdas.end(), lambda) - lambdas.begin()), lambdas.size() - 1) - 1;
		const double t = (lambda - lambdas[o]) / (lambdas[o + 1] - lambdas[o]);
		s.values[i] = (1.0 - t) * values[o] + t * values[o + 1];
	}
	if (normalize) {
		s = s * (CIE_Y_INTEGRAL / spectrumLuminance(s));
	}
	return s;
}

std::optional<DenseSpectrum> namedSpectrum(std::string_view name)
{
	for (size_t i = 0; i < NAMED_SPECTRUM_COUNT; i++) {
		const NamedSpectrumData& data = NAMED_SPECTRA[i];
		if (name == data.name) {
			return piecewiseLinearSpectrum(std::span(data.interleaved, data.count * 2), data.normalized);
		}
	}
	return std::nullopt;
}

std::optional<DenseSpectrum> readSpectrumFile(const std::filesystem::path& path, std::string* error)
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
	if (values.size() < 2 || values.size() % 2 != 0) {
		if (error != nullptr) {
			*error = path.string() + " does not hold (wavelength, value) pairs";
		}
		return std::nullopt;
	}
	return piecewiseLinearSpectrum(values, false);
}

double innerProduct(const DenseSpectrum& a, const DenseSpectrum& b)
{
	double sum = 0.0;
	for (int i = 0; i < CIE_SAMPLES; i++) {
		sum += a.values[i] * b.values[i];
	}
	return sum;
}

double spectrumLuminance(const DenseSpectrum& s)
{
	return innerProduct(matching(1), s);
}

glm::dvec3 spectrumToXyz(const DenseSpectrum& s)
{
	return glm::dvec3(innerProduct(matching(0), s), innerProduct(matching(1), s), innerProduct(matching(2), s)) / CIE_Y_INTEGRAL;
}

glm::dvec3 xyzToLinearSrgb(const glm::dvec3& xyz)
{
	return srgbFromXyz() * xyz;
}

const DenseSpectrum& d65Spectrum()
{
	static const DenseSpectrum d65 = *namedSpectrum("stdillum-D65");
	return d65;
}

glm::vec3 emissionToRgb(const DenseSpectrum& s)
{
	const double luminance = spectrumLuminance(s);
	if (luminance <= 0.0) {
		return glm::vec3(0.f);
	}
	//pbrt's film sums x-bar L over wavelength without dividing by CIE_Y_INTEGRAL, and pbrt divides the light by its
	//luminance: Y comes out exactly 1
	return glm::vec3(xyzToLinearSrgb(spectrumToXyz(s) * (CIE_Y_INTEGRAL / luminance)));
}

glm::vec3 reflectanceToRgb(const DenseSpectrum& s)
{
	return glm::vec3(xyzToLinearSrgb(spectrumToXyz(s * d65Spectrum())));
}

double luminanceWeightedMean(const DenseSpectrum& s)
{
	return spectrumLuminance(s) / spectrumLuminance(constantSpectrum(1.0));
}
