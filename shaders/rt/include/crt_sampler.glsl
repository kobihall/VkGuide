// Kernel 01's generators and its body: every variant of the stage is a .comp that defines one
// CRT_SAMPLER_<KIND> and includes this file. The mathematics is src/sampler.cpp, mirrored line for
// line (bin/sampler_test checks that side); change both together.
//
// Kernel 01 runs twice per frame in a record-writing variant. Once over the whole pool before kernel
// 00, with Params.bounce == CRT_CAMERA_PASS, to write the camera's dimensions (the offset in the
// pixel, the lens position, time, wavelength); then at the top of every bounce, over the live ray
// queue, for that bounce's twelve dimensions. A path's pixel and sample index follow from its pool
// slot, which is its path index: slot = pixel * samplesPerFrame + k, and the sample index is the
// pixel's samples so far plus k. The dimensions are src/sampler.h's:
//
//     0, 1   the offset in the pixel      2, 3   the lens         4   time        5   wavelength
//     6 + 12 b + (0 .. 11)   bounce b: direct.xyzw, indirect.xyzw, medium.xyzw
//
// The sampler's choices arrive packed in Params (src/sampler.h PackedSamplerParams). Requires
// crt_common.glsl and crt_random.glsl, one CRT_SAMPLER_* defined, and for the Sobol samplers the
// SamplerTables binding.

#if !defined(CRT_SAMPLER_INDEPENDENT) && !defined(CRT_SAMPLER_STRATIFIED) && !defined(CRT_SAMPLER_HALTON) \
	&& !defined(CRT_SAMPLER_SOBOL) && !defined(CRT_SAMPLER_PADDED_SOBOL) && !defined(CRT_SAMPLER_ZSOBOL) \
	&& !defined(CRT_SAMPLER_PMJ02BN)
#error "define a CRT_SAMPLER_* before including crt_sampler.glsl"
#endif

#if defined(CRT_SAMPLER_SOBOL) || defined(CRT_SAMPLER_PADDED_SOBOL) || defined(CRT_SAMPLER_ZSOBOL)
#define CRT_SAMPLER_USES_SOBOL
#endif

layout (local_size_x = CRT_WORKGROUP) in;

//---------------------------------------------------------------- the sampler's parameters

uint samplerRandomization()
{
	return pc.samplerOptions & 3u;
}

uint samplerBlockLog2()
{
	return (pc.samplerOptions >> 2u) & 31u;
}

uint samplerResolutionLog2()
{
	return (pc.samplerOptions >> 7u) & 31u;
}

//---------------------------------------------------------------- hashing and permutations

uint samplerMix(uint x)
{
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

uint samplerHash(uint a, uint b)
{
	return samplerMix(a ^ samplerMix(b + 0x9e3779b9u));
}

uint samplerHash(uint a, uint b, uint c)
{
	return samplerMix(samplerHash(a, b) ^ samplerMix(c + 0x85ebca6bu));
}

// the 24 high bits of v as a float in [0, 1): exact, and never 1
float samplerToFloat(uint v)
{
	return float(v >> 8u) * (1.0 / 16777216.0);
}

// pbrt's PermutationElement()
uint samplerPermutationElement(uint i, uint l, uint p)
{
	uint w = l - 1u;
	w |= w >> 1u;
	w |= w >> 2u;
	w |= w >> 4u;
	w |= w >> 8u;
	w |= w >> 16u;
	do {
		i ^= p;
		i *= 0xe170893du;
		i ^= p >> 16u;
		i ^= (i & w) >> 4u;
		i ^= p >> 8u;
		i *= 0x0929eb3fu;
		i ^= p >> 23u;
		i ^= (i & w) >> 1u;
		i *= 1u | p >> 27u;
		i *= 0x6935fa69u;
		i ^= (i & w) >> 11u;
		i *= 0x74dcb303u;
		i ^= (i & w) >> 2u;
		i *= 0x9e501cc3u;
		i ^= (i & w) >> 2u;
		i *= 0xc860a3dfu;
		i &= w;
		i ^= i >> 5u;
	} while (i >= l);
	return (i + p) % l;
}

// a 24-bit float from the PCG stream, for the samplers that jitter and the independent one
float samplerPcgFloat(inout uint state)
{
	state = state * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	word = (word >> 22u) ^ word;
	return samplerToFloat(word);
}

//---------------------------------------------------------------- scrambling

uint samplerScramble(uint randomization, uint seed, uint v)
{
	if (randomization == 1u) {
		return v ^ seed;
	}
	if (randomization == 2u) {
		// Laine and Karras
		v = bitfieldReverse(v);
		v ^= v * 0x3d20adeau;
		v += seed;
		v *= (seed >> 16u) | 1u;
		v ^= v * 0x05526c56u;
		v ^= v * 0x53a22864u;
		return bitfieldReverse(v);
	}
	if (randomization == 3u) {
		if ((seed & 1u) != 0u) {
			v ^= 1u << 31u;
		}
		// the decision for binary digit b depends on the digits above it, so the eight lowest, which
		// samplerToFloat() drops, are left unscrambled (src/sampler.cpp)
		for (uint b = 1u; b < 24u; b++) {
			const uint mask = (~0u) << (32u - b);
			if ((samplerMix((v & mask) ^ seed) & (1u << b)) != 0u) {
				v ^= 1u << (31u - b);
			}
		}
		return v;
	}
	return v;
}

#ifdef CRT_SAMPLER_USES_SOBOL

//---------------------------------------------------------------- 64-bit values as uvec2 (x low, y high)

uvec2 shiftLeft64(uvec2 v, uint n)
{
	if (n == 0u) {
		return v;
	}
	if (n >= 32u) {
		return uvec2(0u, v.x << (n - 32u));
	}
	return uvec2(v.x << n, (v.y << n) | (v.x >> (32u - n)));
}

uvec2 shiftRight64(uvec2 v, uint n)
{
	if (n == 0u) {
		return v;
	}
	if (n >= 32u) {
		return uvec2(v.y >> (n - 32u), 0u);
	}
	return uvec2((v.x >> n) | (v.y << (32u - n)), v.y >> n);
}

// the bits of a 16-bit value spread to the even bits of 32 (pbrt's LeftShift2 on the low half)
uint spreadBits16(uint x)
{
	x &= 0xffffu;
	x = (x | (x << 8u)) & 0x00ff00ffu;
	x = (x | (x << 4u)) & 0x0f0f0f0fu;
	x = (x | (x << 2u)) & 0x33333333u;
	x = (x | (x << 1u)) & 0x55555555u;
	return x;
}

// the interleaving of the bits of x and y, which are below 2^16 (the render's size is checked
// against it: src/rt_gpu.cpp)
uvec2 morton2(uint x, uint y)
{
	return uvec2(spreadBits16(x) | (spreadBits16(y) << 1u), 0u);
}

//---------------------------------------------------------------- the tables
//
// samplerTables[]: a 16-word header, then the tables. src/sampler.cpp packSamplerTablesForGpu() lays
// it out and src/rt_gpu.cpp uploads it.
//   [0] first word of SobolMatrices32[1024][52]     [1] first word of VdCSobolMatrices (lo, hi pairs)
//   [2] first word of VdCSobolMatricesInv           [3] rows in the van der Corput tables
//   [4] first word of the PMJ02BN sets              [5] first word of the blue noise, two to a word
//   [6] first word of PMJ02BN's pixel samples for the render's sample count, (x, y) float pairs

#define CRT_SOBOL_COLUMNS 52u

uint sobolMatrix(uint dimension, uint column)
{
	return samplerTables[samplerTables[0] + dimension * CRT_SOBOL_COLUMNS + column];
}

// Sobol dimension `dimension` at sequence index `index`, scrambled
float sobolSample(uvec2 index, uint dimension, uint randomization, uint scramblerSeed)
{
	uint v = 0u;
	const uint base = samplerTables[0] + dimension * CRT_SOBOL_COLUMNS;
	uint lo = index.x;
	while (lo != 0u) {
		v ^= samplerTables[base + uint(findLSB(lo))];
		lo &= lo - 1u;
	}
	uint hi = index.y;
	while (hi != 0u) {
		v ^= samplerTables[base + 32u + uint(findLSB(hi))];
		hi &= hi - 1u;
	}
	return samplerToFloat(samplerScramble(randomization, scramblerSeed, v));
}

// pbrt's SobolIntervalToIndex(): the sequence index of sample `frame` of pixel (x, y) when one Sobol
// sequence is scaled over a 2^m x 2^m image
uvec2 sobolIntervalToIndex(uint m, uint frame, uint x, uint y)
{
	if (m == 0u) {
		return uvec2(frame, 0u);
	}
	const uint row = (m - 1u) * CRT_SOBOL_COLUMNS;
	const uint vdc = samplerTables[1];
	const uint vdcInverse = samplerTables[2];
	uvec2 index = shiftLeft64(uvec2(frame, 0u), m << 1u);

	uvec2 delta = uvec2(0u);
	uint f = frame;
	while (f != 0u) {
		const uint c = uint(findLSB(f));
		delta ^= uvec2(samplerTables[vdc + 2u * (row + c)], samplerTables[vdc + 2u * (row + c) + 1u]);
		f &= f - 1u;
	}

	uvec2 b = uvec2(((x << m) | y) ^ delta.x, delta.y);
	uint lo = b.x;
	while (lo != 0u) {
		const uint c = uint(findLSB(lo));
		index ^= uvec2(samplerTables[vdcInverse + 2u * (row + c)], samplerTables[vdcInverse + 2u * (row + c) + 1u]);
		lo &= lo - 1u;
	}
	uint hi = b.y;
	while (hi != 0u) {
		const uint c = 32u + uint(findLSB(hi));
		index ^= uvec2(samplerTables[vdcInverse + 2u * (row + c)], samplerTables[vdcInverse + 2u * (row + c) + 1u]);
		hi &= hi - 1u;
	}
	return index;
}

#endif // CRT_SAMPLER_USES_SOBOL

#ifdef CRT_SAMPLER_ZSOBOL

// the 24 permutations of four digits ZSobol picks from, four to a row
const uint Z_PERMUTATIONS[96] = uint[](
	0u, 1u, 2u, 3u, 0u, 1u, 3u, 2u, 0u, 2u, 1u, 3u, 0u, 2u, 3u, 1u, 0u, 3u, 2u, 1u, 0u, 3u, 1u, 2u,
	1u, 0u, 2u, 3u, 1u, 0u, 3u, 2u, 1u, 2u, 0u, 3u, 1u, 2u, 3u, 0u, 1u, 3u, 2u, 0u, 1u, 3u, 0u, 2u,
	2u, 1u, 0u, 3u, 2u, 1u, 3u, 0u, 2u, 0u, 1u, 3u, 2u, 0u, 3u, 1u, 2u, 3u, 0u, 1u, 2u, 3u, 1u, 0u,
	3u, 1u, 2u, 0u, 3u, 1u, 0u, 2u, 3u, 2u, 1u, 0u, 3u, 2u, 0u, 1u, 3u, 0u, 2u, 1u, 3u, 0u, 1u, 2u);

// the hash a digit's permutation is chosen with: the digits above it and the dimension
uint zSobolHash(uvec2 higherDigits, uint dimension)
{
	return samplerHash(higherDigits.x ^ (0x55555555u * dimension), higherDigits.y);
}

// ZSobol's permutation of a Morton-ordered (pixel, sample) index into a Sobol sequence index
uvec2 zSobolIndex(uvec2 mortonIndex, uint dimension)
{
	const uint blockLog2 = samplerBlockLog2();
	const uint nBase4Digits = samplerResolutionLog2() + (blockLog2 + 1u) / 2u;
	uvec2 sampleIndex = uvec2(0u);
	const bool pow2Samples = (blockLog2 & 1u) != 0u;
	const int lastDigit = pow2Samples ? 1 : 0;
	for (int i = int(nBase4Digits) - 1; i >= lastDigit; i--) {
		const uint digitShift = uint(2 * i - (pow2Samples ? 1 : 0));
		uint digit = shiftRight64(mortonIndex, digitShift).x & 3u;
		const uvec2 higherDigits = shiftRight64(mortonIndex, digitShift + 2u);
		const uint p = (zSobolHash(higherDigits, dimension) >> 24u) % 24u;
		digit = Z_PERMUTATIONS[p * 4u + digit];
		sampleIndex |= shiftLeft64(uvec2(digit, 0u), digitShift);
	}
	if (pow2Samples) {
		const uint digit = mortonIndex.x & 1u;
		sampleIndex.x |= digit ^ (zSobolHash(shiftRight64(mortonIndex, 1u), dimension) & 1u);
	}
	return sampleIndex;
}

#endif // CRT_SAMPLER_ZSOBOL

#ifdef CRT_SAMPLER_PMJ02BN

#define CRT_PMJ02BN_SETS 5u
#define CRT_PMJ02BN_SAMPLES 65536u

// the known sample count, which the pixel samples were sorted for (src/sampler.cpp
// preparePmj02BnPixelSamples())
uint pmjSamplesPerPixel()
{
	return clamp(pc.maxSamples, 1u, CRT_PMJ02BN_SAMPLES);
}

// the side of the tile of pixels the pixel samples repeat over
uint pmjTileSize(uint samplesPerPixel)
{
	uint log4 = 0u;
	while ((1u << (2u * log4)) < samplesPerPixel) {
		log4++;
	}
	return 1u << (8u - min(log4, 8u));
}

vec2 pmjPoint(uint set, uint index)
{
	const uint base = samplerTables[4] + 2u * ((set % CRT_PMJ02BN_SETS) * CRT_PMJ02BN_SAMPLES + index % CRT_PMJ02BN_SAMPLES);
	return vec2(samplerToFloat(samplerTables[base]), samplerToFloat(samplerTables[base + 1u]));
}

// pbrt's BlueNoise(): one of 48 128 x 128 textures of 16-bit values, tiled over the image
float blueNoise(uint textureIndex, uint x, uint y)
{
	const uint index = ((textureIndex % 48u) * 128u + x % 128u) * 128u + y % 128u;
	const uint word = samplerTables[samplerTables[5] + (index >> 1u)];
	return float((word >> (16u * (index & 1u))) & 0xFFFFu) * (1.0 / 65535.0);
}

#endif // CRT_SAMPLER_PMJ02BN

#ifdef CRT_SAMPLER_HALTON

#include "crt_halton_tables.glsl"

#define CRT_ONE_MINUS_EPSILON_F 0.99999994

float haltonRadicalInverse(uint baseIndex, uint a)
{
	const uint base = CRT_HALTON_PRIMES[baseIndex];
	const uint limit = 0xFFFFFFFFu / base - base;
	const float invBase = CRT_HALTON_INVERSE_PRIMES[baseIndex];
	float invBaseM = 1.0;
	uint reversedDigits = 0u;
	while (a != 0u && reversedDigits < limit) {
		const uint next = a / base;
		const uint digit = a - next * base;
		reversedDigits = reversedDigits * base + digit;
		invBaseM *= invBase;
		a = next;
	}
	return min(float(reversedDigits) * invBaseM, CRT_ONE_MINUS_EPSILON_F);
}

float haltonScrambledRadicalInverse(uint baseIndex, uint a, uint seed)
{
	const uint base = CRT_HALTON_PRIMES[baseIndex];
	const uint limit = 0xFFFFFFFFu / base - base;
	const float invBase = CRT_HALTON_INVERSE_PRIMES[baseIndex];
	float invBaseM = 1.0;
	uint reversedDigits = 0u;
	const uint digits = CRT_HALTON_SCRAMBLED_DIGITS[baseIndex];
	for (uint digitIndex = 0u; digitIndex < digits && reversedDigits < limit; digitIndex++) {
		const uint next = a / base;
		const uint digitValue = a - next * base;
		const uint permuted = samplerPermutationElement(digitValue, base, samplerHash(base, digitIndex, seed));
		reversedDigits = reversedDigits * base + permuted;
		invBaseM *= invBase;
		a = next;
	}
	return min(invBaseM * float(reversedDigits), CRT_ONE_MINUS_EPSILON_F);
}

float haltonOwenRadicalInverse(uint baseIndex, uint a, uint hash)
{
	const uint base = CRT_HALTON_PRIMES[baseIndex];
	const uint limit = 0xFFFFFFFFu / base - base;
	const float invBase = CRT_HALTON_INVERSE_PRIMES[baseIndex];
	float invBaseM = 1.0;
	uint reversedDigits = 0u;
	const uint digits = CRT_HALTON_OWEN_DIGITS[baseIndex];
	for (uint digitIndex = 0u; digitIndex < digits && reversedDigits < limit; digitIndex++) {
		const uint next = a / base;
		uint digitValue = a - next * base;
		const uint digitHash = samplerMix(hash ^ reversedDigits);
		digitValue = samplerPermutationElement(digitValue, base, digitHash);
		reversedDigits = reversedDigits * base + digitValue;
		invBaseM *= invBase;
		a = next;
	}
	return min(invBaseM * float(reversedDigits), CRT_ONE_MINUS_EPSILON_F);
}

uint haltonInverseRadicalInverse(uint inverse, uint base, uint digits)
{
	uint index = 0u;
	for (uint i = 0u; i < digits; i++) {
		const uint digit = inverse % base;
		inverse /= base;
		index = index * base + digit;
	}
	return index;
}

uint haltonScale(uint base, uint exponent)
{
	uint scale = 1u;
	for (uint i = 0u; i < exponent; i++) {
		scale *= base;
	}
	return scale;
}

uint haltonExponent0()
{
	return (pc.samplerOptions >> 12u) & 15u;
}

uint haltonExponent1()
{
	return (pc.samplerOptions >> 16u) & 15u;
}

#endif // CRT_SAMPLER_HALTON

//---------------------------------------------------------------- one (pixel, sample index)

struct SampleState {
	uint x;
	uint y;
	uint sampleIndex;
	uint pixelHash;
	// Sobol's and ZSobol's sequence index; Halton's in the low word
	uvec2 index;
};

SampleState startSample(uint x, uint y, uint sampleIndex)
{
	SampleState s;
	s.x = x;
	s.y = y;
	s.sampleIndex = sampleIndex;
	s.pixelHash = samplerHash(x, y);
	s.index = uvec2(0u);
#ifdef CRT_SAMPLER_SOBOL
	s.index = sobolIntervalToIndex(samplerResolutionLog2(), sampleIndex, x, y);
#endif
#ifdef CRT_SAMPLER_HALTON
	{
		const uint scale0 = haltonScale(2u, haltonExponent0());
		const uint scale1 = haltonScale(3u, haltonExponent1());
		const uint stride = scale0 * scale1;
		uint haltonIndex = 0u;
		if (stride > 1u) {
			const uint pm0 = x % 128u;
			const uint pm1 = y % 128u;
			const uint inverse0 = pc.samplerHalton & 255u;
			const uint inverse1 = (pc.samplerHalton >> 8u) & 255u;
			haltonIndex += haltonInverseRadicalInverse(pm0, 2u, haltonExponent0()) * (stride / scale0) * inverse0;
			haltonIndex += haltonInverseRadicalInverse(pm1, 3u, haltonExponent1()) * (stride / scale1) * inverse1;
			haltonIndex %= stride;
		}
		haltonIndex += sampleIndex * stride;
		s.index = uvec2(haltonIndex, 0u);
	}
#endif
	return s;
}

#if defined(CRT_SAMPLER_PADDED_SOBOL)

// the index a padded Sobol request reads: the sample's place in its block, permuted by the request,
// and its block's place in the sequence
uvec2 paddedIndex(SampleState s, uint h0)
{
	const uint blockLog2 = samplerBlockLog2();
	const uint mask = (1u << blockLog2) - 1u;
	const uint permuted = samplerPermutationElement(s.sampleIndex & mask, 1u << blockLog2, h0);
	return shiftLeft64(uvec2(s.sampleIndex >> blockLog2, 0u), blockLog2) | uvec2(permuted, 0u);
}

#endif

#if defined(CRT_SAMPLER_ZSOBOL)

uvec2 zsobolIndex(SampleState s, uint dimension)
{
	const uint blockLog2 = samplerBlockLog2();
	const uint mask = (1u << blockLog2) - 1u;
	const uvec2 mortonIndex = shiftLeft64(morton2(s.x, s.y), blockLog2) | uvec2(s.sampleIndex & mask, 0u);
	const uvec2 zIndex = zSobolIndex(mortonIndex, dimension);
	const uint indexBits = 2u * samplerResolutionLog2() + blockLog2;
	return shiftLeft64(uvec2(s.sampleIndex >> blockLog2, 0u), indexBits) | zIndex;
}

#endif

float get1D(SampleState s, uint dimension)
{
#if defined(CRT_SAMPLER_SOBOL)
	return sobolSample(s.index, dimension, samplerRandomization(), samplerHash(dimension, pc.seed));
#elif defined(CRT_SAMPLER_HALTON)
	const uint randomization = samplerRandomization();
	if (randomization == 0u) {
		return haltonRadicalInverse(dimension, s.index.x);
	}
	if (randomization == 1u) {
		return haltonScrambledRadicalInverse(dimension, s.index.x, pc.seed);
	}
	return haltonOwenRadicalInverse(dimension, s.index.x, samplerHash(1u + (dimension << 4u), pc.seed));
#elif defined(CRT_SAMPLER_PADDED_SOBOL)
	const uint h0 = samplerHash(s.pixelHash, dimension, pc.seed);
	const uint h1 = samplerMix(h0 ^ 0x68bc21ebu);
	return sobolSample(paddedIndex(s, h0), 0u, samplerRandomization(), h1);
#elif defined(CRT_SAMPLER_ZSOBOL)
	return sobolSample(zsobolIndex(s, dimension), 0u, samplerRandomization(), samplerMix(samplerHash(dimension, pc.seed) ^ 0x68bc21ebu));
#elif defined(CRT_SAMPLER_STRATIFIED)
	const uint spp = max(pc.maxSamples, 1u);
	const uint hash = samplerHash(s.pixelHash, dimension, pc.seed);
	const uint wrapped = s.sampleIndex % spp;
	const uint stratum = samplerPermutationElement(wrapped, spp, hash);
	uint jitter = pcgHash(hash ^ wrapped);
	const float delta = samplerPcgFloat(jitter);
	return min((float(stratum) + delta) * pc.samplerInvSamples, 0.99999994);
#elif defined(CRT_SAMPLER_PMJ02BN)
	// a stratum per sample, permuted per pixel and dimension, offset by blue noise
	const uint spp = pmjSamplesPerPixel();
	const uint hash = samplerHash(s.pixelHash, dimension, pc.seed);
	const uint index = samplerPermutationElement(s.sampleIndex % spp, spp, hash);
	const float delta = blueNoise(dimension, s.x, s.y);
	return min((float(index) + delta) * pc.samplerInvSamples, 0.99999994);
#else
	return 0.0;
#endif
}

vec2 get2D(SampleState s, uint dimension)
{
#if defined(CRT_SAMPLER_SOBOL) || defined(CRT_SAMPLER_HALTON)
	const float x = get1D(s, dimension);
	const float y = get1D(s, dimension + 1u);
	return vec2(x, y);
#elif defined(CRT_SAMPLER_PADDED_SOBOL)
	const uint h0 = samplerHash(s.pixelHash, dimension, pc.seed);
	const uint h1 = samplerMix(h0 ^ 0x68bc21ebu);
	const uvec2 index = paddedIndex(s, h0);
	const float x = sobolSample(index, 0u, samplerRandomization(), h0);
	const float y = sobolSample(index, 1u, samplerRandomization(), h1);
	return vec2(x, y);
#elif defined(CRT_SAMPLER_ZSOBOL)
	const uvec2 index = zsobolIndex(s, dimension);
	const uint h0 = samplerHash(dimension, pc.seed);
	const uint h1 = samplerMix(h0 ^ 0x68bc21ebu);
	const float x = sobolSample(index, 0u, samplerRandomization(), h0);
	const float y = sobolSample(index, 1u, samplerRandomization(), h1);
	return vec2(x, y);
#elif defined(CRT_SAMPLER_STRATIFIED)
	const uint spp = max(pc.maxSamples, 1u);
	const uint strataX = max(pc.samplerStrata & 0xFFFFu, 1u);
	const uint strataY = max(spp / strataX, 1u);
	const uint hash = samplerHash(s.pixelHash, dimension, pc.seed);
	const uint wrapped = s.sampleIndex % spp;
	const uint stratum = samplerPermutationElement(wrapped, spp, hash);
	const uint sx = stratum % strataX;
	const uint sy = stratum / strataX;
	uint jitter = pcgHash(hash ^ wrapped);
	const float dx = samplerPcgFloat(jitter);
	const float dy = samplerPcgFloat(jitter);
	return vec2(min((float(sx) + dx) * pc.samplerInvStrataX, 0.99999994), min((float(sy) + dy) * pc.samplerInvStrataY, 0.99999994));
#elif defined(CRT_SAMPLER_PMJ02BN)
	// the first five pairs of dimensions read the five sets in order, later ones read them again
	// with the samples permuted; each pixel's points are rotated by blue noise
	const uint spp = pmjSamplesPerPixel();
	uint index = s.sampleIndex % spp;
	const uint instance = dimension / 2u;
	if (instance >= CRT_PMJ02BN_SETS) {
		index = samplerPermutationElement(index, spp, samplerHash(s.pixelHash, dimension, pc.seed));
	}
	vec2 u = pmjPoint(instance, index);
	u.x += blueNoise(dimension, s.x, s.y);
	u.y += blueNoise(dimension + 1u, s.x, s.y);
	if (u.x >= 1.0) {
		u.x -= 1.0;
	}
	if (u.y >= 1.0) {
		u.y -= 1.0;
	}
	return vec2(min(u.x, 0.99999994), min(u.y, 0.99999994));
#else
	return vec2(0.0);
#endif
}

// dimensions 0 and 1: the sample's offset in its pixel
vec2 pixel2D(SampleState s)
{
#if defined(CRT_SAMPLER_SOBOL)
	// the first two Sobol dimensions cover the image: this pixel's part of them
	const float scale = float(1u << samplerResolutionLog2());
	const float u = sobolSample(s.index, 0u, 0u, 0u);
	const float v = sobolSample(s.index, 1u, 0u, 0u);
	return vec2(clamp(u * scale - float(s.x), 0.0, 0.99999994), clamp(v * scale - float(s.y), 0.0, 0.99999994));
#elif defined(CRT_SAMPLER_HALTON)
	const uint scale1 = haltonScale(3u, haltonExponent1());
	const float x = haltonRadicalInverse(0u, s.index.x >> haltonExponent0());
	const float y = haltonRadicalInverse(1u, s.index.x / scale1);
	return vec2(x, y);
#elif defined(CRT_SAMPLER_PMJ02BN)
	// the pixel's cell of the tile set 0 was sorted into
	const uint spp = pmjSamplesPerPixel();
	const uint tile = pmjTileSize(spp);
	const uint cell = s.x % tile + (s.y % tile) * tile;
	const uint base = samplerTables[6] + 2u * (cell * spp + s.sampleIndex % spp);
	return vec2(uintBitsToFloat(samplerTables[base]), uintBitsToFloat(samplerTables[base + 1u]));
#else
	return get2D(s, 0u);
#endif
}

//---------------------------------------------------------------- the record

#ifdef CRT_SAMPLER_INDEPENDENT

void fillCameraRecord(uint pathIndex, uint pixel, uint x, uint y, uint sampleIndex)
{
	// the camera ray's seed, kernel 00's own (the record flag mixes a salt in after, so that the
	// stream the rest of the path spills into is not these numbers again)
	uint rng = pcgHash(pixel ^ pcgHash(sampleIndex ^ pcgHash(pc.seed)));
	float values[6];
	for (uint i = 0u; i < 6u; i++) {
		values[i] = samplerPcgFloat(rng);
	}
	raySamples[pathIndex].direct = vec4(values[0], values[1], values[2], values[3]);
	raySamples[pathIndex].indirect = vec4(values[4], values[5], 0.0, 0.0);
	raySamples[pathIndex].medium = vec4(0.0);
}

void fillBounceRecord(uint pathIndex, uint pixel, uint x, uint y, uint sampleIndex, uint bounce)
{
	// the path's own stream, advanced past the seven numbers it hands over
	uint rng = paths[pathIndex].rngState;
	const float lightChoice = samplerPcgFloat(rng);
	const float lightX = samplerPcgFloat(rng);
	const float lightY = samplerPcgFloat(rng);
	const float lobe = samplerPcgFloat(rng);
	const float directionX = samplerPcgFloat(rng);
	const float directionY = samplerPcgFloat(rng);
	const float roulette = samplerPcgFloat(rng);
	raySamples[pathIndex].direct = vec4(lightChoice, lightX, lightY, 0.0);
	raySamples[pathIndex].indirect = vec4(lobe, directionX, directionY, roulette);
	raySamples[pathIndex].medium = vec4(0.0);
	paths[pathIndex].rngState = rng;
}

#else

void fillCameraRecord(uint pathIndex, uint pixel, uint x, uint y, uint sampleIndex)
{
	SampleState s = startSample(x, y, sampleIndex);
	const vec2 offset = pixel2D(s);
	const vec2 lens = get2D(s, 2u);
	const float time = get1D(s, 4u);
	const float wavelength = get1D(s, 5u);
	raySamples[pathIndex].direct = vec4(offset, lens);
	raySamples[pathIndex].indirect = vec4(time, wavelength, 0.0, 0.0);
	raySamples[pathIndex].medium = vec4(0.0);
}

void fillBounceRecord(uint pathIndex, uint pixel, uint x, uint y, uint sampleIndex, uint bounce)
{
	SampleState s = startSample(x, y, sampleIndex);
	const uint base = 6u + 12u * bounce;
	const float lightChoice = get1D(s, base);
	const vec2 lightPoint = get2D(s, base + 1u);
	const float lobe = get1D(s, base + 4u);
	const vec2 direction = get2D(s, base + 5u);
	const float roulette = get1D(s, base + 7u);
	// the spare in direct.w, and the medium's four, are not generated: nothing reads them yet
	raySamples[pathIndex].direct = vec4(lightChoice, lightPoint, 0.0);
	raySamples[pathIndex].indirect = vec4(lobe, direction, roulette);
	raySamples[pathIndex].medium = vec4(0.0);
}

#endif // CRT_SAMPLER_INDEPENDENT

void main()
{
	const uint index = gl_GlobalInvocationID.x;
	uint pathIndex;
	if (pc.bounce == CRT_CAMERA_PASS) {
		// over the pool: the slots that will spawn, which kernel 00 decides the same way
		if (index >= pc.poolSize) {
			return;
		}
		pathIndex = index;
	} else {
		const uint queue = rayQueueFor(pc.bounce);
		if (index >= headers[queue].rayCount) {
			return;
		}
		pathIndex = queues[queueSlot(queue, index)];
	}

	// the slot is pixel * samplesPerFrame + k, and the pixel's samples so far are the film's count
	const uint pixel = pathIndex / pc.samplesPerFrame;
	const uint k = pathIndex - pixel * pc.samplesPerFrame;
	const uint x = pixel % pc.width;
	const uint y = pixel / pc.width;
	const uint sampleIndex = imageLoad(samplesTaken, ivec2(x, y)).r + k;

	if (pc.bounce == CRT_CAMERA_PASS) {
		if (k < min(sampleBudget[pixel], pc.samplesPerFrame)) {
			fillCameraRecord(pathIndex, pixel, x, y, sampleIndex);
		}
		return;
	}

	fillBounceRecord(pathIndex, pixel, x, y, sampleIndex, pc.bounce);
	// one round of the same PCG the rest of the tracer uses, mixing in the bounce so that two
	// bounces of one path never walk the same stretch of the stream the consumers spill into
	paths[pathIndex].rngState = pcgHash(paths[pathIndex].rngState ^ pcgHash(pc.bounce));
}
