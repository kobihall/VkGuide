// CPU verification of the spectra behind the pbrt import (src/spectrum.h, static library rt_spectrum). No GPU.
//
// What the library promises:
//  - the CIE tables are pbrt's: Y sums to pbrt's CIE_Y_integral, every named spectrum pbrt-v4 knows is there
//  - pbrt's normalizations: an illuminant has luminance CIE_Y_integral, a blackbody peaks at 1 at Wien's wavelength
//  - the RGB is pbrt's sRGB: D65 light and a perfect white reflector are (1, 1, 1), and a light's RGB has
//    luminance 1 after pbrt divides it by its own luminance
//  - against pbrt-v4 itself: the pixel values pbrt rendered for a blackbody, a standard illuminant and a spectral
//    reflectance, seen head on through a box filter (tools/spectrum_tables/pbrt_reference.py made them)
//
// Run:  ./bin/spectrum_test

#include <spectrum.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <fmt/format.h>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
	if (!ok) {
		g_failures++;
		fmt::println("    FAIL: {}", what);
	}
}

bool near(double a, double b, double tolerance)
{
	return std::abs(a - b) <= tolerance * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}

bool near3(const glm::dvec3& a, const glm::dvec3& b, double tolerance)
{
	return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

std::string str(const glm::dvec3& v)
{
	return fmt::format("({:.6f}, {:.6f}, {:.6f})", v.x, v.y, v.z);
}

void testTables()
{
	fmt::println("tables");
	double ySum = 0.0;
	for (float y : spectrum_tables::CIE_Y) {
		ySum += y;
	}
	check(near(ySum, spectrum_tables::CIE_Y_INTEGRAL, 1e-6), fmt::format("the Y matching function sums to {}, pbrt's integral is {}", ySum, spectrum_tables::CIE_Y_INTEGRAL));
	check(spectrum_tables::NAMED_SPECTRUM_COUNT == 88, fmt::format("{} named spectra, pbrt-v4 has 88", spectrum_tables::NAMED_SPECTRUM_COUNT));
	for (size_t i = 0; i < spectrum_tables::NAMED_SPECTRUM_COUNT; i++) {
		const spectrum_tables::NamedSpectrumData& data = spectrum_tables::NAMED_SPECTRA[i];
		bool increasing = data.count >= 2;
		for (size_t j = 1; j < data.count; j++) {
			increasing = increasing && data.interleaved[2 * j] > data.interleaved[2 * j - 2];
		}
		check(increasing, fmt::format("{}: wavelengths increase", data.name));
		check(namedSpectrum(data.name).has_value(), fmt::format("{} resolves by name", data.name));
		if (data.normalized) {
			check(near(spectrumLuminance(*namedSpectrum(data.name)), spectrum_tables::CIE_Y_INTEGRAL, 1e-9), fmt::format("{} is normalized to luminance 1", data.name));
		}
	}
	check(!namedSpectrum("metal-Unobtainium-eta").has_value(), "an unknown name is not found");
}

void testNormalizations()
{
	fmt::println("normalizations");
	for (const double kelvin : { 1500.0, 2700.0, 6500.0, 12000.0 }) {
		const DenseSpectrum b = blackbodySpectrum(kelvin);
		double peak = 0.0;
		for (double v : b.values) {
			peak = std::max(peak, v);
		}
		const int wien = (int)std::lround(2.8977721e-3 / kelvin * 1e9);
		if (wien >= spectrum_tables::LAMBDA_MIN && wien <= spectrum_tables::LAMBDA_MAX) {
			check(near(peak, 1.0, 1e-4), fmt::format("blackbody {} K peaks at 1 inside the visible range, got {}", kelvin, peak));
		} else {
			check(peak < 1.0, fmt::format("blackbody {} K peaks outside the visible range, so below 1 in it", kelvin));
		}
		const glm::dvec3 rgb(emissionToRgb(b));
		const double luminance = 0.2126 * rgb.x + 0.7152 * rgb.y + 0.0722 * rgb.z;
		check(near(luminance, 1.0, 2e-3), fmt::format("blackbody {} K as light has luminance 1, got {}", kelvin, luminance));
	}
	check(near3(glm::dvec3(emissionToRgb(d65Spectrum())), glm::dvec3(1.0), 1e-6), fmt::format("D65 light is white, got {}", str(glm::dvec3(emissionToRgb(d65Spectrum())))));
	check(near3(glm::dvec3(reflectanceToRgb(constantSpectrum(1.0))), glm::dvec3(1.0), 1e-6), "a perfect reflector is white under D65");
	check(near3(glm::dvec3(reflectanceToRgb(constantSpectrum(0.25))), glm::dvec3(0.25), 1e-6), "a gray reflector keeps its value");
	check(near(luminanceWeightedMean(constantSpectrum(1.5)), 1.5, 1e-9), "the mean of a constant is the constant");
	const double bk7 = luminanceWeightedMean(*namedSpectrum("glass-BK7"));
	check(bk7 > 1.515 && bk7 < 1.52, fmt::format("glass-BK7's mean index is near its 1.5185 at 550 nm, got {}", bk7));
}

void testFiles()
{
	fmt::println("files");
	const std::filesystem::path path = std::filesystem::temp_directory_path() / "spectrum_test.spd";
	{
		std::ofstream out(path);
		out << "# a ramp\n300 0.0\n900 0.6 # end\n";
	}
	std::string error;
	const std::optional<DenseSpectrum> s = readSpectrumFile(path, &error);
	check(s.has_value(), "an .spd file reads: " + error);
	if (s.has_value()) {
		check(near((*s)(600), 0.3, 1e-6), fmt::format("an .spd file is linear between its points: {} at 600 nm", (*s)(600)));
	}
	std::filesystem::remove(path);
	check(!readSpectrumFile(path, &error).has_value(), "a missing .spd file fails");
}

// a smooth conductor's color at normal incidence: its Fresnel reflectance per wavelength under D65
glm::dvec3 conductorRgb(const std::string& metal)
{
	const DenseSpectrum eta = *namedSpectrum("metal-" + metal + "-eta");
	const DenseSpectrum k = *namedSpectrum("metal-" + metal + "-k");
	DenseSpectrum f0;
	for (int i = 0; i < spectrum_tables::CIE_SAMPLES; i++) {
		const double e = eta.values[i];
		const double kk = k.values[i];
		f0.values[i] = ((e - 1.0) * (e - 1.0) + kk * kk) / ((e + 1.0) * (e + 1.0) + kk * kk);
	}
	return glm::dvec3(reflectanceToRgb(f0));
}

// pbrt-v4's own pixel values for a light or reflector filling a 32 x 32 image through a box filter at 4096 spp,
// from tools/spectrum_tables/pbrt_reference.py; its noise is about 1e-4
struct PbrtReference {
	const char* what;
	glm::dvec3 rgb;
	glm::dvec3 pbrt;
};

void testAgainstPbrt()
{
	fmt::println("against pbrt-v4");
	const PbrtReference references[] = {
#include "spectrum_pbrt_reference.inc"
	};
	for (const PbrtReference& r : references) {
		check(near3(r.rgb, r.pbrt, 3e-3), fmt::format("{}: here {}, pbrt {}", r.what, str(r.rgb), str(r.pbrt)));
	}
}

}

int main()
{
	testTables();
	testNormalizations();
	testFiles();
	testAgainstPbrt();
	if (g_failures > 0) {
		fmt::println("spectrum_test: {} check(s) FAILED", g_failures);
		return 1;
	}
	fmt::println("spectrum_test: all checks passed");
	return 0;
}
