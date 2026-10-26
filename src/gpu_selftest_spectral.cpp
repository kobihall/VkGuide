// The spectral rendering's GPU self-tests (docs/plans/completed/pbrt-spectral-rendering.md):
//  - BxDF parity: shaders/test/bxdf_parity.comp runs pbrt-v4's glass and conductor (crt_bxdf.glsl) on random local-frame
//    inputs, against src/bxdf.cpp
//  - spectrum parity: shaders/test/spectrum_parity.comp runs crt_spectrum.glsl's four-lane build over SpectralData built
//    by src/spectral.cpp, in every representation the four-lane build carries, against the same file's mirror: the
//    wavelengths a sample gives, packed references at them, tinted and not, a scalar reference, and the film's color

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include <fmt/format.h>

#include <bxdf.h>
#include <color_space.h>
#include <gpu_selftest.h>
#include <spectral.h>
#include <vk_compute.h>
#include <vk_engine.h>

namespace {

double relativeDifference(double a, double b, double floor)
{
	return std::abs(a - b) / std::max({ std::abs(a), std::abs(b), floor });
}

// a case buffer up, a result buffer down, and the pass run once over them
template <typename Case, typename Result>
std::vector<Result> runBatch(VulkanEngine* engine, const ComputePass& pass, const std::vector<Case>& cases, const std::vector<std::pair<uint32_t, const AllocatedBuffer*>>& extra)
{
	const VkDeviceSize caseBytes = sizeof(Case) * cases.size();
	const VkDeviceSize resultBytes = sizeof(Result) * cases.size();
	const AllocatedBuffer caseBuffer = engine->createBuffer(caseBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	const AllocatedBuffer resultBuffer = engine->createBuffer(resultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	std::memcpy(caseBuffer.info.pMappedData, cases.data(), caseBytes);
	vmaFlushAllocation(engine->m_memAllocator, caseBuffer.allocation, 0, caseBytes);
	std::memset(resultBuffer.info.pMappedData, 0, resultBytes);

	const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, pass.setLayout);
	DescriptorWriter writer;
	writer.writeBuffer(90, caseBuffer.buffer, caseBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(91, resultBuffer.buffer, resultBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	for (const auto& [binding, buffer] : extra) {
		writer.writeBuffer((int)binding, buffer->buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	}
	writer.updateSet(engine->m_device, set);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		dispatchComputePassOver(cmd, pass, set, nullptr, { (uint32_t)cases.size(), 1, 1 });
	});
	vmaInvalidateAllocation(engine->m_memAllocator, resultBuffer.allocation, 0, resultBytes);
	std::vector<Result> results(cases.size());
	std::memcpy(results.data(), resultBuffer.info.pMappedData, resultBytes);
	engine->destroyBuffer(caseBuffer);
	engine->destroyBuffer(resultBuffer);
	return results;
}

//---------------------------------------------------------------- BxDFs

// shaders/test/bxdf_parity.comp BxdfParityCase
struct BxdfParityCase {
	glm::uvec4 ids;
	glm::vec4 params;
	glm::vec4 conductorEta;
	glm::vec4 conductorK;
	glm::vec4 wo;
	glm::vec4 wi;
	glm::vec4 u;
	glm::vec4 pad;
};
static_assert(sizeof(BxdfParityCase) == 128);

struct BxdfParityResult {
	glm::vec4 sampleF;
	glm::vec4 sampleWi;
	glm::vec4 evalPdf;
	glm::uvec4 flags;
	glm::vec4 fresnel;
};
static_assert(sizeof(BxdfParityResult) == 80);

glm::vec3 randomUnit(std::mt19937& rng)
{
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	const float z = 1.f - 2.f * unit(rng);
	const float r = std::sqrt(std::max(0.f, 1.f - z * z));
	const float phi = 6.2831853f * unit(rng);
	return glm::vec3(r * std::cos(phi), r * std::sin(phi), z);
}

//---------------------------------------------------------------- spectra

struct SpectrumParityCase {
	glm::uvec4 ids;
	glm::vec4 tint;
};
static_assert(sizeof(SpectrumParityCase) == 32);

struct SpectrumParityResult {
	glm::vec4 lambda;
	glm::vec4 pdf;
	glm::vec4 untinted;
	glm::vec4 tinted;
	glm::vec4 film;
	glm::vec4 pad;
};
static_assert(sizeof(SpectrumParityResult) == 96);

}

SelfTestResult runBxdfParityTest(VulkanEngine* engine)
{
	SelfTestResult result;
	std::mt19937 rng(20261023u);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	struct Material {
		std::string name;
		uint32_t type;
		glm::vec2 alpha;
		float eta;
		glm::vec3 conductorEta;
		glm::vec3 conductorK;
	};
	std::vector<Material> materials;
	for (const float eta : { 1.5f, 1.33f, 2.4f }) {
		for (const glm::vec2 alpha : { glm::vec2(0.f), glm::vec2(0.1f), glm::vec2(0.5f), glm::vec2(0.15f, 0.6f) }) {
			materials.push_back({ fmt::format("glass {} alpha ({}, {})", eta, alpha.x, alpha.y), 6u, alpha, eta, glm::vec3(1.f), glm::vec3(0.f) });
		}
	}
	for (const glm::vec2 alpha : { glm::vec2(0.f), glm::vec2(0.3f), glm::vec2(0.2f, 0.7f) }) {
		materials.push_back({ fmt::format("conductor alpha ({}, {})", alpha.x, alpha.y), 7u, alpha, 1.f, glm::vec3(0.27f, 0.68f, 1.22f), glm::vec3(3.6f, 2.6f, 2.3f) });
	}
	constexpr size_t PER_MATERIAL = 4096;
	std::vector<BxdfParityCase> cases;
	std::vector<size_t> materialOf;
	for (size_t m = 0; m < materials.size(); m++) {
		for (size_t i = 0; i < PER_MATERIAL; i++) {
			BxdfParityCase c {};
			const Material& mat = materials[m];
			c.ids = glm::uvec4(mat.type, unit(rng) < 0.5f ? 0u : 1u, 0u, 0u);
			c.params = glm::vec4(mat.alpha, mat.eta, unit(rng));
			c.conductorEta = glm::vec4(mat.conductorEta, 0.f);
			c.conductorK = glm::vec4(mat.conductorK, 0.f);
			glm::vec3 wo = randomUnit(rng);
			if (mat.type == 7u) {
				wo.z = std::abs(wo.z);
			}
			c.wo = glm::vec4(wo, 0.f);
			c.wi = glm::vec4(randomUnit(rng), 0.f);
			c.u = glm::vec4(unit(rng), unit(rng), 0.f, 0.f);
			cases.push_back(c);
			materialOf.push_back(m);
		}
	}
	ComputePass pass = ComputePassBuilder(engine->m_rootPath + "shaders/test/bxdf_parity.comp.spv")
		.addBinding(90, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding(91, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.setWorkgroupSize(64, 1, 1)
		.build(engine->m_device);
	const std::vector<BxdfParityResult> gpu = runBatch<BxdfParityCase, BxdfParityResult>(engine, pass, cases, {});
	pass.destroy(engine->m_device);

	//the GPU's sqrt, atan and divisions are not the C++ library's: the tolerances are the ones the BSDF parity test
	//uses, and a decision within rounding of its threshold may flip
	constexpr double TOLERANCE = 1e-3;
	constexpr double TOLERANCE_DIRECTION = 1e-4;
	std::vector<size_t> bad(materials.size(), 0), flips(materials.size(), 0);
	double worstValue = 0.0, worstDirection = 0.0, worstFresnel = 0.0;
	for (size_t i = 0; i < cases.size(); i++) {
		const BxdfParityCase& c = cases[i];
		const BxdfParityResult& g = gpu[i];
		const Material& mat = materials[materialOf[i]];
		const TransportMode mode = (TransportMode)c.ids.y;
		const TrowbridgeReitz distribution(mat.alpha.x, mat.alpha.y);
		BxdfSample s;
		bool ok;
		glm::vec3 f;
		float p;
		uint32_t flags;
		if (mat.type == 7u) {
			ConductorBxdf b;
			b.distribution = distribution;
			b.eta = mat.conductorEta;
			b.k = mat.conductorK;
			ok = b.sample(glm::vec3(c.wo), c.params.w, glm::vec2(c.u), mode, s);
			f = b.f(glm::vec3(c.wo), glm::vec3(c.wi), mode);
			p = b.pdf(glm::vec3(c.wo), glm::vec3(c.wi), mode);
			flags = b.flags();
		} else {
			DielectricBxdf b;
			b.eta = mat.eta;
			b.distribution = distribution;
			ok = b.sample(glm::vec3(c.wo), c.params.w, glm::vec2(c.u), mode, s);
			f = b.f(glm::vec3(c.wo), glm::vec3(c.wi), mode);
			p = b.pdf(glm::vec3(c.wo), glm::vec3(c.wi), mode);
			flags = b.flags();
		}
		worstFresnel = std::max({ worstFresnel, relativeDifference(frDielectric(c.wo.z, c.params.z), g.fresnel.x, 1e-3),
			relativeDifference(frComplex(std::abs(c.wo.z), c.conductorEta.x, c.conductorK.x), g.fresnel.y, 1e-3) });
		if (ok != (g.sampleF.w > 0.5f) || (ok && s.flags != g.flags.x)) {
			flips[materialOf[i]]++;
			continue;
		}
		bool wrong = flags != g.flags.z;
		if (ok) {
			double value = relativeDifference(s.pdf, g.sampleWi.w, 1e-3);
			for (int k = 0; k < 3; k++) {
				value = std::max(value, relativeDifference(s.f[k], g.sampleF[k], 1e-3));
				const double d = relativeDifference(s.wi[k], g.sampleWi[k], 1.0);
				worstDirection = std::max(worstDirection, d);
				wrong |= d > TOLERANCE_DIRECTION;
			}
			wrong |= std::bit_cast<uint32_t>(s.eta) != g.flags.y && relativeDifference(s.eta, std::bit_cast<float>(g.flags.y), 1.0) > 1e-6;
			worstValue = std::max(worstValue, value);
			wrong |= value > TOLERANCE;
		}
		double value = relativeDifference(p, g.evalPdf.w, 1e-3);
		for (int k = 0; k < 3; k++) {
			value = std::max(value, relativeDifference(f[k], g.evalPdf[k], 1e-3));
		}
		worstValue = std::max(worstValue, value);
		wrong |= value > TOLERANCE;
		if (wrong) {
			bad[materialOf[i]]++;
		}
	}
	size_t totalBad = 0, totalFlips = 0;
	for (size_t m = 0; m < materials.size(); m++) {
		totalBad += bad[m];
		totalFlips += flips[m];
		if (bad[m] > PER_MATERIAL / 2000 || flips[m] > PER_MATERIAL / 500) {
			result.lines.push_back(fmt::format("{}: {} of {} past the tolerance, {} decision(s) flipped", materials[m].name, bad[m], PER_MATERIAL, flips[m]));
		}
	}
	//a rough glass's transmission divides by (wi.wm + wo.wm / eta)^2, which cancels where the two directions are
	//nearly opposite across the microfacet: there float rounding on either side is amplified, and a case in a few
	//thousand lands past the tolerance (4 of 61440 when this was written, at most 0.8 percent off)
	result.passed = totalBad <= cases.size() / 2000 && totalFlips <= cases.size() / 500 && worstFresnel < 1e-4;
	result.lines.insert(result.lines.begin(), fmt::format("{} cases, {} past the tolerance, {} decision(s) flipped; worst f and pdf {:.2g} (limit {:g}), direction {:.2g} ({:g}), Fresnel {:.2g} (1e-4)",
												 cases.size(), totalBad, totalFlips, worstValue, TOLERANCE, worstDirection, TOLERANCE_DIRECTION, worstFresnel));
	result.lines.insert(result.lines.begin(), result.passed ? "the shader and the mirror agree" : "the shader and the mirror disagree");
	return result;
}

SelfTestResult runSpectrumParityTest(VulkanEngine* engine)
{
	SelfTestResult result;
	std::string error;
	const UpliftTable* table = upliftTable(engine->m_rootPath, ColorSpaceId::Srgb, &error);
	if (table == nullptr) {
		result.lines.push_back("no sRGB uplift table: " + error);
		return result;
	}
	const SpectrumContext context { table, ColorSpaceId::Srgb };

	//the references: colors in each role, every kind, and scalars
	SceneSpectra spectra;
	std::vector<uint32_t> colorRefs, scalarRefs;
	const double bk7[] = { 1.03961212, 0.00600069867, 0.231792344, 0.0200179144, 1.01046945, 103.560653 };
	colorRefs.push_back(spectra.add(rgbSpectrum(glm::vec3(0.8f, 0.3f, 0.1f)), SpectrumRole::Albedo));
	colorRefs.push_back(spectra.add(rgbSpectrum(glm::vec3(0.1f, 0.5f, 0.9f)), SpectrumRole::Albedo));
	colorRefs.push_back(spectra.add(rgbSpectrum(glm::vec3(4.f, 2.f, 1.f)), SpectrumRole::Illuminant));
	colorRefs.push_back(spectra.add(rgbSpectrum(glm::vec3(0.5f)), SpectrumRole::Illuminant));
	colorRefs.push_back(spectra.add(blackbodyRef(2700.f, 2.f), SpectrumRole::Illuminant));
	colorRefs.push_back(spectra.add(*namedRef("stdillum-F11"), SpectrumRole::Illuminant));
	colorRefs.push_back(spectra.add(lineRef(532.f, 8.f, LineProfile::Gaussian, 30.f), SpectrumRole::Illuminant));
	colorRefs.push_back(spectra.add(constantRef(0.6f), SpectrumRole::Albedo));
	scalarRefs.push_back(spectra.add(dispersionRef(DispersionFormula::Sellmeier1, bk7), SpectrumRole::Scalar));
	scalarRefs.push_back(spectra.add(*namedRef("metal-Au-k"), SpectrumRole::Scalar));
	scalarRefs.push_back(spectra.add(constantRef(1.5f), SpectrumRole::Scalar));

	const AllocatedBuffer upliftBuffer = engine->createBuffer(sizeof(float) * (table->zNodes.size() + table->data.size()), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	std::copy(table->zNodes.begin(), table->zNodes.end(), (float*)upliftBuffer.info.pMappedData);
	std::copy(table->data.begin(), table->data.end(), (float*)upliftBuffer.info.pMappedData + table->zNodes.size());
	vmaFlushAllocation(engine->m_memAllocator, upliftBuffer.allocation, 0, VK_WHOLE_SIZE);

	struct Configuration {
		const char* name;
		SpectralSettings settings;
	};
	std::vector<Configuration> configurations;
	{
		SpectralSettings s;
		s.representation = SpectralRepresentation::HeroWavelengths;
		configurations.push_back({ "hero, visible", s });
		s.sampling = WavelengthSampling::Uniform;
		s.lambdaMin = 380.f;
		s.lambdaMax = 1100.f;
		configurations.push_back({ "hero, uniform 380 to 1100 nm", s });
		s = SpectralSettings {};
		s.representation = SpectralRepresentation::HeroWavelengths;
		s.sampling = WavelengthSampling::Lights;
		configurations.push_back({ "hero, the emitters' spectra", s });
		s = SpectralSettings {};
		s.representation = SpectralRepresentation::RgbGaussians;
		configurations.push_back({ "RGB Gaussians", s });
		s.representation = SpectralRepresentation::SpectralBands;
		configurations.push_back({ "4 box bands", s });
		s.bandBasis = BandBasis::Gaussian;
		configurations.push_back({ "4 Gaussian bands", s });
	}
	const std::vector<EmitterSpectrum> emitters = { { blackbodyRef(2700.f), 1.f }, { lineRef(532.f, 8.f, LineProfile::Gaussian, 30.f), 0.5f } };

	std::mt19937 rng(7u);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	bool passed = true;
	for (const Configuration& configuration : configurations) {
		const SpectralSettings& settings = configuration.settings;
		const WavelengthDistribution distribution = buildWavelengthDistribution(emitters, settings, context);
		SpectralDataBuilder builder(settings, context, distribution, 0);
		builder.setScene(spectra, {});
		const std::vector<uint32_t> words = builder.finish();
		const std::vector<Band>& bands = builder.bands();
		const AllocatedBuffer dataBuffer = engine->createBuffer(sizeof(uint32_t) * words.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		std::copy(words.begin(), words.end(), (uint32_t*)dataBuffer.info.pMappedData);
		vmaFlushAllocation(engine->m_memAllocator, dataBuffer.allocation, 0, VK_WHOLE_SIZE);

		std::vector<SpectrumParityCase> cases;
		for (int i = 0; i < 4096; i++) {
			SpectrumParityCase c {};
			c.ids = glm::uvec4(colorRefs[rng() % colorRefs.size()], scalarRefs[rng() % scalarRefs.size()], std::bit_cast<uint32_t>(unit(rng)), 0u);
			c.tint = i % 2 == 0 ? glm::vec4(1.f) : glm::vec4(unit(rng), unit(rng), unit(rng), 0.f);
			cases.push_back(c);
		}
		ComputePass pass = ComputePassBuilder(engine->m_rootPath + "shaders/test/spectrum_parity.comp.spv")
			.addBinding(24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addBinding(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addBinding(90, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addBinding(91, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.setWorkgroupSize(64, 1, 1)
			.addSpecialization(3, (uint32_t)settings.representation)
			.addSpecialization(4, (uint32_t)settings.sampling)
			.build(engine->m_device);
		const std::vector<SpectrumParityResult> gpu = runBatch<SpectrumParityCase, SpectrumParityResult>(engine, pass, cases, { { 24, &dataBuffer }, { 25, &upliftBuffer } });
		pass.destroy(engine->m_device);
		engine->destroyBuffer(dataBuffer);

		//the packed words of a reference, as the CPU mirror evaluates them
		auto packedOf = [&](uint32_t index) {
			const auto& [ref, role] = spectra.list()[index];
			return packSpectrumRef(ref, role, settings, context, bands);
		};
		double worstLambda = 0.0, worstPdf = 0.0, worstValue = 0.0, worstTinted = 0.0, worstFilm = 0.0, worstScalar = 0.0;
		size_t bad = 0;
		const uint32_t lanes = activeLanes(settings);
		for (size_t i = 0; i < cases.size(); i++) {
			const SpectrumParityCase& c = cases[i];
			const SpectrumParityResult& g = gpu[i];
			const SampledWavelengths w = sampleWavelengths(std::bit_cast<float>(c.ids.z), settings, distribution);
			const std::vector<uint32_t> packed = packedOf(c.ids.x);
			float values[4] = { 0.f, 0.f, 0.f, 0.f };
			bool wrong = false;
			for (uint32_t lane = 0; lane < lanes; lane++) {
				const double dl = std::abs(w.lambda[lane] - g.lambda[lane]);
				const double dp = relativeDifference(w.pdf[lane], g.pdf[lane], 1e-6);
				worstLambda = std::max(worstLambda, dl);
				worstPdf = std::max(worstPdf, dp);
				values[lane] = evalPackedLane(packed, lane, w.lambda[lane], settings, context, bands);
				const float tinted = evalPackedLane(packed, lane, w.lambda[lane], settings, context, bands, glm::vec3(c.tint));
				const double dv = relativeDifference(values[lane], g.untinted[lane], 1e-3);
				const double dt = relativeDifference(tinted, g.tinted[lane], 1e-3);
				worstValue = std::max(worstValue, dv);
				worstTinted = std::max(worstTinted, dt);
				//a wavelength to a few parts in a million: the GPU's tan and atan, near an emission line's center
				wrong |= dl > 1e-5 * w.lambda[lane] || dp > 1e-3 || dv > 2e-3 || dt > 2e-3;
			}
			const glm::vec3 film = lanesToRgb(std::span(values, lanes), w, settings, bands);
			for (int k = 0; k < 3; k++) {
				const double df = relativeDifference(film[k], g.film[k], 1e-2);
				worstFilm = std::max(worstFilm, df);
				wrong |= df > 2e-3;
			}
			const double ds = relativeDifference(evalPackedLane(packedOf(c.ids.y), 0, w.lambda[0], settings, context, bands), g.film.w, 1e-3);
			worstScalar = std::max(worstScalar, ds);
			wrong |= ds > 1e-4;
			bad += wrong;
		}
		const bool ok = bad == 0;
		passed &= ok;
		result.lines.push_back(fmt::format("{}: {} of {} past the tolerance; worst wavelength {:.2g} nm, pdf {:.2g}, value {:.2g}, tinted {:.2g}, film {:.2g}, scalar {:.2g}", configuration.name, bad,
			cases.size(), worstLambda, worstPdf, worstValue, worstTinted, worstFilm, worstScalar));
	}
	engine->destroyBuffer(upliftBuffer);
	result.passed = passed;
	result.lines.insert(result.lines.begin(), passed ? "the shader and the mirror agree" : "the shader and the mirror disagree");
	return result;
}
