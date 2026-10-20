// CPU verification of the sample generators behind kernel 01 (src/sampler.h, the line-for-line
// mirror of shaders/rt/include/crt_sampler.glsl). No Vulkan, no GPU.
//
// What each sampler promises, checked over several pixels, seeds and randomizations:
//  - determinism: a sample is a pure function of (pixel, sample index, dimension, seed), asked for
//    in any order
//  - every value is in [0, 1)
//  - the (0, 2)-sequence samplers (padded Sobol, ZSobol, PMJ02BN) give each pixel's 2D requests a
//    (0, m, 2)-net over every aligned run of 2^m samples: every elementary interval of area 2^-m
//    holds exactly one point. Padded Sobol keeps that across consecutive blocks too
//  - one Sobol sequence over the image puts the first scale^2 points, one per pixel, in distinct
//    strata of the whole image
//  - Halton's per-pixel indices tile [0, stride) with no gap or repeat over the image's base scales
//  - the stratified sampler puts exactly one sample in each of its strata
//  - the L2 star discrepancy of 2D point sets is far below independent sampling's
//  - ZSobol's error at one sample per pixel is blue: less of it in the low frequencies than padded
//    Sobol's (PBR 4ed figure 8.38)
// And the tables: the first Sobol matrices are the known ones, the permutation is a bijection, every
// scrambler keeps the digits it should.
//
// Run:  ./bin/sampler_test [--tables assets/sampler]

#include <sampler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <fmt/format.h>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
	if (!ok) {
		fmt::println("    FAIL: {}", what);
		g_failures++;
	}
}

const char* kindName(SamplerKind kind)
{
	switch (kind) {
	case SamplerKind::Independent:
		return "independent";
	case SamplerKind::Stratified:
		return "stratified";
	case SamplerKind::Halton:
		return "halton";
	case SamplerKind::Sobol:
		return "sobol";
	case SamplerKind::PaddedSobol:
		return "padded sobol";
	case SamplerKind::ZSobol:
		return "zsobol";
	case SamplerKind::Pmj02Bn:
		return "pmj02bn";
	case SamplerKind::Count:
		break;
	}
	return "?";
}

const SamplerRandomization ALL_RANDOMIZATIONS[] = { SamplerRandomization::None, SamplerRandomization::PermuteDigits, SamplerRandomization::FastOwen, SamplerRandomization::Owen };

// every elementary interval of area 2^-k, in every one of its k + 1 shapes, holds exactly one point
bool isNet(const std::vector<glm::vec2>& points, uint32_t k)
{
	if (points.size() != (1u << k)) {
		return false;
	}
	for (uint32_t a = 0; a <= k; a++) {
		const uint32_t columns = 1u << a;
		const uint32_t rows = 1u << (k - a);
		std::vector<uint32_t> counts((size_t)columns * rows, 0);
		for (const glm::vec2& p : points) {
			const uint32_t cx = std::min((uint32_t)(p.x * (float)columns), columns - 1);
			const uint32_t cy = std::min((uint32_t)(p.y * (float)rows), rows - 1);
			counts[cy * columns + cx]++;
		}
		for (const uint32_t count : counts) {
			if (count != 1) {
				return false;
			}
		}
	}
	return true;
}

// the L2 star discrepancy of points in [0, 1)^2 (Warnock's formula), squared
double l2StarDiscrepancySquared(const std::vector<glm::vec2>& points)
{
	const double n = (double)points.size();
	double sum1 = 0.0;
	double sum2 = 0.0;
	for (const glm::vec2& p : points) {
		sum1 += (1.0 - (double)p.x * p.x) * (1.0 - (double)p.y * p.y);
	}
	for (const glm::vec2& p : points) {
		for (const glm::vec2& q : points) {
			sum2 += (1.0 - std::max((double)p.x, (double)q.x)) * (1.0 - std::max((double)p.y, (double)q.y));
		}
	}
	return 1.0 / 9.0 - sum1 / (n * 2.0) + sum2 / (n * n);
}

//---------------------------------------------------------------- tables and building blocks

void testTables(const SamplerTables& tables)
{
	fmt::println("tables and building blocks");
	check(tables.loaded(), "tables loaded");
	// dimension 0 is the van der Corput sequence: column i is the single bit 31 - i
	bool vdc = true;
	for (uint32_t i = 0; i < 32; i++) {
		vdc = vdc && tables.sobol[i] == (0x80000000u >> i);
	}
	check(vdc, "Sobol dimension 0 is the identity, bit-reversed");
	// dimension 1: the directions 1, 3, 5, 15, 17, 51, ... of the standard second dimension
	const uint32_t* second = tables.sobol.data() + SOBOL_COLUMNS;
	check(second[0] == 0x80000000u && second[1] == 0xc0000000u && second[2] == 0xa0000000u && second[3] == 0xf0000000u && second[4] == 0x88000000u && second[5] == 0xcc000000u,
		"Sobol dimension 1 starts 1/2, 3/4, 5/8, 15/16, 17/32, 51/64");
	check(tables.vdcRows == 25, "25 van der Corput rows");

	// samplerPermutationElement() is a bijection for lengths that are and are not powers of two
	for (const uint32_t length : { 1u, 2u, 5u, 12u, 64u, 100u, 1000u }) {
		for (const uint32_t seed : { 0u, 1u, 0xdeadbeefu }) {
			std::set<uint32_t> seen;
			bool inRange = true;
			for (uint32_t i = 0; i < length; i++) {
				const uint32_t element = samplerPermutationElement(i, length, seed);
				inRange = inRange && element < length;
				seen.insert(element);
			}
			check(inRange && seen.size() == length, fmt::format("permutation of {} (seed {:#x}) is a bijection", length, seed));
		}
	}

	// samplerToFloat() is exact, monotone and never reaches 1
	check(samplerToFloat(0) == 0.f && samplerToFloat(0xFFFFFFFFu) < 1.f && samplerToFloat(0x80000000u) == 0.5f, "samplerToFloat endpoints");

	// the scramblers: a permutation of the 32-bit values that keeps a digit's ancestry. Applied to the
	// 2^k values with the same high digits they only reorder within the interval they sit in
	for (const SamplerRandomization randomization : ALL_RANDOMIZATIONS) {
		bool intervalsKept = true;
		for (uint32_t k = 1; k <= 8; k++) {
			// the values i / 2^k for i < 2^k, scrambled, must land one per interval of width 2^-k
			std::set<uint32_t> intervals;
			for (uint32_t i = 0; i < (1u << k); i++) {
				const uint32_t value = samplerScramble(randomization, 0x1234567u, i << (32 - k));
				intervals.insert(value >> (32 - k));
			}
			intervalsKept = intervalsKept && intervals.size() == (1u << k);
		}
		check(intervalsKept, fmt::format("scrambler '{}' keeps one value per binary interval", samplerRandomizationName(randomization)));
	}
}

//---------------------------------------------------------------- determinism and range

void testBasics(SamplerTables& tables)
{
	fmt::println("determinism and range");
	for (uint32_t k = 0; k < (uint32_t)SamplerKind::Count; k++) {
		const SamplerKind kind = (SamplerKind)k;
		if (kind == SamplerKind::Independent) {
			continue;
		}
		preparePmj02BnPixelSamples(tables, 32);
		for (const SamplerRandomization randomization : ALL_RANDOMIZATIONS) {
			if ((kind == SamplerKind::Stratified || kind == SamplerKind::Pmj02Bn) && randomization != SamplerRandomization::Owen) {
				continue;
			}
			const SamplerParams params = makeSamplerParams(kind, randomization, 77, 200, 120, 32, false);
			bool deterministic = true;
			bool inRange = true;
			for (uint32_t pixel = 0; pixel < 40; pixel++) {
				const uint32_t x = (pixel * 37) % 200;
				const uint32_t y = (pixel * 11) % 120;
				for (uint32_t sample = 0; sample < 32; sample++) {
					float a[SAMPLE_RECORD_FLOATS];
					float b[SAMPLE_RECORD_FLOATS];
					sampleCameraRecord(params, tables, x, y, sample, a);
					// the other order of asking: bounce first, camera after
					float scratch[SAMPLE_RECORD_FLOATS];
					sampleBounceRecord(params, tables, x, y, sample, 3, scratch);
					sampleCameraRecord(params, tables, x, y, sample, b);
					deterministic = deterministic && memcmp(a, b, sizeof(a)) == 0;
					float bounce[SAMPLE_RECORD_FLOATS];
					sampleBounceRecord(params, tables, x, y, sample, 15, bounce);
					for (const float value : { a[0], a[1], a[2], a[3], a[4], a[5], bounce[0], bounce[1], bounce[2], bounce[4], bounce[5], bounce[6], bounce[7] }) {
						inRange = inRange && value >= 0.f && value < 1.f;
					}
				}
			}
			check(deterministic, fmt::format("{} / {}: deterministic", kindName(kind), samplerRandomizationName(randomization)));
			check(inRange, fmt::format("{} / {}: every value in [0, 1)", kindName(kind), samplerRandomizationName(randomization)));
		}
	}

	// the mean of a 1D request over many pixels and samples is 1/2
	preparePmj02BnPixelSamples(tables, 16);
	for (const SamplerKind kind : { SamplerKind::Stratified, SamplerKind::Halton, SamplerKind::Sobol, SamplerKind::PaddedSobol, SamplerKind::ZSobol, SamplerKind::Pmj02Bn }) {
		const SamplerParams params = makeSamplerParams(kind, SamplerRandomization::Owen, 5, 64, 64, 16, false);
		double sum = 0.0;
		uint32_t count = 0;
		for (uint32_t pixel = 0; pixel < 400; pixel++) {
			for (uint32_t sample = 0; sample < 16; sample++) {
				SampleGenerator generator(params, tables, pixel % 64, pixel / 7 % 64, sample);
				sum += generator.get1D(9);
				count++;
			}
		}
		const double mean = sum / count;
		check(std::abs(mean - 0.5) < 0.01, fmt::format("{}: mean of a 1D request {:.4f}", kindName(kind), mean));
	}
}

//---------------------------------------------------------------- nets

void testNets(const SamplerTables& tables)
{
	fmt::println("(0, m, 2)-nets");
	for (const SamplerKind kind : { SamplerKind::PaddedSobol, SamplerKind::ZSobol }) {
		for (const SamplerRandomization randomization : ALL_RANDOMIZATIONS) {
			// a 100 x 60 image, 64 samples per pixel: one block of 2^6
			const SamplerParams params = makeSamplerParams(kind, randomization, 3, 100, 60, 64, false);
			check(params.blockLog2 == 6, "64 samples is a block of 2^6");
			bool allNets = true;
			for (uint32_t pixel = 0; pixel < 24 && allNets; pixel++) {
				const uint32_t x = (pixel * 41) % 100;
				const uint32_t y = (pixel * 17) % 60;
				for (const uint32_t dimension : { 2u, 6u + 12u * 3 + 5u }) {
					// the complete block: the samplers shuffle which sample takes which point of the net, so a
					// prefix of the block is a random subset of it, not a net (as in pbrt)
					std::vector<glm::vec2> points;
					for (uint32_t s = 0; s < 64; s++) {
						SampleGenerator generator(params, tables, x, y, s);
						points.push_back(generator.get2D(dimension));
					}
					allNets = allNets && isNet(points, 6);
				}
			}
			check(allNets, fmt::format("{} / {}: a pixel's block of 64 samples is a (0, 6, 2)-net per 2D request", kindName(kind), samplerRandomizationName(randomization)));
		}
	}

	// ZSobol has the stronger property per block of the same size
	{
		const SamplerParams params = makeSamplerParams(SamplerKind::ZSobol, SamplerRandomization::Owen, 3, 100, 60, 64, false);
		bool net = true;
		for (uint32_t pixel = 0; pixel < 30; pixel++) {
			std::vector<glm::vec2> points;
			for (uint32_t s = 0; s < 64; s++) {
				SampleGenerator generator(params, tables, (pixel * 13) % 100, (pixel * 29) % 60, s);
				points.push_back(generator.get2D(4));
			}
			net = net && isNet(points, 6);
		}
		check(net, "zsobol: a pixel's 64 samples are a (0, 6, 2)-net");
	}

	// padded Sobol continues across blocks: two blocks of 8 are together a 16-point net
	{
		SamplerParams params = makeSamplerParams(SamplerKind::PaddedSobol, SamplerRandomization::Owen, 11, 64, 64, 8, false);
		check(params.blockLog2 == 3, "8 samples is a block of 2^3");
		bool net = true;
		for (uint32_t pixel = 0; pixel < 20; pixel++) {
			std::vector<glm::vec2> points;
			for (uint32_t s = 0; s < 16; s++) {
				SampleGenerator generator(params, tables, pixel % 64, pixel / 3 % 64, s);
				points.push_back(generator.get2D(7));
			}
			net = net && isNet(points, 4);
		}
		check(net, "padded sobol: blocks 0 and 1 together are a (0, 4, 2)-net");
	}

	// unlimited samples: a block of 64, and the 128 samples of two blocks are a net
	{
		const SamplerParams params = makeSamplerParams(SamplerKind::PaddedSobol, SamplerRandomization::Owen, 11, 64, 64, 1, true);
		check(params.blockLog2 == 6 && params.samplesPerPixel == 0, "unlimited samples: blocks of 64");
		std::vector<glm::vec2> points;
		for (uint32_t s = 0; s < 128; s++) {
			SampleGenerator generator(params, tables, 5, 9, s);
			points.push_back(generator.get2D(7));
		}
		check(isNet(points, 7), "padded sobol, unlimited: 128 samples are a (0, 7, 2)-net");
	}
}

//---------------------------------------------------------------- global Sobol, Halton, stratified

void testGlobalSamplers(const SamplerTables& tables)
{
	fmt::println("one sequence over the image");
	// Sobol: the first sample of every pixel of an 8 x 8 image is a distinct stratum of the image's
	// 64-point net, and the first four samples of all pixels are a 256-point net
	{
		const SamplerParams params = makeSamplerParams(SamplerKind::Sobol, SamplerRandomization::Owen, 2, 8, 8, 4, false);
		check(params.resolutionLog2 == 3, "8 pixels is 2^3");
		for (const uint32_t frames : { 1u, 4u }) {
			std::vector<glm::vec2> points;
			for (uint32_t y = 0; y < 8; y++) {
				for (uint32_t x = 0; x < 8; x++) {
					for (uint32_t s = 0; s < frames; s++) {
						SampleGenerator generator(params, tables, x, y, s);
						const glm::vec2 offset = generator.pixel2D();
						points.push_back((glm::vec2((float)x, (float)y) + offset) / 8.f);
					}
				}
			}
			uint32_t k = 6 + (frames == 4 ? 2u : 0u);
			check(isNet(points, k), fmt::format("sobol: {} samples of every pixel are a (0, {}, 2)-net over the image", frames, k));
		}
	}

	// Halton: the first sample's index in every pixel of the image tiles [0, stride)
	{
		const SamplerParams params = makeSamplerParams(SamplerKind::Halton, SamplerRandomization::PermuteDigits, 2, 16, 9, 4, false);
		const uint32_t scale0 = 1u << params.haltonExponent[0];
		uint32_t scale1 = 1;
		for (uint32_t i = 0; i < params.haltonExponent[1]; i++) {
			scale1 *= 3;
		}
		check(scale0 == 16 && scale1 == 9, fmt::format("halton: base scales {} x {}", scale0, scale1));
		std::set<uint64_t> indices;
		for (uint32_t y = 0; y < scale1; y++) {
			for (uint32_t x = 0; x < scale0; x++) {
				SampleGenerator generator(params, tables, x, y, 0);
				indices.insert(generator.sequenceIndex());
			}
		}
		bool dense = indices.size() == scale0 * scale1 && *indices.rbegin() < scale0 * scale1;
		check(dense, "halton: one index per pixel, together exactly [0, stride)");
		// and the pixel offsets of that first sample are inside the pixel
		bool inside = true;
		for (uint32_t y = 0; y < scale1; y++) {
			for (uint32_t x = 0; x < scale0; x++) {
				SampleGenerator generator(params, tables, x, y, 3);
				const glm::vec2 p = generator.pixel2D();
				inside = inside && p.x >= 0.f && p.x < 1.f && p.y >= 0.f && p.y < 1.f;
			}
		}
		check(inside, "halton: pixel offsets in [0, 1)");
	}

	// stratified: one sample in each stratum, for several sample counts
	{
		bool allStrata = true;
		for (const uint32_t spp : { 1u, 4u, 12u, 16u, 36u, 64u }) {
			const SamplerParams params = makeSamplerParams(SamplerKind::Stratified, SamplerRandomization::Owen, 8, 32, 32, spp, false);
			const uint32_t strataY = spp / params.strataX;
			if (params.strataX * strataY != spp || params.strataX < strataY) {
				allStrata = false;
			}
			for (uint32_t pixel = 0; pixel < 6; pixel++) {
				std::set<uint32_t> strata;
				std::set<uint32_t> strata1D;
				for (uint32_t s = 0; s < spp; s++) {
					SampleGenerator generator(params, tables, pixel, 3, s);
					const glm::vec2 p = generator.get2D(2);
					strata.insert((uint32_t)(p.y * (float)strataY) * params.strataX + (uint32_t)(p.x * (float)params.strataX));
					strata1D.insert((uint32_t)(generator.get1D(6) * (float)spp));
				}
				allStrata = allStrata && strata.size() == spp && strata1D.size() == spp;
			}
		}
		check(allStrata, "stratified: every stratum, 2D and 1D, holds exactly one sample");
	}
}

//---------------------------------------------------------------- PMJ02BN

void testPmj02Bn(SamplerTables& tables)
{
	fmt::println("pmj02bn");
	// the five sets are (0, 2)-sequences: every power-of-two prefix is a net
	bool sets = true;
	for (uint32_t set = 0; set < PMJ02BN_SETS; set++) {
		for (uint32_t k = 0; k <= 12; k += 2) {
			std::vector<glm::vec2> points;
			for (uint32_t i = 0; i < (1u << k); i++) {
				points.push_back(samplerPmj02Bn(tables, set, i));
			}
			sets = sets && isNet(points, k);
		}
	}
	check(sets, "every set's first 4^k points are a (0, 2k, 2)-net");

	// a pixel's samples are its cell of set 0: a net of their own, for power-of-four counts
	for (const uint32_t spp : { 1u, 4u, 16u, 64u, 256u }) {
		preparePmj02BnPixelSamples(tables, spp);
		const SamplerParams params = makeSamplerParams(SamplerKind::Pmj02Bn, SamplerRandomization::Owen, 3, 300, 200, spp, false);
		uint32_t k = 0;
		while ((1u << k) < spp) {
			k++;
		}
		bool nets = true;
		for (uint32_t pixel = 0; pixel < 40; pixel++) {
			std::vector<glm::vec2> points;
			for (uint32_t s = 0; s < spp; s++) {
				SampleGenerator generator(params, tables, (pixel * 37) % 300, (pixel * 13) % 200, s);
				points.push_back(generator.pixel2D());
			}
			nets = nets && isNet(points, k);
		}
		check(nets, fmt::format("{} samples per pixel: each pixel's offsets are a (0, {}, 2)-net", spp, k));
	}

	// a 1D request puts one sample in each of the count's strata
	preparePmj02BnPixelSamples(tables, 36);
	const SamplerParams params = makeSamplerParams(SamplerKind::Pmj02Bn, SamplerRandomization::Owen, 3, 64, 64, 36, false);
	bool strata = true;
	for (uint32_t pixel = 0; pixel < 20; pixel++) {
		std::set<uint32_t> seen;
		for (uint32_t s = 0; s < 36; s++) {
			SampleGenerator generator(params, tables, pixel, 7, s);
			seen.insert((uint32_t)(generator.get1D(11) * 36.f));
		}
		strata = strata && seen.size() == 36;
	}
	check(strata, "a 1D request has one sample per stratum");
}

//---------------------------------------------------------------- discrepancy

void testDiscrepancy(SamplerTables& tables)
{
	fmt::println("discrepancy");
	const uint32_t n = 256;
	// independent points, the reference: the expected squared L2 star discrepancy is about
	// (1/N) (1/4 - 1/12 ... ) - measure it rather than quote it
	std::vector<glm::vec2> random;
	uint32_t state = 12345;
	for (uint32_t i = 0; i < n; i++) {
		const float x = samplerPcgFloat(state);
		random.push_back(glm::vec2(x, samplerPcgFloat(state)));
	}
	const double reference = std::sqrt(l2StarDiscrepancySquared(random));
	fmt::println("    independent: L2* = {:.5f}", reference);

	preparePmj02BnPixelSamples(tables, n);
	for (const SamplerKind kind : { SamplerKind::Stratified, SamplerKind::PaddedSobol, SamplerKind::ZSobol, SamplerKind::Pmj02Bn }) {
		const SamplerParams params = makeSamplerParams(kind, SamplerRandomization::Owen, 21, 64, 64, n, false);
		double worst = 0.0;
		for (uint32_t pixel = 0; pixel < 8; pixel++) {
			std::vector<glm::vec2> points;
			for (uint32_t s = 0; s < n; s++) {
				SampleGenerator generator(params, tables, pixel * 5, pixel * 3, s);
				points.push_back(generator.get2D(8));
			}
			worst = std::max(worst, std::sqrt(std::max(l2StarDiscrepancySquared(points), 0.0)));
		}
		fmt::println("    {:<14} L2* = {:.5f} ({:.0f}% of independent)", kindName(kind), worst, 100.0 * worst / reference);
		check(worst < 0.4 * reference, fmt::format("{}: L2* discrepancy well below independent sampling's", kindName(kind)));
	}
}

//---------------------------------------------------------------- blue noise

// the share of an image's energy at the lowest spatial frequencies, from a direct DFT of the
// frequencies |k| <= 3 against all |k| <= 15
double lowFrequencyShare(const std::vector<double>& image, uint32_t size)
{
	double low = 0.0;
	double total = 0.0;
	for (int ky = -15; ky <= 15; ky++) {
		for (int kx = -15; kx <= 15; kx++) {
			if (kx == 0 && ky == 0) {
				continue;
			}
			double re = 0.0;
			double im = 0.0;
			for (uint32_t y = 0; y < size; y++) {
				for (uint32_t x = 0; x < size; x++) {
					const double angle = 2.0 * 3.14159265358979323846 * ((double)kx * x + (double)ky * y) / (double)size;
					re += image[y * size + x] * std::cos(angle);
					im -= image[y * size + x] * std::sin(angle);
				}
			}
			const double power = re * re + im * im;
			total += power;
			if (kx * kx + ky * ky <= 9) {
				low += power;
			}
		}
	}
	return low / total;
}

void testBlueNoise(const SamplerTables& tables)
{
	fmt::println("blue-noise error");
	// the error of a one-sample estimate of a smooth function's integral, per pixel
	const uint32_t size = 64;
	auto errorImage = [&](SamplerKind kind) {
		const SamplerParams params = makeSamplerParams(kind, SamplerRandomization::Owen, 4, size, size, 1, false);
		std::vector<double> image(size * size);
		double mean = 0.0;
		for (uint32_t y = 0; y < size; y++) {
			for (uint32_t x = 0; x < size; x++) {
				SampleGenerator generator(params, tables, x, y, 0);
				const glm::vec2 u = generator.get2D(2);
				// the integral over the unit square is 1/2
				const double f = std::sin(2.0 * 3.14159265358979323846 * u.x * 0.8) * std::cos(2.0 * 3.14159265358979323846 * u.y * 0.6) + 0.5 + u.x * u.y * 0.3;
				image[y * size + x] = f;
				mean += f;
			}
		}
		mean /= (double)(size * size);
		for (double& value : image) {
			value -= mean;
		}
		return image;
	};
	const double padded = lowFrequencyShare(errorImage(SamplerKind::PaddedSobol), size);
	const double zsobol = lowFrequencyShare(errorImage(SamplerKind::ZSobol), size);
	fmt::println("    share of error power at |k| <= 3: padded sobol {:.4f}, zsobol {:.4f}", padded, zsobol);
	check(zsobol < 0.6 * padded, "zsobol's one-sample error has less low-frequency energy than padded Sobol's");
}

//---------------------------------------------------------------- parameters

void testParams()
{
	fmt::println("parameters");
	bool roundTrip = true;
	for (const uint32_t width : { 2u, 100u, 512u, 1280u, 1920u, 4096u }) {
		for (const uint32_t height : { 2u, 77u, 720u, 1080u }) {
			for (const uint32_t samples : { 1u, 7u, 64u, 1000u }) {
				const SamplerParams params = makeSamplerParams(SamplerKind::ZSobol, SamplerRandomization::FastOwen, 99, width, height, samples, false);
				SamplerParams unpacked = unpackSamplerParams(params.kind, packSamplerParams(params), params.seed, params.samplesPerPixel);
				roundTrip = roundTrip && unpacked == params;
			}
		}
	}
	check(roundTrip, "packing the parameters loses nothing");

	const SamplerParams hd = makeSamplerParams(SamplerKind::Halton, SamplerRandomization::Owen, 1, 1920, 1080, 16, false);
	check(hd.resolutionLog2 == 11, "1920 pixels round up to 2^11");
	check(hd.haltonExponent[0] == 7 && hd.haltonExponent[1] == 5, "halton scales cover 128 x 243");
	check(hd.blockLog2 == 4, "16 samples: a block of 2^4");
	const SamplerParams unlimited = makeSamplerParams(SamplerKind::ZSobol, SamplerRandomization::Owen, 1, 1920, 1080, 16, true);
	check(unlimited.blockLog2 == 6, "unlimited: a block of 2^6");
	const SamplerParams thousand = makeSamplerParams(SamplerKind::ZSobol, SamplerRandomization::Owen, 1, 640, 360, 1000, false);
	check(thousand.blockLog2 == 10, "1000 samples: a block of 2^10");
}

}

int main(int argc, char** argv)
{
	std::filesystem::path directory = "assets/sampler";
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--tables") == 0 && i + 1 < argc) {
			directory = argv[++i];
		}
	}
	SamplerTables tables;
	std::string error;
	if (!loadSamplerTables(directory, tables, error)) {
		fmt::println("cannot load the sampler tables from {}: {}", directory.string(), error);
		return 2;
	}

	testTables(tables);
	testBasics(tables);
	testNets(tables);
	testGlobalSamplers(tables);
	testPmj02Bn(tables);
	testDiscrepancy(tables);
	testBlueNoise(tables);
	testParams();

	if (g_failures == 0) {
		fmt::println("sampler_test: all checks passed");
		return 0;
	}
	fmt::println("sampler_test: {} check(s) FAILED", g_failures);
	return 1;
}
