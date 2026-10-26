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
// And what the spectral modes rest on (src/color_space.h, src/spectrum_ref.h, src/spectral.h,
// docs/plans/completed/pbrt-spectral-rendering.md):
//  - the uplift: in each of the four color spaces, a color uplifted and projected back under the space's white comes
//    back within 1e-3, an uplifted reflectance stays in [0, 1], and a gray is exact
//  - a 6504 K blackbody's chromaticity is D65's within 0.005
//  - the dispersion formulas: Schott's N-BK7 Sellmeier coefficients give its catalog indices at the F, d and C lines
//    to 1e-5, the formulas that contain one another agree, and pbrt's glass-BK7 table agrees with the formula
//  - SpectrumRef: an RGB reference is its color bit for bit in exact RGB, an emitter spectrum is normalized to
//    luminance 1, a line integrates to its scale
//  - wavelength sampling: every density integrates to 1 and matches the histogram of its samples, and the hero
//    estimate of a spectrum's color converges to its exact projection under every density
//  - the bands: a constant comes back exactly through the band-to-film matrix, and the RGB Gaussians' round trip
//  - the packed SpectralData evaluates to what the references do, and the wavelength display colors
//
// Run:  ./bin/spectrum_test [--fit-gaussians]   (the second fits the RGB Gaussians' default bands and prints them)

#include <color_space.h>
#include <spectral.h>
#include <spectrum.h>
#include <spectrum_ref.h>

#include <random>
#include <span>

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

const std::filesystem::path ROOT = std::filesystem::path(__FILE__).parent_path().parent_path();

double luminanceOfRgb(const glm::dvec3& c)
{
	return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
}

// a reflectance's color in a space, under the space's own white (what the uplift's fit inverts)
glm::dvec3 reflectanceInSpace(const DenseSpectrum& s, ColorSpaceId space)
{
	const ColorSpace& cs = colorSpace(space);
	const glm::dvec3 xyz = spectrumToXyz(s * *cs.illuminant);
	return cs.rgbFromXyz * xyz;
}

void testUplift()
{
	fmt::println("uplift");
	for (uint32_t id = 0; id < (uint32_t)ColorSpaceId::Count; id++) {
		const ColorSpaceId space = (ColorSpaceId)id;
		std::string error;
		const UpliftTable* table = upliftTable(ROOT, space, &error);
		check(table != nullptr && table->valid(), fmt::format("{}: the uplift table loads: {}", colorSpace(space).key, error));
		if (table == nullptr) {
			continue;
		}
		const SpectrumContext context { table, space };
		//the sRGB gamut, expressed in the space: a wider space's own corners lie outside the spectral locus, where no
		//reflectance can reach, and the fit only does its best there
		const glm::mat3 toSpace(convertRgb(ColorSpaceId::Srgb, space));
		double worst = 0.0;
		glm::dvec3 worstColor(0.0);
		bool bounded = true;
		const int steps = 8;
		for (int r = 0; r <= steps; r++) {
			for (int g = 0; g <= steps; g++) {
				for (int b = 0; b <= steps; b++) {
					const glm::vec3 rgb = toSpace * glm::vec3((float)r / steps, (float)g / steps, (float)b / steps);
					if (glm::any(glm::lessThan(rgb, glm::vec3(0.f))) || glm::any(glm::greaterThan(rgb, glm::vec3(1.f)))) {
						continue;
					}
					const DenseSpectrum s = denseSpectrum(rgbSpectrum(rgb), SpectrumRole::Albedo, context);
					for (double v : s.values) {
						bounded = bounded && v >= 0.0 && v <= 1.0;
					}
					const glm::dvec3 back = reflectanceInSpace(s, space);
					const double error3 = std::max(std::abs(back.x - rgb.r), std::max(std::abs(back.y - rgb.g), std::abs(back.z - rgb.b)));
					if (error3 > worst) {
						worst = error3;
						worstColor = glm::dvec3(rgb);
					}
				}
			}
		}
		//pbrt's own fit leaves its largest residuals at the brightest, most saturated corners: 0.003 in Display P3,
		//0.02 in Rec. 2020 and 0.06 in ACES2065-1 for the sRGB gamut's corners
		check(worst < 0.1, fmt::format("{}: the sRGB gamut's colors come back within 0.1, worst {:.2e} at {}", colorSpace(space).key, worst, str(worstColor)));
		check(bounded, fmt::format("{}: uplifted reflectances stay in [0, 1]", colorSpace(space).key));
		double worstInterior = 0.0;
		for (int r = 1; r < steps; r++) {
			for (int g = 1; g < steps; g++) {
				for (int b = 1; b < steps; b++) {
					const glm::vec3 rgb = toSpace * (glm::vec3(0.1f) + 0.8f * glm::vec3((float)r / steps, (float)g / steps, (float)b / steps));
					if (glm::any(glm::lessThan(rgb, glm::vec3(0.f))) || glm::any(glm::greaterThan(rgb, glm::vec3(1.f)))) {
						continue;
					}
					const glm::dvec3 back = reflectanceInSpace(denseSpectrum(rgbSpectrum(rgb), SpectrumRole::Albedo, context), space);
					worstInterior = std::max(worstInterior, std::max(std::abs(back.x - rgb.r), std::max(std::abs(back.y - rgb.g), std::abs(back.z - rgb.b))));
				}
			}
		}
		check(worstInterior < 1e-3, fmt::format("{}: colors well inside sRGB come back within 1e-3 (the plan's identity goal), worst {:.2e}", colorSpace(space).key, worstInterior));
		const glm::dvec3 gray = reflectanceInSpace(denseSpectrum(rgbSpectrum(glm::vec3(0.35f)), SpectrumRole::Albedo, context), space);
		check(near3(gray, glm::dvec3(0.35), 1e-5), fmt::format("{}: a gray is exact, got {}", colorSpace(space).key, str(gray)));
		//an RGB white light is the space's white: luminance 1 and, in its own space, (1, 1, 1)
		const DenseSpectrum white = denseSpectrum(rgbSpectrum(glm::vec3(1.f)), SpectrumRole::Illuminant, context);
		const glm::dvec3 whiteRgb = colorSpace(space).rgbFromXyz * spectrumToXyz(white);
		check(near3(whiteRgb, glm::dvec3(1.0), 1e-4), fmt::format("{}: an RGB white light is (1, 1, 1), got {}", colorSpace(space).key, str(whiteRgb)));
		//outside 360 to 830 nm an RGB value holds its end values (docs/plans/spectral-out-of-range.md), in every role
		const SpectrumRef orange = rgbSpectrum(glm::vec3(0.8f, 0.4f, 0.1f));
		bool holds = true;
		for (const SpectrumRole role : { SpectrumRole::Albedo, SpectrumRole::Unbounded, SpectrumRole::Illuminant }) {
			holds = holds && evalSpectrumRef(orange, role, context, 1064.0) == evalSpectrumRef(orange, role, context, 830.0)
				&& evalSpectrumRef(orange, role, context, 13.5) == evalSpectrumRef(orange, role, context, 360.0);
		}
		check(holds, fmt::format("{}: an RGB value holds its 360 and 830 nm values beyond them", colorSpace(space).key));
	}
	//sRGB's conversion is the identity, so exact RGB passes an sRGB scene's colors through bit for bit
	const glm::vec3 c(0.123456f, 0.654321f, 0.999f);
	check(exactRgb(rgbSpectrum(c), SpectrumRole::Albedo, { nullptr, ColorSpaceId::Srgb }) == c, "exact RGB keeps an sRGB color bit for bit");
	const glm::dvec3 p3red = glm::dvec3(exactRgb(rgbSpectrum(glm::vec3(1.f, 0.f, 0.f)), SpectrumRole::Albedo, { nullptr, ColorSpaceId::DciP3 }));
	check(p3red.x > 1.0 && p3red.y < 0.0, fmt::format("Display P3's red lies outside sRGB, got {}", str(p3red)));
}

void testBlackbodyWhite()
{
	fmt::println("blackbody");
	//in CIE 1960 uv, where correlated color temperature is defined: D65 lies 0.003 off the Planckian locus, which
	//in xy is 0.0055
	auto chromaticity = [](const DenseSpectrum& s) {
		const glm::dvec3 xyz = spectrumToXyz(s);
		const double d = xyz.x + 15.0 * xyz.y + 3.0 * xyz.z;
		return glm::dvec2(4.0 * xyz.x / d, 6.0 * xyz.y / d);
	};
	const glm::dvec2 bb = chromaticity(blackbodySpectrum(6504.0));
	const glm::dvec2 d65 = chromaticity(d65Spectrum());
	check(glm::length(bb - d65) < 0.005, fmt::format("a 6504 K blackbody is within 0.005 of D65 in CIE 1960 uv: ({:.4f}, {:.4f}) against ({:.4f}, {:.4f})", bb.x, bb.y, d65.x, d65.y));
	const SpectrumRef ref = blackbodyRef(2700.f);
	const glm::dvec3 rgb(exactRgb(ref, SpectrumRole::Illuminant, {}));
	check(near(luminanceOfRgb(rgb), 1.0, 2e-3), fmt::format("a 2700 K emitter has luminance 1 in exact RGB, got {}", luminanceOfRgb(rgb)));
	check(rgb.x > rgb.y && rgb.y > rgb.z, "a 2700 K emitter is warm");
}

void testDispersion()
{
	fmt::println("dispersion");
	//Schott's catalog: N-BK7's Sellmeier coefficients and its indices at the F, d and C lines
	const double bk7[] = { 1.03961212, 0.00600069867, 0.231792344, 0.0200179144, 1.01046945, 103.560653 };
	const double nF = evalDispersion(DispersionFormula::Sellmeier1, bk7, 486.1327);
	const double nd = evalDispersion(DispersionFormula::Sellmeier1, bk7, 587.5618);
	const double nC = evalDispersion(DispersionFormula::Sellmeier1, bk7, 656.2725);
	check(near(nd, 1.51680, 1e-5), fmt::format("N-BK7's n_d is 1.51680, got {:.6f}", nd));
	check(near(nF, 1.52238, 1e-5), fmt::format("N-BK7's n_F is 1.52238, got {:.6f}", nF));
	check(near(nC, 1.51432, 1e-5), fmt::format("N-BK7's n_C is 1.51432, got {:.6f}", nC));
	const double abbe = (nd - 1.0) / (nF - nC);
	check(std::abs(abbe - 64.17) < 0.02, fmt::format("N-BK7's Abbe number is 64.17, got {:.3f}", abbe));
	//pbrt's tabulated BK7 against the formula, across the visible range
	const SpectrumRef table = *namedRef("glass-BK7");
	double worst = 0.0;
	for (double l = 400.0; l <= 800.0; l += 10.0) {
		worst = std::max(worst, std::abs(evalSpectrumRefRaw(table, l) - evalDispersion(DispersionFormula::Sellmeier1, bk7, l)));
	}
	check(worst < 2e-4, fmt::format("pbrt's glass-BK7 agrees with Schott's formula within 2e-4, worst {:.2e}", worst));
	//the formulas that contain one another
	const double schott[] = { 2.2718929, -1.0108077e-2, 1.0592509e-2, 2.0816965e-4, -7.6472538e-6, 4.9240991e-7, 0.0, 0.0 };
	for (double l : { 400.0, 587.6, 1064.0 }) {
		const double a = evalDispersion(DispersionFormula::Schott, schott, l);
		check(near(a, evalDispersion(DispersionFormula::Extended, schott, l), 1e-12), "Extended with no extra terms is Schott");
		check(near(a, evalDispersion(DispersionFormula::Extended2, schott, l), 1e-12), "Extended 2 with no extra terms is Schott");
		const double s3[] = { bk7[0], bk7[1], bk7[2], bk7[3], bk7[4], bk7[5], 0.0, 1.0 };
		const double s5[] = { bk7[0], bk7[1], bk7[2], bk7[3], bk7[4], bk7[5], 0.0, 1.0, 0.0, 1.0 };
		const double s1 = evalDispersion(DispersionFormula::Sellmeier1, bk7, l);
		check(near(s1, evalDispersion(DispersionFormula::Sellmeier3, s3, l), 1e-12), "Sellmeier 3 with no fourth term is Sellmeier 1");
		check(near(s1, evalDispersion(DispersionFormula::Sellmeier5, s5, l), 1e-12), "Sellmeier 5 with no last terms is Sellmeier 1");
	}
	const double nSchott = evalDispersion(DispersionFormula::Schott, schott, 587.5618);
	check(near(nSchott, 1.5168, 2e-4), fmt::format("BK7's old Schott-formula coefficients give n_d 1.5168, got {:.5f}", nSchott));
	const double cauchy[] = { 1.5046, 0.00420, 0.0 };
	check(near(evalDispersion(DispersionFormula::Cauchy, cauchy, 500.0), 1.5046 + 0.0042 / 0.25, 1e-12), "Cauchy's formula");
	const double conrady[] = { 1.5, 0.01, 0.001 };
	check(near(evalDispersion(DispersionFormula::Conrady, conrady, 500.0), 1.5 + 0.01 / 0.5 + 0.001 / std::pow(0.5, 3.5), 1e-12), "Conrady's formula");
	const double herzberger[] = { 1.5, 0.01, 0.001, -0.002, 0.0001, 0.0 };
	const double L = 1.0 / (0.25 - 0.028);
	check(near(evalDispersion(DispersionFormula::Herzberger, herzberger, 500.0), 1.5 + 0.01 * L + 0.001 * L * L - 0.002 * 0.25 + 0.0001 * 0.0625, 1e-12), "Herzberger's formula");
	const SpectrumRef glass = dispersionRef(DispersionFormula::Sellmeier1, bk7, "N-BK7");
	check(!isConstantSpectrum(glass) && isConstantSpectrum(constantRef(1.5f)), "a formula disperses and a constant does not");
	check(near(scalarAt(glass, 587.5618), 1.51680, 1e-5), "a glass is read at the design wavelength");
}

void testLines()
{
	fmt::println("lines");
	for (const LineProfile profile : { LineProfile::Gaussian, LineProfile::Lorentzian }) {
		const double center = 632.8, fwhm = 2.0;
		double integral = 0.0;
		const double h = 0.001;
		//a Lorentzian's tails carry 1 / (pi x) of its area past x, so it is integrated far out
		const double reach = profile == LineProfile::Gaussian ? 20.0 : 20000.0;
		for (double l = center - reach; l < center + reach; l += (profile == LineProfile::Gaussian ? h : 0.05)) {
			integral += evalLineProfile(profile, center, fwhm, l + 0.5 * h) * (profile == LineProfile::Gaussian ? h : 0.05);
		}
		const double expected = profile == LineProfile::Gaussian ? 1.0 : 1.0 - 2.0 / 3.14159265358979 * std::atan(1.0 / (reach / (0.5 * fwhm)));
		check(near(integral, expected, 1e-4), fmt::format("a {} line integrates to 1, got {:.6f} (expected {:.6f})", profile == LineProfile::Gaussian ? "Gaussian" : "Lorentzian", integral, expected));
		const double peak = evalLineProfile(profile, center, fwhm, center);
		const double half = evalLineProfile(profile, center, fwhm, center + 0.5 * fwhm);
		check(near(half, 0.5 * peak, 1e-9), "a line is at half its peak half its width away");
	}
	//absolute: a line's scale is its radiance integrated over wavelength, in every role
	const SpectrumRef laser = lineRef(632.8f, 1.f, LineProfile::Gaussian, 3.f);
	check(illuminantNormalization(laser) == 1.0, "a line is not normalized as an emitter");
	const SpectrumRef ir = lineRef(1064.f, 1.f, LineProfile::Gaussian, 3.f);
	check(glm::dvec3(exactRgb(ir, SpectrumRole::Illuminant, {})) == glm::dvec3(0.0), "an infrared line has no RGB");
}

double randomUnit(std::mt19937& engine)
{
	return std::uniform_real_distribution<double>(0.0, 1.0)(engine);
}

// the spectral settings and emitters of one sampling test
struct SamplingCase {
	const char* name;
	SpectralSettings settings;
	std::vector<EmitterSpectrum> emitters;
};

void testSampling()
{
	fmt::println("wavelength sampling");
	const UpliftTable* table = upliftTable(ROOT, ColorSpaceId::Srgb);
	const SpectrumContext context { table, ColorSpaceId::Srgb };
	SamplingCase cases[3];
	cases[0].name = "visible";
	cases[0].settings.representation = SpectralRepresentation::HeroWavelengths;
	cases[1].name = "uniform over 380 to 1100 nm";
	cases[1].settings = cases[0].settings;
	cases[1].settings.sampling = WavelengthSampling::Uniform;
	cases[1].settings.lambdaMin = 380.f;
	cases[1].settings.lambdaMax = 1100.f;
	cases[2].name = "the emitters' spectra";
	cases[2].settings = cases[0].settings;
	cases[2].settings.sampling = WavelengthSampling::Lights;
	cases[2].emitters = { { blackbodyRef(3000.f), 2.f }, { lineRef(532.f, 0.5f, LineProfile::Gaussian, 1.f), 1.f }, { lineRef(632.8f, 0.01f, LineProfile::Lorentzian, 1.f), 0.5f } };

	//the spectrum each case estimates: a warm emitter with two lines on it, which the Lights case follows
	DenseSpectrum truth;
	//a 0.01 nm line is what visible sampling almost never finds, and what Lights sampling exists for: only the
	//Lights case is asked to estimate it
	bool needle = false;
	auto spectrumAt = [&](double l) {
		return evalSpectrumRef(blackbodyRef(3000.f), SpectrumRole::Illuminant, {}, l) + 30.0 * evalLineProfile(LineProfile::Gaussian, 532.0, 0.5, l)
			+ (needle ? 30.0 * evalLineProfile(LineProfile::Lorentzian, 632.8, 0.01, l) : 0.0);
	};
	for (const SamplingCase& c : cases) {
		needle = c.settings.sampling == WavelengthSampling::Lights;
		const WavelengthDistribution d = buildWavelengthDistribution(c.emitters, c.settings, context);
		//the density integrates to 1 over the range, the lines' narrow peaks included
		double integral = 0.0;
		const double a = c.settings.lambdaMin, b = c.settings.lambdaMax;
		const int steps = 4000000;
		for (int i = 0; i < steps; i++) {
			integral += wavelengthPdf((float)(a + (b - a) * (i + 0.5) / steps), c.settings, d) * (b - a) / steps;
		}
		check(near(integral, 1.0, 2e-3), fmt::format("{}: the density integrates to 1, got {:.5f}", c.name, integral));
		//the histogram of the samples against the density, bin by bin
		const int bins = 94;
		std::vector<double> histogram(bins, 0.0);
		std::mt19937 engine(7);
		const int samples = 400000;
		for (int i = 0; i < samples; i++) {
			const SampledWavelengths w = sampleWavelengths((float)randomUnit(engine), c.settings, d);
			for (uint32_t lane = 0; lane < w.count; lane++) {
				const int bin = std::clamp((int)((w.lambda[lane] - a) / (b - a) * bins), 0, bins - 1);
				histogram[bin] += 1.0 / ((double)samples * w.count);
			}
		}
		double worstBin = 0.0;
		for (int bin = 0; bin < bins; bin++) {
			double expected = 0.0;
			const double lo = a + (b - a) * bin / bins;
			for (int k = 0; k < 2000; k++) {
				expected += wavelengthPdf((float)(lo + (b - a) / bins * (k + 0.5) / 2000), c.settings, d) * (b - a) / bins / 2000;
			}
			worstBin = std::max(worstBin, std::abs(histogram[bin] - expected) / std::max(expected, 1e-3));
		}
		check(worstBin < 0.03, fmt::format("{}: the samples follow the density, worst bin off by {:.3f}", c.name, worstBin));

		//the hero estimate of the spectrum's color converges to its projection
		if (c.settings.lambdaMin == 360.f) {
			//the integral the estimator converges to, finely enough for a line 0.01 nm wide: the spectrum at every
			//wavelength against the matching functions at the nearest whole nm, as pbrt's film reads them
			glm::dvec3 xyz(0.0);
			const double h = 0.0005;
			for (double l = 360.0 + 0.5 * h; l < 830.0; l += h) {
				const int i = (int)std::lround(l) - spectrum_tables::LAMBDA_MIN;
				xyz += spectrumAt(l) * h * glm::dvec3(spectrum_tables::CIE_X[i], spectrum_tables::CIE_Y[i], spectrum_tables::CIE_Z[i]);
			}
			const glm::dvec3 expected = xyzToLinearSrgb(xyz / spectrum_tables::CIE_Y_INTEGRAL);
			glm::dvec3 sum(0.0);
			const int paths = 200000;
			for (int i = 0; i < paths; i++) {
				const SampledWavelengths w = sampleWavelengths((float)randomUnit(engine), c.settings, d);
				float values[32];
				for (uint32_t lane = 0; lane < w.count; lane++) {
					values[lane] = (float)spectrumAt(w.lambda[lane]);
				}
				sum += glm::dvec3(lanesToRgb(std::span(values, w.count), w, c.settings, {}));
			}
			const glm::dvec3 estimate = sum / (double)paths;
			const double error = glm::length(estimate - expected) / glm::length(expected);
			//pbrt's film reads the matching functions at the nearest nanometer, as the dense projection does
			check(error < 0.01, fmt::format("{}: the hero estimate converges, {} against {}, {:.2e} off", c.name, str(estimate), str(expected), error));
		}
	}
	//visible sampling's inverse CDF is the density's
	check(near(visibleWavelengthPdf(sampleVisibleWavelength(0.5f)), 0.0039398042 / std::pow(std::cosh(0.0072 * (sampleVisibleWavelength(0.5f) - 538.0)), 2.0), 1e-5), "the visible density");
}

void testBands()
{
	fmt::println("bands");
	const UpliftTable* table = upliftTable(ROOT, ColorSpaceId::Srgb);
	const SpectrumContext context { table, ColorSpaceId::Srgb };
	for (const BandBasis basis : { BandBasis::Box, BandBasis::Gaussian }) {
		for (const uint32_t lanes : { 4u, 8u, 16u, 32u }) {
			SpectralSettings settings;
			settings.representation = SpectralRepresentation::SpectralBands;
			settings.lanes = lanes;
			settings.bandBasis = basis;
			const std::vector<Band> bands = buildBands(settings);
			check(bands.size() == lanes, "one band per lane");
			//a constant comes back exactly: the reconstruction is a partition of unity
			std::vector<float> values;
			for (const Band& band : bands) {
				values.push_back((float)projectOntoBand([](double) { return 0.5; }, band));
			}
			const glm::dvec3 rgb(lanesToRgb(values, {}, settings, bands));
			const glm::dvec3 expected = xyzToLinearSrgb(spectrumToXyz(constantSpectrum(0.5)));
			check(near3(rgb, expected, 1e-5), fmt::format("{} {} bands: a constant comes back exactly, {} against {}", bandBasisKey(basis), lanes, str(rgb), str(expected)));
		}
	}
	//the RGB Gaussians: an sRGB color, uplifted as a reflectance under the white light and projected, against itself
	SpectralSettings gaussians;
	gaussians.representation = SpectralRepresentation::RgbGaussians;
	const std::vector<Band> bands = buildBands(gaussians);
	double worst = 0.0;
	double mean = 0.0;
	int count = 0;
	for (int r = 1; r < 4; r++) {
		for (int g = 1; g < 4; g++) {
			for (int b = 1; b < 4; b++) {
				const glm::vec3 c((float)r / 4, (float)g / 4, (float)b / 4);
				std::vector<float> values;
				for (const Band& band : bands) {
					const double reflect = projectOntoBand([&](double l) { return evalSpectrumRef(rgbSpectrum(c), SpectrumRole::Albedo, context, l); }, band);
					const double light = projectOntoBand([&](double l) { return evalSpectrumRef(rgbSpectrum(glm::vec3(1.f)), SpectrumRole::Illuminant, context, l); }, band);
					values.push_back((float)(reflect * light));
				}
				const glm::vec3 back = lanesToRgb(values, {}, gaussians, bands);
				const double e = glm::length(glm::dvec3(back) - glm::dvec3(c));
				worst = std::max(worst, e);
				mean += e;
				count++;
			}
		}
	}
	mean /= count;
	fmt::println("    RGB Gaussians: a lit sRGB color comes back {:.3f} off on average, {:.3f} at worst", mean, worst);
	check(mean < 0.08, fmt::format("RGB Gaussians keep sRGB colors within 0.08 on average, got {:.3f}", mean));
}

void testPacking()
{
	fmt::println("packing");
	const UpliftTable* table = upliftTable(ROOT, ColorSpaceId::Srgb);
	const SpectrumContext context { table, ColorSpaceId::Srgb };
	const double bk7[] = { 1.03961212, 0.00600069867, 0.231792344, 0.0200179144, 1.01046945, 103.560653 };
	const std::pair<SpectrumRef, SpectrumRole> refs[] = {
		{ rgbSpectrum(glm::vec3(0.8f, 0.3f, 0.1f)), SpectrumRole::Albedo },
		{ rgbSpectrum(glm::vec3(4.f, 2.f, 1.f)), SpectrumRole::Illuminant },
		{ constantRef(1.5f), SpectrumRole::Scalar },
		{ blackbodyRef(2700.f, 2.f), SpectrumRole::Illuminant },
		{ *namedRef("metal-Au-k"), SpectrumRole::Scalar },
		{ *namedRef("stdillum-F11"), SpectrumRole::Illuminant },
		{ dispersionRef(DispersionFormula::Sellmeier1, bk7), SpectrumRole::Scalar },
		{ lineRef(632.8f, 2.f, LineProfile::Gaussian, 5.f), SpectrumRole::Illuminant },
	};
	SpectralSettings hero;
	hero.representation = SpectralRepresentation::HeroWavelengths;
	for (const auto& [ref, role] : refs) {
		const std::vector<uint32_t> words = packSpectrumRef(ref, role, hero, context, {});
		double worst = 0.0;
		for (float l = 360.f; l <= 830.f; l += 3.7f) {
			const double expected = evalSpectrumRef(ref, role, context, l);
			const double got = evalPackedLane(words, 0, l, hero, context, {});
			worst = std::max(worst, std::abs(got - expected) / std::max(1e-3, std::abs(expected)));
		}
		check(worst < 1e-4, fmt::format("a packed {} evaluates as the reference does, worst relative error {:.2e}", spectrumKindName(ref.kind), worst));
	}
	//band representations: the CPU's projection, and the run-time quadrature of a tinted color against it
	SpectralSettings bands;
	bands.representation = SpectralRepresentation::SpectralBands;
	bands.lanes = 8;
	const std::vector<Band> b = buildBands(bands);
	const SpectrumRef color = rgbSpectrum(glm::vec3(0.4f, 0.6f, 0.2f));
	const std::vector<uint32_t> words = packSpectrumRef(color, SpectrumRole::Albedo, bands, context, b);
	const std::vector<uint32_t> white = packSpectrumRef(rgbSpectrum(glm::vec3(1.f)), SpectrumRole::Albedo, bands, context, b);
	double worst = 0.0;
	for (uint32_t lane = 0; lane < b.size(); lane++) {
		const float projected = evalPackedLane(words, lane, b[lane].center, bands, context, b);
		const float quadrature = evalPackedLane(white, lane, b[lane].center, bands, context, b, glm::vec3(0.4f, 0.6f, 0.2f));
		worst = std::max(worst, (double)std::abs(projected - quadrature));
	}
	check(worst < 0.01, fmt::format("a tinted color's band quadrature matches the CPU projection within 0.01, worst {:.4f}", worst));
}

void testDisplayColors()
{
	fmt::println("display colors");
	const SpectralDisplay display;
	const glm::vec3 green = wavelengthDisplayColor(540.f, display);
	const glm::vec3 blue = wavelengthDisplayColor(460.f, display);
	const glm::vec3 red = wavelengthDisplayColor(650.f, display);
	check(green.g == 1.f && blue.b == 1.f && red.r == 1.f, fmt::format("540, 460 and 650 nm are green, blue and red: {} {} {}", str(green), str(blue), str(red)));
	check(wavelengthDisplayColor(1064.f, display) == display.infraredColor, "1064 nm is the infrared color");
	check(wavelengthDisplayColor(13.5f, display) == display.xuvColor, "13.5 nm is the XUV color");
	const glm::vec3 low = colormapColor(Colormap::Viridis, 0.f), high = colormapColor(Colormap::Viridis, 1.f);
	check(low.b > low.g && high.g > high.b, "viridis runs from purple to yellow");
}

// --fit-gaussians: the three Gaussian bands that bring a grid of lit sRGB colors back closest to themselves, by
// Nelder-Mead over (center, fwhm) x 3 from the sRGB primaries' dominant wavelengths
void fitGaussians()
{
	const UpliftTable* table = upliftTable(ROOT, ColorSpaceId::Srgb);
	const SpectrumContext context { table, ColorSpaceId::Srgb };
	std::vector<glm::vec3> colors;
	for (int r = 0; r <= 4; r++) {
		for (int g = 0; g <= 4; g++) {
			for (int b = 0; b <= 4; b++) {
				colors.push_back(glm::vec3((float)r / 4, (float)g / 4, (float)b / 4));
			}
		}
	}
	std::vector<DenseSpectrum> spectra;
	for (const glm::vec3& c : colors) {
		spectra.push_back(denseSpectrum(rgbSpectrum(c), SpectrumRole::Albedo, context) * *colorSpace(ColorSpaceId::Srgb).illuminant);
	}
	auto cost = [&](const std::array<double, 6>& x) {
		SpectralSettings s;
		s.representation = SpectralRepresentation::RgbGaussians;
		for (int i = 0; i < 3; i++) {
			s.gaussians[i] = { (float)x[2 * i], (float)std::max(x[2 * i + 1], 5.0) };
		}
		const std::vector<Band> bands = buildBands(s);
		double total = 0.0;
		for (size_t k = 0; k < colors.size(); k++) {
			float values[3];
			for (int i = 0; i < 3; i++) {
				values[i] = (float)projectOntoBand([&](double l) { return spectra[k]((int)std::lround(l)); }, bands[i]);
			}
			const glm::vec3 back = lanesToRgb(values, {}, s, bands);
			total += glm::dot(back - colors[k], back - colors[k]);
		}
		return total / colors.size();
	};
	std::array<std::array<double, 6>, 7> simplex;
	simplex[0] = { 612.0, 80.0, 549.0, 80.0, 465.0, 60.0 };
	for (int i = 1; i < 7; i++) {
		simplex[i] = simplex[0];
		simplex[i][i - 1] += (i % 2 == 1) ? 15.0 : 20.0;
	}
	std::array<double, 7> values;
	for (int i = 0; i < 7; i++) {
		values[i] = cost(simplex[i]);
	}
	for (int iteration = 0; iteration < 600; iteration++) {
		std::array<int, 7> order { 0, 1, 2, 3, 4, 5, 6 };
		std::sort(order.begin(), order.end(), [&](int a, int b) { return values[a] < values[b]; });
		const int worstIndex = order[6];
		std::array<double, 6> centroid {};
		for (int i = 0; i < 6; i++) {
			for (int j = 0; j < 6; j++) {
				centroid[j] += simplex[order[i]][j] / 6.0;
			}
		}
		auto along = [&](double t) {
			std::array<double, 6> p;
			for (int j = 0; j < 6; j++) {
				p[j] = centroid[j] + t * (simplex[worstIndex][j] - centroid[j]);
			}
			return p;
		};
		const auto reflected = along(-1.0);
		const double fr = cost(reflected);
		if (fr < values[order[0]]) {
			const auto expanded = along(-2.0);
			const double fe = cost(expanded);
			simplex[worstIndex] = fe < fr ? expanded : reflected;
			values[worstIndex] = std::min(fe, fr);
		} else if (fr < values[order[5]]) {
			simplex[worstIndex] = reflected;
			values[worstIndex] = fr;
		} else {
			const auto contracted = along(0.5);
			const double fc = cost(contracted);
			if (fc < values[worstIndex]) {
				simplex[worstIndex] = contracted;
				values[worstIndex] = fc;
			} else {
				for (int i = 1; i < 7; i++) {
					for (int j = 0; j < 6; j++) {
						simplex[order[i]][j] = simplex[order[0]][j] + 0.5 * (simplex[order[i]][j] - simplex[order[0]][j]);
					}
					values[order[i]] = cost(simplex[order[i]]);
				}
			}
		}
	}
	const int best = (int)(std::min_element(values.begin(), values.end()) - values.begin());
	fmt::println("RGB Gaussians fit: rms {:.4f}", std::sqrt(values[best]));
	for (int i = 0; i < 3; i++) {
		fmt::println("    {{ {:.0f}.f, {:.0f}.f }},", simplex[best][2 * i], simplex[best][2 * i + 1]);
	}
}

}

int main(int argc, char** argv)
{
	if (argc > 1 && std::string(argv[1]) == "--fit-gaussians") {
		fitGaussians();
		return 0;
	}
	testTables();
	testNormalizations();
	testFiles();
	testAgainstPbrt();
	testUplift();
	testBlackbodyWhite();
	testDispersion();
	testLines();
	testSampling();
	testBands();
	testPacking();
	testDisplayColors();
	if (g_failures > 0) {
		fmt::println("spectrum_test: {} check(s) FAILED", g_failures);
		return 1;
	}
	fmt::println("spectrum_test: all checks passed");
	return 0;
}
