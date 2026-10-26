#include <color_space.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

namespace {

const DenseSpectrum& namedIlluminant(const char* name)
{
	static std::map<std::string, DenseSpectrum> cache;
	static std::mutex mutex;
	std::lock_guard lock(mutex);
	auto found = cache.find(name);
	if (found == cache.end()) {
		found = cache.emplace(name, namedSpectrum(name).value_or(DenseSpectrum {})).first;
	}
	return found->second;
}

// pbrt's RGBColorSpace constructor: the primaries' XYZ scaled so that RGB (1, 1, 1) is the illuminant's white
ColorSpace makeSpace(ColorSpaceId id, const char* key, const char* name, const char* gamut, const char* tableFile, glm::dvec2 r, glm::dvec2 g, glm::dvec2 b,
	const char* illuminant)
{
	ColorSpace space;
	space.id = id;
	space.key = key;
	space.name = name;
	space.gamut = gamut;
	space.tableFile = tableFile;
	space.r = r;
	space.g = g;
	space.b = b;
	space.illuminant = &namedIlluminant(illuminant);
	const glm::dvec3 w = spectrumToXyz(*space.illuminant);
	auto fromxyY = [](glm::dvec2 c) { return glm::dvec3(c.x / c.y, 1.0, (1.0 - c.x - c.y) / c.y); };
	//glm is column-major: the columns are the primaries' XYZ
	const glm::dmat3 rgb(fromxyY(r), fromxyY(g), fromxyY(b));
	const glm::dvec3 c = glm::inverse(rgb) * w;
	space.xyzFromRgb = glm::dmat3(rgb[0] * c.x, rgb[1] * c.y, rgb[2] * c.z);
	space.rgbFromXyz = glm::inverse(space.xyzFromRgb);
	return space;
}

const std::array<ColorSpace, (size_t)ColorSpaceId::Count>& spaces()
{
	//pbrt's RGBColorSpace::Init(), primaries and illuminants as given there
	static const std::array<ColorSpace, (size_t)ColorSpaceId::Count> table = {
		makeSpace(ColorSpaceId::Srgb, "srgb", "sRGB (Rec. 709)", "sRGB", "uplift_srgb.bin", { 0.64, 0.33 }, { 0.30, 0.60 }, { 0.15, 0.06 }, "stdillum-D65"),
		makeSpace(ColorSpaceId::DciP3, "dci-p3", "Display P3 (D65)", "DCI_P3", "uplift_dci_p3.bin", { 0.68, 0.32 }, { 0.265, 0.690 }, { 0.15, 0.06 }, "stdillum-D65"),
		makeSpace(ColorSpaceId::Rec2020, "rec2020", "Rec. 2020", "REC2020", "uplift_rec2020.bin", { 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 }, "stdillum-D65"),
		makeSpace(ColorSpaceId::Aces2065_1, "aces2065-1", "ACES2065-1", "ACES2065_1", "uplift_aces2065_1.bin", { 0.7347, 0.2653 }, { 0.0, 1.0 }, { 0.0001, -0.077 },
			"illum-acesD60"),
	};
	return table;
}

float lerp(float t, float a, float b)
{
	return (1.f - t) * a + t * b;
}

}

const ColorSpace& colorSpace(ColorSpaceId id)
{
	return spaces()[std::min((size_t)id, (size_t)ColorSpaceId::Count - 1)];
}

std::optional<ColorSpaceId> findColorSpace(std::string_view key)
{
	for (const ColorSpace& space : spaces()) {
		if (key == space.key) {
			return space.id;
		}
	}
	return std::nullopt;
}

glm::dmat3 convertRgb(ColorSpaceId from, ColorSpaceId to)
{
	if (from == to) {
		return glm::dmat3(1.0);
	}
	return colorSpace(to).rgbFromXyz * colorSpace(from).xyzFromRgb;
}

float sigmoid(float x)
{
	if (std::isinf(x)) {
		return x > 0.f ? 1.f : 0.f;
	}
	return 0.5f + x / (2.f * std::sqrt(1.f + x * x));
}

float evalSigmoidPolynomial(const SigmoidCoefficients& c, float lambda)
{
	//pbrt's EvaluatePolynomial(lambda, c2, c1, c0): Horner's rule, written out as the shader writes it
	return sigmoid(std::fma(std::fma(c.c0, lambda, c.c1), lambda, c.c2));
}

std::filesystem::path upliftTablePath(const std::filesystem::path& root, ColorSpaceId space)
{
	return root / "assets" / "spectra" / colorSpace(space).tableFile;
}

std::optional<UpliftTable> loadUpliftTable(const std::filesystem::path& path, ColorSpaceId space, std::string* error)
{
	auto fail = [&](const std::string& message) -> std::optional<UpliftTable> {
		if (error != nullptr) {
			*error = message;
		}
		return std::nullopt;
	};
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return fail("cannot open " + path.string() + " (the build writes it with tools/rgb2spec/rgb2spec_opt)");
	}
	char magic[8];
	uint32_t version = 0;
	uint32_t res = 0;
	file.read(magic, sizeof(magic));
	file.read(reinterpret_cast<char*>(&version), sizeof(version));
	file.read(reinterpret_cast<char*>(&res), sizeof(res));
	if (!file || std::memcmp(magic, "RGB2SPEC", 8) != 0 || version != 1 || res < 2 || res > 256) {
		return fail(path.string() + " is not an RGB2SPEC table of version 1");
	}
	UpliftTable table;
	table.space = space;
	table.res = (int)res;
	table.zNodes.resize(res);
	table.data.resize((size_t)9 * res * res * res);
	file.read(reinterpret_cast<char*>(table.zNodes.data()), (std::streamsize)(table.zNodes.size() * sizeof(float)));
	file.read(reinterpret_cast<char*>(table.data.data()), (std::streamsize)(table.data.size() * sizeof(float)));
	if (!file) {
		return fail(path.string() + " is shorter than its resolution says");
	}
	return table;
}

const UpliftTable* upliftTable(const std::filesystem::path& root, ColorSpaceId space, std::string* error)
{
	static std::map<std::pair<std::string, ColorSpaceId>, std::optional<UpliftTable>> cache;
	static std::mutex mutex;
	std::lock_guard lock(mutex);
	const auto key = std::make_pair(root.string(), space);
	auto found = cache.find(key);
	if (found == cache.end()) {
		std::string message;
		found = cache.emplace(key, loadUpliftTable(upliftTablePath(root, space), space, &message)).first;
		if (!found->second && error != nullptr) {
			*error = message;
		}
	}
	return found->second ? &*found->second : nullptr;
}

SigmoidCoefficients rgbToCoefficients(const UpliftTable& table, glm::vec3 rgb)
{
	rgb = glm::clamp(rgb, glm::vec3(0.f), glm::vec3(1.f));
	//a gray is a constant spectrum, which the polynomial represents exactly
	if (rgb.r == rgb.g && rgb.g == rgb.b) {
		return { 0.f, 0.f, (rgb.r - 0.5f) / std::sqrt(rgb.r * (1.f - rgb.r)) };
	}

	//the largest component picks the table; the other two, over it, place the color within
	const int res = table.res;
	const int maxc = (rgb.r > rgb.g) ? ((rgb.r > rgb.b) ? 0 : 2) : ((rgb.g > rgb.b) ? 1 : 2);
	const float z = rgb[maxc];
	const float x = rgb[(maxc + 1) % 3] * (float)(res - 1) / z;
	const float y = rgb[(maxc + 2) % 3] * (float)(res - 1) / z;

	const int xi = std::min((int)x, res - 2);
	const int yi = std::min((int)y, res - 2);
	//pbrt's FindInterval: the last node at or below z, kept inside [0, res - 2]
	int zi = 0;
	{
		int first = 1;
		int size = res - 2;
		while (size > 0) {
			const int half = size >> 1;
			const int middle = first + half;
			const bool predicate = table.zNodes[middle] < z;
			first = predicate ? middle + 1 : first;
			size = predicate ? size - (half + 1) : half;
		}
		zi = std::clamp(first - 1, 0, res - 2);
	}
	const float dx = x - (float)xi;
	const float dy = y - (float)yi;
	const float dz = (z - table.zNodes[zi]) / (table.zNodes[zi + 1] - table.zNodes[zi]);

	auto co = [&](int ox, int oy, int oz, int i) {
		return table.data[((((size_t)maxc * res + (zi + oz)) * res + (yi + oy)) * res + (xi + ox)) * 3 + i];
	};
	float c[3];
	for (int i = 0; i < 3; i++) {
		c[i] = lerp(dz, lerp(dy, lerp(dx, co(0, 0, 0, i), co(1, 0, 0, i)), lerp(dx, co(0, 1, 0, i), co(1, 1, 0, i))),
			lerp(dy, lerp(dx, co(0, 0, 1, i), co(1, 0, 1, i)), lerp(dx, co(0, 1, 1, i), co(1, 1, 1, i))));
	}
	return { c[0], c[1], c[2] };
}

UpliftedRgb upliftRgb(const UpliftTable& table, glm::vec3 rgb, SpectrumRole role)
{
	UpliftedRgb u;
	rgb = glm::max(rgb, glm::vec3(0.f));
	if (role == SpectrumRole::Albedo) {
		u.c = rgbToCoefficients(table, glm::min(rgb, glm::vec3(1.f)));
		u.scale = 1.f;
		return u;
	}
	//RGBUnboundedSpectrum and RGBIlluminantSpectrum: twice the largest component, so the rest lands in [0, 1/2]
	const float m = std::max(rgb.r, std::max(rgb.g, rgb.b));
	u.scale = 2.f * m;
	u.c = rgbToCoefficients(table, u.scale > 0.f ? rgb / u.scale : glm::vec3(0.f));
	return u;
}

double evalUpliftedRgb(const UpliftedRgb& u, SpectrumRole role, ColorSpaceId space, double lambda)
{
	using namespace spectrum_tables;
	//RGB data is defined only where the fit was made; outside it the nearer end is held
	const double held = std::clamp(lambda, (double)LAMBDA_MIN, (double)LAMBDA_MAX);
	const double s = (double)u.scale * evalSigmoidPolynomial(u.c, (float)held);
	if (role == SpectrumRole::Illuminant) {
		//pbrt's DenselySampledSpectrum: the sample at the nearest whole nanometer
		return s * (*colorSpace(space).illuminant)((int)std::lround(held));
	}
	return s;
}
