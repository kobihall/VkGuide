// The optical bodies' GPU self-test (docs/plans/completed/physoptics-optical-surfaces.md, -materials.md): shaders/test/
// optic_parity.comp runs crt_optic.glsl's intersector over every element's body, and crt_coating.glsl's table lookup and
// transfer matrix over the coatings, against their float mirrors in src/optic_surface.cpp and src/optic_coating.cpp. The
// bodies and the coatings are packed as the renderer packs them (packOpticBody(), OpticalLibrary::packCoatingRecord()).

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <random>

#include <fmt/format.h>

#include <gpu_selftest.h>
#include <optic_catalog.h>
#include <optic_library.h>
#include <vk_compute.h>
#include <vk_engine.h>

namespace {

struct OpticParityCase {
	glm::uvec4 ids;
	glm::vec4 a;
	glm::vec4 b;
	glm::vec4 pad;
};
static_assert(sizeof(OpticParityCase) == 64);

struct OpticParityResult {
	glm::vec4 first;
	glm::vec4 second;
};
static_assert(sizeof(OpticParityResult) == 32);

constexpr uint32_t NO_SURFACE = 0xFFFFFFFFu;

}

SelfTestResult runOpticParityTest(VulkanEngine* engine)
{
	SelfTestResult result;
	const OpticalLibrary& library = engine->m_opticalLibrary;
	OpticBuildContext context;
	context.library = &library;

	// every kind at its default size, and a bi-convex lens, packed one after another
	std::vector<glm::vec4> opticData;
	struct Body {
		OpticPrescriptionF floats;
		uint32_t base;
		std::string name;
	};
	std::vector<Body> bodies;
	for (size_t k = 0; k < OPTIC_KIND_COUNT; k++) {
		SceneOptic optic = makeOptic((OpticKind)k);
		OpticReadout readout;
		const OpticPrescription body = buildOptic(optic, context, readout);
		if (body.surfaces.empty()) {
			continue;
		}
		Body entry { toFloat(body), 0, opticTraits((OpticKind)k).name };
		entry.base = packOpticBody(entry.floats, opticData);
		bodies.push_back(std::move(entry));
	}
	{
		SceneOptic lens = makeOptic(OpticKind::SphericalLens);
		setOpticValue(lens, "form", 1);
		setOpticValue(lens, "r1", 0.1);
		setOpticValue(lens, "r2", 0.1);
		OpticReadout readout;
		Body entry { toFloat(buildOptic(lens, context, readout)), 0, "bi-convex lens" };
		entry.base = packOpticBody(entry.floats, opticData);
		bodies.push_back(std::move(entry));
	}

	// three coatings: a stack on a spectral grid, a stack and a protected metal in exact RGB
	std::vector<glm::vec4> coatingData;
	struct Table {
		const OpticCoating* coating;
		CoatingTable table;
		uint32_t header;
		bool rgb;
	};
	std::vector<Table> tables;
	const IndexLookup index = library.indexLookup();
	SceneSpectra spectra;
	auto addSpectrum = [&spectra](const SpectrumRef& ref) { return spectra.add(ref, SpectrumRole::Scalar); };
	for (const auto& [key, rgb] : { std::pair { "thorlabs_BSW", false }, std::pair { "thorlabs_A", true }, std::pair { "thorlabs_P01", true } }) {
		const OpticCoating* coating = library.findCoating(key);
		if (coating == nullptr) {
			result.lines.push_back(fmt::format("the {} preset is missing", key));
			return result;
		}
		std::vector<float> lambdas;
		for (int l = 400; l <= 800; l++) {
			lambdas.push_back((float)l);
		}
		Table entry { coating, rgb ? coatingTableRgb(*coating, index, GLASS_AIR, "N-BK7", {}, 587.6) : coatingTable(*coating, index, GLASS_AIR, "N-BK7", lambdas), 0, rgb };
		entry.header = library.packCoatingRecord(*coating, entry.table, rgb ? 0.f : 400.f, rgb ? 0.f : 1.f, rgb, addSpectrum, coatingData);
		tables.push_back(std::move(entry));
	}

	// the cases: rays at every body from around it, then lookups and transfer matrices at random inputs
	std::mt19937 rng(20261029);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	std::vector<OpticParityCase> cases;
	for (const Body& body : bodies) {
		const glm::vec3 center = 0.5f * (body.floats.bounds.min + body.floats.bounds.max);
		const glm::vec3 extent = body.floats.bounds.max - body.floats.bounds.min;
		for (int i = 0; i < 1024; i++) {
			const glm::vec3 through = center + (glm::vec3(unit(rng), unit(rng), unit(rng)) - 0.5f) * extent;
			const float z = 2.f * unit(rng) - 1.f;
			const float phi = 6.2831853f * unit(rng);
			const glm::vec3 d(std::sqrt(1.f - z * z) * std::cos(phi), std::sqrt(1.f - z * z) * std::sin(phi), z);
			const glm::vec3 o = through - 2.f * glm::length(extent) * d;
			cases.push_back(OpticParityCase { glm::uvec4(0u, body.base, (uint32_t)body.floats.surfaces.size(), 0u), glm::vec4(o, 1e-7f), glm::vec4(d, 1e30f), glm::vec4(0.f) });
		}
	}
	const size_t bodyCases = cases.size();
	for (const Table& table : tables) {
		for (int i = 0; i < 1024; i++) {
			const uint32_t direction = unit(rng) < 0.5f ? 0u : 1u;
			const float column = table.rgb ? (float)(i % 3) : 400.f * unit(rng);
			const float cosine = unit(rng);
			const float eta = direction == 0u ? 1.f / 1.52f : 1.52f;
			cases.push_back(OpticParityCase { glm::uvec4(1u, table.header, direction, 0u), glm::vec4(column, cosine, eta, 0.f), glm::vec4(0.f), glm::vec4(0.f) });
		}
	}
	const size_t lookupCases = cases.size() - bodyCases;
	for (const Table& table : tables) {
		if (!table.rgb) {
			continue;
		}
		for (int i = 0; i < 512; i++) {
			const uint32_t direction = unit(rng) < 0.5f ? 0u : 1u;
			const uint32_t channel = (uint32_t)i % 3u;
			const double lambda = channel == 0 ? 630.0 : (channel == 1 ? 532.0 : 465.0);
			const std::complex<double> air = index(GLASS_AIR, lambda);
			const std::complex<double> glass = index("N-BK7", lambda);
			const std::complex<double> incident = direction == 0u ? air : glass;
			const std::complex<double> other = direction == 0u ? glass : air;
			cases.push_back(OpticParityCase { glm::uvec4(2u, table.header, direction, channel), glm::vec4(0.f, 0.05f + 0.95f * unit(rng), 0.f, 0.f),
				glm::vec4((float)incident.real(), (float)incident.imag(), (float)other.real(), (float)other.imag()), glm::vec4(0.f) });
		}
	}

	// up, run, down
	if (opticData.empty()) {
		opticData.push_back(glm::vec4(0.f));
	}
	const VkDeviceSize caseBytes = sizeof(OpticParityCase) * cases.size();
	const VkDeviceSize resultBytes = sizeof(OpticParityResult) * cases.size();
	auto upload = [engine](const void* data, VkDeviceSize bytes) {
		const AllocatedBuffer buffer = engine->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		std::memcpy(buffer.info.pMappedData, data, bytes);
		vmaFlushAllocation(engine->m_memAllocator, buffer.allocation, 0, bytes);
		return buffer;
	};
	const AllocatedBuffer caseBuffer = upload(cases.data(), caseBytes);
	const AllocatedBuffer opticBuffer = upload(opticData.data(), sizeof(glm::vec4) * opticData.size());
	const AllocatedBuffer coatingBuffer = upload(coatingData.data(), sizeof(glm::vec4) * coatingData.size());
	const AllocatedBuffer resultBuffer = engine->createBuffer(resultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	ComputePass pass = ComputePassBuilder(engine->m_rootPath + "shaders/test/optic_parity.comp.spv")
		.addBinding(55, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding(56, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding(90, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding(91, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.setWorkgroupSize(64)
		.build(engine->m_device);
	const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, pass.setLayout);
	DescriptorWriter writer;
	writer.writeBuffer(55, opticBuffer.buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(56, coatingBuffer.buffer, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(90, caseBuffer.buffer, caseBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(91, resultBuffer.buffer, resultBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.updateSet(engine->m_device, set);
	engine->immediateSubmit([&](VkCommandBuffer cmd) { dispatchComputePassOver(cmd, pass, set, nullptr, { (uint32_t)cases.size(), 1, 1 }); });
	vmaInvalidateAllocation(engine->m_memAllocator, resultBuffer.allocation, 0, resultBytes);
	std::vector<OpticParityResult> gpu(cases.size());
	std::memcpy(gpu.data(), resultBuffer.info.pMappedData, resultBytes);
	pass.destroy(engine->m_device);
	for (const AllocatedBuffer& buffer : { caseBuffer, opticBuffer, coatingBuffer, resultBuffer }) {
		engine->destroyBuffer(buffer);
	}

	// the bodies: the same surface, t to 1e-6 relative, the same normal. A ray that passes a rim within float's reach of
	// it can find the neighbor surface on one side and not the other: a flip, counted apart and bounded
	size_t hits = 0;
	size_t flips = 0;
	size_t mismatches = 0;
	double worstT = 0.0;
	double worstNormal = 0.0;
	size_t caseIndex = 0;
	for (const Body& body : bodies) {
		for (int i = 0; i < 1024; i++, caseIndex++) {
			const OpticParityCase& c = cases[caseIndex];
			OpticHitT<float> cpuHit;
			const bool cpu = intersectOpticBody(body.floats, glm::vec3(c.a), glm::vec3(c.b), c.a.w, c.b.w, cpuHit);
			const uint32_t gpuSurface = std::bit_cast<uint32_t>(gpu[caseIndex].first.x);
			const uint32_t cpuSurface = cpu ? cpuHit.surface : NO_SURFACE;
			if (gpuSurface != cpuSurface) {
				flips++;
				continue;
			}
			if (!cpu) {
				continue;
			}
			hits++;
			const double tError = std::abs((double)gpu[caseIndex].first.y - (double)cpuHit.t) / std::max((double)cpuHit.t, 1e-6);
			const OpticSurfaceT<float>& surface = body.floats.surfaces[cpuHit.surface];
			const glm::vec3 normal = toBodyVector(surface, surfaceNormalLocal(surface, cpuHit.local));
			const double normalError = glm::length(glm::vec3(gpu[caseIndex].second) - normal);
			if (tError > 1e-5 || normalError > 1e-4) {
				mismatches++;
			}
			worstT = std::max(worstT, tError);
			worstNormal = std::max(worstNormal, normalError);
		}
	}
	// the lookups, to the float rounding of their arithmetic
	double worstLookup = 0.0;
	for (size_t i = 0; i < lookupCases; i++, caseIndex++) {
		const OpticParityCase& c = cases[caseIndex];
		const Table& table = tables[i / 1024];
		const glm::vec4 cpu = lookupCoatingTable(table.table, c.ids.z, c.a.x, c.a.y, c.a.z);
		const glm::vec4 difference = glm::abs(gpu[caseIndex].first - cpu);
		worstLookup = std::max({ worstLookup, (double)difference.x, (double)difference.y, (double)difference.z, (double)difference.w });
	}
	// the transfer matrix at a hit, against the double one
	double worstExact = 0.0;
	for (; caseIndex < cases.size(); caseIndex++) {
		const OpticParityCase& c = cases[caseIndex];
		const OpticCoating* coating = nullptr;
		for (const Table& table : tables) {
			if (table.header == c.ids.y) {
				coating = table.coating;
			}
		}
		const double lambda = c.ids.w == 0 ? 630.0 : (c.ids.w == 1 ? 532.0 : 465.0);
		const std::complex<double> incident(c.b.x, c.b.y);
		const std::complex<double> other(c.b.z, c.b.w);
		const bool fromPlus = c.ids.z == 0u;
		const CoatingResponse cpu = evaluateCoating(*coating, index, lambda, std::acos((double)c.a.y), fromPlus ? incident : other, fromPlus ? other : incident, fromPlus);
		const glm::vec4& g = gpu[caseIndex].first;
		worstExact = std::max({ worstExact, std::abs(g.x - cpu.Rs), std::abs(g.y - cpu.Rp), std::abs(g.z - cpu.Ts), std::abs(g.w - cpu.Tp) });
	}

	const bool flipsBounded = flips * 1000 <= bodyCases;
	result.passed = mismatches == 0 && flipsBounded && hits > bodyCases / 4 && worstLookup < 1e-5 && worstExact < 2e-3;
	result.lines.push_back(result.passed ? "the shader and the mirror agree" : "the shader and the mirror DISAGREE");
	result.lines.push_back(fmt::format("bodies: {} rays at {} bodies, {} hits; {} past the tolerance, {} surface decision(s) flipped at a rim (limit 1 in 1000); "
		"worst t {:.2g} relative (limit 1e-5), normal {:.2g}", bodyCases, bodies.size(), hits, mismatches, flips, worstT, worstNormal));
	result.lines.push_back(fmt::format("coating tables: {} lookups, worst difference {:.2g} (limit 1e-5)", lookupCases, worstLookup));
	result.lines.push_back(fmt::format("transfer matrix at a hit: worst difference from the double one {:.2g} (limit 2e-3)", worstExact));
	return result;
}
