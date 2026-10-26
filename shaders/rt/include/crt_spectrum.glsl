// THE SPECTRUM A PATH CARRIES (docs/plans/completed/pbrt-spectral-rendering.md): its arithmetic, its wavelengths, the
// scene's SpectrumRefs evaluated at them, and its conversion to the film's linear sRGB. The line-for-line mirror of
// src/spectral.cpp and src/color_space.cpp, whose SpectralDataBuilder packs the SpectralData (binding 24) read here.
//
// A KERNEL THAT INCLUDES THIS FILE ITSELF is built once per lane family (CMakeLists.txt): without CRT_LANES, the exact
// RGB build, where Spectrum is a vec3, every function below is the vec3 expression it replaces, and nothing reads
// bindings 24, 25 or 63; with CRT_LANES 1, 4, 8, 16 or 32, the representation in SpectralData's header decides what
// the lanes mean:
//   monochromatic   one lane, the header's wavelength
//   RGB Gaussians   three bands with Gaussian sensitivities, in four lanes
//   spectral bands  CRT_LANES bands over the range, every path carrying every band
//   hero            CRT_LANES wavelengths drawn per path from one sample, PathState.lambdaU (PBR 4ed 4.5.4)
//
// The radiance slot (binding 4) stays a vec4 of film-space RGB in every family: a kernel that adds radiance converts
// its lanes when it adds them (filmAdd()), so kernel 09 and everything after it see no difference, and a contribution
// added before a dispersive refraction keeps the wavelength densities it had (PBR 4ed chapter 13's exercise).
//
// Requires crt_common.glsl.

#ifndef CRT_SPECTRUM_GLSL
#define CRT_SPECTRUM_GLSL

#define CRT_REPRESENTATION_RGB 0u
#define CRT_REPRESENTATION_MONOCHROMATIC 1u
#define CRT_REPRESENTATION_GAUSSIANS 2u
#define CRT_REPRESENTATION_BANDS 3u
#define CRT_REPRESENTATION_HERO 4u

//---------------------------------------------------------------- arithmetic
//
// Every operation a kernel does on a spectrum goes through these, so one source serves every family. In the exact RGB
// build each is the vec3 expression it names; the wide families loop over their vec4s.

#if !defined(CRT_LANE_VECS)
Spectrum spectrumConst(float v)
{
	return Spectrum(v);
}

Spectrum spectrumMul(Spectrum a, Spectrum b)
{
	return a * b;
}

Spectrum spectrumAdd(Spectrum a, Spectrum b)
{
	return a + b;
}

Spectrum spectrumScale(Spectrum a, float s)
{
	return a * s;
}

// a / s: not a * (1 / s), which rounds differently
Spectrum spectrumDivide(Spectrum a, float s)
{
	return a / s;
}

// a + (b - a) t, as GLSL's mix()
Spectrum spectrumMix(Spectrum a, Spectrum b, float t)
{
	return mix(a, b, t);
}

Spectrum spectrumOneMinus(Spectrum a)
{
	return 1.0 - a;
}

Spectrum spectrumMax(Spectrum a, float floorValue)
{
	return max(a, Spectrum(floorValue));
}

Spectrum spectrumClamp(Spectrum a, float lo, float hi)
{
	return clamp(a, Spectrum(lo), Spectrum(hi));
}

float spectrumLane(Spectrum a, uint i)
{
#if defined(CRT_SPECTRAL) && CRT_LANES == 1
	return a;
#else
	return a[i];
#endif
}

void spectrumSetLane(inout Spectrum a, uint i, float v)
{
#if defined(CRT_SPECTRAL) && CRT_LANES == 1
	a = v;
#else
	a[i] = v;
#endif
}
#else
Spectrum spectrumConst(float v)
{
	Spectrum s;
	for (int i = 0; i < CRT_LANE_VECS; i++) {
		s.c[i] = vec4(v);
	}
	return s;
}

Spectrum spectrumMul(Spectrum a, Spectrum b)
{
	for (int i = 0; i < CRT_LANE_VECS; i++) {
		a.c[i] *= b.c[i];
	}
	return a;
}

Spectrum spectrumAdd(Spectrum a, Spectrum b)
{
	for (int i = 0; i < CRT_LANE_VECS; i++) {
		a.c[i] += b.c[i];
	}
	return a;
}

Spectrum spectrumScale(Spectrum a, float s)
{
	for (int i = 0; i < CRT_LANE_VECS; i++) {
		a.c[i] *= s;
	}
	return a;
}

Spectrum spectrumDivide(Spectrum a, float s)
{
	for (int i = 0; i < CRT_LANE_VECS; i++) {
		a.c[i] /= s;
	}
	return a;
}

Spectrum spectrumMix(Spectrum a, Spectrum b, float t)
{
	for (int i = 0; i < CRT_LANE_VECS; i++) {
		a.c[i] = mix(a.c[i], b.c[i], t);
	}
	return a;
}

Spectrum spectrumOneMinus(Spectrum a)
{
	for (int i = 0; i < CRT_LANE_VECS; i++) {
		a.c[i] = 1.0 - a.c[i];
	}
	return a;
}

Spectrum spectrumMax(Spectrum a, float floorValue)
{
	for (int i = 0; i < CRT_LANE_VECS; i++) {
		a.c[i] = max(a.c[i], vec4(floorValue));
	}
	return a;
}

Spectrum spectrumClamp(Spectrum a, float lo, float hi)
{
	for (int i = 0; i < CRT_LANE_VECS; i++) {
		a.c[i] = clamp(a.c[i], vec4(lo), vec4(hi));
	}
	return a;
}

float spectrumLane(Spectrum a, uint i)
{
	return a.c[i >> 2u][i & 3u];
}

void spectrumSetLane(inout Spectrum a, uint i, float v)
{
	a.c[i >> 2u][i & 3u] = v;
}
#endif

// how many values a Spectrum holds: 3 in the exact RGB build, CRT_LANES in a spectral one
#ifdef CRT_SPECTRAL
#define CRT_SPECTRUM_SIZE CRT_LANES
#else
#define CRT_SPECTRUM_SIZE 3
#endif

// THE BOUND OF A LOOP OVER LANES THAT DOES REAL WORK (an evaluation, a table walk). In the 8-, 16- and 32-lane builds
// it is read from SpectralData's header, so the compiler cannot unroll the loop: unrolled, every evaluation is inlined
// once per lane at every call site, and Metal's compiler failed outright on the 16-lane complex build of kernel 06.
// The narrow builds keep a constant, which they unroll cheaply. Lanes past the bound stay at the zero they started at
#if defined(CRT_SPECTRAL) && CRT_LANES >= 8
#define CRT_LANE_END (spectralWords[1])
#else
#define CRT_LANE_END uint(CRT_SPECTRUM_SIZE)
#endif

// the lanes a representation uses: 3 for exact RGB and the RGB Gaussians, else the family's
#ifdef CRT_SPECTRAL
uint spectrumActiveLanes()
{
	return spectralWords[1];
}
#else
uint spectrumActiveLanes()
{
	return 3u;
}
#endif

// the largest value: what Russian roulette survives by, and what tells a contribution worth a shadow ray. A lane a
// representation does not use is zero, so it never wins
float spectrumMaxValue(Spectrum a)
{
#ifndef CRT_SPECTRAL
	return max(a.r, max(a.g, a.b));
#else
	float m = spectrumLane(a, 0u);
	for (uint i = 1u; i < uint(CRT_LANES); i++) {
		m = max(m, spectrumLane(a, i));
	}
	return m;
#endif
}

// what a choice between lobes weighs a colored quantity by: the luminance in exact RGB, as the materials always did;
// the mean over the lanes a representation uses in the spectral builds, which have no luminance per lane
float spectrumSelectionWeight(Spectrum a)
{
#ifndef CRT_SPECTRAL
	return dot(a, vec3(0.2126, 0.7152, 0.0722));
#else
	float sum = 0.0;
	for (uint i = 0u; i < CRT_LANE_END; i++) {
		sum += spectrumLane(a, i);
	}
	return sum / float(spectrumActiveLanes());
#endif
}

#ifdef CRT_SPECTRAL

//---------------------------------------------------------------- SpectralData

// the header's words: src/spectral.h SpectralHeaderWord
#define CRT_SH_REPRESENTATION 0u
#define CRT_SH_LANES 1u
#define CRT_SH_SAMPLING 2u
#define CRT_SH_COLOR_SPACE 3u
#define CRT_SH_LAMBDA_MIN 4u
#define CRT_SH_LAMBDA_MAX 5u
#define CRT_SH_MONO_LAMBDA 6u
#define CRT_SH_BAND_COUNT 7u
#define CRT_SH_CIE 8u
#define CRT_SH_ILLUMINANT 9u
#define CRT_SH_BANDS 10u
#define CRT_SH_DISTRIBUTION 11u
#define CRT_SH_MATERIALS 12u
#define CRT_SH_BACKGROUND 13u
#define CRT_SH_UPLIFT_RES 14u
#define CRT_SH_IMAGE_BUCKETS 15u
#define CRT_SH_SRGB_TO_WORKING 16u
#define CRT_SH_XYZ_TO_RGB 25u
#define CRT_SH_DESIGN_LAMBDA 34u
#define CRT_SH_REFS 36u

// a packed reference's tag: src/spectral.h SpectralTag
#define CRT_SPECTRUM_ZERO 0u
#define CRT_SPECTRUM_RGB 1u
#define CRT_SPECTRUM_CONSTANT 2u
#define CRT_SPECTRUM_BLACKBODY 3u
#define CRT_SPECTRUM_TABULATED 4u
#define CRT_SPECTRUM_DISPERSION 5u
#define CRT_SPECTRUM_LINE 6u
#define CRT_SPECTRUM_BANDS 7u
// and its role, bits 8 to 15: src/color_space.h SpectrumRole
#define CRT_ROLE_ALBEDO 0u
#define CRT_ROLE_UNBOUNDED 1u
#define CRT_ROLE_ILLUMINANT 2u
#define CRT_ROLE_SCALAR 3u

#define CRT_SPECTRAL_NONE 0xFFFFFFFFu
#define CRT_SPECTRUM_PI 3.14159265358979323846
// the largest float below 1
#define CRT_SPECTRUM_ONE_MINUS_EPSILON 0.99999994
#define CRT_BAND_WORDS 16u
#define CRT_WAVELENGTH_BINS 512u
#define CRT_MAX_SAMPLING_LINES 8u
#define CRT_CIE_SAMPLES 471
#define CRT_CIE_Y_INTEGRAL 106.856895
#define CRT_SPECTRAL_MATERIAL_WORDS 8u
#define CRT_SLOT_ALBEDO 0u
#define CRT_SLOT_EMISSION 1u
#define CRT_SLOT_AUX0 2u
#define CRT_SLOT_AUX1 3u

float spectralFloat(uint word)
{
	return uintBitsToFloat(spectralWords[word]);
}

// The representation and the wavelength sampling a spectral build is specialized for, set when the pipeline is built
// (src/rt_gpu.cpp pass()): MoltenVK folds the branches of every other one away before Metal compiles the kernel, which
// keeps the first build of a family to seconds. SpectralData's header says the same, for the tests that read it
layout (constant_id = 3) const uint CRT_REPRESENTATION_SPEC = 4u;
// 0 visible, 1 uniform, 2 the emitters' spectra mixed with the base density (src/spectral.h WavelengthSampling)
layout (constant_id = 4) const uint CRT_SAMPLING_SPEC = 0u;

uint spectralRepresentation()
{
	return CRT_REPRESENTATION_SPEC;
}

bool spectralBanded()
{
	const uint r = spectralRepresentation();
	return r == CRT_REPRESENTATION_GAUSSIANS || r == CRT_REPRESENTATION_BANDS;
}

mat3 spectralMatrix(uint word)
{
	return mat3(spectralFloat(word), spectralFloat(word + 1u), spectralFloat(word + 2u), spectralFloat(word + 3u), spectralFloat(word + 4u), spectralFloat(word + 5u),
		spectralFloat(word + 6u), spectralFloat(word + 7u), spectralFloat(word + 8u));
}

// the CIE functions and the working space's illuminant at the nearest whole nanometer, as pbrt's
// DenselySampledSpectrum reads them: zero outside 360 to 830
vec3 cieAt(float lambda)
{
	const int i = int(round(lambda)) - 360;
	if (i < 0 || i >= CRT_CIE_SAMPLES) {
		return vec3(0.0);
	}
	const uint base = spectralWords[CRT_SH_CIE] + uint(i);
	return vec3(spectralFloat(base), spectralFloat(base + uint(CRT_CIE_SAMPLES)), spectralFloat(base + 2u * uint(CRT_CIE_SAMPLES)));
}

float illuminantAt(float lambda)
{
	const int i = clamp(int(round(lambda)) - 360, 0, CRT_CIE_SAMPLES - 1);
	return spectralFloat(spectralWords[CRT_SH_ILLUMINANT] + uint(i));
}

//---------------------------------------------------------------- wavelengths

// THE PATH'S WAVELENGTHS, recomputed from PathState.lambdaU where a kernel needs them rather than stored (pbrt-v4
// chapter 15's exercise on compact wavelengths). Set by spectralBegin() at the top of every kernel that evaluates a
// spectrum or converts one
float g_lambda[CRT_LANES];
float g_lambdaPdf[CRT_LANES];

// PBR 4ed 5.4.2: pbrt's visible-importance density and its inverse CDF
float sampleVisibleWavelength(float u)
{
	return 538.0 - 138.888889 * atanh(0.85691062 - 1.82750197 * u);
}

float visibleWavelengthPdf(float lambda)
{
	if (lambda < 360.0 || lambda > 830.0) {
		return 0.0;
	}
	const float c = cosh(0.0072 * (lambda - 538.0));
	return 0.0039398042 / (c * c);
}

bool usesVisibleDensity()
{
	return CRT_SAMPLING_SPEC != 1u && (spectralWords[CRT_SH_SAMPLING] & 0xFFu) == 0u;
}

float baseSample(float u)
{
	if (usesVisibleDensity()) {
		return sampleVisibleWavelength(u);
	}
	const float a = spectralFloat(CRT_SH_LAMBDA_MIN);
	return a + u * (spectralFloat(CRT_SH_LAMBDA_MAX) - a);
}

float basePdf(float lambda)
{
	if (usesVisibleDensity()) {
		return visibleWavelengthPdf(lambda);
	}
	const float a = spectralFloat(CRT_SH_LAMBDA_MIN);
	const float b = spectralFloat(CRT_SH_LAMBDA_MAX);
	return (lambda >= a && lambda <= b) ? 1.0 / (b - a) : 0.0;
}

// the emitters' part of Lights sampling: the distribution's words are baseShare, tableShare, the line count, a pad, the
// cdf (bins + 1), the density (bins), and the lines as (center, half width, weight, pad)
float cauchySpan(float center, float halfWidth, float a, float b)
{
	return atan((b - center) / halfWidth) - atan((a - center) / halfWidth);
}

float sampleMixture(float u)
{
	const uint d = spectralWords[CRT_SH_DISTRIBUTION];
	const float baseShare = spectralFloat(d);
	const float tableShare = spectralFloat(d + 1u);
	const uint lineCount = spectralWords[d + 2u];
	if (CRT_SAMPLING_SPEC != 2u || u < baseShare || baseShare >= 1.0) {
		return baseSample(min(u / baseShare, CRT_SPECTRUM_ONE_MINUS_EPSILON));
	}
	const float a = spectralFloat(CRT_SH_LAMBDA_MIN);
	const float b = spectralFloat(CRT_SH_LAMBDA_MAX);
	float v = min((u - baseShare) / (1.0 - baseShare), CRT_SPECTRUM_ONE_MINUS_EPSILON);
	if (v < tableShare) {
		v /= tableShare;
		const uint cdf = d + 4u;
		uint lo = 0u;
		uint hi = CRT_WAVELENGTH_BINS;
		while (hi - lo > 1u) {
			const uint middle = (lo + hi) / 2u;
			if (spectralFloat(cdf + middle) <= v) {
				lo = middle;
			} else {
				hi = middle;
			}
		}
		const float width = spectralFloat(cdf + lo + 1u) - spectralFloat(cdf + lo);
		const float t = width > 0.0 ? (v - spectralFloat(cdf + lo)) / width : 0.5;
		return a + (b - a) * (float(lo) + t) / float(CRT_WAVELENGTH_BINS);
	}
	v -= tableShare;
	const uint lines = d + 4u + 2u * CRT_WAVELENGTH_BINS + 1u;
	for (uint i = 0u; i < lineCount; i++) {
		const float center = spectralFloat(lines + 4u * i);
		const float halfWidth = spectralFloat(lines + 4u * i + 1u);
		const float weight = spectralFloat(lines + 4u * i + 2u);
		if (v < weight || i + 1u == lineCount) {
			const float lo = atan((a - center) / halfWidth);
			const float w = clamp(v / weight, 0.0, CRT_SPECTRUM_ONE_MINUS_EPSILON);
			return clamp(center + halfWidth * tan(lo + w * cauchySpan(center, halfWidth, a, b)), a, b);
		}
		v -= weight;
	}
	return baseSample(0.5);
}

float wavelengthPdf(float lambda)
{
	if (CRT_SAMPLING_SPEC != 2u) {
		return basePdf(lambda);
	}
	const uint d = spectralWords[CRT_SH_DISTRIBUTION];
	const float baseShare = spectralFloat(d);
	float p = baseShare * basePdf(lambda);
	if (baseShare < 1.0) {
		const float tableShare = spectralFloat(d + 1u);
		const uint lineCount = spectralWords[d + 2u];
		const float a = spectralFloat(CRT_SH_LAMBDA_MIN);
		const float b = spectralFloat(CRT_SH_LAMBDA_MAX);
		float emitters = 0.0;
		if (tableShare > 0.0 && lambda >= a && lambda <= b) {
			const uint bin = min(uint((lambda - a) / (b - a) * float(CRT_WAVELENGTH_BINS)), CRT_WAVELENGTH_BINS - 1u);
			emitters += tableShare * spectralFloat(d + 4u + CRT_WAVELENGTH_BINS + 1u + bin);
		}
		const uint lines = d + 4u + 2u * CRT_WAVELENGTH_BINS + 1u;
		for (uint i = 0u; i < lineCount; i++) {
			const float center = spectralFloat(lines + 4u * i);
			const float halfWidth = spectralFloat(lines + 4u * i + 1u);
			const float weight = spectralFloat(lines + 4u * i + 2u);
			if (lambda >= a && lambda <= b) {
				const float t = (lambda - center) / halfWidth;
				emitters += weight / (halfWidth * (1.0 + t * t) * cauchySpan(center, halfWidth, a, b));
			}
		}
		p += (1.0 - baseShare) * emitters;
	}
	return p;
}

// a band's words: center, lo, hi, sigma, the quadrature's four nodes and four weights, the band-to-film column, a pad
uint bandWord(uint band)
{
	return spectralWords[CRT_SH_BANDS] + band * CRT_BAND_WORDS;
}

// Sets g_lambda and g_lambdaPdf for a path whose sample is `lambdaU`. Lanes past the representation's count are given
// the last band's or the first wavelength's, with a density of 0, and carry zero throughput
void spectralBegin(float lambdaU)
{
	const uint representation = spectralRepresentation();
	if (representation == CRT_REPRESENTATION_MONOCHROMATIC) {
		for (uint i = 0u; i < CRT_LANE_END; i++) {
			g_lambda[i] = spectralFloat(CRT_SH_MONO_LAMBDA);
			g_lambdaPdf[i] = i == 0u ? 1.0 : 0.0;
		}
		return;
	}
	const uint used = spectrumActiveLanes();
	if (representation != CRT_REPRESENTATION_HERO) {
		for (uint i = 0u; i < CRT_LANE_END; i++) {
			g_lambda[i] = spectralFloat(bandWord(min(i, used - 1u)));
			g_lambdaPdf[i] = i < used ? 1.0 : 0.0;
		}
		return;
	}
	const float step = 1.0 / float(used);
	for (uint i = 0u; i < CRT_LANE_END; i++) {
		float up = lambdaU + float(i) * step;
		if (up >= 1.0) {
			up -= 1.0;
		}
		g_lambda[i] = sampleMixture(up);
		g_lambdaPdf[i] = i < used ? wavelengthPdf(g_lambda[i]) : 0.0;
	}
}

//---------------------------------------------------------------- the RGB uplift (src/color_space.cpp)

float sigmoid(float x)
{
	if (isinf(x)) {
		return x > 0.0 ? 1.0 : 0.0;
	}
	return 0.5 + x / (2.0 * sqrt(1.0 + x * x));
}

float evalSigmoidPolynomial(vec3 c, float lambda)
{
	return sigmoid(fma(fma(c.x, lambda, c.y), lambda, c.z));
}

// pbrt's RGBToSpectrumTable::operator() over binding 25: an RGB in [0, 1] to its three coefficients
vec3 rgbToCoefficients(vec3 rgb)
{
	rgb = clamp(rgb, vec3(0.0), vec3(1.0));
	if (rgb.r == rgb.g && rgb.g == rgb.b) {
		return vec3(0.0, 0.0, (rgb.r - 0.5) / sqrt(rgb.r * (1.0 - rgb.r)));
	}
	const int res = int(spectralWords[CRT_SH_UPLIFT_RES]);
	const int maxc = (rgb.r > rgb.g) ? ((rgb.r > rgb.b) ? 0 : 2) : ((rgb.g > rgb.b) ? 1 : 2);
	const float z = rgb[maxc];
	const float x = rgb[(maxc + 1) % 3] * float(res - 1) / z;
	const float y = rgb[(maxc + 2) % 3] * float(res - 1) / z;
	const int xi = min(int(x), res - 2);
	const int yi = min(int(y), res - 2);
	int first = 1;
	int size = res - 2;
	while (size > 0) {
		const int halfSize = size >> 1;
		const int middle = first + halfSize;
		const bool predicate = upliftData[middle] < z;
		first = predicate ? middle + 1 : first;
		size = predicate ? size - (halfSize + 1) : halfSize;
	}
	const int zi = clamp(first - 1, 0, res - 2);
	const float dx = x - float(xi);
	const float dy = y - float(yi);
	const float dz = (z - upliftData[zi]) / (upliftData[zi + 1] - upliftData[zi]);
	vec3 c;
	for (int i = 0; i < 3; i++) {
		const int base = res;
		const int s0 = (((maxc * res + zi) * res + yi) * res + xi) * 3 + i;
		const int sx = 3;
		const int sy = res * 3;
		const int sz = res * res * 3;
		const float c000 = upliftData[base + s0];
		const float c100 = upliftData[base + s0 + sx];
		const float c010 = upliftData[base + s0 + sy];
		const float c110 = upliftData[base + s0 + sy + sx];
		const float c001 = upliftData[base + s0 + sz];
		const float c101 = upliftData[base + s0 + sz + sx];
		const float c011 = upliftData[base + s0 + sz + sy];
		const float c111 = upliftData[base + s0 + sz + sy + sx];
		c[i] = mix(mix(mix(c000, c100, dx), mix(c010, c110, dx), dy), mix(mix(c001, c101, dx), mix(c011, c111, dx), dy), dz);
	}
	return c;
}

// an RGB's coefficients (xyz) and scale (w) in a role: src/color_space.cpp upliftRgb()
vec4 upliftRgb(vec3 rgb, uint role)
{
	rgb = max(rgb, vec3(0.0));
	if (role == CRT_ROLE_ALBEDO) {
		return vec4(rgbToCoefficients(min(rgb, vec3(1.0))), 1.0);
	}
	const float scale = 2.0 * max(rgb.r, max(rgb.g, rgb.b));
	return vec4(rgbToCoefficients(scale > 0.0 ? rgb / scale : vec3(0.0)), scale);
}

// an uplifted RGB's value at lambda; RGB data is held at the nearer end of 360 to 830 nm outside it
float evalUpliftedRgb(vec4 u, uint role, float lambda)
{
	const float held = clamp(lambda, 360.0, 830.0);
	const float s = u.w * evalSigmoidPolynomial(u.xyz, held);
	return role == CRT_ROLE_ILLUMINANT ? s * illuminantAt(held) : s;
}

// an uplifted RGB (coefficients and scale, upliftRgb()) at the path's lanes, or projected onto the bands by their
// quadrature
Spectrum spectrumFromUplift(vec4 u, uint role)
{
	Spectrum s = spectrumConst(0.0);
	if (spectralBanded()) {
		const uint bands = spectralWords[CRT_SH_BAND_COUNT];
		for (uint b = 0u; b < bands; b++) {
			const uint w = bandWord(b);
			float sum = 0.0;
			for (uint k = 0u; k < 4u; k++) {
				sum += spectralFloat(w + 8u + k) * evalUpliftedRgb(u, role, spectralFloat(w + 4u + k));
			}
			spectrumSetLane(s, b, sum);
		}
		return s;
	}
	for (uint i = 0u; i < CRT_LANE_END; i++) {
		spectrumSetLane(s, i, g_lambdaPdf[i] > 0.0 || i == 0u ? evalUpliftedRgb(u, role, g_lambda[i]) : 0.0);
	}
	return s;
}

// a color only known at run time, already in the working space, as a spectrum in a role
Spectrum spectrumFromRgb(vec3 rgb, uint role)
{
	return spectrumFromUplift(upliftRgb(rgb, role), role);
}

// linear sRGB (a texel, the environment, the sky) in the working space, clamped at zero
vec3 srgbToWorking(vec3 rgb)
{
	return max(spectralMatrix(CRT_SH_SRGB_TO_WORKING) * rgb, vec3(0.0));
}

//---------------------------------------------------------------- the scene's references (src/spectral.cpp)

// Planck's law up to a constant, lambda in nm read as micrometers (src/spectral.cpp reducedPlanck())
float reducedPlanck(float lambda, float kelvin)
{
	if (!(kelvin > 0.0) || !(lambda > 0.0)) {
		return 0.0;
	}
	const float x = lambda * 1e-3;
	const float x2 = x * x;
	return 1.0 / (x2 * x2 * x * (exp(14387.770 / (x * kelvin)) - 1.0));
}

float sellmeierTerms(uint c, uint terms, float l2)
{
	float sum = 0.0;
	for (uint i = 0u; i < terms; i++) {
		sum += spectralFloat(c + 2u * i) * l2 / (l2 - spectralFloat(c + 2u * i + 1u));
	}
	return sum;
}

// src/spectrum_ref.cpp evalDispersion(), the coefficients at word c
float evalDispersion(uint formula, uint c, float lambda)
{
	const float l = lambda * 1e-3;
	const float l2 = l * l;
	const float li2 = 1.0 / l2;
	float n2 = 1.0;
	switch (formula) {
	case 1u:
		n2 = spectralFloat(c) + spectralFloat(c + 1u) * l2 + li2 * (spectralFloat(c + 2u) + li2 * (spectralFloat(c + 3u) + li2 * (spectralFloat(c + 4u) + li2 * spectralFloat(c + 5u))));
		break;
	case 2u:
		n2 = 1.0 + sellmeierTerms(c, 3u, l2);
		break;
	case 3u: {
		const float L = 1.0 / (l2 - 0.028);
		return spectralFloat(c) + spectralFloat(c + 1u) * L + spectralFloat(c + 2u) * L * L + spectralFloat(c + 3u) * l2 + spectralFloat(c + 4u) * l2 * l2 + spectralFloat(c + 5u) * l2 * l2 * l2;
	}
	case 4u:
		n2 = 1.0 + spectralFloat(c) + spectralFloat(c + 1u) * l2 / (l2 - spectralFloat(c + 2u) * spectralFloat(c + 2u)) + spectralFloat(c + 3u) / (l2 - spectralFloat(c + 4u) * spectralFloat(c + 4u));
		break;
	case 5u:
		return spectralFloat(c) + spectralFloat(c + 1u) / l + spectralFloat(c + 2u) / pow(l, 3.5);
	case 6u:
		n2 = 1.0 + sellmeierTerms(c, 4u, l2);
		break;
	case 7u:
		n2 = spectralFloat(c) + spectralFloat(c + 1u) / (l2 - spectralFloat(c + 2u)) - spectralFloat(c + 3u) * l2;
		break;
	case 8u:
		n2 = spectralFloat(c) + spectralFloat(c + 1u) * l2 / (l2 - spectralFloat(c + 2u)) - spectralFloat(c + 3u) * l2;
		break;
	case 9u:
		n2 = spectralFloat(c) + spectralFloat(c + 1u) * l2 / (l2 - spectralFloat(c + 2u)) + spectralFloat(c + 3u) * l2 / (l2 - spectralFloat(c + 4u));
		break;
	case 10u:
		n2 = spectralFloat(c) + spectralFloat(c + 1u) * l2
			+ li2 * (spectralFloat(c + 2u) + li2 * (spectralFloat(c + 3u) + li2 * (spectralFloat(c + 4u) + li2 * (spectralFloat(c + 5u) + li2 * (spectralFloat(c + 6u) + li2 * spectralFloat(c + 7u))))));
		break;
	case 11u:
		n2 = 1.0 + sellmeierTerms(c, 5u, l2);
		break;
	case 12u:
		n2 = spectralFloat(c) + spectralFloat(c + 1u) * l2 + li2 * (spectralFloat(c + 2u) + li2 * (spectralFloat(c + 3u) + li2 * (spectralFloat(c + 4u) + li2 * spectralFloat(c + 5u))))
			+ spectralFloat(c + 6u) * l2 * l2 + spectralFloat(c + 7u) * l2 * l2 * l2;
		break;
	case 13u:
		n2 = spectralFloat(c) + spectralFloat(c + 1u) * l2 + spectralFloat(c + 2u) * l2 * l2
			+ li2 * (spectralFloat(c + 3u) + li2 * (spectralFloat(c + 4u) + li2 * (spectralFloat(c + 5u) + li2 * (spectralFloat(c + 6u) + li2 * (spectralFloat(c + 7u) + li2 * spectralFloat(c + 8u))))));
		break;
	case 100u:
		return spectralFloat(c) + spectralFloat(c + 1u) * li2 + spectralFloat(c + 2u) * li2 * li2;
	}
	return sqrt(max(n2, 0.0));
}

float evalLineProfile(uint profile, float center, float fwhm, float lambda)
{
	if (!(fwhm > 0.0)) {
		return 0.0;
	}
	const float x = lambda - center;
	if (profile == 1u) {
		const float gamma = 0.5 * fwhm;
		return gamma / (CRT_SPECTRUM_PI * (x * x + gamma * gamma));
	}
	const float sigma = fwhm / 2.3548200450309493;
	return exp(-0.5 * x * x / (sigma * sigma)) / (sigma * 2.5066282746310002);
}

// linear between the pairs, the end values held beyond them
float evalTabulated(uint w, float lambda)
{
	const uint n = spectralWords[w];
	const uint ls = w + 1u;
	const uint vs = w + 1u + n;
	if (lambda <= spectralFloat(ls)) {
		return spectralFloat(vs);
	}
	if (lambda >= spectralFloat(ls + n - 1u)) {
		return spectralFloat(vs + n - 1u);
	}
	uint lo = 0u;
	uint hi = n - 1u;
	while (hi - lo > 1u) {
		const uint middle = (lo + hi) / 2u;
		if (spectralFloat(ls + middle) <= lambda) {
			lo = middle;
		} else {
			hi = middle;
		}
	}
	const float l0 = spectralFloat(ls + lo);
	const float t = (lambda - l0) / (spectralFloat(ls + hi) - l0);
	return (1.0 - t) * spectralFloat(vs + lo) + t * spectralFloat(vs + hi);
}

// one packed reference's value at a wavelength, for the pointwise kinds a color can be. An index's formulas are read
// by scalarFromRef() alone, so the code every color evaluation inlines stays small
float evalRefAt(uint ref, uint tag, float lambda)
{
	switch (tag) {
	case CRT_SPECTRUM_CONSTANT:
		return spectralFloat(ref + 1u);
	case CRT_SPECTRUM_BLACKBODY:
		return reducedPlanck(lambda, spectralFloat(ref + 1u)) * spectralFloat(ref + 2u);
	case CRT_SPECTRUM_TABULATED:
		return evalTabulated(ref + 1u, lambda);
	case CRT_SPECTRUM_LINE:
		return spectralFloat(ref + 4u) * evalLineProfile(spectralWords[ref + 3u], spectralFloat(ref + 1u), spectralFloat(ref + 2u), lambda);
	}
	return 0.0;
}

// THE SCENE'S SPECTRA AT THE PATH'S LANES: the reference packed at word `ref`, with an RGB reference multiplied by
// `tint` first (a texture's texel in the working space; vec3(1) for none), as src/spectral.cpp evalPackedLane() does.
// CRT_SPECTRAL_NONE with a role reads `tint` itself as an RGB value in that role (the environment's texel). Written so
// the table lookup appears once: every caller inlines this, and the lookup is most of a kernel's build time
Spectrum spectrumFromRefOrRgb(uint ref, vec3 tint, uint noneRole)
{
	uint tag = CRT_SPECTRUM_RGB;
	uint role = noneRole;
	vec3 rgb = vec3(1.0);
	if (ref != CRT_SPECTRAL_NONE) {
		tag = spectralWords[ref] & 0xFFu;
		role = (spectralWords[ref] >> 8u) & 0xFFu;
		if (tag == CRT_SPECTRUM_RGB) {
			rgb = vec3(spectralFloat(ref + 1u), spectralFloat(ref + 2u), spectralFloat(ref + 3u));
		}
	}
	const bool tinted = tint != vec3(1.0);
	// an untinted color's coefficients, and its projection onto the bands, were made on the CPU
	if (tag == CRT_SPECTRUM_RGB && ref != CRT_SPECTRAL_NONE && !tinted && spectralBanded()) {
		Spectrum s = spectrumConst(0.0);
		const uint bands = spectralWords[CRT_SH_BAND_COUNT];
		for (uint b = 0u; b < bands; b++) {
			spectrumSetLane(s, b, spectralFloat(ref + 8u + b));
		}
		return s;
	}
	// the one lookup: the tinted color, or the tint alone as the reflectance a non-RGB reference is multiplied by
	vec4 u = vec4(0.0);
	const bool lookup = tag == CRT_SPECTRUM_RGB ? (tinted || ref == CRT_SPECTRAL_NONE) : tinted;
	if (lookup) {
		u = upliftRgb(tag == CRT_SPECTRUM_RGB ? rgb * tint : tint, tag == CRT_SPECTRUM_RGB ? role : CRT_ROLE_ALBEDO);
	} else if (tag == CRT_SPECTRUM_RGB) {
		u = vec4(spectralFloat(ref + 4u), spectralFloat(ref + 5u), spectralFloat(ref + 6u), spectralFloat(ref + 7u));
	}
	if (tag == CRT_SPECTRUM_RGB) {
		return spectrumFromUplift(u, role);
	}
	Spectrum s = spectrumConst(0.0);
	if (tag == CRT_SPECTRUM_BANDS) {
		const uint bands = spectralWords[CRT_SH_BAND_COUNT];
		for (uint b = 0u; b < bands; b++) {
			spectrumSetLane(s, b, spectralFloat(ref + 1u + b));
		}
	} else if (tag == CRT_SPECTRUM_CONSTANT) {
		s = spectrumConst(spectralFloat(ref + 1u));
	} else {
		for (uint i = 0u; i < CRT_LANE_END; i++) {
			spectrumSetLane(s, i, g_lambdaPdf[i] > 0.0 || i == 0u ? evalRefAt(ref, tag, g_lambda[i]) : 0.0);
		}
	}
	if (tinted) {
		s = spectrumMul(s, spectrumFromUplift(u, CRT_ROLE_ALBEDO));
	}
	return s;
}

Spectrum spectrumFromRef(uint ref, vec3 tint)
{
	return spectrumFromRefOrRgb(ref, tint, CRT_ROLE_ALBEDO);
}

// a scalar reference (an index) at one lane's wavelength, or at its band's center
float scalarFromRef(uint ref, uint lane)
{
	if (ref == CRT_SPECTRAL_NONE) {
		return 0.0;
	}
	const uint tag = spectralWords[ref] & 0xFFu;
	if (tag == CRT_SPECTRUM_BANDS) {
		return spectralFloat(ref + 1u + lane);
	}
	if (tag == CRT_SPECTRUM_DISPERSION) {
		return evalDispersion(spectralWords[ref + 1u], ref + 2u, g_lambda[lane]);
	}
	return evalRefAt(ref, tag, g_lambda[lane]);
}

// the packed offset of the scene's spectrum `index` (src/spectral.h SceneSpectra), which is how the materials' records
// and the lights name theirs
uint sceneSpectrumRef(uint index)
{
	return index == CRT_SPECTRAL_NONE ? CRT_SPECTRAL_NONE : spectralWords[spectralWords[CRT_SH_REFS] + index];
}

// a scalar reference that is not a formula (a conductor's eta or k: a constant, a table, or bands) at every lane. The
// kind is decided once, outside the lanes, so a kernel inlines one table walk rather than one per lane and kind
Spectrum spectrumFromScalarRef(uint ref)
{
	if (ref == CRT_SPECTRAL_NONE) {
		return spectrumConst(0.0);
	}
	const uint tag = spectralWords[ref] & 0xFFu;
	Spectrum s = spectrumConst(0.0);
	if (tag == CRT_SPECTRUM_BANDS) {
		const uint bands = spectralWords[CRT_SH_BAND_COUNT];
		for (uint b = 0u; b < bands; b++) {
			spectrumSetLane(s, b, spectralFloat(ref + 1u + b));
		}
	} else if (tag == CRT_SPECTRUM_TABULATED) {
		for (uint i = 0u; i < CRT_LANE_END; i++) {
			spectrumSetLane(s, i, evalTabulated(ref + 1u, g_lambda[i]));
		}
	} else if (tag == CRT_SPECTRUM_CONSTANT) {
		s = spectrumConst(spectralFloat(ref + 1u));
	} else if (tag == CRT_SPECTRUM_DISPERSION) {
		// a formula on a conductor is unusual, and read at the first lane alone
		s = spectrumConst(evalDispersion(spectralWords[ref + 1u], ref + 2u, g_lambda[0]));
	}
	return s;
}

// a material's reference in a slot (src/spectral.h SpectralSlot), and its emission's scale
uint materialSpectrumRef(uint material, uint slot)
{
	return sceneSpectrumRef(spectralWords[spectralWords[CRT_SH_MATERIALS] + material * CRT_SPECTRAL_MATERIAL_WORDS + slot]);
}

float materialEmissionScale(uint material)
{
	return spectralFloat(spectralWords[CRT_SH_MATERIALS] + material * CRT_SPECTRAL_MATERIAL_WORDS + 4u);
}

//---------------------------------------------------------------- dispersion

// a bit per active lane. A shift by 32 is undefined in GLSL (Metal gives 1u << 0), so 32 lanes are named outright
uint spectrumFullLaneMask()
{
	const uint lanes = spectrumActiveLanes();
	return lanes >= 32u ? 0xFFFFFFFFu : (1u << lanes) - 1u;
}

uint spectrumLiveLanes(uint laneMask)
{
	return bitCount(laneMask & spectrumFullLaneMask());
}

// PBR 4ed 4.5.4 and 10.5.1, pbrt's TerminateSecondary(): a dispersive refraction sends each wavelength its own way, so
// the path keeps one lane and multiplies it by the lanes that were alive, which keeps every estimate unbiased (the
// contributions already added keep the densities they had). Hero keeps lane 0 if alive; the bands keep a live lane
// chosen by `u`. Returns the lane kept
uint terminateSecondary(inout Spectrum throughput, inout uint laneMask, float u)
{
	const uint live = spectrumLiveLanes(laneMask);
	if (live <= 1u) {
		return findLSB(laneMask);
	}
	uint keep = findLSB(laneMask);
	if (spectralRepresentation() != CRT_REPRESENTATION_HERO) {
		uint pick = min(uint(u * float(live)), live - 1u);
		for (uint i = 0u; i < CRT_LANE_END; i++) {
			if ((laneMask & (1u << i)) != 0u) {
				if (pick == 0u) {
					keep = i;
					break;
				}
				pick--;
			}
		}
	}
	const float kept = spectrumLane(throughput, keep) * float(live);
	throughput = spectrumConst(0.0);
	spectrumSetLane(throughput, keep, kept);
	laneMask = 1u << keep;
	return keep;
}

//---------------------------------------------------------------- to the film

// The lanes as the film's linear sRGB (src/spectral.cpp lanesToRgb()):
//   monochromatic  the one value in every channel
//   hero           pbrt's Monte Carlo estimate of the XYZ integrals, sum(L x-bar / pdf) / N / CIE_Y_INTEGRAL, then
//                  XYZ to linear sRGB (PBR 4ed eq. 4.23)
//   bands          the band-to-film matrix
vec3 spectrumToFilm(Spectrum s)
{
	const uint representation = spectralRepresentation();
	if (representation == CRT_REPRESENTATION_MONOCHROMATIC) {
		return vec3(spectrumLane(s, 0u));
	}
	if (representation == CRT_REPRESENTATION_HERO) {
		vec3 xyz = vec3(0.0);
		for (uint i = 0u; i < CRT_LANE_END; i++) {
			if (g_lambdaPdf[i] > 0.0) {
				xyz += cieAt(g_lambda[i]) * (spectrumLane(s, i) / g_lambdaPdf[i]);
			}
		}
		xyz *= 1.0 / (float(spectrumActiveLanes()) * float(CRT_CIE_Y_INTEGRAL));
		return spectralMatrix(CRT_SH_XYZ_TO_RGB) * xyz;
	}
	vec3 rgb = vec3(0.0);
	const uint bands = spectralWords[CRT_SH_BAND_COUNT];
	for (uint b = 0u; b < bands; b++) {
		const uint w = bandWord(b) + 12u;
		rgb += spectrumLane(s, b) * vec3(spectralFloat(w), spectralFloat(w + 1u), spectralFloat(w + 2u));
	}
	return rgb;
}

// the working space's white as an emitter, the RGB (1, 1, 1) uplifted: the space's illuminant itself, without a lookup
Spectrum spectrumWhiteIlluminant()
{
	Spectrum s = spectrumConst(0.0);
	if (spectralBanded()) {
		const uint bands = spectralWords[CRT_SH_BAND_COUNT];
		for (uint b = 0u; b < bands; b++) {
			const uint w = bandWord(b);
			float sum = 0.0;
			for (uint k = 0u; k < 4u; k++) {
				sum += spectralFloat(w + 8u + k) * illuminantAt(clamp(spectralFloat(w + 4u + k), 360.0, 830.0));
			}
			spectrumSetLane(s, b, sum);
		}
		return s;
	}
	for (uint i = 0u; i < CRT_LANE_END; i++) {
		spectrumSetLane(s, i, illuminantAt(clamp(g_lambda[i], 360.0, 830.0)));
	}
	return s;
}

// a reflectance's color, as the auxiliary outputs report an albedo: its product with the working space's white,
// through the film, so a reflectance of 1 everywhere is white in every representation (the mean of the samples, for
// hero)
vec3 reflectanceToFilm(Spectrum reflectance)
{
	return spectrumToFilm(spectrumMul(reflectance, spectrumWhiteIlluminant()));
}

#endif

// THE ONE PLACE RADIANCE REACHES THE FILM: adds a contribution to the radiance slot as the film's color, and returns
// that color for the light groups. In the exact RGB build, the vec3 add it always was
vec3 filmAdd(uint slot, Spectrum contribution)
{
#ifndef CRT_SPECTRAL
	radiance[slot].rgb += contribution;
	return contribution;
#else
	const vec3 rgb = spectrumToFilm(contribution);
	radiance[slot].rgb += rgb;
	return rgb;
#endif
}

#endif
