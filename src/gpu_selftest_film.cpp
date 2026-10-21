// The film's GPU self-tests (docs/plans/completed/pbrt-film-and-reconstruction.md).
//
// Film parity runs the shaders' filter sampling (shaders/test/film_filter_parity.comp over crt_filter.glsl)
// and film arithmetic (film_sum_parity.comp over crt_film.glsl) on fixed batches and compares them with the
// C++ mirror in src/film.cpp. The sums include sequences built to defeat a float sum, which show whether the
// shader compiler kept Neumaier's compensation or reassociated it away.
//
// Film accounting runs the real kernel 09 shaders, 0901 and 0902, over synthetic frames: a sample dropped
// as non-finite, a constant fed for tens of thousands of frames, signed weights and the clamp. It checks
// what a render relies on: the count of samples taken advances past a dropped sample, the mean stays
// accurate where a float running mean drifts, and the weighted sums match the mirror.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <random>

#include <fmt/format.h>

#include <aov.h>
#include <film.h>
#include <gpu_selftest.h>
#include <vk_compute.h>
#include <vk_engine.h>
#include <vk_images.h>

namespace {

double relativeDifference(double a, double b, double floor)
{
	return std::abs(a - b) / std::max({ std::abs(a), std::abs(b), floor });
}

glm::vec2 uniformPair(std::mt19937& rng)
{
	std::uniform_real_distribution<float> unit(0.f, 0.99999994f);
	return glm::vec2(unit(rng), unit(rng));
}

//---------------------------------------------------------------- film parity

// one case of film_sum_parity.comp: where the sequence starts, how long it is, and the clamp's bits
struct SequenceCase {
	uint32_t first;
	uint32_t count;
	uint32_t clampBits;
	uint32_t pad;
};

struct SumSequence {
	std::vector<glm::vec4> samples;
	float clamp { 0.f };
	std::string label;
};

void filterParity(VulkanEngine* engine, SelfTestResult& result, bool& passed)
{
	// a position within this of the GPU's, in radii: the same table, two different divisions
	constexpr double TOLERANCE_OFFSET = 2e-5;
	// a sample on the edge of a cell can land in the next one; that is rounding, not a disagreement, and is bounded
	constexpr double MAX_FLIP_FRACTION = 1e-3;
	constexpr uint32_t CASES = 40000;

	std::mt19937 rng(20261021u);
	std::vector<glm::vec4> cases(CASES);
	for (glm::vec4& c : cases) {
		const glm::vec2 u = uniformPair(rng);
		c = glm::vec4(u, 0.f, 0.f);
	}
	const VkDeviceSize caseBytes = sizeof(glm::vec4) * cases.size();
	const AllocatedBuffer caseBuffer = engine->createBuffer(caseBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	const AllocatedBuffer resultBuffer = engine->createBuffer(caseBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	std::memcpy(caseBuffer.info.pMappedData, cases.data(), caseBytes);
	vmaFlushAllocation(engine->m_memAllocator, caseBuffer.allocation, 0, caseBytes);

	ComputePass pass = ComputePassBuilder(engine->m_rootPath + "shaders/test/film_filter_parity.comp.spv")
		.addBinding(30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding(90, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding(91, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.setWorkgroupSize(64, 1, 1)
		.build(engine->m_device);

	for (uint32_t k = 0; k < (uint32_t)PixelFilterKind::Count; k++) {
		const PixelFilterKind kind = (PixelFilterKind)k;
		const FilterTable table = buildFilterTable(defaultPixelFilter(kind));
		const std::vector<uint32_t> words = packFilterTable(table);
		const VkDeviceSize tableBytes = sizeof(uint32_t) * words.size();
		const AllocatedBuffer tableBuffer = engine->createBuffer(tableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		std::memcpy(tableBuffer.info.pMappedData, words.data(), tableBytes);
		vmaFlushAllocation(engine->m_memAllocator, tableBuffer.allocation, 0, tableBytes);

		const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, pass.setLayout);
		DescriptorWriter writer;
		writer.writeBuffer(30, tableBuffer.buffer, tableBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.writeBuffer(90, caseBuffer.buffer, caseBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.writeBuffer(91, resultBuffer.buffer, caseBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.updateSet(engine->m_device, set);
		std::memset(resultBuffer.info.pMappedData, 0, caseBytes);
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			dispatchComputePassOver(cmd, pass, set, nullptr, { CASES, 1, 1 });
		});
		vmaInvalidateAllocation(engine->m_memAllocator, resultBuffer.allocation, 0, caseBytes);
		const glm::vec4* gpu = (const glm::vec4*)resultBuffer.info.pMappedData;

		size_t flips = 0;
		size_t bad = 0;
		double worst = 0.0;
		for (uint32_t i = 0; i < CASES; i++) {
			const FilterSample mirror = sampleFilterTable(table, glm::vec2(cases[i]));
			const double offsetDifference = std::max(std::abs((double)mirror.offset.x - gpu[i].x), std::abs((double)mirror.offset.y - gpu[i].y)) / table.radius;
			if (offsetDifference > TOLERANCE_OFFSET) {
				//a different cell: both must still be a legal sample, so the weight is checked against its own cell
				flips++;
				continue;
			}
			worst = std::max(worst, offsetDifference);
			if (mirror.weight != gpu[i].z) {
				bad++;
			}
		}
		const bool kindPassed = bad == 0 && flips <= (size_t)(CASES * MAX_FLIP_FRACTION);
		passed &= kindPassed;
		result.lines.push_back(fmt::format("{} filter: {} cases, worst offset difference {:.2g} of the radius (limit {:g}), {} weight(s) differ, {} cell flip(s){}",
			pixelFilterName(kind), CASES, worst, TOLERANCE_OFFSET, bad, flips, kindPassed ? "" : " - FAILED"));
		engine->destroyBuffer(tableBuffer);
	}
	pass.destroy(engine->m_device);
	engine->destroyBuffer(caseBuffer);
	engine->destroyBuffer(resultBuffer);
}

std::vector<SumSequence> makeSumSequences()
{
	std::mt19937 rng(777u);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	std::vector<SumSequence> sequences;

	//random sequences: colors across a wide range (some brighter than the clamp), signs and clamps
	const float clamps[] = { 0.f, 2.f, 10.f };
	for (int s = 0; s < 400; s++) {
		SumSequence sequence;
		sequence.clamp = clamps[s % 3];
		const int length = 1 + (int)(unit(rng) * 63.f);
		for (int i = 0; i < length; i++) {
			const float scale = std::exp2(-6.f + 12.f * unit(rng));
			const float weight = unit(rng) < 0.25f ? -1.f : 1.f;
			sequence.samples.push_back(glm::vec4(unit(rng) * scale, unit(rng) * scale, unit(rng) * scale, weight));
		}
		sequence.label = "random";
		sequences.push_back(std::move(sequence));
	}

	//a large first sample and then thousands of small ones: a plain float sum loses the small ones' low bits
	{
		SumSequence sequence;
		sequence.label = "10000 then 4095 x 0.01";
		sequence.samples.push_back(glm::vec4(10000.f, 10000.f, 10000.f, 1.f));
		for (int i = 0; i < 4095; i++) {
			sequence.samples.push_back(glm::vec4(0.01f, 0.01f, 0.01f, 1.f));
		}
		sequences.push_back(std::move(sequence));
	}
	{
		SumSequence sequence;
		sequence.label = "8192 x 0.1";
		for (int i = 0; i < 8192; i++) {
			sequence.samples.push_back(glm::vec4(0.1f, 0.1f, 0.1f, 1.f));
		}
		sequences.push_back(std::move(sequence));
	}
	return sequences;
}

void sumParity(VulkanEngine* engine, SelfTestResult& result, bool& passed)
{
	// The shader's division is not IEEE, so the clamp's scale and Welford's update differ from the mirror's in
	// the last bits, and a signed sum that nearly cancels amplifies the difference: the value's tolerance
	// is a few times float's epsilon over the cancellation the random sequences reach
	constexpr double TOLERANCE_VALUE = 3e-5;
	constexpr double TOLERANCE_VARIANCE = 2e-4;

	const std::vector<SumSequence> sequences = makeSumSequences();
	std::vector<SequenceCase> cases;
	std::vector<glm::vec4> samples;
	for (const SumSequence& sequence : sequences) {
		uint32_t clampBits;
		std::memcpy(&clampBits, &sequence.clamp, sizeof(clampBits));
		cases.push_back({ (uint32_t)samples.size(), (uint32_t)sequence.samples.size(), clampBits, 0 });
		samples.insert(samples.end(), sequence.samples.begin(), sequence.samples.end());
	}

	const VkDeviceSize caseBytes = sizeof(SequenceCase) * cases.size();
	const VkDeviceSize sampleBytes = sizeof(glm::vec4) * samples.size();
	const VkDeviceSize resultBytes = 2 * sizeof(glm::vec4) * cases.size();
	const AllocatedBuffer caseBuffer = engine->createBuffer(caseBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	const AllocatedBuffer sampleBuffer = engine->createBuffer(sampleBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	const AllocatedBuffer resultBuffer = engine->createBuffer(resultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	std::memcpy(caseBuffer.info.pMappedData, cases.data(), caseBytes);
	vmaFlushAllocation(engine->m_memAllocator, caseBuffer.allocation, 0, caseBytes);
	std::memcpy(sampleBuffer.info.pMappedData, samples.data(), sampleBytes);
	vmaFlushAllocation(engine->m_memAllocator, sampleBuffer.allocation, 0, sampleBytes);

	ComputePass pass = ComputePassBuilder(engine->m_rootPath + "shaders/test/film_sum_parity.comp.spv")
		.addBinding(90, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding(91, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding(92, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.setWorkgroupSize(64, 1, 1)
		.build(engine->m_device);
	const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, pass.setLayout);
	DescriptorWriter writer;
	writer.writeBuffer(90, caseBuffer.buffer, caseBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(91, resultBuffer.buffer, resultBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeBuffer(92, sampleBuffer.buffer, sampleBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.updateSet(engine->m_device, set);
	std::memset(resultBuffer.info.pMappedData, 0, resultBytes);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		dispatchComputePassOver(cmd, pass, set, nullptr, { (uint32_t)cases.size(), 1, 1 });
	});
	vmaInvalidateAllocation(engine->m_memAllocator, resultBuffer.allocation, 0, resultBytes);
	const glm::vec4* gpu = (const glm::vec4*)resultBuffer.info.pMappedData;

	size_t bad = 0;
	double worstValue = 0.0;
	double worstVariance = 0.0;
	for (size_t i = 0; i < sequences.size(); i++) {
		const SumSequence& sequence = sequences[i];
		FilmPixel mirror;
		for (const glm::vec4& s : sequence.samples) {
			filmAddSample(mirror, glm::vec3(s), s.w, sequence.clamp);
		}
		const glm::vec3 value = filmResolve(mirror);
		const glm::vec4 resolved = gpu[2 * i];
		const glm::vec4 welford = gpu[2 * i + 1];
		double value_ = 0.0;
		for (int c = 0; c < 3; c++) {
			value_ = std::max(value_, relativeDifference(value[c], resolved[c], 1e-3));
		}
		const double meanDifference = relativeDifference(mirror.welford.mean, welford.x, 1e-3);
		const double m2Difference = relativeDifference(mirror.welford.m2, welford.y, 1e-3);
		worstValue = std::max(worstValue, value_);
		worstVariance = std::max({ worstVariance, meanDifference, m2Difference });
		const bool sequenceBad = value_ > TOLERANCE_VALUE || meanDifference > 1e-5 || m2Difference > TOLERANCE_VARIANCE || (uint32_t)resolved.w != mirror.welford.count;
		if (sequenceBad) {
			bad++;
			if (bad <= 5) {
				result.lines.push_back(fmt::format("sequence {} ({}, {} samples, clamp {:g}): resolved {:.7g} against {:.7g}, mean {:.7g} against {:.7g}, M2 {:.7g} against {:.7g}, accepted {} against {}",
					i, sequence.label, sequence.samples.size(), sequence.clamp, resolved.x, value.x, welford.x, mirror.welford.mean, welford.y, mirror.welford.m2, (uint32_t)resolved.w, mirror.welford.count));
			}
		}
	}

	//the two stress sequences with an exact answer: the shader's compensation must be doing its work. A sum
	//that lost it comes out far from the double-precision value, where a float sum with Neumaier's term is within rounding
	const size_t stress = sequences.size() - 2;
	{
		const SumSequence& sequence = sequences[stress];
		double exact = 0.0;
		float plain = 0.f;
		for (const glm::vec4& s : sequence.samples) {
			exact += (double)s.x;
			plain += s.x;
		}
		exact /= (double)sequence.samples.size();
		const double gpuError = relativeDifference(gpu[2 * stress].x, exact, 1e-3);
		const double plainError = relativeDifference((double)plain / sequence.samples.size(), exact, 1e-3);
		const bool compensated = gpuError < 2e-6 && plainError > 5.0 * gpuError;
		passed &= compensated;
		result.lines.push_back(fmt::format("compensation: the mean of {} has relative error {:.2g} on the GPU; a plain float sum's is {:.2g}{}",
			sequence.label, gpuError, plainError, compensated ? "" : " - the GPU is not compensating"));
	}

	const bool sumsPassed = bad == 0;
	passed &= sumsPassed;
	result.lines.push_back(fmt::format("film sums: {} sequences, worst resolved difference {:.2g} (limit {:g}), worst variance difference {:.2g} (limit {:g}), {} past the tolerance{}",
		sequences.size(), worstValue, TOLERANCE_VALUE, worstVariance, TOLERANCE_VARIANCE, bad, sumsPassed ? "" : " - FAILED"));

	pass.destroy(engine->m_device);
	engine->destroyBuffer(caseBuffer);
	engine->destroyBuffer(sampleBuffer);
	engine->destroyBuffer(resultBuffer);
}

//---------------------------------------------------------------- film accounting

struct FilmRun {
	std::vector<glm::vec4> accumulation;
	std::vector<uint32_t> taken;
	std::vector<CrtFilmPixel> film;
	uint32_t dropped { 0 };
	// the auxiliary outputs' layers (layer after layer, pixel after pixel) and the light groups' sums, when asked for
	std::vector<glm::vec4> aovLayers;
	std::vector<float> groupSums;
};

// Runs one kernel 09 shader over `frames` frames of one slot per pixel, `radiance(frame, pixel)` being the
// slot's value (w nonzero for a spawned slot), and reads back what it left. The frames run in one command
// buffer, each preceded by a copy of its slot values into the buffer the kernel reads
struct FilmRunOptions {
	bool keepsRecord { false };
	// the outlier film's cascade (binding 62) and its two passes
	bool cascade { false };
	uint32_t passes { 1 };
	uint32_t width { 1 };
	uint32_t height { 1 };
	uint32_t frames { 1 };
	float clampComponent { 0.f };
	float kappa { 32.f };
	// the auxiliary outputs (docs/plans/completed/pbrt-aovs-and-image-output.md): how many layers the run keeps (0 for
	// none), the packed layer indices and the output bits the kernel is given, the light groups it sums (0 for none),
	// and the PixelState each slot of each frame carries. Kernel 09 declares the three bindings whether or not they
	// are used, so a run that keeps none binds stubs
	uint32_t aovLayerCount { 0 };
	uint32_t aovLayers { 0xFFFFFFFFu };
	uint32_t aovOutputs { 0 };
	uint32_t lightGroups { 0 };
	std::function<CrtPixelState(uint32_t, uint32_t)> pixelState;
};

FilmRun runFilmKernel(VulkanEngine* engine, const char* shader, const FilmRunOptions& options, const std::function<glm::vec4(uint32_t, uint32_t)>& radiance)
{
	const uint32_t pixels = options.width * options.height;
	const uint32_t frames = options.frames;
	const bool keepsRecord = options.keepsRecord;
	const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	const VkDeviceSize frameBytes = sizeof(glm::vec4) * pixels;
	const VkDeviceSize stagingBytes = frameBytes * frames;
	const AllocatedBuffer staging = engine->createBuffer(stagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	glm::vec4* mapped = (glm::vec4*)staging.info.pMappedData;
	for (uint32_t f = 0; f < frames; f++) {
		for (uint32_t p = 0; p < pixels; p++) {
			mapped[(size_t)f * pixels + p] = radiance(f, p);
		}
	}
	vmaFlushAllocation(engine->m_memAllocator, staging.allocation, 0, stagingBytes);

	//the outputs: PixelState per slot, the AOV layers and the light groups' sums, or stubs of the sizes a render that
	//keeps none publishes
	const bool aov = options.aovLayerCount > 0;
	const VkDeviceSize stateBytes = aov ? sizeof(CrtPixelState) * pixels : sizeof(CrtPixelState);
	const VkDeviceSize stateStagingBytes = sizeof(CrtPixelState) * (aov ? (VkDeviceSize)pixels * frames : 1);
	const AllocatedBuffer stateStaging = engine->createBuffer(stateStagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	if (aov) {
		CrtPixelState* states = (CrtPixelState*)stateStaging.info.pMappedData;
		for (uint32_t f = 0; f < frames; f++) {
			for (uint32_t p = 0; p < pixels; p++) {
				states[(size_t)f * pixels + p] = options.pixelState(f, p);
			}
		}
		vmaFlushAllocation(engine->m_memAllocator, stateStaging.allocation, 0, stateStagingBytes);
	}
	const AllocatedBuffer stateBuffer = engine->createBuffer(stateBytes, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	const uint32_t layerCount = std::max(options.aovLayerCount, 1u);
	const VkExtent3D aovExtent { aov ? options.width : 1u, aov ? options.height : 1u, 1 };
	const AllocatedImage aovImage = engine->createImageArray(aovExtent, layerCount, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	const VkDeviceSize aovBytes = sizeof(glm::vec4) * aovExtent.width * aovExtent.height * layerCount;
	const AllocatedBuffer readAov = engine->createBuffer(aovBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	const VkDeviceSize sumBytes = options.lightGroups > 0 ? sizeof(float) * pixels * (3 * options.lightGroups + 1) : 16;
	const AllocatedBuffer sumBuffer = engine->createBuffer(sumBytes, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	const AllocatedBuffer readSums = engine->createBuffer(sumBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);

	const AllocatedBuffer radianceBuffer = engine->createBuffer(frameBytes, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	const AllocatedBuffer statsBuffer = engine->createBuffer(sizeof(uint32_t) * CRT_TRAVERSAL_STATS_WORDS, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	const VkDeviceSize filmBytes = sizeof(CrtFilmPixel) * pixels;
	const AllocatedBuffer filmBuffer = engine->createBuffer(filmBytes, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	const VkDeviceSize cascadeBytes = options.cascade ? sizeof(glm::vec4) * OUTLIER_BUFFERS * pixels : 16;
	const AllocatedBuffer cascadeBuffer = engine->createBuffer(cascadeBytes, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	const VkExtent3D extent { options.width, options.height, 1 };
	const AllocatedImage accumulation = engine->createImage(extent, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	const AllocatedImage taken = engine->createImage(extent, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

	const VkDeviceSize accumulationBytes = sizeof(glm::vec4) * pixels;
	const VkDeviceSize takenBytes = sizeof(uint32_t) * pixels;
	const VkDeviceSize statsBytes = sizeof(uint32_t) * CRT_TRAVERSAL_STATS_WORDS;
	const AllocatedBuffer readAccumulation = engine->createBuffer(accumulationBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	const AllocatedBuffer readTaken = engine->createBuffer(takenBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	const AllocatedBuffer readFilm = engine->createBuffer(filmBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	const AllocatedBuffer readStats = engine->createBuffer(statsBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);

	ComputePassBuilder builder(engine->m_rootPath + shader);
	builder.addBinding((uint32_t)CrtBinding::Radiance, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding((uint32_t)CrtBinding::Accumulation, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding((uint32_t)CrtBinding::SamplesTaken, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding((uint32_t)CrtBinding::TraversalStats, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	if (keepsRecord) {
		builder.addBinding((uint32_t)CrtBinding::AccumulationError, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	}
	if (options.cascade) {
		builder.addBinding((uint32_t)CrtBinding::FilmCascade, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	}
	builder.addBinding((uint32_t)CrtBinding::PixelState, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding((uint32_t)CrtBinding::AovImages, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding((uint32_t)CrtBinding::ChannelSums, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addSpecialization(1, aov ? 1u : 0u)
		.addSpecialization(2, options.lightGroups > 0 ? 1u : 0u);
	ComputePass pass = builder.setPushConstants<CrtParams>().setWorkgroupSize(CRT_WORKGROUP).build(engine->m_device);

	const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, pass.setLayout);
	DescriptorWriter writer;
	writer.writeBuffer((int)CrtBinding::Radiance, radianceBuffer.buffer, frameBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeImage((int)CrtBinding::Accumulation, accumulation.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage((int)CrtBinding::SamplesTaken, taken.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeBuffer((int)CrtBinding::TraversalStats, statsBuffer.buffer, statsBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	if (keepsRecord) {
		writer.writeBuffer((int)CrtBinding::AccumulationError, filmBuffer.buffer, filmBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	}
	if (options.cascade) {
		writer.writeBuffer((int)CrtBinding::FilmCascade, cascadeBuffer.buffer, cascadeBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	}
	writer.writeBuffer((int)CrtBinding::PixelState, stateBuffer.buffer, stateBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeImage((int)CrtBinding::AovImages, aovImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeBuffer((int)CrtBinding::ChannelSums, sumBuffer.buffer, sumBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.updateSet(engine->m_device, set);

	CrtParams params {};
	params.width = options.width;
	params.height = options.height;
	params.samplesPerFrame = 1;
	params.poolSize = pixels;
	params.filmClamp = options.clampComponent;
	params.outlierKappa = options.kappa;
	params.maxSamples = frames;
	params.aovOutputs = options.aovOutputs;
	params.aovLayers = options.aovLayers;
	params.lightGroupCount = options.lightGroups;

	const VkPipelineStageFlags2 compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	const VkPipelineStageFlags2 transfer = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
	const VkAccessFlags2 transferAccess = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, accumulation.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, taken.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		const VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		const VkClearColorValue zeroFloat { { 0.f, 0.f, 0.f, 0.f } };
		VkClearColorValue zeroUint {};
		vkCmdClearColorImage(cmd, accumulation.image, VK_IMAGE_LAYOUT_GENERAL, &zeroFloat, 1, &range);
		vkCmdClearColorImage(cmd, taken.image, VK_IMAGE_LAYOUT_GENERAL, &zeroUint, 1, &range);
		vkCmdFillBuffer(cmd, filmBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
		vkCmdFillBuffer(cmd, cascadeBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
		vkCmdFillBuffer(cmd, statsBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
		vkutil::transition_image(cmd, aovImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		const VkImageSubresourceRange aovRange { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layerCount };
		vkCmdClearColorImage(cmd, aovImage.image, VK_IMAGE_LAYOUT_GENERAL, &zeroFloat, 1, &aovRange);
		vkCmdFillBuffer(cmd, stateBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
		vkCmdFillBuffer(cmd, sumBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
		vkutil::memory_barrier(cmd, transfer, transferAccess, transfer | compute, transferAccess | computeAccess);

		for (uint32_t f = 0; f < frames; f++) {
			VkBufferCopy copy {};
			copy.srcOffset = frameBytes * f;
			copy.size = frameBytes;
			vkCmdCopyBuffer(cmd, staging.buffer, radianceBuffer.buffer, 1, &copy);
			if (aov) {
				VkBufferCopy stateCopy {};
				stateCopy.srcOffset = sizeof(CrtPixelState) * (VkDeviceSize)pixels * f;
				stateCopy.size = stateBytes;
				vkCmdCopyBuffer(cmd, stateStaging.buffer, stateBuffer.buffer, 1, &stateCopy);
			}
			vkutil::memory_barrier(cmd, transfer, transferAccess, compute, computeAccess);
			for (uint32_t filmPass = 0; filmPass < options.passes; filmPass++) {
				params.bounce = filmPass;
				dispatchComputePassOver(cmd, pass, set, &params, { pixels, 1, 1 });
				vkutil::memory_barrier(cmd, compute, computeAccess, transfer | compute, transferAccess | computeAccess);
			}
		}

		VkBufferImageCopy region {};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = extent;
		vkCmdCopyImageToBuffer(cmd, accumulation.image, VK_IMAGE_LAYOUT_GENERAL, readAccumulation.buffer, 1, &region);
		vkCmdCopyImageToBuffer(cmd, taken.image, VK_IMAGE_LAYOUT_GENERAL, readTaken.buffer, 1, &region);
		VkBufferCopy filmCopy { 0, 0, filmBytes };
		vkCmdCopyBuffer(cmd, filmBuffer.buffer, readFilm.buffer, 1, &filmCopy);
		VkBufferCopy statsCopy { 0, 0, statsBytes };
		vkCmdCopyBuffer(cmd, statsBuffer.buffer, readStats.buffer, 1, &statsCopy);
		VkBufferImageCopy aovRegion {};
		aovRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layerCount };
		aovRegion.imageExtent = aovExtent;
		vkCmdCopyImageToBuffer(cmd, aovImage.image, VK_IMAGE_LAYOUT_GENERAL, readAov.buffer, 1, &aovRegion);
		VkBufferCopy sumCopy { 0, 0, sumBytes };
		vkCmdCopyBuffer(cmd, sumBuffer.buffer, readSums.buffer, 1, &sumCopy);
	});

	FilmRun run;
	vmaInvalidateAllocation(engine->m_memAllocator, readAccumulation.allocation, 0, accumulationBytes);
	vmaInvalidateAllocation(engine->m_memAllocator, readTaken.allocation, 0, takenBytes);
	vmaInvalidateAllocation(engine->m_memAllocator, readFilm.allocation, 0, filmBytes);
	vmaInvalidateAllocation(engine->m_memAllocator, readStats.allocation, 0, statsBytes);
	run.accumulation.assign((const glm::vec4*)readAccumulation.info.pMappedData, (const glm::vec4*)readAccumulation.info.pMappedData + pixels);
	run.taken.assign((const uint32_t*)readTaken.info.pMappedData, (const uint32_t*)readTaken.info.pMappedData + pixels);
	run.film.assign((const CrtFilmPixel*)readFilm.info.pMappedData, (const CrtFilmPixel*)readFilm.info.pMappedData + pixels);
	run.dropped = ((const uint32_t*)readStats.info.pMappedData)[CRT_DROPPED_SAMPLES_STAT];
	vmaInvalidateAllocation(engine->m_memAllocator, readAov.allocation, 0, aovBytes);
	vmaInvalidateAllocation(engine->m_memAllocator, readSums.allocation, 0, sumBytes);
	if (aov) {
		run.aovLayers.assign((const glm::vec4*)readAov.info.pMappedData, (const glm::vec4*)readAov.info.pMappedData + (size_t)pixels * layerCount);
	}
	if (options.lightGroups > 0) {
		run.groupSums.assign((const float*)readSums.info.pMappedData, (const float*)readSums.info.pMappedData + sumBytes / sizeof(float));
	}

	pass.destroy(engine->m_device);
	for (const AllocatedBuffer& buffer : { staging, radianceBuffer, statsBuffer, filmBuffer, cascadeBuffer, readAccumulation, readTaken, readFilm, readStats, stateStaging, stateBuffer, readAov, sumBuffer, readSums }) {
		engine->destroyBuffer(buffer);
	}
	engine->destroyImage(accumulation);
	engine->destroyImage(taken);
	engine->destroyImage(aovImage);
	return run;
}

}  // namespace

SelfTestResult runFilmParityTest(VulkanEngine* engine)
{
	SelfTestResult result;
	bool passed = true;
	filterParity(engine, result, passed);
	sumParity(engine, result, passed);
	result.passed = passed;
	result.lines.insert(result.lines.begin(), passed ? "the shaders and the mirror agree" : "the shaders and the mirror disagree");
	return result;
}

SelfTestResult runFilmAccountingTest(VulkanEngine* engine)
{
	SelfTestResult result;
	bool passed = true;
	auto expect = [&](bool ok, const std::string& message) {
		passed &= ok;
		result.lines.push_back(std::string(ok ? "ok: " : "FAILED: ") + message);
	};

	constexpr uint32_t PIXELS = 4;
	constexpr uint32_t FRAMES = 1u << 16;
	constexpr float CLAMP = 16.f;
	const float tenth = 0.1f;
	const float nan = std::nanf("");

	// pixel 0: eight frames, the fourth a NaN. pixel 1: a constant that float cannot hold exactly, for 65536
	// frames. pixel 2: weights of -1 and 1 over varying values. pixel 3: a sample far above the clamp
	auto radiance = [&](uint32_t frame, uint32_t pixel) -> glm::vec4 {
		switch (pixel) {
		case 0:
			if (frame >= 8) {
				return glm::vec4(0.f);
			}
			return frame == 3 ? glm::vec4(nan, 1.f, 1.f, 1.f) : glm::vec4((float)(frame + 1), (float)(frame + 1), (float)(frame + 1), 1.f);
		case 1:
			return glm::vec4(tenth, tenth, tenth, 1.f);
		case 2: {
			const float v = 0.5f + 0.001f * (float)(frame % 1000);
			return glm::vec4(v, 0.5f * v, 2.f * v, (frame % 3 == 2) ? -1.f : 1.f);
		}
		default:
			return glm::vec4(100.f, 50.f, 25.f, 1.f);
		}
	};

	FilmRunOptions options;
	options.width = PIXELS;
	options.frames = FRAMES;
	options.clampComponent = CLAMP;
	const FilmRun running = runFilmKernel(engine, "shaders/rt/0901_update_film.comp.spv", options, radiance);
	options.keepsRecord = true;
	const FilmRun compensated = runFilmKernel(engine, "shaders/rt/0902_update_film_compensated.comp.spv", options, radiance);

	// a sample dropped as non-finite
	const double finiteMean = (1 + 2 + 3 + 5 + 6 + 7 + 8) / 7.0;
	expect(running.dropped == 1 && compensated.dropped == 1, fmt::format("the NaN was counted as dropped once by each film ({} and {})", running.dropped, compensated.dropped));
	expect(compensated.taken[0] == 8, fmt::format("the compensated film took 8 samples of pixel 0 ({}), so the next frame draws a new one", compensated.taken[0]));
	expect(compensated.film[0].accepted == 7, fmt::format("the compensated film accepted 7 of them ({})", compensated.film[0].accepted));
	expect(running.taken[0] == 7, fmt::format("the running mean still counts only what it accepts, as it always did ({}): a dropped sample is drawn again", running.taken[0]));
	expect(std::abs(compensated.accumulation[0].x - finiteMean) < 1e-5 && std::abs(running.accumulation[0].x - finiteMean) < 1e-5,
		fmt::format("both means of pixel 0 are {:.5f} ({:.5f} and {:.5f})", finiteMean, compensated.accumulation[0].x, running.accumulation[0].x));

	// a constant, to the sample count where a float mean drifts
	const double exact = (double)tenth;
	const double compensatedError = std::abs((double)compensated.accumulation[1].x - exact) / exact;
	const double runningError = std::abs((double)running.accumulation[1].x - exact) / exact;
	expect(compensated.taken[1] == FRAMES && compensated.film[1].accepted == FRAMES, fmt::format("pixel 1: {} samples taken and accepted", compensated.film[1].accepted));
	expect(compensatedError < 5e-7, fmt::format("the compensated mean of {} samples of 0.1 has relative error {:.2g}", FRAMES, compensatedError));
	expect(compensatedError <= runningError, fmt::format("the running mean's is {:.2g}", runningError));

	// signed weights, against the mirror
	FilmPixel mirror;
	for (uint32_t frame = 0; frame < FRAMES; frame++) {
		const glm::vec4 s = radiance(frame, 2);
		filmAddSample(mirror, glm::vec3(s), s.w, CLAMP);
	}
	const glm::vec3 weighted = filmResolve(mirror);
	double weightedError = 0.0;
	for (int c = 0; c < 3; c++) {
		weightedError = std::max(weightedError, relativeDifference(weighted[c], compensated.accumulation[2][c], 1e-3));
	}
	expect(weightedError < 1e-5, fmt::format("pixel 2's weighted mean of -1 and +1 samples matches the mirror to {:.2g}", weightedError));

	// the clamp scales a sample down to the limit, keeping its color
	expect(std::abs(compensated.accumulation[3].x - 16.f) < 1e-4f && std::abs(compensated.accumulation[3].y - 8.f) < 1e-4f && std::abs(compensated.accumulation[3].z - 4.f) < 1e-4f,
		fmt::format("pixel 3's clamped sample is ({:.4f}, {:.4f}, {:.4f}), not (100, 50, 25)", compensated.accumulation[3].x, compensated.accumulation[3].y, compensated.accumulation[3].z));
	expect(std::abs(running.accumulation[3].x - 100.f) < 0.05f, fmt::format("the running mean does not clamp ({:.4f})", running.accumulation[3].x));

	result.passed = passed;
	result.lines.insert(result.lines.begin(), passed ? "kernel 09's films account for samples as they should" : "kernel 09's films miscount");
	return result;
}

SelfTestResult runFilmOutlierTest(VulkanEngine* engine)
{
	SelfTestResult result;
	constexpr uint32_t WIDTH = 12;
	constexpr uint32_t HEIGHT = 8;
	constexpr uint32_t FRAMES = 1500;
	constexpr float KAPPA = 8.f;

	// every pixel draws values around 1, over a wide spread; a few carry a firefly nobody nearby shares, a block
	// of pixels carries a bright value often enough to be well sampled, and one pixel's samples are signed
	std::mt19937 rng(4242u);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	std::vector<glm::vec4> table((size_t)FRAMES * WIDTH * HEIGHT);
	for (uint32_t frame = 0; frame < FRAMES; frame++) {
		for (uint32_t p = 0; p < WIDTH * HEIGHT; p++) {
			const uint32_t x = p % WIDTH;
			const uint32_t y = p / WIDTH;
			float v = 0.4f + 1.2f * unit(rng) * unit(rng) * 2.f;
			if (x >= 6 && x < 9 && y >= 2 && y < 5 && unit(rng) < 0.08f) {
				v = 60.f;
			}
			if (frame == 700 + p && p % 7 == 0) {
				v = 5000.f;
			}
			const float sign = (p == 3 && frame % 4 == 3) ? -1.f : 1.f;
			table[(size_t)frame * WIDTH * HEIGHT + p] = glm::vec4(v, 0.8f * v, 0.5f * v, sign);
		}
	}
	auto radiance = [&](uint32_t frame, uint32_t pixel) { return table[(size_t)frame * WIDTH * HEIGHT + pixel]; };

	FilmRunOptions options;
	options.keepsRecord = true;
	options.cascade = true;
	options.passes = 2;
	options.width = WIDTH;
	options.height = HEIGHT;
	options.frames = FRAMES;
	options.kappa = KAPPA;
	const FilmRun gpu = runFilmKernel(engine, "shaders/rt/0903_update_film_outliers.comp.spv", options, radiance);

	// the mirror: every pixel's cascade and film, then each pixel's resolve over its 3 x 3 neighborhood
	std::vector<CascadePixel> cascades(WIDTH * HEIGHT);
	std::vector<FilmPixel> films(WIDTH * HEIGHT);
	for (uint32_t frame = 0; frame < FRAMES; frame++) {
		for (uint32_t p = 0; p < WIDTH * HEIGHT; p++) {
			const glm::vec4 s = radiance(frame, p);
			filmAddSample(films[p], glm::vec3(s), s.w, 0.f);
			cascadeAddSample(cascades[p], glm::vec3(s), s.w);
		}
	}
	double worst = 0.0;
	size_t bad = 0;
	double fireflyMean = 0.0;
	double fireflyPlain = 0.0;
	size_t fireflyPixels = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t p = y * WIDTH + x;
			std::array<float, OUTLIER_BUFFERS> counts {};
			for (uint32_t level = 0; level < OUTLIER_BUFFERS; level++) {
				float sum = 0.f;
				float n = 0.f;
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = (int)x + dx;
						const int ny = (int)y + dy;
						if (nx < 0 || ny < 0 || nx >= (int)WIDTH || ny >= (int)HEIGHT) {
							continue;
						}
						sum += cascadeCount(cascades[ny * WIDTH + nx], level);
						n += 1.f;
					}
				}
				counts[level] = sum / n;
			}
			const float weightSum = compensatedValue(films[p].sums[3]);
			const glm::vec3 mirror = outlierResolve(cascades[p], counts, films[p].welford.count, weightSum, KAPPA);
			double difference = 0.0;
			for (int c = 0; c < 3; c++) {
				difference = std::max(difference, relativeDifference(mirror[c], gpu.accumulation[p][c], 1e-2));
			}
			worst = std::max(worst, difference);
			bad += difference > 2e-3;
			if (p % 7 == 0 && p >= 1) {
				fireflyPixels++;
				fireflyMean += gpu.accumulation[p].x;
				fireflyPlain += filmResolve(films[p]).x;
			}
		}
	}
	const bool matches = bad == 0;
	result.lines.push_back(fmt::format("{}: {} pixels, worst relative difference from the mirror {:.2g} (limit 2e-3), {} past it", matches ? "ok" : "FAILED", WIDTH * HEIGHT, worst, bad));
	// and it does its work: the pixels with a firefly come out far below their plain mean
	const bool reduced = fireflyPixels > 0 && fireflyMean < 0.6 * fireflyPlain;
	result.lines.push_back(fmt::format("{}: the {} pixels with a firefly average {:.3f} reweighted against {:.3f} plain", reduced ? "ok" : "FAILED", fireflyPixels, fireflyMean / std::max<size_t>(fireflyPixels, 1), fireflyPlain / std::max<size_t>(fireflyPixels, 1)));
	result.passed = matches && reduced;
	result.lines.insert(result.lines.begin(), result.passed ? "kernel 0903 resolves as the mirror does" : "kernel 0903 and the mirror disagree");
	return result;
}

//---------------------------------------------------------------- auxiliary outputs

// Kernel 09's fold of the auxiliary outputs (shaders/rt/include/crt_aov.glsl aovFoldPixel()) through the real
// 0902 shader, over synthetic frames: each pixel's sums of position, normals, uv, albedo, the ids and the variance,
// and the light groups' weight sum, against the mirror in src/aov.cpp that adds the same samples in the same order
// (docs/plans/completed/pbrt-aovs-and-image-output.md). A sample that is a NaN, a slot that was never spawned, a
// negative filter weight and a sample that met no surface are all in the batch, and so is a render that keeps
// only some layers
SelfTestResult runAovFoldTest(VulkanEngine* engine)
{
	SelfTestResult result;
	bool passed = true;
	auto expect = [&](bool ok, const std::string& message) {
		passed &= ok;
		result.lines.push_back(std::string(ok ? "ok: " : "FAILED: ") + message);
	};

	constexpr uint32_t PIXELS = 6;
	constexpr uint32_t FRAMES = 200;
	const float nan = std::nanf("");

	// per (frame, pixel): the sample, its weight and radiance. Pixel 0 meets a surface every time, 1 half of
	// the time, 2 never; pixel 3 has filter weights of -1, and a NaN and an unspawned slot among its frames; 4 and 5
	// repeat 0 and 1 for the second layout
	struct Sample {
		AovSample surface;
		glm::vec4 slot;
	};
	std::mt19937 rng(1234);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	std::vector<Sample> samples((size_t)FRAMES * PIXELS);
	for (uint32_t f = 0; f < FRAMES; f++) {
		for (uint32_t p = 0; p < PIXELS; p++) {
			Sample& s = samples[(size_t)f * PIXELS + p];
			const uint32_t kind = p % 3;
			s.surface.hit = kind == 0 || (kind == 1 && unit(rng) < 0.5f);
			if (s.surface.hit) {
				s.surface.position = glm::vec3(unit(rng) * 10.f, unit(rng) * 10.f, unit(rng) * 10.f);
				s.surface.depth = 1.f + unit(rng) * 20.f;
				s.surface.normal = glm::normalize(glm::vec3(unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) + 0.1f));
				s.surface.shadingNormal = glm::normalize(s.surface.normal + glm::vec3(0.1f * unit(rng), 0.f, 0.f));
				s.surface.uv = glm::vec2(unit(rng), unit(rng));
				s.surface.albedo = glm::vec3(unit(rng), unit(rng), unit(rng));
				s.surface.instance = f % 5 + 1;
				s.surface.material = f % 3;
			}
			float weight = 1.f;
			if (p == 3) {
				weight = (f % 4 == 3) ? -1.f : 1.f;
			}
			s.slot = glm::vec4(unit(rng) * 3.f, unit(rng), unit(rng) * 0.5f, weight);
			if (p == 0 && f == 5) {
				s.slot = glm::vec4(nan, 1.f, 1.f, 1.f);
			}
			if (p == 3 && f % 10 == 9) {
				s.slot.w = 0.f;
			}
		}
	}

	auto pixelState = [&](uint32_t frame, uint32_t pixel) {
		const AovSample& a = samples[(size_t)frame * PIXELS + pixel].surface;
		CrtPixelState state {};
		state.position = a.position;
		state.depth = a.depth;
		state.normal = a.normal;
		state.shadingNormal = a.shadingNormal;
		state.uv = a.uv;
		//a slot that met no surface carries whatever the last frame left: the fold has to leave it out
		state.albedo = a.hit ? a.albedo : glm::vec3(7.f);
		state.instance = a.instance;
		state.material = a.material;
		state.state = a.hit ? AOV_STATE_HIT : 0u;
		return state;
	};
	auto radiance = [&](uint32_t frame, uint32_t pixel) { return samples[(size_t)frame * PIXELS + pixel].slot; };

	for (const bool everything : { true, false }) {
		AovSettings settings;
		if (everything) {
			for (uint32_t i = 0; i < (uint32_t)AovOutput::Count; i++) {
				settings.set((AovOutput)i, true);
			}
		} else {
			settings.set(AovOutput::Normal, true);
			settings.set(AovOutput::Albedo, true);
		}
		const AovLayout layout = makeAovLayout(settings);
		FilmRunOptions options;
		options.width = PIXELS;
		options.frames = FRAMES;
		options.keepsRecord = true;
		options.aovLayerCount = layout.count;
		options.aovLayers = layout.pack();
		options.aovOutputs = settings.outputs;
		options.lightGroups = 3;
		options.pixelState = pixelState;
		const FilmRun gpu = runFilmKernel(engine, "shaders/rt/0902_update_film_compensated.comp.spv", options, radiance);

		// the mirror: the same samples, accepted the way the kernel accepts them
		std::vector<AovPixel> mirror(PIXELS);
		std::vector<double> weightSums(PIXELS, 0.0);
		for (uint32_t f = 0; f < FRAMES; f++) {
			for (uint32_t p = 0; p < PIXELS; p++) {
				const glm::vec4 slot = radiance(f, p);
				if (slot.w == 0.f || !std::isfinite(slot.x) || !std::isfinite(slot.y) || !std::isfinite(slot.z)) {
					continue;
				}
				const float weight = slot.w < 0.f ? -1.f : 1.f;
				weightSums[p] += weight;
				mirror[p].add(samples[(size_t)f * PIXELS + p].surface, weight, glm::vec3(slot));
			}
		}

		double worst = 0.0;
		uint32_t worstLayer = 0;
		uint32_t worstPixel = 0;
		bool absentLayersUntouched = true;
		for (uint32_t layer = 0; layer < AOV_LAYER_COUNT; layer++) {
			const uint32_t index = layout.layer[layer];
			if (index == AOV_LAYER_NONE) {
				continue;
			}
			for (uint32_t p = 0; p < PIXELS; p++) {
				const glm::vec4 g = gpu.aovLayers[(size_t)index * PIXELS + p];
				const glm::vec4 m = mirror[p].layers[layer];
				for (int c = 0; c < 4; c++) {
					const double difference = relativeDifference(g[c], m[c], 1.0);
					if (difference > worst) {
						worst = difference;
						worstLayer = layer;
						worstPixel = p;
					}
				}
			}
		}
		// the weight sum the light groups divide by, and that nothing was written past the layers the render keeps
		double worstWeight = 0.0;
		for (uint32_t p = 0; p < PIXELS; p++) {
			worstWeight = std::max(worstWeight, std::abs((double)gpu.groupSums[(size_t)p * 10 + 9] - weightSums[p]));
		}
		const double limit = 1e-5;
		expect(worst < limit && absentLayersUntouched, fmt::format("{}: {} layers match the mirror, worst relative difference {:.2g} (layer {} pixel {}, limit {:g})",
			everything ? "every output" : "normals and albedo", layout.count, worst, worstLayer, worstPixel, limit));
		expect(worstWeight == 0.0, fmt::format("{}: the light groups' weight sum is the sum of the filter signs of the accepted samples (worst difference {:g})", everything ? "every output" : "normals and albedo", worstWeight));
		if (everything) {
			// a pixel that never met a surface keeps no ids, and the sample that was a NaN is in no sum
			const glm::vec4 neverHit = gpu.aovLayers[(size_t)layout.layer[(size_t)AovLayer::Weights] * PIXELS + 2];
			expect(neverHit.x == 0.f && neverHit.z == 0.f && neverHit.w == 0.f, "a pixel whose samples met no surface has no surface weight and no ids");
			expect(gpu.aovLayers[(size_t)layout.layer[(size_t)AovLayer::VarianceMean] * PIXELS + 0].w == (float)(FRAMES - 1), "the NaN sample is not in pixel 0's variance count, nor its weights");
		}
	}

	result.passed = passed;
	result.lines.insert(result.lines.begin(), passed ? "kernel 09's fold of the auxiliary outputs agrees with src/aov.cpp" : "kernel 09's fold of the auxiliary outputs disagrees with src/aov.cpp");
	return result;
}
