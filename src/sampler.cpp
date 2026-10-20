#include <sampler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <fmt/format.h>

#include <sampler_halton_tables.h>

namespace {

constexpr float ONE_MINUS_EPSILON = 0x1.fffffep-1f;

// pbrt's MaxHaltonResolution: the image is tiled in 128 x 128 blocks of pixels that share one set
constexpr uint32_t MAX_HALTON_RESOLUTION = 128;

// the 24 permutations of four digits ZSobol picks from
constexpr uint8_t Z_PERMUTATIONS[24][4] = {
	{ 0, 1, 2, 3 }, { 0, 1, 3, 2 }, { 0, 2, 1, 3 }, { 0, 2, 3, 1 }, { 0, 3, 2, 1 }, { 0, 3, 1, 2 },
	{ 1, 0, 2, 3 }, { 1, 0, 3, 2 }, { 1, 2, 0, 3 }, { 1, 2, 3, 0 }, { 1, 3, 2, 0 }, { 1, 3, 0, 2 },
	{ 2, 1, 0, 3 }, { 2, 1, 3, 0 }, { 2, 0, 1, 3 }, { 2, 0, 3, 1 }, { 2, 3, 0, 1 }, { 2, 3, 1, 0 },
	{ 3, 1, 2, 0 }, { 3, 1, 0, 2 }, { 3, 2, 1, 0 }, { 3, 2, 0, 1 }, { 3, 0, 2, 1 }, { 3, 0, 1, 2 },
};

uint32_t ceilLog2(uint32_t v)
{
	uint32_t log = 0;
	while ((1ull << log) < v) {
		log++;
	}
	return log;
}

int64_t extendedGcd(int64_t a, int64_t b, int64_t& x, int64_t& y)
{
	if (b == 0) {
		x = 1;
		y = 0;
		return a;
	}
	int64_t xp, yp;
	const int64_t d = a / b;
	const int64_t gcd = extendedGcd(b, a % b, xp, yp);
	x = yp;
	y = xp - d * yp;
	return gcd;
}

// pbrt's multiplicativeInverse(): x with a * x == 1 (mod n)
uint32_t multiplicativeInverse(int64_t a, int64_t n)
{
	int64_t x, y;
	extendedGcd(a, n, x, y);
	return (uint32_t)(((x % n) + n) % n);
}

bool readFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes, std::string& error)
{
	FILE* file = fopen(path.string().c_str(), "rb");
	if (file == nullptr) {
		error = fmt::format("cannot open {}", path.string());
		return false;
	}
	fseek(file, 0, SEEK_END);
	const long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	bytes.resize((size_t)std::max(size, 0l));
	const size_t read = bytes.empty() ? 0 : fread(bytes.data(), 1, bytes.size(), file);
	fclose(file);
	if (read != bytes.size()) {
		error = fmt::format("cannot read {}", path.string());
		return false;
	}
	return true;
}

template <typename T>
bool copyArray(const std::vector<uint8_t>& bytes, size_t& offset, size_t count, std::vector<T>& out)
{
	if (offset + count * sizeof(T) > bytes.size()) {
		return false;
	}
	out.resize(count);
	memcpy(out.data(), bytes.data() + offset, count * sizeof(T));
	offset += count * sizeof(T);
	return true;
}

}

const char* samplerRandomizationName(SamplerRandomization randomization)
{
	switch (randomization) {
	case SamplerRandomization::None:
		return "None";
	case SamplerRandomization::PermuteDigits:
		return "Random digit permutations";
	case SamplerRandomization::FastOwen:
		return "Fast Owen scrambling";
	case SamplerRandomization::Owen:
	case SamplerRandomization::Count:
		break;
	}
	return "Owen scrambling";
}

const char* samplerRandomizationId(SamplerRandomization randomization)
{
	switch (randomization) {
	case SamplerRandomization::None:
		return "none";
	case SamplerRandomization::PermuteDigits:
		return "permute_digits";
	case SamplerRandomization::FastOwen:
		return "fast_owen";
	case SamplerRandomization::Owen:
	case SamplerRandomization::Count:
		break;
	}
	return "owen";
}

SamplerRandomization samplerRandomizationFromId(const std::string& id)
{
	for (uint32_t i = 0; i < (uint32_t)SamplerRandomization::Count; i++) {
		if (id == samplerRandomizationId((SamplerRandomization)i)) {
			return (SamplerRandomization)i;
		}
	}
	return SamplerRandomization::Owen;
}

//---------------------------------------------------------------- parameters

SamplerParams makeSamplerParams(SamplerKind kind, SamplerRandomization randomization, uint32_t seed, uint32_t width, uint32_t height, uint32_t maxSamples, bool unlimited)
{
	SamplerParams params;
	params.kind = kind;
	params.randomization = randomization;
	params.seed = seed;
	params.samplesPerPixel = unlimited ? 0u : std::max(maxSamples, 1u);
	// 64 samples per block when the render has no end, the smallest power of two that holds the
	// render's samples otherwise
	params.blockLog2 = unlimited ? 6u : std::min(ceilLog2(std::max(maxSamples, 1u)), 20u);
	params.resolutionLog2 = ceilLog2(std::max(std::max(width, height), 1u));

	// the rows and columns of a stratified pixel: the divisor of the sample count nearest its root
	if (params.samplesPerPixel > 0) {
		for (uint32_t x = (uint32_t)std::sqrt((double)params.samplesPerPixel); x >= 1; x--) {
			if (params.samplesPerPixel % x == 0) {
				params.strataX = x;
				break;
			}
		}
		// the strata run along the longer side: more across than down
		params.strataX = params.samplesPerPixel / params.strataX;
	}

	// pbrt's HaltonSampler constructor
	const uint32_t resolution[2] = { width, height };
	uint32_t scale[2];
	for (uint32_t i = 0; i < 2; i++) {
		const uint32_t base = i == 0 ? 2 : 3;
		scale[i] = 1;
		params.haltonExponent[i] = 0;
		while (scale[i] < std::min(resolution[i], MAX_HALTON_RESOLUTION)) {
			scale[i] *= base;
			params.haltonExponent[i]++;
		}
	}
	params.haltonInverse[0] = multiplicativeInverse(scale[1], scale[0]);
	params.haltonInverse[1] = multiplicativeInverse(scale[0], scale[1]);
	return params;
}

PackedSamplerParams packSamplerParams(const SamplerParams& params)
{
	PackedSamplerParams packed;
	packed.sampler = (uint32_t)params.randomization | (params.blockLog2 << 2) | (params.resolutionLog2 << 7)
		| (params.haltonExponent[0] << 12) | (params.haltonExponent[1] << 16);
	packed.halton = params.haltonInverse[0] | (params.haltonInverse[1] << 8);
	packed.strata = params.strataX;
	// the stratified sampler's reciprocals, divided here because a GPU's division is not IEEE
	const uint32_t samples = std::max(params.samplesPerPixel, 1u);
	const uint32_t strataX = std::max(params.strataX, 1u);
	packed.inverseSamples = 1.0f / (float)samples;
	packed.inverseStrataX = 1.0f / (float)strataX;
	packed.inverseStrataY = 1.0f / (float)std::max(samples / strataX, 1u);
	return packed;
}

SamplerParams unpackSamplerParams(SamplerKind kind, const PackedSamplerParams& packed, uint32_t seed, uint32_t samplesPerPixel)
{
	SamplerParams params;
	params.kind = kind;
	params.seed = seed;
	params.samplesPerPixel = samplesPerPixel;
	params.randomization = (SamplerRandomization)(packed.sampler & 3u);
	params.blockLog2 = (packed.sampler >> 2) & 31u;
	params.resolutionLog2 = (packed.sampler >> 7) & 31u;
	params.haltonExponent[0] = (packed.sampler >> 12) & 15u;
	params.haltonExponent[1] = (packed.sampler >> 16) & 15u;
	params.haltonInverse[0] = packed.halton & 255u;
	params.haltonInverse[1] = (packed.halton >> 8) & 255u;
	params.strataX = packed.strata & 0xFFFFu;
	return params;
}

//---------------------------------------------------------------- tables

bool loadSamplerTables(const std::filesystem::path& directory, SamplerTables& tables, std::string& error)
{
	std::vector<uint8_t> bytes;

	if (!readFile(directory / "sobol_matrices.bin", bytes, error)) {
		return false;
	}
	{
		uint32_t header[4];
		if (bytes.size() < 16 || memcmp(bytes.data(), "SOBL", 4) != 0) {
			error = "sobol_matrices.bin is not a Sobol table file";
			return false;
		}
		memcpy(header, bytes.data() + 4, 12);
		if (header[0] != SOBOL_DIMENSIONS || header[1] != SOBOL_COLUMNS) {
			error = fmt::format("sobol_matrices.bin holds {} x {} matrices, expected {} x {}", header[0], header[1], SOBOL_DIMENSIONS, SOBOL_COLUMNS);
			return false;
		}
		tables.vdcRows = header[2];
		size_t offset = 16;
		if (!copyArray(bytes, offset, (size_t)SOBOL_DIMENSIONS * SOBOL_COLUMNS, tables.sobol)
			|| !copyArray(bytes, offset, (size_t)tables.vdcRows * SOBOL_COLUMNS, tables.vdc)
			|| !copyArray(bytes, offset, (size_t)tables.vdcRows * SOBOL_COLUMNS, tables.vdcInverse)) {
			error = "sobol_matrices.bin is truncated";
			return false;
		}
	}

	if (!readFile(directory / "pmj02bn.bin", bytes, error)) {
		return false;
	}
	{
		uint32_t header[2];
		if (bytes.size() < 12 || memcmp(bytes.data(), "PMJ2", 4) != 0) {
			error = "pmj02bn.bin is not a PMJ02BN table file";
			return false;
		}
		memcpy(header, bytes.data() + 4, 8);
		size_t offset = 12;
		if (header[0] != PMJ02BN_SETS || header[1] != PMJ02BN_SAMPLES || !copyArray(bytes, offset, (size_t)PMJ02BN_SETS * PMJ02BN_SAMPLES * 2, tables.pmj02bn)) {
			error = "pmj02bn.bin has the wrong size";
			return false;
		}
	}

	if (!readFile(directory / "blue_noise.bin", bytes, error)) {
		return false;
	}
	{
		uint32_t header[2];
		if (bytes.size() < 12 || memcmp(bytes.data(), "BLUE", 4) != 0) {
			error = "blue_noise.bin is not a blue noise file";
			return false;
		}
		memcpy(header, bytes.data() + 4, 8);
		size_t offset = 12;
		if (header[0] != BLUE_NOISE_TEXTURES || header[1] != BLUE_NOISE_RESOLUTION
			|| !copyArray(bytes, offset, (size_t)BLUE_NOISE_TEXTURES * BLUE_NOISE_RESOLUTION * BLUE_NOISE_RESOLUTION, tables.blueNoise)) {
			error = "blue_noise.bin has the wrong size";
			return false;
		}
	}
	return true;
}

std::vector<uint32_t> packSamplerTablesForGpu(const SamplerTables& tables)
{
	constexpr uint32_t HEADER_WORDS = 16;
	std::vector<uint32_t> words(HEADER_WORDS, 0u);
	words[0] = (uint32_t)words.size();
	words.insert(words.end(), tables.sobol.begin(), tables.sobol.end());
	words[1] = (uint32_t)words.size();
	for (const uint64_t value : tables.vdc) {
		words.push_back((uint32_t)value);
		words.push_back((uint32_t)(value >> 32));
	}
	words[2] = (uint32_t)words.size();
	for (const uint64_t value : tables.vdcInverse) {
		words.push_back((uint32_t)value);
		words.push_back((uint32_t)(value >> 32));
	}
	words[3] = tables.vdcRows;
	words[4] = (uint32_t)words.size();
	words.insert(words.end(), tables.pmj02bn.begin(), tables.pmj02bn.end());
	words[5] = (uint32_t)words.size();
	for (size_t i = 0; i < tables.blueNoise.size(); i += 2) {
		const uint32_t high = i + 1 < tables.blueNoise.size() ? tables.blueNoise[i + 1] : 0u;
		words.push_back((uint32_t)tables.blueNoise[i] | (high << 16));
	}
	words[6] = (uint32_t)words.size();
	words.resize(words.size() + 2 * (size_t)PMJ02BN_SAMPLES, 0u);
	return words;
}

std::vector<uint32_t> packPmj02BnPixelSamples(const SamplerTables& tables)
{
	std::vector<uint32_t> words(2 * tables.pmjPixelSamples.size());
	memcpy(words.data(), tables.pmjPixelSamples.data(), sizeof(uint32_t) * words.size());
	return words;
}

uint32_t pmj02BnTileSize(uint32_t samplesPerPixel)
{
	// log4 of the sample count rounded up to a power of four
	uint32_t log4 = 0;
	while ((1u << (2 * log4)) < samplesPerPixel) {
		log4++;
	}
	return 1u << (8 - std::min(log4, 8u));
}

void preparePmj02BnPixelSamples(SamplerTables& tables, uint32_t samplesPerPixel)
{
	samplesPerPixel = std::clamp(samplesPerPixel, 1u, PMJ02BN_MAX_SAMPLES_PER_PIXEL);
	if (tables.pmjSamplesPerPixel == samplesPerPixel || tables.pmj02bn.empty()) {
		return;
	}
	// pbrt's PMJ02BNSampler constructor, in double as pbrt does it on the CPU: every point of set 0
	// lands in one cell of a tile x tile grid, and each cell keeps its first samplesPerPixel
	const uint32_t tile = pmj02BnTileSize(samplesPerPixel);
	tables.pmjPixelSamples.assign((size_t)tile * tile * samplesPerPixel, glm::vec2(0.f));
	std::vector<uint32_t> stored((size_t)tile * tile, 0u);
	for (uint32_t i = 0; i < PMJ02BN_SAMPLES; i++) {
		const double px = tables.pmj02bn[2 * (size_t)i] * 0x1p-32 * tile;
		const double py = tables.pmj02bn[2 * (size_t)i + 1] * 0x1p-32 * tile;
		const uint32_t cell = (uint32_t)px + (uint32_t)py * tile;
		if (stored[cell] == samplesPerPixel) {
			continue;
		}
		tables.pmjPixelSamples[(size_t)cell * samplesPerPixel + stored[cell]] = glm::vec2((float)(px - std::floor(px)), (float)(py - std::floor(py)));
		stored[cell]++;
	}
	tables.pmjSamplesPerPixel = samplesPerPixel;
}

glm::vec2 samplerPmj02Bn(const SamplerTables& tables, uint32_t set, uint32_t index)
{
	const size_t base = 2 * ((size_t)(set % PMJ02BN_SETS) * PMJ02BN_SAMPLES + index % PMJ02BN_SAMPLES);
	return glm::vec2(samplerToFloat(tables.pmj02bn[base]), samplerToFloat(tables.pmj02bn[base + 1]));
}

float samplerBlueNoise(const SamplerTables& tables, uint32_t textureIndex, uint32_t x, uint32_t y)
{
	const size_t index = ((size_t)(textureIndex % BLUE_NOISE_TEXTURES) * BLUE_NOISE_RESOLUTION + x % BLUE_NOISE_RESOLUTION) * BLUE_NOISE_RESOLUTION + y % BLUE_NOISE_RESOLUTION;
	return (float)tables.blueNoise[index] * (1.0f / 65535.0f);
}

//---------------------------------------------------------------- building blocks

uint32_t samplerMix(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

uint32_t samplerHash(uint32_t a, uint32_t b)
{
	return samplerMix(a ^ samplerMix(b + 0x9e3779b9u));
}

uint32_t samplerHash(uint32_t a, uint32_t b, uint32_t c)
{
	return samplerMix(samplerHash(a, b) ^ samplerMix(c + 0x85ebca6bu));
}

uint32_t samplerPermutationElement(uint32_t i, uint32_t l, uint32_t p)
{
	uint32_t w = l - 1;
	w |= w >> 1;
	w |= w >> 2;
	w |= w >> 4;
	w |= w >> 8;
	w |= w >> 16;
	do {
		i ^= p;
		i *= 0xe170893du;
		i ^= p >> 16;
		i ^= (i & w) >> 4;
		i ^= p >> 8;
		i *= 0x0929eb3fu;
		i ^= p >> 23;
		i ^= (i & w) >> 1;
		i *= 1u | p >> 27;
		i *= 0x6935fa69u;
		i ^= (i & w) >> 11;
		i *= 0x74dcb303u;
		i ^= (i & w) >> 2;
		i *= 0x9e501cc3u;
		i ^= (i & w) >> 2;
		i *= 0xc860a3dfu;
		i &= w;
		i ^= i >> 5;
	} while (i >= l);
	return (i + p) % l;
}

uint32_t samplerReverseBits(uint32_t v)
{
	v = (v << 16) | (v >> 16);
	v = ((v & 0x00ff00ffu) << 8) | ((v & 0xff00ff00u) >> 8);
	v = ((v & 0x0f0f0f0fu) << 4) | ((v & 0xf0f0f0f0u) >> 4);
	v = ((v & 0x33333333u) << 2) | ((v & 0xccccccccu) >> 2);
	v = ((v & 0x55555555u) << 1) | ((v & 0xaaaaaaaau) >> 1);
	return v;
}

float samplerToFloat(uint32_t v)
{
	return (float)(v >> 8) * (1.0f / 16777216.0f);
}

uint32_t samplerScramble(SamplerRandomization randomization, uint32_t seed, uint32_t v)
{
	switch (randomization) {
	case SamplerRandomization::None:
	case SamplerRandomization::Count:
		return v;
	case SamplerRandomization::PermuteDigits:
		return v ^ seed;
	case SamplerRandomization::FastOwen:
		v = samplerReverseBits(v);
		v ^= v * 0x3d20adeau;
		v += seed;
		v *= (seed >> 16) | 1u;
		v ^= v * 0x05526c56u;
		v ^= v * 0x53a22864u;
		return samplerReverseBits(v);
	case SamplerRandomization::Owen:
		if ((seed & 1u) != 0u) {
			v ^= 1u << 31;
		}
		// the decision for binary digit b depends on the digits above it, so the eight lowest, which
		// samplerToFloat() drops, are left unscrambled: the float is the same either way, at 23
		// decisions instead of pbrt's 31
		for (uint32_t b = 1; b < 24; b++) {
			const uint32_t mask = (~0u) << (32 - b);
			if ((samplerMix((v & mask) ^ seed) & (1u << b)) != 0u) {
				v ^= 1u << (31 - b);
			}
		}
		return v;
	}
	return v;
}

uint32_t samplerPcgHash(uint32_t v)
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float samplerPcgFloat(uint32_t& state)
{
	state = state * 747796405u + 2891336453u;
	uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	word = (word >> 22u) ^ word;
	return samplerToFloat(word);
}

uint64_t samplerMorton2(uint32_t x, uint32_t y)
{
	auto spread = [](uint64_t v) {
		v &= 0xffffffffull;
		v = (v ^ (v << 16)) & 0x0000ffff0000ffffull;
		v = (v ^ (v << 8)) & 0x00ff00ff00ff00ffull;
		v = (v ^ (v << 4)) & 0x0f0f0f0f0f0f0f0full;
		v = (v ^ (v << 2)) & 0x3333333333333333ull;
		v = (v ^ (v << 1)) & 0x5555555555555555ull;
		return v;
	};
	return (spread(y) << 1) | spread(x);
}

float samplerSobol(const SamplerTables& tables, uint64_t index, uint32_t dimension, SamplerRandomization randomization, uint32_t scramblerSeed)
{
	uint32_t v = 0;
	const uint32_t base = dimension * SOBOL_COLUMNS;
	for (uint32_t i = 0; index != 0; index >>= 1, i++) {
		if ((index & 1u) != 0u) {
			v ^= tables.sobol[base + i];
		}
	}
	return samplerToFloat(samplerScramble(randomization, scramblerSeed, v));
}

uint64_t samplerSobolIntervalToIndex(const SamplerTables& tables, uint32_t m, uint32_t frame, uint32_t x, uint32_t y)
{
	if (m == 0) {
		return frame;
	}
	const uint32_t row = (m - 1) * SOBOL_COLUMNS;
	uint64_t index = (uint64_t)frame << (m << 1);

	uint64_t delta = 0;
	uint32_t f = frame;
	for (uint32_t c = 0; f != 0; f >>= 1, c++) {
		if ((f & 1u) != 0u) {
			delta ^= tables.vdc[row + c];
		}
	}

	uint64_t b = (((uint64_t)x << m) | (uint64_t)y) ^ delta;
	for (uint32_t c = 0; b != 0; b >>= 1, c++) {
		if ((b & 1u) != 0u) {
			index ^= tables.vdcInverse[row + c];
		}
	}
	return index;
}

namespace {

// the hash a ZSobol digit's permutation is chosen with: the digits above it and the dimension
uint32_t zSobolHash(uint64_t higherDigits, uint32_t dimension)
{
	return samplerHash((uint32_t)higherDigits ^ (0x55555555u * dimension), (uint32_t)(higherDigits >> 32));
}

}

uint64_t samplerZSobolIndex(uint64_t mortonIndex, uint32_t dimension, uint32_t blockLog2, uint32_t resolutionLog2)
{
	const uint32_t nBase4Digits = resolutionLog2 + (blockLog2 + 1) / 2;
	uint64_t sampleIndex = 0;
	// pbrt's GetSampleIndex(): randomly permute each full base-4 digit of the Morton index, the
	// permutation chosen by the digits above it, so that every aligned run of 4^k indices is
	// shuffled among itself and the sequence stays a net
	const bool pow2Samples = (blockLog2 & 1u) != 0u;
	const int lastDigit = pow2Samples ? 1 : 0;
	for (int i = (int)nBase4Digits - 1; i >= lastDigit; i--) {
		const int digitShift = 2 * i - (pow2Samples ? 1 : 0);
		uint32_t digit = (uint32_t)(mortonIndex >> digitShift) & 3u;
		const uint64_t higherDigits = mortonIndex >> (digitShift + 2);
		const uint32_t p = (zSobolHash(higherDigits, dimension) >> 24) % 24u;
		digit = Z_PERMUTATIONS[p][digit];
		sampleIndex |= (uint64_t)digit << digitShift;
	}
	// a power-of-two (but not four) sample count leaves one binary digit
	if (pow2Samples) {
		const uint32_t digit = (uint32_t)(mortonIndex & 1u);
		sampleIndex |= digit ^ (zSobolHash(mortonIndex >> 1, dimension) & 1u);
	}
	return sampleIndex;
}

float samplerRadicalInverse(uint32_t baseIndex, uint32_t a)
{
	const uint32_t base = HALTON_PRIMES[baseIndex];
	// stop once reversedDigits is at the limit: the next digit could overflow it
	const uint32_t limit = 0xFFFFFFFFu / base - base;
	const float invBase = HALTON_INVERSE_PRIMES[baseIndex];
	float invBaseM = 1.0f;
	uint32_t reversedDigits = 0;
	while (a != 0u && reversedDigits < limit) {
		const uint32_t next = a / base;
		const uint32_t digit = a - next * base;
		reversedDigits = reversedDigits * base + digit;
		invBaseM *= invBase;
		a = next;
	}
	return std::min((float)reversedDigits * invBaseM, ONE_MINUS_EPSILON);
}

float samplerScrambledRadicalInverse(uint32_t baseIndex, uint32_t a, uint32_t seed)
{
	const uint32_t base = HALTON_PRIMES[baseIndex];
	const uint32_t limit = 0xFFFFFFFFu / base - base;
	const float invBase = HALTON_INVERSE_PRIMES[baseIndex];
	float invBaseM = 1.0f;
	uint32_t reversedDigits = 0;
	const uint32_t digits = HALTON_SCRAMBLED_DIGITS[baseIndex];
	for (uint32_t digitIndex = 0; digitIndex < digits && reversedDigits < limit; digitIndex++) {
		// permute the least significant digit: pbrt's DigitPermutation, computed instead of stored
		const uint32_t next = a / base;
		const uint32_t digitValue = a - next * base;
		const uint32_t permuted = samplerPermutationElement(digitValue, base, samplerHash(base, digitIndex, seed));
		reversedDigits = reversedDigits * base + permuted;
		invBaseM *= invBase;
		a = next;
	}
	return std::min(invBaseM * (float)reversedDigits, ONE_MINUS_EPSILON);
}

float samplerOwenRadicalInverse(uint32_t baseIndex, uint32_t a, uint32_t hash)
{
	const uint32_t base = HALTON_PRIMES[baseIndex];
	const uint32_t limit = 0xFFFFFFFFu / base - base;
	const float invBase = HALTON_INVERSE_PRIMES[baseIndex];
	float invBaseM = 1.0f;
	uint32_t reversedDigits = 0;
	const uint32_t digits = HALTON_OWEN_DIGITS[baseIndex];
	for (uint32_t digitIndex = 0; digitIndex < digits && reversedDigits < limit; digitIndex++) {
		const uint32_t next = a / base;
		uint32_t digitValue = a - next * base;
		const uint32_t digitHash = samplerMix(hash ^ reversedDigits);
		digitValue = samplerPermutationElement(digitValue, base, digitHash);
		reversedDigits = reversedDigits * base + digitValue;
		invBaseM *= invBase;
		a = next;
	}
	return std::min(invBaseM * (float)reversedDigits, ONE_MINUS_EPSILON);
}

//---------------------------------------------------------------- the generators

namespace {

// Halton: the digit-reversed value of `a` in `exponent` digits of `base`, undone (pbrt's
// InverseRadicalInverse), which finds the index of the pixel's first sample
uint32_t inverseRadicalInverse(uint32_t inverse, uint32_t base, uint32_t digits)
{
	uint32_t index = 0;
	for (uint32_t i = 0; i < digits; i++) {
		const uint32_t digit = inverse % base;
		inverse /= base;
		index = index * base + digit;
	}
	return index;
}

uint32_t haltonScale(uint32_t base, uint32_t exponent)
{
	uint32_t scale = 1;
	for (uint32_t i = 0; i < exponent; i++) {
		scale *= base;
	}
	return scale;
}

}

SampleGenerator::SampleGenerator(const SamplerParams& params, const SamplerTables& tables, uint32_t x, uint32_t y, uint32_t sampleIndex)
	: m_params(params), m_tables(tables), m_x(x), m_y(y), m_sampleIndex(sampleIndex)
{
	m_pixelHash = samplerHash(x, y);
	switch (params.kind) {
	case SamplerKind::Sobol:
		m_index = samplerSobolIntervalToIndex(tables, params.resolutionLog2, sampleIndex, x, y);
		break;
	case SamplerKind::Halton: {
		// pbrt's StartPixelSample(): the Halton index of the pixel's first sample, then its stride
		const uint32_t scale0 = haltonScale(2, params.haltonExponent[0]);
		const uint32_t scale1 = haltonScale(3, params.haltonExponent[1]);
		const uint32_t stride = scale0 * scale1;
		uint32_t haltonIndex = 0;
		if (stride > 1) {
			const uint32_t pm[2] = { x % MAX_HALTON_RESOLUTION, y % MAX_HALTON_RESOLUTION };
			const uint32_t scales[2] = { scale0, scale1 };
			for (uint32_t i = 0; i < 2; i++) {
				const uint32_t dimOffset = inverseRadicalInverse(pm[i], i == 0 ? 2 : 3, params.haltonExponent[i]);
				haltonIndex += dimOffset * (stride / scales[i]) * params.haltonInverse[i];
			}
			haltonIndex %= stride;
		}
		haltonIndex += sampleIndex * stride;
		m_index = haltonIndex;
		break;
	}
	default:
		break;
	}
}

namespace {

// a Halton dimension: the base is the dimension's prime
float haltonDimension(const SamplerParams& params, uint32_t index, uint32_t dimension)
{
	switch (params.randomization) {
	case SamplerRandomization::None:
	case SamplerRandomization::Count:
		return samplerRadicalInverse(dimension, index);
	case SamplerRandomization::PermuteDigits:
		return samplerScrambledRadicalInverse(dimension, index, params.seed);
	case SamplerRandomization::FastOwen:
	case SamplerRandomization::Owen:
		// pbrt's Halton has no separate fast Owen. Its hash is MixBits(1 + (dimension << 4)), which
		// leaves the render's seed out, so that every render of a scene is the same picture; the seed
		// goes in here
		return samplerOwenRadicalInverse(dimension, index, samplerHash(1u + (dimension << 4), params.seed));
	}
	return 0.f;
}

}

glm::vec2 SampleGenerator::pixel2D()
{
	switch (m_params.kind) {
	case SamplerKind::Sobol: {
		// the first two Sobol dimensions cover the image: take this pixel's part of them
		const float scale = (float)(1u << m_params.resolutionLog2);
		const float u = samplerSobol(m_tables, m_index, 0, SamplerRandomization::None, 0);
		const float v = samplerSobol(m_tables, m_index, 1, SamplerRandomization::None, 0);
		return glm::vec2(std::clamp(u * scale - (float)m_x, 0.f, ONE_MINUS_EPSILON), std::clamp(v * scale - (float)m_y, 0.f, ONE_MINUS_EPSILON));
	}
	case SamplerKind::Halton: {
		const uint32_t scale1 = haltonScale(3, m_params.haltonExponent[1]);
		return glm::vec2(samplerRadicalInverse(0, (uint32_t)(m_index >> m_params.haltonExponent[0])), samplerRadicalInverse(1, (uint32_t)m_index / scale1));
	}
	case SamplerKind::Pmj02Bn: {
		// the pixel's cell of the tile set 0 was sorted into
		const uint32_t spp = std::clamp(m_params.samplesPerPixel, 1u, PMJ02BN_MAX_SAMPLES_PER_PIXEL);
		const uint32_t tile = pmj02BnTileSize(spp);
		const uint32_t cell = m_x % tile + (m_y % tile) * tile;
		return m_tables.pmjPixelSamples[(size_t)cell * spp + m_sampleIndex % spp];
	}
	default:
		return get2D(0);
	}
}

float SampleGenerator::get1D(uint32_t dimension)
{
	switch (m_params.kind) {
	case SamplerKind::Sobol:
		return samplerSobol(m_tables, m_index, dimension, m_params.randomization, samplerHash(dimension, m_params.seed));
	case SamplerKind::Halton:
		return haltonDimension(m_params, (uint32_t)m_index, dimension);
	case SamplerKind::PaddedSobol: {
		const uint32_t h0 = samplerHash(m_pixelHash, dimension, m_params.seed);
		const uint32_t h1 = samplerMix(h0 ^ 0x68bc21ebu);
		const uint32_t mask = (1u << m_params.blockLog2) - 1u;
		const uint32_t permuted = samplerPermutationElement(m_sampleIndex & mask, 1u << m_params.blockLog2, h0);
		const uint64_t index = ((uint64_t)(m_sampleIndex >> m_params.blockLog2) << m_params.blockLog2) | permuted;
		return samplerSobol(m_tables, index, 0, m_params.randomization, h1);
	}
	case SamplerKind::ZSobol: {
		const uint32_t mask = (1u << m_params.blockLog2) - 1u;
		const uint64_t mortonIndex = (samplerMorton2(m_x, m_y) << m_params.blockLog2) | (m_sampleIndex & mask);
		const uint64_t zIndex = samplerZSobolIndex(mortonIndex, dimension, m_params.blockLog2, m_params.resolutionLog2);
		const uint32_t indexBits = 2 * m_params.resolutionLog2 + m_params.blockLog2;
		const uint64_t index = ((uint64_t)(m_sampleIndex >> m_params.blockLog2) << indexBits) | zIndex;
		return samplerSobol(m_tables, index, 0, m_params.randomization, samplerMix(samplerHash(dimension, m_params.seed) ^ 0x68bc21ebu));
	}
	case SamplerKind::Pmj02Bn: {
		// pbrt's Get1D(): a stratum per sample, permuted per pixel and dimension, offset by blue noise
		const uint32_t spp = std::clamp(m_params.samplesPerPixel, 1u, PMJ02BN_MAX_SAMPLES_PER_PIXEL);
		const uint32_t hash = samplerHash(m_pixelHash, dimension, m_params.seed);
		const uint32_t index = samplerPermutationElement(m_sampleIndex % spp, spp, hash);
		const float delta = samplerBlueNoise(m_tables, dimension, m_x, m_y);
		return std::min(((float)index + delta) * (1.0f / (float)spp), ONE_MINUS_EPSILON);
	}
	case SamplerKind::Stratified: {
		const uint32_t spp = std::max(m_params.samplesPerPixel, 1u);
		const uint32_t hash = samplerHash(m_pixelHash, dimension, m_params.seed);
		const uint32_t stratum = samplerPermutationElement(m_sampleIndex % spp, spp, hash);
		uint32_t jitter = samplerPcgHash(hash ^ (m_sampleIndex % spp));
		const float delta = samplerPcgFloat(jitter);
		return std::min(((float)stratum + delta) * (1.0f / (float)spp), ONE_MINUS_EPSILON);
	}
	default:
		break;
	}
	return 0.f;
}

glm::vec2 SampleGenerator::get2D(uint32_t dimension)
{
	switch (m_params.kind) {
	case SamplerKind::Sobol:
	case SamplerKind::Halton:
		return glm::vec2(get1D(dimension), get1D(dimension + 1));
	case SamplerKind::PaddedSobol: {
		const uint32_t h0 = samplerHash(m_pixelHash, dimension, m_params.seed);
		const uint32_t h1 = samplerMix(h0 ^ 0x68bc21ebu);
		const uint32_t mask = (1u << m_params.blockLog2) - 1u;
		const uint32_t permuted = samplerPermutationElement(m_sampleIndex & mask, 1u << m_params.blockLog2, h0);
		const uint64_t index = ((uint64_t)(m_sampleIndex >> m_params.blockLog2) << m_params.blockLog2) | permuted;
		return glm::vec2(samplerSobol(m_tables, index, 0, m_params.randomization, h0), samplerSobol(m_tables, index, 1, m_params.randomization, h1));
	}
	case SamplerKind::ZSobol: {
		const uint32_t mask = (1u << m_params.blockLog2) - 1u;
		const uint64_t mortonIndex = (samplerMorton2(m_x, m_y) << m_params.blockLog2) | (m_sampleIndex & mask);
		const uint64_t zIndex = samplerZSobolIndex(mortonIndex, dimension, m_params.blockLog2, m_params.resolutionLog2);
		const uint32_t indexBits = 2 * m_params.resolutionLog2 + m_params.blockLog2;
		const uint64_t index = ((uint64_t)(m_sampleIndex >> m_params.blockLog2) << indexBits) | zIndex;
		const uint32_t h0 = samplerHash(dimension, m_params.seed);
		const uint32_t h1 = samplerMix(h0 ^ 0x68bc21ebu);
		return glm::vec2(samplerSobol(m_tables, index, 0, m_params.randomization, h0), samplerSobol(m_tables, index, 1, m_params.randomization, h1));
	}
	case SamplerKind::Pmj02Bn: {
		// pbrt's Get2D(): the first five pairs of dimensions read the five sets in order, so a pixel's
		// samples are a prefix of a (0, 2)-sequence; later ones read them again with the samples
		// permuted. Each pixel's set is rotated by blue noise (Cranley-Patterson), so that the error
		// of neighbouring pixels differs in a blue-noise way
		const uint32_t spp = std::clamp(m_params.samplesPerPixel, 1u, PMJ02BN_MAX_SAMPLES_PER_PIXEL);
		uint32_t index = m_sampleIndex % spp;
		const uint32_t instance = dimension / 2;
		if (instance >= PMJ02BN_SETS) {
			index = samplerPermutationElement(index, spp, samplerHash(m_pixelHash, dimension, m_params.seed));
		}
		glm::vec2 u = samplerPmj02Bn(m_tables, instance, index);
		u.x += samplerBlueNoise(m_tables, dimension, m_x, m_y);
		u.y += samplerBlueNoise(m_tables, dimension + 1, m_x, m_y);
		if (u.x >= 1.f) {
			u.x -= 1.f;
		}
		if (u.y >= 1.f) {
			u.y -= 1.f;
		}
		return glm::vec2(std::min(u.x, ONE_MINUS_EPSILON), std::min(u.y, ONE_MINUS_EPSILON));
	}
	case SamplerKind::Stratified: {
		const uint32_t spp = std::max(m_params.samplesPerPixel, 1u);
		const uint32_t strataX = std::max(m_params.strataX, 1u);
		const uint32_t strataY = std::max(spp / strataX, 1u);
		const uint32_t hash = samplerHash(m_pixelHash, dimension, m_params.seed);
		const uint32_t stratum = samplerPermutationElement(m_sampleIndex % spp, spp, hash);
		const uint32_t sx = stratum % strataX;
		const uint32_t sy = stratum / strataX;
		uint32_t jitter = samplerPcgHash(hash ^ (m_sampleIndex % spp));
		const float dx = samplerPcgFloat(jitter);
		const float dy = samplerPcgFloat(jitter);
		return glm::vec2(std::min(((float)sx + dx) * (1.0f / (float)strataX), ONE_MINUS_EPSILON), std::min(((float)sy + dy) * (1.0f / (float)strataY), ONE_MINUS_EPSILON));
	}
	default:
		break;
	}
	return glm::vec2(0.f);
}

//---------------------------------------------------------------- the record

void sampleCameraRecord(const SamplerParams& params, const SamplerTables& tables, uint32_t x, uint32_t y, uint32_t sampleIndex, float record[SAMPLE_RECORD_FLOATS])
{
	SampleGenerator generator(params, tables, x, y, sampleIndex);
	for (uint32_t i = 0; i < SAMPLE_RECORD_FLOATS; i++) {
		record[i] = 0.f;
	}
	const glm::vec2 pixel = generator.pixel2D();
	const glm::vec2 lens = generator.get2D(2);
	record[SAMPLE_CAMERA_PIXEL] = pixel.x;
	record[SAMPLE_CAMERA_PIXEL + 1] = pixel.y;
	record[SAMPLE_CAMERA_LENS] = lens.x;
	record[SAMPLE_CAMERA_LENS + 1] = lens.y;
	record[SAMPLE_CAMERA_TIME] = generator.get1D(4);
	record[SAMPLE_CAMERA_WAVELENGTH] = generator.get1D(5);
}

void sampleBounceRecord(const SamplerParams& params, const SamplerTables& tables, uint32_t x, uint32_t y, uint32_t sampleIndex, uint32_t bounce, float record[SAMPLE_RECORD_FLOATS])
{
	SampleGenerator generator(params, tables, x, y, sampleIndex);
	for (uint32_t i = 0; i < SAMPLE_RECORD_FLOATS; i++) {
		record[i] = 0.f;
	}
	const uint32_t base = SAMPLE_CAMERA_DIMENSIONS + SAMPLE_BOUNCE_DIMENSIONS * bounce;
	record[SAMPLE_DIRECT_LIGHT] = generator.get1D(base + SAMPLE_DIRECT_LIGHT);
	const glm::vec2 point = generator.get2D(base + SAMPLE_DIRECT_POINT);
	record[SAMPLE_DIRECT_POINT] = point.x;
	record[SAMPLE_DIRECT_POINT + 1] = point.y;
	record[SAMPLE_INDIRECT_LOBE] = generator.get1D(base + SAMPLE_INDIRECT_LOBE);
	const glm::vec2 direction = generator.get2D(base + SAMPLE_INDIRECT_DIRECTION);
	record[SAMPLE_INDIRECT_DIRECTION] = direction.x;
	record[SAMPLE_INDIRECT_DIRECTION + 1] = direction.y;
	record[SAMPLE_INDIRECT_ROULETTE] = generator.get1D(base + SAMPLE_INDIRECT_ROULETTE);
}

void sampleIndependentRecord(uint32_t& rngState, bool camera, float record[SAMPLE_RECORD_FLOATS])
{
	for (uint32_t i = 0; i < SAMPLE_RECORD_FLOATS; i++) {
		record[i] = 0.f;
	}
	if (camera) {
		for (uint32_t i = 0; i < SAMPLE_CAMERA_DIMENSIONS; i++) {
			record[i] = samplerPcgFloat(rngState);
		}
		return;
	}
	for (const uint32_t field : { SAMPLE_DIRECT_LIGHT, SAMPLE_DIRECT_POINT, SAMPLE_DIRECT_POINT + 1, SAMPLE_INDIRECT_LOBE, SAMPLE_INDIRECT_DIRECTION, SAMPLE_INDIRECT_DIRECTION + 1, SAMPLE_INDIRECT_ROULETTE }) {
		record[field] = samplerPcgFloat(rngState);
	}
}
