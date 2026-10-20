// The BSDF parity self-test: shaders/test/bsdf_parity.comp runs scatterSurface(), evalSurface() and
// pdfSurface() of crt_scatter.glsl on a fixed batch, and this compares the results with the C++
// mirror in src/bsdf.cpp. A batch is every material of tests/bsdf_test.cpp over random surface
// normals, incident directions, evaluated directions and random numbers, in both ways kernel 06
// draws them: from the path's PCG stream and from a sample record.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include <fmt/format.h>

#include <bsdf.h>
#include <gpu_selftest.h>
#include <vk_compute.h>
#include <vk_engine.h>

namespace {

// shaders/test/bsdf_parity.comp ParityCase
struct ParityCase {
	glm::vec4 albedoParam;
	glm::vec4 inDir;
	glm::vec4 normal;
	glm::vec4 wi;
	glm::vec4 materialValues;
	glm::uvec4 ids;
	glm::vec4 fieldsA;
	glm::vec4 fieldsB;
};
static_assert(sizeof(ParityCase) == 128);

// shaders/test/bsdf_parity.comp ParityResult
struct ParityResult {
	glm::vec4 attenuationKept;
	glm::vec4 outDir;
	glm::vec4 evalPdf;
	glm::uvec4 flags;
};
static_assert(sizeof(ParityResult) == 64);

// the RaySamples record of crt_common.glsl: direct, indirect and medium, 48 bytes
struct RaySamplesRecord {
	glm::vec4 direct;
	glm::vec4 indirect;
	glm::vec4 medium;
};
static_assert(sizeof(RaySamplesRecord) == 48);

struct NamedMaterial {
	std::string name;
	BsdfMaterial material;
};

std::vector<NamedMaterial> parityMaterials()
{
	auto make = [](BsdfType type, float param, float metallic = 0.f, float roughness = 0.5f) {
		BsdfMaterial m;
		m.type = type;
		m.albedo = glm::vec3(0.9f, 0.6f, 0.3f);
		m.param = param;
		m.metallic = metallic;
		m.roughness = roughness;
		return m;
	};
	std::vector<NamedMaterial> list;
	list.push_back({ "lambertian", make(BsdfType::Lambertian, 0.f) });
	for (float smoothness : { 0.2f, 0.6f, 1.0f }) {
		list.push_back({ fmt::format("phong {:g}", smoothness), make(BsdfType::Phong, smoothness) });
	}
	for (float fuzz : { 0.f, 0.05f, 0.3f, 1.0f }) {
		list.push_back({ fmt::format("metal fuzz {:g}", fuzz), make(BsdfType::Metal, fuzz) });
	}
	for (float ior : { 1.5f, 1.33f }) {
		list.push_back({ fmt::format("dielectric {:g}", ior), make(BsdfType::Dielectric, ior) });
	}
	for (float roughness : { 0.2f, 0.6f, 1.0f }) {
		for (float metallic : { 0.f, 0.5f, 1.f }) {
			list.push_back({ fmt::format("pbr rough {:g} metal {:g}", roughness, metallic), make(BsdfType::Pbr, 0.f, metallic, roughness) });
		}
	}
	return list;
}

glm::vec3 randomUnitVector(std::mt19937& rng)
{
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	const float z = 1.f - 2.f * unit(rng);
	const float r = std::sqrt(std::max(0.f, 1.f - z * z));
	const float phi = 2.f * BSDF_PI * unit(rng);
	return glm::vec3(r * std::cos(phi), r * std::sin(phi), z);
}

// a unit vector on the side of `normal` it points to, at least `minCosine` above the surface
glm::vec3 randomAbove(std::mt19937& rng, const glm::vec3& normal, float minCosine)
{
	for (;;) {
		glm::vec3 v = randomUnitVector(rng);
		if (glm::dot(v, normal) < 0.f) {
			v = -v;
		}
		if (glm::dot(v, normal) >= minCosine) {
			return v;
		}
	}
}

ParityCase makeCase(std::mt19937& rng, const BsdfMaterial& material)
{
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	const glm::vec3 normal = randomUnitVector(rng);
	const glm::vec3 inDir = -randomAbove(rng, normal, 0.02f);
	// 80 percent of the evaluated directions are above the surface
	const glm::vec3 wi = unit(rng) < 0.8f ? randomAbove(rng, normal, 0.f) : -randomAbove(rng, normal, 0.f);

	ParityCase c {};
	c.albedoParam = glm::vec4(material.albedo, material.param);
	c.inDir = glm::vec4(inDir, 0.f);
	c.normal = glm::vec4(normal, 0.f);
	c.wi = glm::vec4(wi, 0.f);
	c.materialValues = glm::vec4(material.metallic, material.roughness, unit(rng) < 0.5f ? 1.f : 0.f, 0.f);
	c.ids = glm::uvec4((uint32_t)material.type, (uint32_t)rng(), 0u, 0u);
	for (int k = 0; k < 4; k++) {
		c.fieldsA[k] = unit(rng);
	}
	for (int k = 0; k < 4; k++) {
		c.fieldsB[k] = unit(rng);
	}
	return c;
}

struct Comparison {
	size_t cases { 0 };
	// the kept flag or the number of draws differs: a decision that rounding flipped
	size_t flips { 0 };
	// the values past the tolerance, among the cases that did not flip
	size_t outOfTolerance { 0 };
	double maxAttenuation { 0.0 };
	double maxDirection { 0.0 };
	double maxEval { 0.0 };
	double maxPdf { 0.0 };
};

// The difference of two values relative to the larger of their magnitudes and `floor`, so that two
// numbers far below the quantity's scale compare equal. A direction's components are at most 1 and
// a density can be anything
double relativeDifference(double a, double b, double floor)
{
	return std::abs(a - b) / std::max({ std::abs(a), std::abs(b), floor });
}

}  // namespace

SelfTestResult runBsdfParityTest(VulkanEngine* engine)
{
	SelfTestResult result;
	const std::vector<NamedMaterial> materials = parityMaterials();
	constexpr size_t CASES_PER_MATERIAL = 2048;

	std::mt19937 rng(20261020u);
	std::vector<ParityCase> cases;
	std::vector<size_t> materialOf;
	for (size_t m = 0; m < materials.size(); m++) {
		for (size_t i = 0; i < CASES_PER_MATERIAL; i++) {
			cases.push_back(makeCase(rng, materials[m].material));
			materialOf.push_back(m);
		}
	}
	std::vector<RaySamplesRecord> records(cases.size());
	for (size_t i = 0; i < cases.size(); i++) {
		records[i].direct = cases[i].fieldsA;
		records[i].indirect = cases[i].fieldsB;
		records[i].medium = glm::vec4(0.f);
	}

	const VkDeviceSize caseBytes = sizeof(ParityCase) * cases.size();
	const VkDeviceSize resultBytes = sizeof(ParityResult) * cases.size();
	const VkDeviceSize recordBytes = sizeof(RaySamplesRecord) * cases.size();
	const AllocatedBuffer caseBuffer = engine->createBuffer(caseBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	const AllocatedBuffer recordBuffer = engine->createBuffer(recordBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	const AllocatedBuffer resultBuffer = engine->createBuffer(resultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	std::memcpy(caseBuffer.info.pMappedData, cases.data(), caseBytes);
	vmaFlushAllocation(engine->m_memAllocator, caseBuffer.allocation, 0, caseBytes);
	std::memcpy(recordBuffer.info.pMappedData, records.data(), recordBytes);
	vmaFlushAllocation(engine->m_memAllocator, recordBuffer.allocation, 0, recordBytes);

	// The tolerances. The GPU's sin, cos, pow and sqrt are not the C++ library's, so values agree to
	// a few ulps at best and to the conditioning of the formula at worst: a GGX density at
	// roughness 0.2 divides by (cos^2 (a^2 - 1) + 1)^2, which cancels near the pole, and a
	// phong direction at smoothness 1 takes sqrt(1 - cos^2) of a cosine within 1e-3 of 1. What
	// the tolerances must catch is an error that changes an image, which is orders larger
	constexpr double TOLERANCE_DIRECTION = 1e-4;
	constexpr double TOLERANCE_ATTENUATION = 1e-3;
	constexpr double TOLERANCE_DENSITY = 1e-3;
	// a decision (a lobe, reflect or refract, a sample kept) that sits within rounding of its
	// threshold can come out differently; that is not a disagreement, and is bounded
	constexpr double MAX_FLIP_FRACTION = 1e-3;

	bool passed = true;
	for (const bool record : { false, true }) {
		ComputePass pass = ComputePassBuilder(engine->m_rootPath + "shaders/test/bsdf_parity.comp.spv")
			.addBinding(23, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addBinding(90, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addBinding(91, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.setWorkgroupSize(64, 1, 1)
			.addSpecialization(0, record ? 1u : 0u)
			.build(engine->m_device);

		const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, pass.setLayout);
		DescriptorWriter writer;
		writer.writeBuffer(23, recordBuffer.buffer, recordBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.writeBuffer(90, caseBuffer.buffer, caseBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.writeBuffer(91, resultBuffer.buffer, resultBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.updateSet(engine->m_device, set);

		std::memset(resultBuffer.info.pMappedData, 0, resultBytes);
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			dispatchComputePassOver(cmd, pass, set, nullptr, { (uint32_t)cases.size(), 1, 1 });
		});
		vmaInvalidateAllocation(engine->m_memAllocator, resultBuffer.allocation, 0, resultBytes);
		const ParityResult* gpu = (const ParityResult*)resultBuffer.info.pMappedData;

		std::vector<Comparison> byMaterial(materials.size());
		for (size_t i = 0; i < cases.size(); i++) {
			const ParityCase& c = cases[i];
			const BsdfMaterial& material = materials[materialOf[i]].material;
			Comparison& cmp = byMaterial[materialOf[i]];
			cmp.cases++;

			BsdfDraws draws;
			draws.rng = c.ids.y;
			draws.record = record;
			for (int k = 0; k < 8; k++) {
				draws.fields[k] = k < 4 ? c.fieldsA[k] : c.fieldsB[k - 4];
			}
			const glm::vec3 inDir(c.inDir);
			const glm::vec3 normal(c.normal);
			glm::vec3 attenuation(0.f);
			glm::vec3 outDir(0.f);
			const bool kept = scatterSurface(material, inDir, normal, c.materialValues.z > 0.5f, draws, attenuation, outDir);

			const ParityResult& g = gpu[i];
			if (kept != (g.attenuationKept.w > 0.5f) || draws.rng != g.flags.y) {
				cmp.flips++;
				continue;
			}
			bool bad = false;
			if (kept) {
				double worst = 0.0;
				for (int k = 0; k < 3; k++) {
					worst = std::max(worst, relativeDifference(attenuation[k], g.attenuationKept[k], 0.1));
				}
				cmp.maxAttenuation = std::max(cmp.maxAttenuation, worst);
				bad |= worst > TOLERANCE_ATTENUATION;
				worst = 0.0;
				for (int k = 0; k < 3; k++) {
					worst = std::max(worst, relativeDifference(outDir[k], g.outDir[k], 1.0));
				}
				cmp.maxDirection = std::max(cmp.maxDirection, worst);
				bad |= worst > TOLERANCE_DIRECTION;
			}
			const glm::vec3 wi(c.wi);
			const glm::vec3 f = evalSurface(material, inDir, normal, wi);
			const float p = pdfSurface(material, inDir, normal, wi);
			double worstEval = 0.0;
			for (int k = 0; k < 3; k++) {
				worstEval = std::max(worstEval, relativeDifference(f[k], g.evalPdf[k], 1e-3));
			}
			const double worstPdf = relativeDifference(p, g.evalPdf.w, 1e-3);
			cmp.maxEval = std::max(cmp.maxEval, worstEval);
			cmp.maxPdf = std::max(cmp.maxPdf, worstPdf);
			bad |= worstEval > TOLERANCE_DENSITY || worstPdf > TOLERANCE_DENSITY;
			bad |= (isSpecularMaterial(material) ? 1u : 0u) != g.flags.x;
			if (bad) {
				cmp.outOfTolerance++;
			}
		}
		pass.destroy(engine->m_device);

		size_t totalCases = 0;
		size_t totalFlips = 0;
		size_t totalBad = 0;
		for (size_t m = 0; m < materials.size(); m++) {
			const Comparison& cmp = byMaterial[m];
			totalCases += cmp.cases;
			totalFlips += cmp.flips;
			totalBad += cmp.outOfTolerance;
			if (cmp.outOfTolerance > 0 || cmp.flips > cmp.cases * MAX_FLIP_FRACTION) {
				result.lines.push_back(fmt::format("[{}] {}: {} of {} past the tolerance, {} decision(s) flipped; worst attenuation {:.2g}, direction {:.2g}, f cos {:.2g}, pdf {:.2g}", record ? "record" : "stream", materials[m].name, cmp.outOfTolerance, cmp.cases, cmp.flips, cmp.maxAttenuation, cmp.maxDirection, cmp.maxEval, cmp.maxPdf));
			}
		}
		Comparison worst;
		for (const Comparison& cmp : byMaterial) {
			worst.maxAttenuation = std::max(worst.maxAttenuation, cmp.maxAttenuation);
			worst.maxDirection = std::max(worst.maxDirection, cmp.maxDirection);
			worst.maxEval = std::max(worst.maxEval, cmp.maxEval);
			worst.maxPdf = std::max(worst.maxPdf, cmp.maxPdf);
		}
		const bool modePassed = totalBad == 0 && totalFlips <= totalCases * MAX_FLIP_FRACTION;
		passed &= modePassed;
		result.lines.insert(result.lines.begin(), fmt::format("{} mode: {} cases, {} past the tolerance, {} decision(s) flipped; worst attenuation {:.2g} (limit {:g}), direction {:.2g} ({:g}), f cos {:.2g} and pdf {:.2g} ({:g})", record ? "record" : "stream", totalCases, totalBad, totalFlips, worst.maxAttenuation, TOLERANCE_ATTENUATION, worst.maxDirection, TOLERANCE_DIRECTION, worst.maxEval, worst.maxPdf, TOLERANCE_DENSITY));
	}

	engine->destroyBuffer(caseBuffer);
	engine->destroyBuffer(recordBuffer);
	engine->destroyBuffer(resultBuffer);
	result.passed = passed;
	result.lines.insert(result.lines.begin(), passed ? "the shader and the mirror agree" : "the shader and the mirror disagree");
	return result;
}
