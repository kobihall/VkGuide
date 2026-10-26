#include <spectral.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace spectrum_tables;

// fitted by ./bin/spectrum_test --fit-gaussians: see the comment in spectral.h. The rms error over a 5^3 grid of lit
// sRGB colors is 0.063
const std::array<GaussianBand, 3> DEFAULT_RGB_GAUSSIANS = { {
	{ 616.f, 39.f },
	{ 536.f, 46.f },
	{ 449.f, 38.f },
} };

namespace {

constexpr float PI_F = 3.14159265358979323846f;

// the CIE matching functions at a wavelength, as pbrt's DenselySampledSpectrum reads them: the nearest whole nm,
// zero outside 360 to 830
glm::vec3 cieAt(float lambda)
{
	const int i = (int)std::lround(lambda) - LAMBDA_MIN;
	if (i < 0 || i >= CIE_SAMPLES) {
		return glm::vec3(0.f);
	}
	return glm::vec3(CIE_X[i], CIE_Y[i], CIE_Z[i]);
}

glm::mat3 srgbFromXyzF()
{
	return glm::mat3(colorSpace(ColorSpaceId::Srgb).rgbFromXyz);
}

float smoothstepF(float a, float b, float x)
{
	const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}

// 4-point Gauss-Legendre on [-1, 1] and Gauss-Hermite for exp(-x^2)
constexpr float LEGENDRE_NODES[4] = { -0.8611363116f, -0.3399810436f, 0.3399810436f, 0.8611363116f };
constexpr float LEGENDRE_WEIGHTS[4] = { 0.3478548451f, 0.6521451549f, 0.6521451549f, 0.3478548451f };
constexpr float HERMITE_NODES[4] = { -1.6506801239f, -0.5246476233f, 0.5246476233f, 1.6506801239f };
constexpr float HERMITE_WEIGHTS[4] = { 0.0813128354f, 0.8049140900f, 0.8049140900f, 0.0813128354f };

bool isBandRepresentation(SpectralRepresentation r)
{
	return r == SpectralRepresentation::RgbGaussians || r == SpectralRepresentation::SpectralBands;
}

bool usesVisibleDensity(const SpectralSettings& s)
{
	return s.sampling != WavelengthSampling::Uniform && s.lambdaMin == SPECTRAL_LAMBDA_MIN && s.lambdaMax == SPECTRAL_LAMBDA_MAX;
}

float baseSample(float u, const SpectralSettings& s)
{
	if (usesVisibleDensity(s)) {
		return sampleVisibleWavelength(u);
	}
	return s.lambdaMin + u * (s.lambdaMax - s.lambdaMin);
}

float basePdf(float lambda, const SpectralSettings& s)
{
	if (usesVisibleDensity(s)) {
		return visibleWavelengthPdf(lambda);
	}
	return (lambda >= s.lambdaMin && lambda <= s.lambdaMax) ? 1.f / (s.lambdaMax - s.lambdaMin) : 0.f;
}

// a truncated Cauchy density of half width g around c on [a, b], closed form both ways
float cauchySpan(const SamplingLine& line, float a, float b)
{
	return std::atan((b - line.center) / line.halfWidth) - std::atan((a - line.center) / line.halfWidth);
}

float cauchySample(const SamplingLine& line, float u, float a, float b)
{
	const float lo = std::atan((a - line.center) / line.halfWidth);
	return std::clamp(line.center + line.halfWidth * std::tan(lo + u * cauchySpan(line, a, b)), a, b);
}

float cauchyPdf(const SamplingLine& line, float lambda, float a, float b)
{
	if (lambda < a || lambda > b) {
		return 0.f;
	}
	const float t = (lambda - line.center) / line.halfWidth;
	return 1.f / (line.halfWidth * (1.f + t * t) * cauchySpan(line, a, b));
}

}

//---------------------------------------------------------------- settings

SpectralSettings clampSpectralSettings(SpectralSettings s)
{
	if (s.lanes != 4 && s.lanes != 8 && s.lanes != 16 && s.lanes != 32) {
		s.lanes = s.lanes < 6 ? 4 : s.lanes < 12 ? 8 : s.lanes < 24 ? 16 : 32;
	}
	auto wavelength = [](float l) { return std::clamp(std::isfinite(l) ? l : DEFAULT_DESIGN_WAVELENGTH, SPECTRAL_RANGE_FLOOR, SPECTRAL_RANGE_CEILING); };
	s.monochromaticWavelength = wavelength(s.monochromaticWavelength);
	s.designWavelength = wavelength(s.designWavelength);
	s.lambdaMin = wavelength(s.lambdaMin);
	s.lambdaMax = std::clamp(wavelength(s.lambdaMax), std::min(s.lambdaMin + 1.f, SPECTRAL_RANGE_CEILING), SPECTRAL_RANGE_CEILING);
	s.lambdaMin = std::min(s.lambdaMin, s.lambdaMax - 1.f);
	for (GaussianBand& g : s.gaussians) {
		g.center = wavelength(g.center);
		g.fwhm = std::clamp(std::isfinite(g.fwhm) ? g.fwhm : 50.f, 5.f, 2000.f);
	}
	s.lightSamplingShare = std::clamp(std::isfinite(s.lightSamplingShare) ? s.lightSamplingShare : 0.5f, 0.f, 0.99f);
	s.imageBuckets = std::min(s.imageBuckets, 256u);
	if ((uint32_t)s.representation >= (uint32_t)SpectralRepresentation::Count) {
		s.representation = SpectralRepresentation::ExactRgb;
	}
	if ((uint32_t)s.sampling >= (uint32_t)WavelengthSampling::Count) {
		s.sampling = WavelengthSampling::Visible;
	}
	if ((uint32_t)s.bandBasis >= (uint32_t)BandBasis::Count) {
		s.bandBasis = BandBasis::Box;
	}
	if ((uint32_t)s.colorSpace >= (uint32_t)ColorSpaceId::Count) {
		s.colorSpace = ColorSpaceId::Srgb;
	}
	return s;
}

const char* spectralRepresentationKey(SpectralRepresentation representation)
{
	switch (representation) {
	case SpectralRepresentation::ExactRgb: return "rgb";
	case SpectralRepresentation::Monochromatic: return "monochromatic";
	case SpectralRepresentation::RgbGaussians: return "gaussians";
	case SpectralRepresentation::SpectralBands: return "bands";
	case SpectralRepresentation::HeroWavelengths: return "hero";
	case SpectralRepresentation::Count: break;
	}
	return "rgb";
}

const char* spectralRepresentationName(SpectralRepresentation representation)
{
	switch (representation) {
	case SpectralRepresentation::ExactRgb: return "Exact RGB";
	case SpectralRepresentation::Monochromatic: return "Monochromatic";
	case SpectralRepresentation::RgbGaussians: return "RGB Gaussians";
	case SpectralRepresentation::SpectralBands: return "Spectral bands";
	case SpectralRepresentation::HeroWavelengths: return "Hero wavelengths";
	case SpectralRepresentation::Count: break;
	}
	return "Exact RGB";
}

const char* wavelengthSamplingKey(WavelengthSampling sampling)
{
	switch (sampling) {
	case WavelengthSampling::Visible: return "visible";
	case WavelengthSampling::Uniform: return "uniform";
	case WavelengthSampling::Lights: return "lights";
	case WavelengthSampling::Count: break;
	}
	return "visible";
}

const char* bandBasisKey(BandBasis basis)
{
	return basis == BandBasis::Gaussian ? "gaussian" : "box";
}

const char* monochromeDisplayKey(MonochromeDisplay mode)
{
	switch (mode) {
	case MonochromeDisplay::Grayscale: return "grayscale";
	case MonochromeDisplay::WavelengthColor: return "wavelength";
	case MonochromeDisplay::Colormap: return "colormap";
	case MonochromeDisplay::Count: break;
	}
	return "grayscale";
}

const char* colormapKey(Colormap map)
{
	switch (map) {
	case Colormap::Viridis: return "viridis";
	case Colormap::Inferno: return "inferno";
	case Colormap::Turbo: return "turbo";
	case Colormap::Count: break;
	}
	return "viridis";
}

uint32_t spectralImageBuckets(const SpectralSettings& settings)
{
	if (settings.imageBuckets == 0) {
		return 0;
	}
	switch (settings.representation) {
	case SpectralRepresentation::HeroWavelengths:
		return std::min(settings.imageBuckets, 256u);
	case SpectralRepresentation::RgbGaussians:
	case SpectralRepresentation::SpectralBands:
		return activeLanes(settings);
	default:
		return 0;
	}
}

glm::vec2 spectralBucketRange(const SpectralSettings& settings, uint32_t bucket)
{
	if (settings.representation == SpectralRepresentation::RgbGaussians || settings.representation == SpectralRepresentation::SpectralBands) {
		const std::vector<Band> bands = buildBands(settings);
		const Band& b = bands[std::min<size_t>(bucket, bands.size() - 1)];
		return { b.lo, b.hi };
	}
	const uint32_t buckets = std::max(spectralImageBuckets(settings), 1u);
	const float width = (settings.lambdaMax - settings.lambdaMin) / (float)buckets;
	return { settings.lambdaMin + width * (float)bucket, settings.lambdaMin + width * (float)(bucket + 1) };
}

uint32_t laneFamily(const SpectralSettings& settings)
{
	switch (settings.representation) {
	case SpectralRepresentation::ExactRgb: return 0;
	case SpectralRepresentation::Monochromatic: return 1;
	case SpectralRepresentation::RgbGaussians: return 4;
	case SpectralRepresentation::SpectralBands:
	case SpectralRepresentation::HeroWavelengths:
		for (const uint32_t family : SPECTRAL_FAMILIES) {
			if (family >= 4 && family >= settings.lanes) {
				return family;
			}
		}
		return 32;
	case SpectralRepresentation::Count: break;
	}
	return 0;
}

std::string laneFamilySuffix(uint32_t family)
{
	return family == 0 ? std::string() : ".lanes" + std::to_string(family);
}

uint32_t activeLanes(const SpectralSettings& settings)
{
	switch (settings.representation) {
	case SpectralRepresentation::ExactRgb: return 3;
	case SpectralRepresentation::Monochromatic: return 1;
	case SpectralRepresentation::RgbGaussians: return 3;
	default: return laneFamily(settings);
	}
}

glm::vec2 spectralRange(const SpectralSettings& settings)
{
	switch (settings.representation) {
	case SpectralRepresentation::ExactRgb:
	case SpectralRepresentation::RgbGaussians:
		return { SPECTRAL_LAMBDA_MIN, SPECTRAL_LAMBDA_MAX };
	case SpectralRepresentation::Monochromatic:
		return { settings.monochromaticWavelength, settings.monochromaticWavelength };
	default:
		return { settings.lambdaMin, settings.lambdaMax };
	}
}

//---------------------------------------------------------------- display colors

glm::vec3 wavelengthDisplayColor(float lambda, const SpectralDisplay& display)
{
	auto visible = [](float l) {
		//interpolated between whole nanometers, unlike the film's nearest lookup: a slider through the spectrum
		//should change the color smoothly
		const float x = std::clamp(l, 380.f, 780.f) - (float)LAMBDA_MIN;
		const int i = std::min((int)x, CIE_SAMPLES - 2);
		const float t = x - (float)i;
		const glm::vec3 xyz = glm::mix(glm::vec3(CIE_X[i], CIE_Y[i], CIE_Z[i]), glm::vec3(CIE_X[i + 1], CIE_Y[i + 1], CIE_Z[i + 1]), t);
		glm::vec3 rgb = srgbFromXyzF() * xyz;
		//into the gamut by adding white, then the brightest channel to 1
		const float lowest = std::min(rgb.r, std::min(rgb.g, rgb.b));
		if (lowest < 0.f) {
			rgb -= lowest;
		}
		const float highest = std::max(rgb.r, std::max(rgb.g, rgb.b));
		return highest > 0.f ? rgb / highest : glm::vec3(0.f);
	};
	if (lambda < 100.f) {
		return display.xuvColor;
	}
	if (lambda < 200.f) {
		return glm::mix(display.xuvColor, display.ultravioletColor, smoothstepF(100.f, 200.f, lambda));
	}
	if (lambda < 380.f) {
		return glm::mix(display.ultravioletColor, visible(380.f), smoothstepF(360.f, 380.f, lambda));
	}
	if (lambda > 780.f) {
		return glm::mix(visible(780.f), display.infraredColor, smoothstepF(780.f, 800.f, lambda));
	}
	return visible(lambda);
}

glm::vec3 colormapColor(Colormap map, float t)
{
	t = std::clamp(t, 0.f, 1.f);
	if (map == Colormap::Turbo) {
		//Mikhailov 2019, Google's polynomial approximation of Turbo
		const glm::vec4 v4(1.f, t, t * t, t * t * t);
		const glm::vec2 v2 = glm::vec2(v4.z, v4.w) * v4.z;
		const glm::vec4 red4(0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f);
		const glm::vec4 green4(0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f);
		const glm::vec4 blue4(0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f);
		const glm::vec2 red2(-152.94239396f, 59.28637943f);
		const glm::vec2 green2(4.27729857f, 2.82956604f);
		const glm::vec2 blue2(-89.90310912f, 27.34824973f);
		return glm::clamp(glm::vec3(glm::dot(v4, red4) + glm::dot(v2, red2), glm::dot(v4, green4) + glm::dot(v2, green2), glm::dot(v4, blue4) + glm::dot(v2, blue2)),
			glm::vec3(0.f), glm::vec3(1.f));
	}
	//degree-6 least-squares fits of matplotlib's maps (Matt Zucker, shadertoy WlfXRN)
	glm::vec3 c[7];
	if (map == Colormap::Inferno) {
		c[0] = { 0.0002189403691192265f, 0.001651004631001012f, -0.01948089843709184f };
		c[1] = { 0.1065134194856116f, 0.5639564367884091f, 3.932712388889277f };
		c[2] = { 11.60249308247187f, -3.972853965665698f, -15.9423941062914f };
		c[3] = { -41.70399613139459f, 17.43639888205313f, 44.35414519872813f };
		c[4] = { 77.162935699427f, -33.40235894210092f, -81.80730925738993f };
		c[5] = { -71.31942824499214f, 32.62606426397723f, 73.20951985803202f };
		c[6] = { 25.13112622477341f, -12.24266895238567f, -23.07032500287172f };
	} else {
		c[0] = { 0.2777273272234177f, 0.005407344544966578f, 0.3340998053353061f };
		c[1] = { 0.1050930431085774f, 1.404613529898575f, 1.384590162594685f };
		c[2] = { -0.3308618287255563f, 0.214847559468213f, 0.09509516302823659f };
		c[3] = { -4.634230498983486f, -5.799100973351585f, -19.33244095627987f };
		c[4] = { 6.228269936347081f, 14.17993336680509f, 56.69055260068105f };
		c[5] = { 4.776384997670288f, -13.74514537774601f, -65.35303263337234f };
		c[6] = { -5.435455855934631f, 4.645852612178535f, 26.3124352495832f };
	}
	const glm::vec3 v = c[0] + t * (c[1] + t * (c[2] + t * (c[3] + t * (c[4] + t * (c[5] + t * c[6])))));
	return glm::clamp(v, glm::vec3(0.f), glm::vec3(1.f));
}

//---------------------------------------------------------------- wavelengths

float sampleVisibleWavelength(float u)
{
	return 538.f - 138.888889f * std::atanh(0.85691062f - 1.82750197f * u);
}

float visibleWavelengthPdf(float lambda)
{
	if (lambda < 360.f || lambda > 830.f) {
		return 0.f;
	}
	const float c = std::cosh(0.0072f * (lambda - 538.f));
	return 0.0039398042f / (c * c);
}

WavelengthDistribution buildWavelengthDistribution(std::span<const EmitterSpectrum> emitters, const SpectralSettings& settings, const SpectrumContext& context)
{
	WavelengthDistribution d;
	d.cdf.assign(WAVELENGTH_TABLE_BINS + 1, 0.f);
	d.density.assign(WAVELENGTH_TABLE_BINS, 0.f);
	for (uint32_t i = 0; i <= WAVELENGTH_TABLE_BINS; i++) {
		d.cdf[i] = (float)i / (float)WAVELENGTH_TABLE_BINS;
	}
	const float a = settings.lambdaMin;
	const float b = settings.lambdaMax;
	const double binWidth = (double)(b - a) / WAVELENGTH_TABLE_BINS;
	if (settings.sampling != WavelengthSampling::Lights || !(b > a)) {
		return d;
	}

	//the smooth spectra, each normalized over the range and weighed by its power, into one table; the lines apart,
	//strongest first, the rest folded into the table as their profiles
	std::vector<double> table(WAVELENGTH_TABLE_BINS, 0.0);
	double tablePower = 0.0;
	std::vector<SamplingLine> lines;
	double linePower = 0.0;
	std::vector<std::pair<double, const EmitterSpectrum*>> lineEmitters;
	for (const EmitterSpectrum& e : emitters) {
		if (!(e.power > 0.f)) {
			continue;
		}
		if (e.spectrum.kind == SpectrumKind::Line && e.spectrum.value >= a && e.spectrum.value <= b) {
			lineEmitters.push_back({ e.power, &e });
		} else {
			std::vector<double> shape(WAVELENGTH_TABLE_BINS);
			double sum = 0.0;
			for (uint32_t i = 0; i < WAVELENGTH_TABLE_BINS; i++) {
				//the bin's mean from three points, which a smooth spectrum needs and a line folded in here gets roughly
				double v = 0.0;
				for (int k = 0; k < 3; k++) {
					v += std::max(0.0, evalSpectrumRef(e.spectrum, SpectrumRole::Illuminant, context, a + binWidth * (i + (k + 0.5) / 3.0)));
				}
				shape[i] = v / 3.0;
				sum += shape[i];
			}
			if (sum > 0.0) {
				for (uint32_t i = 0; i < WAVELENGTH_TABLE_BINS; i++) {
					table[i] += e.power * shape[i] / sum;
				}
				tablePower += e.power;
			}
		}
	}
	std::sort(lineEmitters.begin(), lineEmitters.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
	for (size_t i = 0; i < lineEmitters.size(); i++) {
		const SpectrumRef& s = lineEmitters[i].second->spectrum;
		if (i < MAX_SAMPLING_LINES) {
			lines.push_back({ s.value, std::max(0.5f * s.width, 1e-4f), (float)lineEmitters[i].first });
			linePower += lineEmitters[i].first;
		} else {
			const uint32_t bin = std::min((uint32_t)((s.value - a) / binWidth), WAVELENGTH_TABLE_BINS - 1);
			table[bin] += lineEmitters[i].first;
			tablePower += lineEmitters[i].first;
		}
	}
	const double total = tablePower + linePower;
	if (!(total > 0.0)) {
		return d;
	}
	d.baseShare = 1.f - std::clamp(settings.lightSamplingShare, 0.f, 0.99f);
	d.tableShare = (float)(tablePower / total);
	for (SamplingLine& line : lines) {
		line.weight = (float)(line.weight / total);
	}
	d.lines = lines;
	if (tablePower > 0.0) {
		double running = 0.0;
		for (uint32_t i = 0; i < WAVELENGTH_TABLE_BINS; i++) {
			d.density[i] = (float)(table[i] / tablePower / binWidth);
			running += table[i] / tablePower;
			d.cdf[i + 1] = (float)std::min(running, 1.0);
		}
		d.cdf[WAVELENGTH_TABLE_BINS] = 1.f;
	}
	return d;
}

namespace {

float sampleMixture(float u, const SpectralSettings& s, const WavelengthDistribution& d)
{
	if (u < d.baseShare || d.baseShare >= 1.f) {
		return baseSample(std::min(u / d.baseShare, 0.99999994f), s);
	}
	float v = std::min((u - d.baseShare) / (1.f - d.baseShare), 0.99999994f);
	if (v < d.tableShare) {
		v /= d.tableShare;
		//the bin by binary search over the cdf, then linear within it
		uint32_t lo = 0;
		uint32_t hi = WAVELENGTH_TABLE_BINS;
		while (hi - lo > 1) {
			const uint32_t middle = (lo + hi) / 2;
			if (d.cdf[middle] <= v) {
				lo = middle;
			} else {
				hi = middle;
			}
		}
		const float width = d.cdf[lo + 1] - d.cdf[lo];
		const float t = width > 0.f ? (v - d.cdf[lo]) / width : 0.5f;
		return s.lambdaMin + (s.lambdaMax - s.lambdaMin) * ((float)lo + t) / (float)WAVELENGTH_TABLE_BINS;
	}
	v -= d.tableShare;
	for (size_t i = 0; i < d.lines.size(); i++) {
		const SamplingLine& line = d.lines[i];
		if (v < line.weight || i + 1 == d.lines.size()) {
			return cauchySample(line, std::clamp(v / line.weight, 0.f, 0.99999994f), s.lambdaMin, s.lambdaMax);
		}
		v -= line.weight;
	}
	return baseSample(0.5f, s);
}

}

float wavelengthPdf(float lambda, const SpectralSettings& s, const WavelengthDistribution& d)
{
	float p = d.baseShare * basePdf(lambda, s);
	if (d.baseShare < 1.f) {
		float emitters = 0.f;
		if (d.tableShare > 0.f && lambda >= s.lambdaMin && lambda <= s.lambdaMax) {
			const uint32_t bin = std::min((uint32_t)((lambda - s.lambdaMin) / (s.lambdaMax - s.lambdaMin) * (float)WAVELENGTH_TABLE_BINS), WAVELENGTH_TABLE_BINS - 1);
			emitters += d.tableShare * d.density[bin];
		}
		for (const SamplingLine& line : d.lines) {
			emitters += line.weight * cauchyPdf(line, lambda, s.lambdaMin, s.lambdaMax);
		}
		p += (1.f - d.baseShare) * emitters;
	}
	return p;
}

SampledWavelengths sampleWavelengths(float u, const SpectralSettings& settings, const WavelengthDistribution& distribution)
{
	SampledWavelengths w;
	w.count = activeLanes(settings);
	if (settings.representation == SpectralRepresentation::Monochromatic) {
		w.lambda[0] = settings.monochromaticWavelength;
		w.pdf[0] = 1.f;
		return w;
	}
	if (settings.representation != SpectralRepresentation::HeroWavelengths) {
		const std::vector<Band> bands = buildBands(settings);
		for (uint32_t i = 0; i < w.count; i++) {
			w.lambda[i] = bands[i].center;
			w.pdf[i] = 1.f;
		}
		return w;
	}
	const float step = 1.f / (float)w.count;
	for (uint32_t i = 0; i < w.count; i++) {
		float up = u + (float)i * step;
		if (up >= 1.f) {
			up -= 1.f;
		}
		w.lambda[i] = sampleMixture(up, settings, distribution);
		w.pdf[i] = wavelengthPdf(w.lambda[i], settings, distribution);
	}
	return w;
}

//---------------------------------------------------------------- bands

std::vector<Band> buildBands(const SpectralSettings& settings)
{
	std::vector<Band> bands;
	if (settings.representation == SpectralRepresentation::RgbGaussians) {
		for (const GaussianBand& g : settings.gaussians) {
			Band band;
			band.center = g.center;
			band.sigma = g.fwhm / 2.3548200450f;
			band.lo = band.center - 3.f * band.sigma;
			band.hi = band.center + 3.f * band.sigma;
			bands.push_back(band);
		}
	} else if (settings.representation == SpectralRepresentation::SpectralBands) {
		const uint32_t n = laneFamily(settings);
		const float width = (settings.lambdaMax - settings.lambdaMin) / (float)n;
		for (uint32_t i = 0; i < n; i++) {
			Band band;
			band.lo = settings.lambdaMin + width * (float)i;
			band.hi = band.lo + width;
			band.center = 0.5f * (band.lo + band.hi);
			if (settings.bandBasis == BandBasis::Gaussian) {
				//neighbors cross at half their height
				band.sigma = width / 2.3548200450f;
			}
			bands.push_back(band);
		}
	}
	for (Band& band : bands) {
		for (uint32_t k = 0; k < BAND_QUADRATURE; k++) {
			if (band.sigma > 0.f) {
				band.nodes[k] = band.center + 1.4142135624f * band.sigma * HERMITE_NODES[k];
				band.weights[k] = HERMITE_WEIGHTS[k] / 1.7724538509f;
			} else {
				band.nodes[k] = 0.5f * (band.lo + band.hi) + 0.5f * (band.hi - band.lo) * LEGENDRE_NODES[k];
				band.weights[k] = 0.5f * LEGENDRE_WEIGHTS[k];
			}
		}
	}

	//the film matrix: the reconstruction sum(L_b psi_b) with psi_b = phi_b / sum(phi) against the matching
	//functions, at every whole nanometer the CIE tables have
	auto phi = [&](const Band& band, double lambda) -> double {
		if (band.sigma > 0.f) {
			const double x = (lambda - band.center) / band.sigma;
			return std::exp(-0.5 * x * x);
		}
		return (lambda >= band.lo && lambda < band.hi) || (lambda == band.hi && band.hi == settings.lambdaMax) ? 1.0 : 0.0;
	};
	std::vector<glm::dvec3> xyz(bands.size(), glm::dvec3(0.0));
	for (int i = 0; i < CIE_SAMPLES; i++) {
		const double lambda = LAMBDA_MIN + i;
		double total = 0.0;
		for (const Band& band : bands) {
			total += phi(band, lambda);
		}
		if (!(total > 1e-12)) {
			continue;
		}
		for (size_t b = 0; b < bands.size(); b++) {
			const double psi = phi(bands[b], lambda) / total;
			xyz[b] += psi * glm::dvec3(CIE_X[i], CIE_Y[i], CIE_Z[i]) / CIE_Y_INTEGRAL;
		}
	}
	for (size_t b = 0; b < bands.size(); b++) {
		bands[b].toRgb = glm::vec3(colorSpace(ColorSpaceId::Srgb).rgbFromXyz * xyz[b]);
	}
	return bands;
}

double projectOntoBand(const std::function<double(double)>& f, const Band& band)
{
	//the basis-weighted average, by the midpoint rule at 0.25 nm (6 sigma either side for a Gaussian)
	const double lo = band.sigma > 0.f ? band.center - 6.0 * band.sigma : band.lo;
	const double hi = band.sigma > 0.f ? band.center + 6.0 * band.sigma : band.hi;
	const int steps = std::max(4, (int)std::ceil((hi - lo) / 0.25));
	const double h = (hi - lo) / steps;
	double sum = 0.0;
	double weight = 0.0;
	for (int i = 0; i < steps; i++) {
		const double lambda = lo + (i + 0.5) * h;
		double w = 1.0;
		if (band.sigma > 0.f) {
			const double x = (lambda - band.center) / band.sigma;
			w = std::exp(-0.5 * x * x);
		}
		sum += w * f(lambda);
		weight += w;
	}
	return weight > 0.0 ? sum / weight : 0.0;
}

//---------------------------------------------------------------- to the film

glm::vec3 lanesToRgb(std::span<const float> values, const SampledWavelengths& wavelengths, const SpectralSettings& settings, std::span<const Band> bands)
{
	switch (settings.representation) {
	case SpectralRepresentation::ExactRgb:
		return glm::vec3(values[0], values[1], values[2]);
	case SpectralRepresentation::Monochromatic:
		return glm::vec3(values[0]);
	case SpectralRepresentation::HeroWavelengths: {
		glm::vec3 xyz(0.f);
		for (uint32_t i = 0; i < wavelengths.count; i++) {
			if (wavelengths.pdf[i] > 0.f) {
				xyz += cieAt(wavelengths.lambda[i]) * (values[i] / wavelengths.pdf[i]);
			}
		}
		xyz *= 1.f / ((float)wavelengths.count * (float)CIE_Y_INTEGRAL);
		return srgbFromXyzF() * xyz;
	}
	default: {
		glm::vec3 rgb(0.f);
		for (size_t b = 0; b < bands.size(); b++) {
			rgb += values[b] * bands[b].toRgb;
		}
		return rgb;
	}
	}
}

//---------------------------------------------------------------- SpectralData

uint32_t SpectralDataBuilder::floatBits(float value)
{
	uint32_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

namespace {

uint32_t bitsOf(float value)
{
	uint32_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

// Planck's law up to a constant factor, lambda in nm read as micrometers: 1 / (x^5 (exp(c2 / (x T)) - 1)), with
// c2 = h c / k = 14387.77 um K from pbrt's constants. crt_spectrum.glsl evaluates the same in float
double reducedPlanck(double lambda, double kelvin)
{
	if (!(kelvin > 0.0) || !(lambda > 0.0)) {
		return 0.0;
	}
	const double x = lambda * 1e-3;
	const double x2 = x * x;
	return 1.0 / (x2 * x2 * x * (std::exp(14387.770 / (x * kelvin)) - 1.0));
}

float floatOf(uint32_t bits)
{
	float value;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

// a reference's identity for interning: every field that changes how it packs
std::string internKey(const SpectrumRef& ref)
{
	std::string key;
	auto put = [&](const void* data, size_t bytes) { key.append(static_cast<const char*>(data), bytes); };
	put(&ref.kind, sizeof(ref.kind));
	put(&ref.rgb, sizeof(ref.rgb));
	put(&ref.value, sizeof(ref.value));
	put(&ref.scale, sizeof(ref.scale));
	put(&ref.width, sizeof(ref.width));
	put(&ref.profile, sizeof(ref.profile));
	put(&ref.formula, sizeof(ref.formula));
	put(ref.coefficients.data(), sizeof(double) * ref.coefficients.size());
	put(ref.samples.data(), sizeof(float) * ref.samples.size());
	return key;
}

}

std::vector<uint32_t> packSpectrumRef(const SpectrumRef& ref, SpectrumRole role, const SpectralSettings& settings, const SpectrumContext& context, std::span<const Band> bands)
{
	std::vector<uint32_t> w;
	const uint32_t roleBits = (uint32_t)role << 8;
	const bool banded = isBandRepresentation(settings.representation);
	//a band representation projects every reference on the CPU; a scalar (an index) is read at the band's center
	auto bandValues = [&](std::vector<uint32_t>& out) {
		for (const Band& band : bands) {
			const double value = role == SpectrumRole::Scalar ? evalSpectrumRef(ref, role, context, band.center)
															: projectOntoBand([&](double l) { return evalSpectrumRef(ref, role, context, l); }, band);
			out.push_back(bitsOf((float)value));
		}
	};
	if (ref.kind == SpectrumKind::Rgb && role != SpectrumRole::Scalar) {
		//the color, its uplift, and (banded) its projection: a texture's tint needs the color, and an untinted lookup
		//the rest
		w.push_back(TAG_RGB | roleBits);
		const glm::vec3 rgb = glm::max(ref.rgb, glm::vec3(0.f));
		w.push_back(bitsOf(rgb.r));
		w.push_back(bitsOf(rgb.g));
		w.push_back(bitsOf(rgb.b));
		UpliftedRgb u;
		if (context.table != nullptr) {
			u = upliftRgb(*context.table, rgb, role);
		}
		w.push_back(bitsOf(u.c.c0));
		w.push_back(bitsOf(u.c.c1));
		w.push_back(bitsOf(u.c.c2));
		w.push_back(bitsOf(u.scale));
		if (banded) {
			bandValues(w);
		}
		return w;
	}
	if (banded) {
		w.push_back(TAG_BANDS | roleBits);
		bandValues(w);
		return w;
	}
	const double normalization = role == SpectrumRole::Illuminant ? illuminantNormalization(ref) : 1.0;
	switch (ref.kind) {
	case SpectrumKind::Rgb:
		//a scalar RGB: its luminance
		w.push_back(TAG_CONSTANT | roleBits);
		w.push_back(bitsOf((float)scalarAt(ref, 0.0)));
		break;
	case SpectrumKind::Constant:
		w.push_back(TAG_CONSTANT | roleBits);
		w.push_back(bitsOf((float)(ref.value * normalization)));
		break;
	case SpectrumKind::Blackbody: {
		//Planck's law in micrometers with its constants dropped (reducedPlanck()), and one factor that brings it to
		//1 at the peak and applies the scale and the normalization: float holds it at any wavelength the range allows
		w.push_back(TAG_BLACKBODY | roleBits);
		w.push_back(bitsOf(ref.value));
		const double peak = reducedPlanck(2.8977721e-3 / ref.value * 1e9, ref.value);
		w.push_back(bitsOf((float)(ref.scale * normalization / peak)));
		break;
	}
	case SpectrumKind::Named:
	case SpectrumKind::Tabulated:
	case SpectrumKind::File: {
		w.push_back(TAG_TABULATED | roleBits);
		const uint32_t n = (uint32_t)(ref.samples.size() / 2);
		w.push_back(n);
		for (uint32_t i = 0; i < n; i++) {
			w.push_back(bitsOf(ref.samples[2 * i]));
		}
		for (uint32_t i = 0; i < n; i++) {
			w.push_back(bitsOf((float)(ref.samples[2 * i + 1] * ref.scale * normalization)));
		}
		break;
	}
	case SpectrumKind::Dispersion:
		w.push_back(TAG_DISPERSION | roleBits);
		w.push_back((uint32_t)ref.formula);
		for (const double c : ref.coefficients) {
			w.push_back(bitsOf((float)c));
		}
		break;
	case SpectrumKind::Line:
		w.push_back(TAG_LINE | roleBits);
		w.push_back(bitsOf(ref.value));
		w.push_back(bitsOf(ref.width));
		w.push_back((uint32_t)ref.profile);
		w.push_back(bitsOf(ref.scale));
		break;
	}
	return w;
}

SpectralDataBuilder::SpectralDataBuilder(const SpectralSettings& settings, const SpectrumContext& context, const WavelengthDistribution& distribution, uint32_t imageBuckets)
	: m_settings(settings), m_context(context)
{
	m_bands = buildBands(settings);
	m_words.assign(SPECTRAL_HEADER_WORDS, 0u);
	m_words[SH_REPRESENTATION] = (uint32_t)settings.representation;
	m_words[SH_LANES] = activeLanes(settings);
	m_words[SH_SAMPLING] = usesVisibleDensity(settings) ? (uint32_t)WavelengthSampling::Visible : (uint32_t)WavelengthSampling::Uniform;
	if (settings.sampling == WavelengthSampling::Lights && distribution.baseShare < 1.f) {
		m_words[SH_SAMPLING] |= 0x100u;
	}
	m_words[SH_COLOR_SPACE] = (uint32_t)settings.colorSpace;
	m_words[SH_LAMBDA_MIN] = floatBits(settings.lambdaMin);
	m_words[SH_LAMBDA_MAX] = floatBits(settings.lambdaMax);
	m_words[SH_MONO_LAMBDA] = floatBits(settings.monochromaticWavelength);
	m_words[SH_BAND_COUNT] = (uint32_t)m_bands.size();
	m_words[SH_UPLIFT_RES] = context.table != nullptr ? (uint32_t)context.table->res : 0u;
	m_words[SH_IMAGE_BUCKETS] = imageBuckets;
	m_words[SH_DESIGN_LAMBDA] = floatBits(settings.designWavelength);
	const glm::mat3 toWorking(convertRgb(ColorSpaceId::Srgb, settings.colorSpace));
	const glm::mat3 xyzToRgb(colorSpace(ColorSpaceId::Srgb).rgbFromXyz);
	for (int c = 0; c < 3; c++) {
		for (int r = 0; r < 3; r++) {
			m_words[SH_SRGB_TO_WORKING + 3 * c + r] = floatBits(toWorking[c][r]);
			m_words[SH_XYZ_TO_RGB + 3 * c + r] = floatBits(xyzToRgb[c][r]);
		}
	}

	//the CIE functions, x then y then z, and the working space's illuminant, at every whole nm from 360
	m_words[SH_CIE] = (uint32_t)m_words.size();
	for (const float* table : { CIE_X, CIE_Y, CIE_Z }) {
		for (int i = 0; i < CIE_SAMPLES; i++) {
			putFloat(table[i]);
		}
	}
	m_words[SH_ILLUMINANT] = (uint32_t)m_words.size();
	const DenseSpectrum& illuminant = *colorSpace(settings.colorSpace).illuminant;
	for (int i = 0; i < CIE_SAMPLES; i++) {
		putFloat((float)illuminant.values[i]);
	}

	m_words[SH_BANDS] = (uint32_t)m_words.size();
	for (const Band& band : m_bands) {
		putFloat(band.center);
		putFloat(band.lo);
		putFloat(band.hi);
		putFloat(band.sigma);
		for (const float node : band.nodes) {
			putFloat(node);
		}
		for (const float weight : band.weights) {
			putFloat(weight);
		}
		putFloat(band.toRgb.r);
		putFloat(band.toRgb.g);
		putFloat(band.toRgb.b);
		putFloat(0.f);
	}

	//baseShare, tableShare, line count, pad; the cdf; the density; the lines (center, half width, weight, pad)
	m_words[SH_DISTRIBUTION] = (uint32_t)m_words.size();
	putFloat(distribution.baseShare);
	putFloat(distribution.tableShare);
	m_words.push_back((uint32_t)distribution.lines.size());
	m_words.push_back(0u);
	for (uint32_t i = 0; i <= WAVELENGTH_TABLE_BINS; i++) {
		putFloat(i < distribution.cdf.size() ? distribution.cdf[i] : 1.f);
	}
	for (uint32_t i = 0; i < WAVELENGTH_TABLE_BINS; i++) {
		putFloat(i < distribution.density.size() ? distribution.density[i] : 0.f);
	}
	for (uint32_t i = 0; i < MAX_SAMPLING_LINES; i++) {
		const SamplingLine line = i < distribution.lines.size() ? distribution.lines[i] : SamplingLine {};
		putFloat(line.center);
		putFloat(line.halfWidth);
		putFloat(line.weight);
		putFloat(0.f);
	}
	m_poolStart = (uint32_t)m_words.size();
}

uint32_t SpectralDataBuilder::add(const SpectrumRef& ref, SpectrumRole role)
{
	const auto key = std::make_pair(internKey(ref), (uint32_t)role);
	if (const auto found = m_interned.find(key); found != m_interned.end()) {
		return found->second;
	}
	const uint32_t offset = (uint32_t)m_words.size();
	const std::vector<uint32_t> packed = packSpectrumRef(ref, role, m_settings, m_context, m_bands);
	m_words.insert(m_words.end(), packed.begin(), packed.end());
	m_interned.emplace(key, offset);
	return offset;
}

void SpectralDataBuilder::setScene(const SceneSpectra& spectra, std::span<const MaterialSpectralRecord> materials)
{
	m_refTable.clear();
	for (const auto& [ref, role] : spectra.list()) {
		m_refTable.push_back(add(ref, role));
	}
	m_materials.assign(materials.begin(), materials.end());
}

void SpectralDataBuilder::setBackground(const SpectrumRef& ref)
{
	m_background = add(ref, SpectrumRole::Illuminant);
}

std::vector<uint32_t> SpectralDataBuilder::finish()
{
	std::vector<uint32_t> words = m_words;
	words[SH_POOL] = m_poolStart;
	words[SH_BACKGROUND] = m_background;
	words[SH_REFS] = (uint32_t)words.size();
	words[SH_REF_COUNT] = (uint32_t)m_refTable.size();
	words.insert(words.end(), m_refTable.begin(), m_refTable.end());
	words[SH_MATERIALS] = (uint32_t)words.size();
	for (const auto& record : m_materials) {
		words.insert(words.end(), record.begin(), record.end());
	}
	//never empty, so a scene without materials still binds a valid record
	if (m_materials.empty()) {
		const MaterialSpectralRecord empty = emptyMaterialSpectralRecord();
		words.insert(words.end(), empty.begin(), empty.end());
	}
	return words;
}

uint32_t SceneSpectra::add(const SpectrumRef& ref, SpectrumRole role)
{
	const std::string key = internKey(ref) + (char)role;
	const auto [it, inserted] = m_index.try_emplace(key, (uint32_t)m_list.size());
	if (inserted) {
		m_list.emplace_back(ref, role);
	}
	return it->second;
}

MaterialSpectralRecord emptyMaterialSpectralRecord()
{
	MaterialSpectralRecord record;
	record.fill(SPECTRAL_NONE);
	record[SLOT_COUNT] = 0u;
	return record;
}

float evalPackedLane(std::span<const uint32_t> w, uint32_t lane, float lambda, const SpectralSettings& settings, const SpectrumContext& context,
	std::span<const Band> bands, glm::vec3 tint)
{
	if (w.empty()) {
		return 0.f;
	}
	const uint32_t tag = w[0] & 0xFFu;
	const SpectrumRole role = (SpectrumRole)((w[0] >> 8) & 0xFFu);
	const bool banded = isBandRepresentation(settings.representation);
	auto rgbAt = [&](const UpliftedRgb& u, float l) {
		return (float)evalUpliftedRgb(u, role, settings.colorSpace, l);
	};
	switch (tag) {
	case TAG_RGB: {
		const glm::vec3 rgb(floatOf(w[1]), floatOf(w[2]), floatOf(w[3]));
		const bool tinted = tint != glm::vec3(1.f);
		UpliftedRgb u;
		if (tinted) {
			if (context.table == nullptr) {
				return 0.f;
			}
			u = upliftRgb(*context.table, rgb * tint, role);
		} else {
			u.c = { floatOf(w[4]), floatOf(w[5]), floatOf(w[6]) };
			u.scale = floatOf(w[7]);
		}
		if (!banded) {
			return rgbAt(u, lambda);
		}
		if (!tinted) {
			return floatOf(w[8 + lane]);
		}
		const Band& band = bands[lane];
		float sum = 0.f;
		for (uint32_t k = 0; k < BAND_QUADRATURE; k++) {
			sum += band.weights[k] * rgbAt(u, band.nodes[k]);
		}
		return sum;
	}
	}
	//a non-RGB reference: its value, times the tint's own reflectance where one is given, as the shader does
	float value = 0.f;
	switch (tag) {
	case TAG_BANDS:
		value = floatOf(w[1 + lane]);
		break;
	case TAG_CONSTANT:
		value = floatOf(w[1]);
		break;
	case TAG_BLACKBODY:
		value = (float)reducedPlanck(lambda, floatOf(w[1])) * floatOf(w[2]);
		break;
	case TAG_TABULATED: {
		const uint32_t n = w[1];
		std::vector<float> samples;
		for (uint32_t i = 0; i < n; i++) {
			samples.push_back(floatOf(w[2 + i]));
			samples.push_back(floatOf(w[2 + n + i]));
		}
		value = (float)evalSpectrumRefRaw(tabulatedRef(samples), lambda);
		break;
	}
	case TAG_DISPERSION: {
		std::array<double, DISPERSION_COEFFICIENTS> c;
		for (size_t i = 0; i < DISPERSION_COEFFICIENTS; i++) {
			c[i] = floatOf(w[2 + i]);
		}
		value = (float)evalDispersion((DispersionFormula)w[1], c, lambda);
		break;
	}
	case TAG_LINE:
		value = (float)(floatOf(w[4]) * evalLineProfile((LineProfile)w[3], floatOf(w[1]), floatOf(w[2]), lambda));
		break;
	}
	if (tint != glm::vec3(1.f) && context.table != nullptr) {
		const UpliftedRgb u = upliftRgb(*context.table, tint, SpectrumRole::Albedo);
		float reflectance = 0.f;
		if (banded) {
			const Band& band = bands[lane];
			for (uint32_t k = 0; k < BAND_QUADRATURE; k++) {
				reflectance += band.weights[k] * (float)evalUpliftedRgb(u, SpectrumRole::Albedo, settings.colorSpace, band.nodes[k]);
			}
		} else {
			reflectance = (float)evalUpliftedRgb(u, SpectrumRole::Albedo, settings.colorSpace, lambda);
		}
		value *= reflectance;
	}
	return value;
}
