#pragma once

// Sample generation for the GPU path tracer, as plain C++: the generators of
// shaders/rt/include/crt_sampler.glsl, mirrored line for line so that bin/sampler_test can check
// them against the properties they promise without a GPU. A change to one side must be made to the
// other.
//
// Kernel 01 fills a RaySamples record per path and bounce (crt_samples.glsl); this file computes
// what it holds. Every sampler is a pure function of (pixel, sample index, dimension, seed) and
// the render's SamplerParams, which is what makes a fixed seed reproduce an image whatever order
// the paths run in. The dimensions follow the record's layout, not pbrt-v4's: 0 and 1 the pixel,
// 2 and 3 the lens, 4 time, 5 wavelength, then twelve per bounce.
//
// The generators follow pbrt-v4 (PBR 4ed chapter 8; src/pbrt/samplers.h, util/lowdiscrepancy.h)
// with two deliberate differences. Its hashes are 64-bit (MixBits, MurmurHash64A) and these are
// 32-bit: the Owen scrambler alone calls the mixer 31 times per sample, and a 64-bit multiply is
// several 32-bit ones on the GPU. A sample is therefore not bit-equal to pbrt's, only equal in
// distribution. And the samplers that pbrt gives a fixed samples-per-pixel count to interleave the
// pixels with (padded Sobol, ZSobol) work in blocks of 2^blockLog2 samples instead, so a render of
// unknown length keeps going: sample n is element n mod 2^L of block n >> L, and the blocks are
// consecutive stretches of the Sobol sequence.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

enum class SamplerKind : uint32_t {
	// the path's PCG stream written into the record
	Independent,
	// jittered strata, each pixel's strata permuted per dimension
	Stratified,
	// Halton over the image: neighbouring pixels share one well-distributed set (Grunschloss's
	// index mapping)
	Halton,
	// one Sobol sequence over the whole image
	Sobol,
	// a (0, 2)-sequence per pixel and request, randomly padded
	PaddedSobol,
	// padded Sobol indexed by the pixel's Morton code: blue-noise error (Ahmed and Wonka 2020)
	ZSobol,
	// progressive multi-jittered (0, 2) samples with blue noise
	Pmj02Bn,
	Count
};

// How a Sobol or Halton point is scrambled. None keeps the deterministic sequence
enum class SamplerRandomization : uint32_t {
	None,
	// XOR with a random word: Sobol keeps its (0, 2) net, Halton permutes each base's digits
	PermuteDigits,
	// Laine and Karras's hash-based approximation of Owen scrambling, in the form pbrt uses
	FastOwen,
	// the full nested uniform scramble, one hashed decision per bit
	Owen,
	Count
};

const char* samplerRandomizationName(SamplerRandomization randomization);
// the id a scene file stores, and the inverse; an unknown id reads as Owen
const char* samplerRandomizationId(SamplerRandomization randomization);
SamplerRandomization samplerRandomizationFromId(const std::string& id);

// the record: twelve floats, RaySamples in crt_samples.glsl. Fields are named by the dimension
// of a bounce they hold, direct.xyzw first, then indirect.xyzw, then medium.xyzw
inline constexpr uint32_t SAMPLE_RECORD_FLOATS = 12;
inline constexpr uint32_t SAMPLE_RECORD_BYTES = 48;
// how many dimensions the camera takes before the first bounce, and how many each bounce takes
inline constexpr uint32_t SAMPLE_CAMERA_DIMENSIONS = 6;
inline constexpr uint32_t SAMPLE_BOUNCE_DIMENSIONS = 12;
// the most dimensions a render of the deepest path (CRT_MAX_DEPTH bounces) can ask for
inline constexpr uint32_t SAMPLE_MAX_DIMENSIONS = SAMPLE_CAMERA_DIMENSIONS + 16 * SAMPLE_BOUNCE_DIMENSIONS;

// Field indices into the record, shared by kernels 00, 01 and 06 (crt_samples.glsl CRT_SAMPLE_*).
// A bounce's fields are its dimensions in order: the first is dimension 6 + 12 b
inline constexpr uint32_t SAMPLE_DIRECT_LIGHT = 0;   // direct.x, the light choice
inline constexpr uint32_t SAMPLE_DIRECT_POINT = 1;   // direct.yz, the point on the light
inline constexpr uint32_t SAMPLE_DIRECT_SPARE = 3;   // direct.w
inline constexpr uint32_t SAMPLE_INDIRECT_LOBE = 4;  // indirect.x, the lobe or transmission choice
inline constexpr uint32_t SAMPLE_INDIRECT_DIRECTION = 5;  // indirect.yz, the direction
inline constexpr uint32_t SAMPLE_INDIRECT_ROULETTE = 7;   // indirect.w
inline constexpr uint32_t SAMPLE_MEDIUM_DISTANCE = 8;     // medium.x
inline constexpr uint32_t SAMPLE_MEDIUM_TYPE = 9;         // medium.y
inline constexpr uint32_t SAMPLE_MEDIUM_PHASE = 10;       // medium.zw
// the camera pass writes the same twelve floats: dimensions 0 to 5 in fields 0 to 5
inline constexpr uint32_t SAMPLE_CAMERA_PIXEL = 0;       // direct.xy
inline constexpr uint32_t SAMPLE_CAMERA_LENS = 2;        // direct.zw
inline constexpr uint32_t SAMPLE_CAMERA_TIME = 4;        // indirect.x
inline constexpr uint32_t SAMPLE_CAMERA_WAVELENGTH = 5;  // indirect.y

// Everything a generator needs besides (pixel, sample index): the render's seed, the image, and
// the choices the Sampler settings and the sample count decide. PackedSamplerParams carries them
// to the shaders in CrtParams
struct SamplerParams {
	SamplerKind kind { SamplerKind::ZSobol };
	SamplerRandomization randomization { SamplerRandomization::Owen };
	uint32_t seed { 0 };
	// the known samples per pixel, for the samplers that need it (stratified, PMJ02BN): the render's
	// Max samples. 0 when the render is unlimited
	uint32_t samplesPerPixel { 0 };
	// the block of 2^blockLog2 samples padded Sobol and ZSobol interleave over
	uint32_t blockLog2 { 6 };
	// log2 of the image's larger side rounded up to a power of two: the Sobol and ZSobol scale
	uint32_t resolutionLog2 { 0 };
	// stratified: the strata across (the rows are samplesPerPixel / strataX)
	uint32_t strataX { 1 };
	// Halton: the exponents and multiplicative inverses of the base scales that cover the image
	// (2^exponent[0] x 3^exponent[1] pixels, at most 128 and 243), as pbrt's HaltonSampler computes them
	uint32_t haltonExponent[2] { 0, 0 };
	uint32_t haltonInverse[2] { 0, 0 };

	bool operator==(const SamplerParams&) const = default;
};

// The parameters of a render of width x height pixels: `maxSamples` is the Max samples setting,
// ignored (and the block fixed at 64) when `unlimited`
SamplerParams makeSamplerParams(SamplerKind kind, SamplerRandomization randomization, uint32_t seed, uint32_t width, uint32_t height, uint32_t maxSamples, bool unlimited);

// SamplerParams as the three words of CrtParams the shaders unpack (crt_sampler.glsl)
struct PackedSamplerParams {
	// bits 0-1 randomization, 2-6 blockLog2, 7-11 resolutionLog2, 12-15 and 16-19 the Halton exponents
	uint32_t sampler { 0 };
	// bits 0-7 and 8-15 the Halton inverses
	uint32_t halton { 0 };
	// bits 0-15 strataX
	uint32_t strata { 0 };
	// stratified: 1 / samples per pixel, 1 / strata across and 1 / strata down. The shader multiplies
	// by these instead of dividing, so that it rounds as this code does
	float inverseSamples { 1.f };
	float inverseStrataX { 1.f };
	float inverseStrataY { 1.f };
};
PackedSamplerParams packSamplerParams(const SamplerParams& params);
// the inverse, given the fields that are not packed
SamplerParams unpackSamplerParams(SamplerKind kind, const PackedSamplerParams& packed, uint32_t seed, uint32_t samplesPerPixel);

// pbrt-v4's tables: the Sobol generator matrices (1024 dimensions of 52 32-bit columns, and the
// van der Corput pair that maps a pixel and a frame to a sequence index), the PMJ02BN point sets
// and the 128 x 128 blue-noise textures. tools/sampler_tables/convert_pbrt_tables.py makes the
// files in assets/sampler/ from pbrt's sources
inline constexpr uint32_t SOBOL_DIMENSIONS = 1024;
inline constexpr uint32_t SOBOL_COLUMNS = 52;
inline constexpr uint32_t PMJ02BN_SETS = 5;
inline constexpr uint32_t PMJ02BN_SAMPLES = 65536;
inline constexpr uint32_t BLUE_NOISE_TEXTURES = 48;
inline constexpr uint32_t BLUE_NOISE_RESOLUTION = 128;

struct SamplerTables {
	// [dimension * SOBOL_COLUMNS + column]
	std::vector<uint32_t> sobol;
	// [(m - 1) * SOBOL_COLUMNS + column], m = 1 .. vdcRows
	std::vector<uint64_t> vdc;
	std::vector<uint64_t> vdcInverse;
	uint32_t vdcRows { 0 };
	// [(set * PMJ02BN_SAMPLES + sample) * 2 + axis], fixed point: the value is x / 2^32
	std::vector<uint32_t> pmj02bn;
	// [(texture * BLUE_NOISE_RESOLUTION + x) * BLUE_NOISE_RESOLUTION + y], x / 65535
	std::vector<uint16_t> blueNoise;

	// PMJ02BN's pixel samples for one sample count, built from the tables above by
	// preparePmj02BnPixelSamples(): set 0's points sorted into the cells of a tile of pixels,
	// pmjSamplesPerPixel to each cell, [(cellX + cellY * tile) * samplesPerPixel + sample]
	std::vector<glm::vec2> pmjPixelSamples;
	uint32_t pmjSamplesPerPixel { 0 };

	bool loaded() const { return !sobol.empty() && !pmj02bn.empty() && !blueNoise.empty(); }
};

// reads sobol_matrices.bin, pmj02bn.bin and blue_noise.bin from `directory`. False with a message
// in `error` when a file is missing or malformed
bool loadSamplerTables(const std::filesystem::path& directory, SamplerTables& tables, std::string& error);

// the most samples per pixel PMJ02BN supports: one set's points
inline constexpr uint32_t PMJ02BN_MAX_SAMPLES_PER_PIXEL = PMJ02BN_SAMPLES;
// the side, in pixels, of the tile PMJ02BN's pixel samples repeat over: 2^(8 - log4 of the sample
// count rounded up to a power of four), so that the tile's cells share set 0's 65536 points
uint32_t pmj02BnTileSize(uint32_t samplesPerPixel);
// pbrt's PMJ02BNSampler constructor: sorts set 0 into pixel cells for `samplesPerPixel`. Does
// nothing when the tables already hold that count
void preparePmj02BnPixelSamples(SamplerTables& tables, uint32_t samplesPerPixel);

// The tables as the GPU reads them (SamplerTableBuffer in crt_common.glsl, crt_sampler.glsl): a
// 16-word header, then SobolMatrices32, then the van der Corput matrices as (low, high) word pairs.
//   [0] first word of SobolMatrices32    [1] first word of VdCSobolMatrices
//   [2] first word of VdCSobolMatricesInv    [3] van der Corput rows
//   [4] first word of the PMJ02BN sets        [5] first word of the blue-noise textures, two to a word
//   [6] first word of PMJ02BN's pixel samples, a region of PMJ02BN_SAMPLES (x, y) float pairs that
//       the renderer fills for the render's sample count (packPmj02BnPixelSamples())
std::vector<uint32_t> packSamplerTablesForGpu(const SamplerTables& tables);
// tables.pmjPixelSamples as the words of the region at header word 6
std::vector<uint32_t> packPmj02BnPixelSamples(const SamplerTables& tables);
// the largest image side a sampler that interleaves pixels by Morton code supports: the shaders
// spread 16 bits per axis
inline constexpr uint32_t SAMPLER_MAX_IMAGE_SIDE = 65535;

//---------------------------------------------------------------- building blocks

// the 32-bit mixer behind every hash here (Wellons's lowbias32)
uint32_t samplerMix(uint32_t x);
uint32_t samplerHash(uint32_t a, uint32_t b);
uint32_t samplerHash(uint32_t a, uint32_t b, uint32_t c);
// pbrt's PermutationElement(): element `i` of a random permutation of [0, l) chosen by `p`,
// computed without storing the permutation
uint32_t samplerPermutationElement(uint32_t i, uint32_t l, uint32_t p);
uint32_t samplerReverseBits(uint32_t v);
// the 24 high bits of `v` as a float in [0, 1): exact, and never 1
float samplerToFloat(uint32_t v);
// one scrambler applied to a 32-bit fixed-point Sobol value. Owen scrambling leaves the 8 low bits
// unscrambled: samplerToFloat() drops them
uint32_t samplerScramble(SamplerRandomization randomization, uint32_t seed, uint32_t v);
// the 32-bit PCG the rest of the tracer uses (crt_random.glsl), for the independent sampler
uint32_t samplerPcgHash(uint32_t v);
float samplerPcgFloat(uint32_t& state);

// the interleaving of the x and y bits of a pixel
uint64_t samplerMorton2(uint32_t x, uint32_t y);
// Sobol dimension `dimension` at sequence index `index`, scrambled
float samplerSobol(const SamplerTables& tables, uint64_t index, uint32_t dimension, SamplerRandomization randomization, uint32_t seed);
// the sequence index of the sample `frame` of pixel (x, y) when one Sobol sequence is scaled over a
// 2^m x 2^m image (pbrt's SobolIntervalToIndex)
uint64_t samplerSobolIntervalToIndex(const SamplerTables& tables, uint32_t m, uint32_t frame, uint32_t x, uint32_t y);
// ZSobol's permutation of a Morton-ordered (pixel, sample) index into a Sobol sequence index
uint64_t samplerZSobolIndex(uint64_t mortonIndex, uint32_t dimension, uint32_t blockLog2, uint32_t resolutionLog2);
float samplerRadicalInverse(uint32_t baseIndex, uint32_t a);
float samplerScrambledRadicalInverse(uint32_t baseIndex, uint32_t a, uint32_t seed);
float samplerOwenRadicalInverse(uint32_t baseIndex, uint32_t a, uint32_t hash);

//---------------------------------------------------------------- the generators

// One (pixel, sample index) under a render's parameters: asks for the dimensions the record holds
class SampleGenerator {
public:
	SampleGenerator(const SamplerParams& params, const SamplerTables& tables, uint32_t x, uint32_t y, uint32_t sampleIndex);

	// the pixel position in [0, 1)^2 (dimensions 0 and 1): the sample's offset inside its pixel
	glm::vec2 pixel2D();
	// the samples that start at `dimension`
	float get1D(uint32_t dimension);
	glm::vec2 get2D(uint32_t dimension);
	// the Sobol or Halton sequence index of this sample (0 for the samplers that index per request)
	uint64_t sequenceIndex() const { return m_index; }

private:
	const SamplerParams& m_params;
	const SamplerTables& m_tables;
	uint32_t m_x;
	uint32_t m_y;
	uint32_t m_sampleIndex;
	// the sequence index that does not depend on the dimension: Sobol's and Halton's
	uint64_t m_index { 0 };
	uint32_t m_pixelHash { 0 };
};

// The camera's six dimensions for sample `sampleIndex` of pixel (x, y), into record[12]: the pixel
// and lens offsets in direct, time and wavelength in indirect.xy
void sampleCameraRecord(const SamplerParams& params, const SamplerTables& tables, uint32_t x, uint32_t y, uint32_t sampleIndex, float record[SAMPLE_RECORD_FLOATS]);
// Bounce `bounce`'s dimensions into record[12]. The medium fields are not generated yet (the
// participating-media plan switches them on) and are zero, as is the spare in direct.w
void sampleBounceRecord(const SamplerParams& params, const SamplerTables& tables, uint32_t x, uint32_t y, uint32_t sampleIndex, uint32_t bounce, float record[SAMPLE_RECORD_FLOATS]);

// PMJ02BN's building blocks: point `index` of set `set` (24 bits of its fixed-point value), and the
// blue-noise texture value pbrt's BlueNoise() returns
glm::vec2 samplerPmj02Bn(const SamplerTables& tables, uint32_t set, uint32_t index);
float samplerBlueNoise(const SamplerTables& tables, uint32_t textureIndex, uint32_t x, uint32_t y);

// the independent sampler: the record from the path's PCG state, advancing it
void sampleIndependentRecord(uint32_t& rngState, bool camera, float record[SAMPLE_RECORD_FLOATS]);
