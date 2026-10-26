#include <rt_gpu.h>

#include <chrono>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rt_schedule.h>
#include <vk_engine.h>
#include <vk_readback.h>
#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_tonemap.h>

namespace {

//shaders/analysis_view.comp's push constants
struct AnalysisPushConstants {
	uint32_t view;
	uint32_t samples;
	uint32_t baseSamples;
	uint32_t hasReference;
	// 1 when binding 5 holds the compensated film's per-pixel record
	uint32_t hasFilmRecord;
};

//shaders/aov_view.comp's push constants
struct AovViewPushConstants {
	uint32_t view;
	uint32_t layers;
	uint32_t maxSamples;
	float depthScale;
};

//shaders/relight.comp's push constants: scale x tint per group, rgb
struct RelightPushConstants {
	uint32_t width;
	uint32_t height;
	uint32_t groupCount;
	uint32_t buckets;
	glm::vec4 gain[MAX_LIGHT_GROUPS];
};

//the selected sampler writes the RaySamples record: kernels 00 and 06 are built to read it
bool samplerWritesRecord(const KernelSelection& selection)
{
	const KernelVariant* sampler = selection.selected(KernelSlot::GenerateSamples);
	return sampler != nullptr && sampler->implemented && sampler->writesRecord;
}

//shaders/image_error.comp's workgroup, each of which leaves one vec2 of partial sums
constexpr uint32_t ERROR_WORKGROUP = 16;

constexpr VkDeviceSize TRAVERSAL_STATS_BYTES = sizeof(uint32_t) * CRT_TRAVERSAL_STATS_WORDS;

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}


}

const char* crtDebugViewName(CrtDebugView view)
{
	switch (view) {
	case CrtDebugView::None:
		return "none";
	case CrtDebugView::PrimaryDirection:
		return "primary direction";
	case CrtDebugView::HitMiss:
		return "hit / miss";
	case CrtDebugView::Normal:
		return "normal";
	case CrtDebugView::BounceHeat:
		return "bounce heat";
	case CrtDebugView::SampleCountHeat:
		return "sample-count heat";
	case CrtDebugView::TraversalCost:
		return "BVH traversal cost";
	case CrtDebugView::MisWeights:
		return "MIS weights (red BSDF, green light)";
	case CrtDebugView::NormalDisagreement:
		return "shading vs geometric normal (red past 90 degrees)";
	case CrtDebugView::AmbientOcclusion:
		return "ambient occlusion";
	case CrtDebugView::Count:
		break;
	}
	return "unknown";
}

const char* crtDebugViewId(CrtDebugView view)
{
	static const char* const ids[(size_t)CrtDebugView::Count] = { "none", "direction", "hit", "normal", "bounce", "samples", "traversal", "mis", "disagreement", "ao" };
	return view < CrtDebugView::Count ? ids[(size_t)view] : "";
}

CrtDebugView crtDebugViewFromId(const std::string& id)
{
	for (uint32_t i = 0; i < (uint32_t)CrtDebugView::Count; i++) {
		if (id == crtDebugViewId((CrtDebugView)i)) {
			return (CrtDebugView)i;
		}
	}
	return CrtDebugView::Count;
}

const char* crtAnalysisViewName(CrtAnalysisView view)
{
	switch (view) {
	case CrtAnalysisView::None:
		return "Picture";
	case CrtAnalysisView::ReferenceError:
		return "Error against reference";
	case CrtAnalysisView::ReferenceDifference:
		return "Difference from reference";
	case CrtAnalysisView::Noise:
		return "Noise (convergence)";
	case CrtAnalysisView::EnergyAboveOne:
		return "Energy above 1";
	case CrtAnalysisView::DroppedSamples:
		return "Dropped samples";
	case CrtAnalysisView::StandardError:
		return "Standard error";
	case CrtAnalysisView::Count:
		break;
	}
	return "?";
}

const char* crtAnalysisViewDescription(CrtAnalysisView view)
{
	switch (view) {
	case CrtAnalysisView::None:
		return "The rendered picture.";
	case CrtAnalysisView::ReferenceError:
		return "Each pixel's error against the reference: the square root of (render - reference)^2 / (reference^2 + 0.01), on a log scale from 0.1% (blue) to 100% (red). Needs a reference of the render's size (Compare to reference); magenta without one.";
	case CrtAnalysisView::ReferenceDifference:
		return "Render minus reference, relative to the reference: red where the render is brighter, blue where it is darker. Noise alternates between the two; a bias is one colour over an area. Needs a reference of the render's size.";
	case CrtAnalysisView::Noise:
		return "An estimate of each pixel's remaining error from how much it changed since the film held half the samples, on the log scale of the error view. It needs no reference and falls as the render converges. Grey until the film has doubled once.";
	case CrtAnalysisView::EnergyAboveOne:
		return "Pixels with a channel above 1 in red, brighter with the excess; the rest in grey. For furnace scenes, where a passive material must not exceed 1.";
	case CrtAnalysisView::DroppedSamples:
		return "Pixels where kernel 09 dropped a non-finite sample, in red; the rest dim grey. A dropped sample biases its pixel, so any red is a NaN or an infinity in a path.";
	case CrtAnalysisView::StandardError:
		return "The standard error of each pixel's mean over the mean, from the running variance of its samples' luminance, on the log scale of the error view. Needs no reference; grey with the Running mean film, which keeps no variance.";
	case CrtAnalysisView::Count:
		break;
	}
	return "";
}

const char* crtAovViewName(CrtAovView view)
{
	switch (view) {
	case CrtAovView::Position:
		return "AOV: Position";
	case CrtAovView::Depth:
		return "AOV: Depth";
	case CrtAovView::Normal:
		return "AOV: Normal";
	case CrtAovView::ShadingNormal:
		return "AOV: Shading normal";
	case CrtAovView::Uv:
		return "AOV: UV";
	case CrtAovView::Albedo:
		return "AOV: Albedo";
	case CrtAovView::InstanceId:
		return "AOV: Instance id";
	case CrtAovView::MaterialId:
		return "AOV: Material id";
	case CrtAovView::Variance:
		return "AOV: Variance";
	case CrtAovView::SampleCount:
		return "AOV: Sample count";
	case CrtAovView::Count:
		break;
	}
	return "?";
}

AovOutput crtAovViewOutput(CrtAovView view)
{
	switch (view) {
	case CrtAovView::Position:
		return AovOutput::Position;
	case CrtAovView::Depth:
		return AovOutput::Depth;
	case CrtAovView::Normal:
		return AovOutput::Normal;
	case CrtAovView::ShadingNormal:
		return AovOutput::ShadingNormal;
	case CrtAovView::Uv:
		return AovOutput::Uv;
	case CrtAovView::Albedo:
		return AovOutput::Albedo;
	case CrtAovView::InstanceId:
	case CrtAovView::MaterialId:
		return AovOutput::Ids;
	case CrtAovView::Variance:
		return AovOutput::Variance;
	case CrtAovView::SampleCount:
	case CrtAovView::Count:
		break;
	}
	return AovOutput::SampleCount;
}

uint64_t aovImageBytes(const AovSettings& aov, uint32_t width, uint32_t height)
{
	return (uint64_t)makeAovLayout(aov).count * width * height * sizeof(glm::vec4);
}

uint64_t lightGroupSumBytes(uint32_t groups, uint32_t width, uint32_t height, uint32_t buckets)
{
	return groups == 0 && buckets == 0 ? 0 : (uint64_t)width * height * (3ull * groups + 1ull + buckets) * sizeof(float);
}

OutputsFallback outputsFallback(const AovSettings& aov, uint32_t lightGroupCount, uint32_t width, uint32_t height)
{
	OutputsFallback fallback;
	if (aovImageBytes(aov, width, height) > OUTPUT_BYTES_LIMIT) {
		fallback.aov = false;
		fallback.reason = fmt::format("The auxiliary outputs take {:.0f} MB at {} x {}, more than {} MB, so this size renders without them.",
			double(aovImageBytes(aov, width, height)) / (1024.0 * 1024.0), width, height, OUTPUT_BYTES_LIMIT >> 20);
	}
	if (lightGroupSumBytes(lightGroupCount, width, height) > OUTPUT_BYTES_LIMIT) {
		fallback.lightGroups = false;
		if (!fallback.reason.empty()) {
			fallback.reason += " ";
		}
		fallback.reason += fmt::format("The light groups' sums take {:.0f} MB at {} x {}, more than {} MB, so this size renders without light groups.",
			double(lightGroupSumBytes(lightGroupCount, width, height)) / (1024.0 * 1024.0), width, height, OUTPUT_BYTES_LIMIT >> 20);
	}
	return fallback;
}

VkDescriptorType crtBindingType(CrtBinding binding)
{
	switch (binding) {
	case CrtBinding::Accumulation:
	case CrtBinding::SamplesTaken:
	case CrtBinding::AovImages:
		return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	case CrtBinding::EnvironmentMap:
	case CrtBinding::MaterialTextures:
		return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	default:
		return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	}
}

//---------------------------------------------------------------- the resource table

void CrtResourceTable::setBuffer(CrtBinding binding, VkBuffer buffer, VkDeviceSize range, VkDeviceSize offset)
{
	m_entries[(size_t)binding] = Entry { .published = true, .buffer = buffer, .offset = offset, .range = range };
}

void CrtResourceTable::setImage(CrtBinding binding, VkImageView view, VkImageLayout layout, VkSampler sampler)
{
	m_entries[(size_t)binding] = Entry { .published = true, .view = view, .sampler = sampler, .layout = layout };
}

void CrtResourceTable::clear(CrtBinding binding)
{
	m_entries[(size_t)binding] = Entry {};
}

void CrtResourceTable::write(DescriptorWriter& writer, CrtBinding binding) const
{
	const Entry& entry = m_entries[(size_t)binding];
	if (!entry.published) {
		//a kernel declared a binding that no owner has published: its descriptor would name
		//whatever it named last, or nothing. A programming error, and the kind that shows up as an
		//intermittent wrong image rather than a crash, so it stops here
		fmt::println("CrtResourceTable: binding {} is declared by a kernel but no owner published it", (uint32_t)binding);
		abort();
	}
	const VkDescriptorType type = crtBindingType(binding);
	if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
		writer.writeBuffer((int)binding, entry.buffer, entry.range, entry.offset, type);
	} else {
		writer.writeImage((int)binding, entry.view, entry.sampler, entry.layout, type);
	}
}

//---------------------------------------------------------------- lifetime

GpuRenderer::GpuRenderer()
{
	for (uint32_t i = 0; i < (uint32_t)IntegratorId::Count; i++) {
		m_schedules.push_back(createSchedule((IntegratorId)i));
	}
}

GpuRenderer::~GpuRenderer() = default;

void GpuRenderer::init(VulkanEngine* engine)
{
	const std::string shaderDir = engine->m_rootPath + "shaders/";

	//timestamps are optional on the device; the readout is simply absent without them
	const VkPhysicalDeviceLimits& limits = engine->m_gpuProperties.limits;
	m_hasTimestamps = limits.timestampComputeAndGraphics == VK_TRUE && limits.timestampPeriod > 0.f;
	m_timestampPeriodNs = limits.timestampPeriod;
	if (m_hasTimestamps) {
		VkQueryPoolCreateInfo queryInfo { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queryInfo.queryCount = 2 * FRAME_OVERLAP;
		checkVkResult(vkCreateQueryPool(engine->m_device, &queryInfo, nullptr, &m_queryPool));

		//queries must be reset before their first use; the per-frame reset covers every later one
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			vkCmdResetQueryPool(cmd, m_queryPool, 0, 2 * FRAME_OVERLAP);
		});
	}
	fmt::println("GpuRenderer: timestamps {} (period {} ns)", m_hasTimestamps ? "available" : "unavailable", m_timestampPeriodNs);

	m_traversalStats = engine->createBuffer(TRAVERSAL_STATS_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_statsReadback = engine->createBuffer((VkDeviceSize)FRAME_OVERLAP * TRAVERSAL_STATS_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	m_resources.setBuffer(CrtBinding::TraversalStats, m_traversalStats.buffer, TRAVERSAL_STATS_BYTES);
	m_slotRender.assign(FRAME_OVERLAP, 0);
	m_slotErrorGroups.assign(FRAME_OVERLAP, 0);
	m_slotErrorSamples.assign(FRAME_OVERLAP, 0);

	//the reference measurement: not a wavefront kernel, a plain image pass over two images
	m_errorPass = ComputePassBuilder(shaderDir + "image_error.comp.spv")
		.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.setWorkgroupSize(ERROR_WORKGROUP, ERROR_WORKGROUP)
		.build(engine->m_device);

	//the analysis views: a plain image pass over the film, the reference, the noise base and the display image
	m_analysisPass = ComputePassBuilder(shaderDir + "analysis_view.comp.spv")
		.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.setPushConstants<AnalysisPushConstants>()
		.build(engine->m_device);

	//the auxiliary outputs' pictures, and Relight: plain passes over the sums the render keeps
	m_aovViewPass = ComputePassBuilder(shaderDir + "aov_view.comp.spv")
		.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.setPushConstants<AovViewPushConstants>()
		.build(engine->m_device);
	m_relightPass = ComputePassBuilder(shaderDir + "relight.comp.spv")
		.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.setPushConstants<RelightPushConstants>()
		.build(engine->m_device);

	//the film's two bindings must always name a buffer too: the pixel filter's table, and the film record
	//(a stub until a render keeps one)
	uploadFilterTable(engine, PixelFilterSettings {});
	ensureFilmRecords(engine, 1, 1, false, false);
	//and the outputs' bindings: stubs until a render keeps any
	ensureOutputs(engine, 1, 1);
	//the environment tables' binding must always name a buffer, a map or no map
	uploadEnvironment(engine, nullptr);
	uploadSamplerTables(engine);

	//what a new scene renders with, so its first render does not stall on pipeline creation
	preparePipelines(engine, defaultKernelSelection());
}

void GpuRenderer::destroy(VulkanEngine* engine)
{
	if (m_scheduleActive) {
		m_schedules[(size_t)m_activeIntegrator]->release(*this, engine);
		m_scheduleActive = false;
	}
	m_pool.destroy(engine, m_resources);
	freeFilm(engine);

	if (m_sceneBuffer.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_sceneBuffer);
		m_sceneBuffer = {};
	}
	for (AllocatedBuffer* buffer : { &m_spectralData, &m_upliftBuffer }) {
		if (buffer->buffer != VK_NULL_HANDLE) {
			engine->destroyBuffer(*buffer);
			*buffer = {};
		}
	}
	if (m_geometryBuffer.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_geometryBuffer);
		m_geometryBuffer = {};
	}
	m_uploadedScene.reset();
	m_uploadedGeometry.reset();
	if (m_environmentBuffer.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_environmentBuffer);
		m_environmentBuffer = {};
	}
	m_uploadedEnvironment.reset();
	m_hasEnvironmentBuffer = false;
	if (m_samplerTableBuffer.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_samplerTableBuffer);
		m_samplerTableBuffer = {};
	}
	if (m_filmRecords.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_filmRecords);
		engine->destroyBuffer(m_filmCascade);
		m_filmRecords = {};
		m_filmCascade = {};
	}
	if (m_filterBuffer.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_filterBuffer);
		m_filterBuffer = {};
		m_hasFilterBuffer = false;
	}
	clearReference(engine);
	if (m_errorPartials.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_errorPartials);
		engine->destroyBuffer(m_errorReadback);
		m_errorPartials = {};
		m_errorReadback = {};
		m_errorPartialsBytes = 0;
	}
	destroyAovViews(engine);
	if (m_aovImage.image != VK_NULL_HANDLE) {
		engine->destroyImage(m_aovImage);
		m_aovImage = {};
	}
	if (m_channelSums.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_channelSums);
		m_channelSums = {};
	}
	if (m_hasRelit) {
		engine->destroyImage(m_relit);
		m_relit = {};
		m_hasRelit = false;
	}
	m_aovViewPass.destroy(engine->m_device);
	m_relightPass.destroy(engine->m_device);
	m_errorPass.destroy(engine->m_device);
	m_analysisPass.destroy(engine->m_device);
	if (m_traversalStats.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_traversalStats);
		engine->destroyBuffer(m_statsReadback);
		m_traversalStats = {};
		m_statsReadback = {};
	}
	if (m_queryPool != VK_NULL_HANDLE) {
		vkDestroyQueryPool(engine->m_device, m_queryPool, nullptr);
		m_queryPool = VK_NULL_HANDLE;
	}

	for (auto& [key, pass] : m_passes) {
		pass.destroy(engine->m_device);
	}
	m_passes.clear();
	m_resources = {};
}

//---------------------------------------------------------------- pipelines

std::vector<CrtBinding> GpuRenderer::bindingsOf(const KernelVariant& variant, uint32_t traversal, uint32_t family)
{
	std::vector<CrtBinding> bindings = variant.bindings;
	if (variant.laneDependent && family != 0) {
		bindings.insert(bindings.end(), { CrtBinding::SpectralData, CrtBinding::SpectralUplift, CrtBinding::PathSpectra });
	}
	if (variant.traces) {
		if (const TraversalVariant* search = traversalVariant(traversal)) {
			bindings.insert(bindings.end(), search->bindings.begin(), search->bindings.end());
		}
	}
	//each once: a kernel that samples textures itself and a traversal whose alpha test does both
	//name MaterialTextures, and a set layout may hold a binding number only once
	std::sort(bindings.begin(), bindings.end());
	bindings.erase(std::unique(bindings.begin(), bindings.end()), bindings.end());
	return bindings;
}

const ComputePass& GpuRenderer::pass(VulkanEngine* engine, const KernelVariant& variant, uint32_t traversal, MaterialClass materialClass, uint32_t features)
{
	const uint32_t key = variant.traces ? traversal : NO_TRAVERSAL;
	const uint32_t classKey = variant.perMaterialClass ? (uint32_t)materialClass : NO_CLASS;
	//the features that matter to this kernel: bit 0 the sample record to a kernel that reads it, bits 1 and 2 the
	//outputs to a kernel that writes or folds them
	const uint32_t family = variant.laneDependent ? (features >> 8u) & 0xFFu : 0u;
	const uint32_t recordKey = (variant.readsRecord ? (features & 1u) : 0u) | (variant.usesOutputs ? (features & 14u) : 0u) | (family << 8u)
		| (family != 0 ? features & 0xFF0000u : 0u);
	const auto found = m_passes.find({ &variant, key, classKey, recordKey });
	if (found != m_passes.end()) {
		return found->second;
	}

	//Every kernel variant becomes one ComputePass - one per traversal if it traces - built from the
	//bindings IT declares and nothing else (src/rt_kernels.h). A kernel that reads three buffers gets
	//a three-binding layout at the global binding numbers of shaders/rt/include/crt_common.glsl - a
	//set layout with gaps is legal, and keeping the numbers global is what lets every kernel share
	//one include file while binding a different subset of it.
	//
	//Adding a strategy therefore adds a table entry and nothing here.
	std::string shader = engine->m_rootPath + "shaders/" + variant.shader;
	if (variant.traces) {
		const TraversalVariant* search = traversalVariant(traversal);
		shader += std::string(".") + (search != nullptr ? search->id : "none");
	}
	if (variant.perMaterialClass) {
		shader += std::string(".") + materialClassId(materialClass);
	}
	shader += laneFamilySuffix(family);
	shader += ".spv";

	ComputePassBuilder builder(shader);
	for (const CrtBinding binding : bindingsOf(variant, traversal, family)) {
		builder.addBinding((uint32_t)binding, crtBindingType(binding));
	}
	//a kernel that draws its numbers through crt_samples.glsl is specialized for whether there is a
	//record to read them from (CRT_SAMPLE_RECORD), so without one it is the code it always was
	if (variant.readsRecord) {
		builder.addSpecialization(0, recordKey & 1u);
	}
	//and for whether the render keeps auxiliary outputs (CRT_AOV) and light groups (CRT_LIGHT_GROUPS), the same
	//way, so a render that keeps neither runs the code it always did
	if (variant.usesOutputs) {
		builder.addSpecialization(1, (recordKey >> 1) & 1u);
		builder.addSpecialization(2, (recordKey >> 2) & 1u);
		builder.addSpecialization(5, (recordKey >> 3) & 1u);
	}
	//a spectral build, for its representation (CRT_REPRESENTATION_SPEC) and wavelength sampling (CRT_SAMPLING_SPEC),
	//so it carries one representation's code (crt_spectrum.glsl)
	if (family != 0) {
		builder.addSpecialization(3, (features >> 16u) & 0xFu);
		builder.addSpecialization(4, (features >> 20u) & 0xFu);
	}
	//MoltenVK compiles the shader to Metal here, which takes seconds for a large kernel the first time a build is used
	//on a machine (Metal caches it afterwards): a slow one is reported, so a pause has a name
	const auto before = std::chrono::steady_clock::now();
	const auto inserted = m_passes.emplace(std::make_tuple(&variant, key, classKey, recordKey), builder
		.setPushConstants<CrtParams>()
		.setWorkgroupSize(CRT_WORKGROUP)
		.build(engine->m_device));
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
	if (seconds > 0.25) {
		fmt::println("GpuRenderer: built {} in {:.1f} s", shader.substr(shader.find("shaders/") + 8), seconds);
	}
	return inserted.first->second;
}

const ComputePass& GpuRenderer::pass(VulkanEngine* engine, const KernelVariant& variant, MaterialClass materialClass)
{
	return pass(engine, variant, m_snapshot.kernels.traversal, materialClass, pipelineFeatures(m_snapshot.kernels));
}

uint32_t GpuRenderer::pipelineFeatures(const KernelSelection& selection) const
{
	//the spectral build's specialization: the representation in bits 16 to 19, the wavelength sampling in 20 to 23
	const SpectralSettings& spectral = m_snapshot.settings.spectral;
	const uint32_t family = laneFamily(spectral);
	const uint32_t spectralBits = family != 0 ? ((uint32_t)spectral.representation << 16u) | ((uint32_t)spectral.sampling << 20u) : 0u;
	return (samplerWritesRecord(selection) ? 1u : 0u) | m_outputFeatures | (family << 8u) | spectralBits;
}

void GpuRenderer::preparePipelines(VulkanEngine* engine, const KernelSelection& selection)
{
	for (const KernelSlot slot : selection.selectedIntegrator().stages) {
		const KernelVariant* variant = selection.selected(slot);
		if (variant == nullptr || !variant->implemented) {
			continue;
		}
		const uint32_t classes = variant->perMaterialClass ? (uint32_t)MaterialClass::Count : 1u;
		for (uint32_t materialClass = 0; materialClass < classes; materialClass++) {
			pass(engine, *variant, selection.traversal, (MaterialClass)materialClass, pipelineFeatures(selection));
		}
	}
}

void GpuRenderer::dispatch(VkCommandBuffer cmd, VulkanEngine* engine, const KernelVariant& variant, const CrtParams& params, const DispatchSize& size,
	MaterialClass materialClass)
{
	const uint32_t traversal = m_snapshot.kernels.traversal;
	const uint32_t features = pipelineFeatures(m_snapshot.kernels);
	const ComputePass& computePass = pass(engine, variant, traversal, materialClass, features);
	const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, computePass.setLayout);

	//a kernel gets only the bindings it declared, written from whatever their owners published
	DescriptorWriter writer;
	for (const CrtBinding binding : bindingsOf(variant, traversal, variant.laneDependent ? (features >> 8u) & 0xFFu : 0u)) {
		m_resources.write(writer, binding);
	}
	writer.updateSet(engine->m_device, set);

	if (size.indirectBuffer != VK_NULL_HANDLE) {
		dispatchComputePassIndirect(cmd, computePass, set, &params, size.indirectBuffer, size.indirectOffset);
	} else {
		dispatchComputePass(cmd, computePass, set, &params, { size.groups, 1, 1 });
	}
}

//---------------------------------------------------------------- the spectral data

TonemapDisplay GpuRenderer::tonemapDisplay() const
{
	TonemapDisplay display;
	const SpectralSettings& spectral = m_snapshot.settings.spectral;
	if (spectral.representation != SpectralRepresentation::Monochromatic) {
		return display;
	}
	switch (spectralDisplay.mode) {
	case MonochromeDisplay::Grayscale:
	case MonochromeDisplay::Count:
		break;
	case MonochromeDisplay::WavelengthColor:
		display.mode = TonemapDisplay::Mode::Tint;
		display.tint = wavelengthDisplayColor(spectral.monochromaticWavelength, spectralDisplay);
		break;
	case MonochromeDisplay::Colormap:
		display.mode = TonemapDisplay::Mode::Colormap;
		display.colormap = (uint32_t)spectralDisplay.colormap;
		display.decades = spectralDisplay.logarithmic ? std::max(spectralDisplay.decades, 0.1f) : 0.f;
		break;
	}
	return display;
}

void GpuRenderer::uploadSpectral(VulkanEngine* engine)
{
	m_spectralNote.clear();
	SpectralSettings& spectral = m_snapshot.settings.spectral;
	if (laneFamily(spectral) == 0) {
		return;
	}
	//the working space's uplift table, which the build writes; a spectral render without one runs in exact RGB
	std::string error;
	const UpliftTable* table = upliftTable(engine->m_rootPath, spectral.colorSpace, &error);
	if (table == nullptr) {
		m_spectralNote = fmt::format("No uplift table for {}, so the render runs in exact RGB: {}", colorSpace(spectral.colorSpace).name, error);
		fmt::println("GpuRenderer: {}", m_spectralNote);
		spectral.representation = SpectralRepresentation::ExactRgb;
		m_spectralBuckets = 0;
		m_outputFeatures &= ~8u;
		return;
	}
	const SpectrumContext context { table, spectral.colorSpace };

	//binding 25, once per working space: the z nodes, then the coefficients
	if (m_upliftSpace != spectral.colorSpace || m_upliftBuffer.buffer == VK_NULL_HANDLE) {
		retireBuffer(engine, m_upliftBuffer);
		const VkDeviceSize bytes = sizeof(float) * (table->zNodes.size() + table->data.size());
		m_upliftBuffer = engine->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
		float* mapped = (float*)m_upliftBuffer.info.pMappedData;
		std::copy(table->zNodes.begin(), table->zNodes.end(), mapped);
		std::copy(table->data.begin(), table->data.end(), mapped + table->zNodes.size());
		vmaFlushAllocation(engine->m_memAllocator, m_upliftBuffer.allocation, 0, bytes); //flush vma on MoltenVK
		m_upliftSpace = spectral.colorSpace;
		m_resources.setBuffer(CrtBinding::SpectralUplift, m_upliftBuffer.buffer, bytes);
	}

	//binding 24: the representation's tables and the scene's spectra. The Lights sampling follows every light's
	//spectrum by its power
	std::vector<EmitterSpectrum> emitters;
	if (m_snapshot.scene != nullptr && spectral.sampling == WavelengthSampling::Lights) {
		const auto& list = m_snapshot.scene->spectra.list();
		for (const GpuLight& light : m_snapshot.scene->lights.lights) {
			if (light.spectrum < list.size()) {
				emitters.push_back({ list[light.spectrum].first, light.power });
			}
		}
	}
	const WavelengthDistribution distribution = buildWavelengthDistribution(emitters, spectral, context);
	SpectralDataBuilder builder(spectral, context, distribution, m_spectralBuckets);
	if (m_snapshot.scene != nullptr) {
		builder.setScene(m_snapshot.scene->spectra, m_snapshot.scene->materialSpectra);
	}
	builder.setBackground(rgbSpectrum(m_snapshot.backgroundColor));
	const std::vector<uint32_t> words = builder.finish();
	retireBuffer(engine, m_spectralData);
	const VkDeviceSize bytes = sizeof(uint32_t) * words.size();
	m_spectralData = engine->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	std::copy(words.begin(), words.end(), (uint32_t*)m_spectralData.info.pMappedData);
	vmaFlushAllocation(engine->m_memAllocator, m_spectralData.allocation, 0, bytes); //flush vma on MoltenVK
	m_resources.setBuffer(CrtBinding::SpectralData, m_spectralData.buffer, bytes);
}

//---------------------------------------------------------------- the film

void GpuRenderer::ensureFilm(VulkanEngine* engine, uint32_t width, uint32_t height)
{
	if (m_hasFilm && m_accumulation.imageExtent.width == width && m_accumulation.imageExtent.height == height) {
		return;
	}
	//the previous render's frames may still be writing the old images
	vkDeviceWaitIdle(engine->m_device);
	freeFilm(engine);

	const VkExtent3D extent { width, height, 1 };
	m_accumulation = engine->createImage(extent, TonemapPass::LINEAR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	m_samplesTaken = engine->createImage(extent, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	m_noiseBase = engine->createImage(extent, TonemapPass::LINEAR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	m_noiseBaseSamples = 0;
	m_nextNoiseBase = 0;

	//both images live in GENERAL for good: written by the film stage, read by the tonemap and
	//cleared by the command buffer, all of which accept it
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_accumulation.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, m_samplesTaken.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, m_noiseBase.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	});
	m_resources.setImage(CrtBinding::Accumulation, m_accumulation.imageView, VK_IMAGE_LAYOUT_GENERAL);
	m_resources.setImage(CrtBinding::SamplesTaken, m_samplesTaken.imageView, VK_IMAGE_LAYOUT_GENERAL);
	m_hasFilm = true;
}

void GpuRenderer::freeFilm(VulkanEngine* engine)
{
	if (!m_hasFilm) {
		return;
	}
	engine->destroyImage(m_accumulation);
	engine->destroyImage(m_samplesTaken);
	engine->destroyImage(m_noiseBase);
	m_accumulation = m_samplesTaken = m_noiseBase = {};
	m_resources.clear(CrtBinding::Accumulation);
	m_resources.clear(CrtBinding::SamplesTaken);
	m_hasFilm = false;
	m_hasImage = false;
}

void GpuRenderer::ensureFilmRecords(VulkanEngine* engine, uint32_t width, uint32_t height, bool needed, bool cascade)
{
	const uint32_t pixels = needed ? width * height : 1u;
	const uint32_t cascadePixels = cascade ? pixels : 1u;
	if (m_filmRecords.buffer != VK_NULL_HANDLE && m_filmRecordsLive == needed && m_filmRecordPixels == pixels && m_filmCascadeLive == cascade && m_filmCascadePixels == cascadePixels) {
		return;
	}
	//the previous render's frames may still be reading the old record
	vkDeviceWaitIdle(engine->m_device);
	if (m_filmRecords.buffer != VK_NULL_HANDLE) {
		engine->destroyBuffer(m_filmRecords);
		engine->destroyBuffer(m_filmCascade);
	}
	m_filmRecordBytes = sizeof(CrtFilmPixel) * (VkDeviceSize)pixels;
	m_filmCascadeBytes = FILM_CASCADE_BYTES_PER_PIXEL * (VkDeviceSize)cascadePixels;
	const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	m_filmRecords = engine->createBuffer(m_filmRecordBytes, usage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_filmCascade = engine->createBuffer(m_filmCascadeBytes, usage, VMA_MEMORY_USAGE_GPU_ONLY);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkCmdFillBuffer(cmd, m_filmRecords.buffer, 0, VK_WHOLE_SIZE, 0);
		vkCmdFillBuffer(cmd, m_filmCascade.buffer, 0, VK_WHOLE_SIZE, 0);
	});
	m_resources.setBuffer(CrtBinding::AccumulationError, m_filmRecords.buffer, m_filmRecordBytes);
	m_resources.setBuffer(CrtBinding::FilmCascade, m_filmCascade.buffer, m_filmCascadeBytes);
	m_filmRecordsLive = needed;
	m_filmRecordPixels = pixels;
	m_filmCascadeLive = cascade;
	m_filmCascadePixels = cascadePixels;
	if (needed) {
		fmt::println("GpuRenderer: film record {} x {} x {} bytes = {:.0f} MB{}", width, height, sizeof(CrtFilmPixel), double(m_filmRecordBytes) / (1024.0 * 1024.0),
			cascade ? fmt::format(", cascade {:.0f} MB", double(m_filmCascadeBytes) / (1024.0 * 1024.0)) : std::string());
	}
}

//---------------------------------------------------------------- the outputs

void GpuRenderer::ensureOutputs(VulkanEngine* engine, uint32_t width, uint32_t height)
{
	//a render that keeps none gets a one-texel image and a few bytes, so the bindings always name something
	const bool aovLive = m_aovLayout.count > 0;
	const VkExtent2D extent = aovLive ? VkExtent2D { width, height } : VkExtent2D { 1, 1 };
	const uint32_t layers = aovLive ? m_aovLayout.count : 1u;
	const bool sumsLive = m_lightGroupCount > 0 || m_spectralBuckets > 0;
	const VkDeviceSize sumBytes = sumsLive ? lightGroupSumBytes(m_lightGroupCount, width, height, m_spectralBuckets) : 16;

	const bool imageSame = m_aovImage.image != VK_NULL_HANDLE && m_aovImageExtent.width == extent.width && m_aovImageExtent.height == extent.height && m_aovImageLayers == layers;
	const bool sumsSame = m_channelSums.buffer != VK_NULL_HANDLE && m_channelSumBytes == sumBytes;
	if (imageSame && sumsSame) {
		return;
	}
	//the previous render's frames may still use the old ones
	vkDeviceWaitIdle(engine->m_device);
	if (!imageSame) {
		if (m_aovImage.image != VK_NULL_HANDLE) {
			engine->destroyImage(m_aovImage);
		}
		m_aovImage = engine->createImageArray({ extent.width, extent.height, 1 }, layers, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			vkutil::transition_image(cmd, m_aovImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			const VkClearColorValue zero { { 0.f, 0.f, 0.f, 0.f } };
			const VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
			vkCmdClearColorImage(cmd, m_aovImage.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
		});
		m_aovImageExtent = extent;
		m_aovImageLayers = layers;
		m_resources.setImage(CrtBinding::AovImages, m_aovImage.imageView, VK_IMAGE_LAYOUT_GENERAL);
		if (aovLive) {
			fmt::println("GpuRenderer: auxiliary outputs {} x {} x {} layers = {:.0f} MB", extent.width, extent.height, layers, double(aovImageBytes(m_aov, width, height)) / (1024.0 * 1024.0));
		}
	}
	if (!sumsSame) {
		if (m_channelSums.buffer != VK_NULL_HANDLE) {
			engine->destroyBuffer(m_channelSums);
		}
		m_channelSums = engine->createBuffer(sumBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			vkCmdFillBuffer(cmd, m_channelSums.buffer, 0, VK_WHOLE_SIZE, 0);
		});
		m_channelSumBytes = sumBytes;
		m_resources.setBuffer(CrtBinding::ChannelSums, m_channelSums.buffer, sumBytes);
		if (sumsLive) {
			fmt::println("GpuRenderer: channel sums {} x {} x ({} groups, {} spectral buckets) = {:.0f} MB", width, height, m_lightGroupCount, m_spectralBuckets, double(sumBytes) / (1024.0 * 1024.0));
		}
	}
}

void GpuRenderer::ensureRelitImage(VulkanEngine* engine)
{
	const VkExtent3D extent = m_accumulation.imageExtent;
	if (m_hasRelit && m_relit.imageExtent.width == extent.width && m_relit.imageExtent.height == extent.height) {
		return;
	}
	vkDeviceWaitIdle(engine->m_device);
	if (m_hasRelit) {
		engine->destroyImage(m_relit);
	}
	m_relit = engine->createImage(extent, TonemapPass::LINEAR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_relit.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	});
	m_hasRelit = true;
}

bool GpuRenderer::hasAovView(CrtAovView view) const
{
	for (const AovViewImage& entry : m_aovViews) {
		if (entry.view == view) {
			return true;
		}
	}
	return false;
}

void GpuRenderer::destroyAovViews(VulkanEngine* engine)
{
	if (m_aovViews.empty()) {
		return;
	}
	//unregister first so no new frame references a view, then wait out the ones that already do
	for (const AovViewImage& entry : m_aovViews) {
		engine->m_displayRegistry.unregisterImage(entry.name);
	}
	vkDeviceWaitIdle(engine->m_device);
	for (const AovViewImage& entry : m_aovViews) {
		engine->destroyImage(entry.image);
	}
	m_aovViews.clear();
}

void GpuRenderer::syncAovViews(VulkanEngine* engine)
{
	//the views the render's outputs call for, at the render's size
	std::vector<CrtAovView> wanted;
	for (uint32_t i = 0; i < (uint32_t)CrtAovView::Count; i++) {
		const CrtAovView view = (CrtAovView)i;
		const AovOutput output = crtAovViewOutput(view);
		const bool on = output == AovOutput::SampleCount ? m_aov.has(AovOutput::SampleCount) : (m_aov.has(output) && m_aovLayout.count > 0);
		if (on) {
			wanted.push_back(view);
		}
	}
	bool same = wanted.size() == m_aovViews.size();
	for (size_t i = 0; same && i < wanted.size(); i++) {
		same = m_aovViews[i].view == wanted[i] && m_aovViews[i].image.imageExtent.width == m_snapshot.width && m_aovViews[i].image.imageExtent.height == m_snapshot.height;
	}
	if (same) {
		return;
	}
	destroyAovViews(engine);
	for (const CrtAovView view : wanted) {
		AovViewImage entry;
		entry.view = view;
		entry.name = crtAovViewName(view);
		entry.image = engine->createImage({ m_snapshot.width, m_snapshot.height, 1 }, TonemapPass::DISPLAY_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		//black until the first pass writes it, and in the layout the registry reads it in
		AllocatedImage image = entry.image;
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			vkutil::transition_image(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			const VkClearColorValue black { { 0.f, 0.f, 0.f, 1.f } };
			const VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
			vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
			vkutil::transition_image(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});
		engine->m_displayRegistry.registerImage(entry.name, entry.image.imageView, { m_snapshot.width, m_snapshot.height });
		m_aovViews.push_back(std::move(entry));
	}
}

void GpuRenderer::recordAovViews(VkCommandBuffer cmd, VulkanEngine* engine)
{
	if (m_aovViews.empty()) {
		return;
	}
	//kernel 09's sums were written before this, by the frame's schedule
	vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	for (const AovViewImage& entry : m_aovViews) {
		const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, m_aovViewPass.setLayout);
		DescriptorWriter writer;
		writer.writeImage(0, m_aovImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		writer.writeImage(1, m_samplesTaken.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		writer.writeImage(2, entry.image.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		writer.updateSet(engine->m_device, set);

		const AovViewPushConstants constants { (uint32_t)entry.view, m_aovLayout.pack(), m_maxSamples, m_depthScale };
		vkutil::transition_image(cmd, entry.image.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		dispatchComputePassOver(cmd, m_aovViewPass, set, &constants, entry.image.imageExtent);
		vkutil::transition_image(cmd, entry.image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
}

void GpuRenderer::recordRelight(VkCommandBuffer cmd, VulkanEngine* engine)
{
	const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, m_relightPass.setLayout);
	DescriptorWriter writer;
	writer.writeBuffer(0, m_channelSums.buffer, m_channelSumBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.writeImage(1, m_relit.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.updateSet(engine->m_device, set);

	RelightPushConstants constants {};
	constants.width = m_accumulation.imageExtent.width;
	constants.height = m_accumulation.imageExtent.height;
	constants.groupCount = m_lightGroupCount;
	constants.buckets = m_spectralBuckets;
	for (uint32_t g = 0; g < MAX_LIGHT_GROUPS; g++) {
		constants.gain[g] = glm::vec4(relightGains[g].tint * relightGains[g].scale, 0.f);
	}
	//the sums were written by the frame's schedule (and the weight sum by kernel 09), before the tonemap reads this
	vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	dispatchComputePassOver(cmd, m_relightPass, set, &constants, m_relit.imageExtent);
	vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

bool GpuRenderer::readOutputs(VulkanEngine* engine, RenderOutputsData& data)
{
	if (!hasImage()) {
		return false;
	}
	data = {};
	data.width = m_accumulation.imageExtent.width;
	data.height = m_accumulation.imageExtent.height;
	data.aov = m_aov;
	data.layout = m_aovLayout;
	data.samplesPerPixel = m_samplesAccumulated;
	if (m_snapshot.settings.spectral.representation == SpectralRepresentation::Monochromatic) {
		data.monochromaticWavelength = m_snapshot.settings.spectral.monochromaticWavelength;
	}
	std::vector<uint8_t> bytes;
	if (m_aovLayout.count > 0) {
		if (!readbackImage(engine, m_aovImage.image, VK_IMAGE_LAYOUT_GENERAL, m_aovImage.imageExtent, m_aovLayout.count, sizeof(glm::vec4), bytes)) {
			return false;
		}
		data.aovLayers.resize(bytes.size() / sizeof(float));
		std::memcpy(data.aovLayers.data(), bytes.data(), bytes.size());
	}
	if (m_lightGroupCount > 0 || m_spectralBuckets > 0) {
		if (!readbackBuffer(engine, m_channelSums.buffer, lightGroupSumBytes(m_lightGroupCount, data.width, data.height, m_spectralBuckets), bytes)) {
			return false;
		}
		data.lightGroups = m_lightGroupCount;
		data.spectralBuckets = m_spectralBuckets;
		for (uint32_t b = 0; b < m_spectralBuckets; b++) {
			const glm::vec2 range = spectralBucketRange(m_snapshot.settings.spectral, b);
			data.spectralWavelengths.push_back(0.5f * (range.x + range.y));
		}
		data.groupSums.resize(bytes.size() / sizeof(float));
		std::memcpy(data.groupSums.data(), bytes.data(), bytes.size());
	}
	if (!readbackImage(engine, m_samplesTaken.image, VK_IMAGE_LAYOUT_GENERAL, m_samplesTaken.imageExtent, 1, sizeof(uint32_t), bytes)) {
		return false;
	}
	data.samplesTaken.resize(bytes.size() / sizeof(uint32_t));
	std::memcpy(data.samplesTaken.data(), bytes.data(), bytes.size());
	return true;
}

void GpuRenderer::uploadFilterTable(VulkanEngine* engine, const PixelFilterSettings& filter)
{
	if (m_hasFilterBuffer && m_uploadedFilter == filter) {
		return;
	}
	const std::vector<uint32_t> words = packFilterTable(buildFilterTable(filter));
	const VkDeviceSize bytes = sizeof(uint32_t) * words.size();

	AllocatedBuffer staging = engine->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(staging.info.pMappedData, words.data(), bytes);
	vmaFlushAllocation(engine->m_memAllocator, staging.allocation, 0, bytes); //flush vma on MoltenVK

	retireBuffer(engine, m_filterBuffer);
	m_filterBuffer = engine->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy copy {};
		copy.size = bytes;
		vkCmdCopyBuffer(cmd, staging.buffer, m_filterBuffer.buffer, 1, &copy);
	});
	engine->destroyBuffer(staging);
	m_resources.setBuffer(CrtBinding::FilterTable, m_filterBuffer.buffer, bytes);

	m_uploadedFilter = filter;
	m_hasFilterBuffer = true;
}

//---------------------------------------------------------------- the scene

void GpuRenderer::retireBuffer(VulkanEngine* engine, AllocatedBuffer& buffer)
{
	if (buffer.buffer == VK_NULL_HANDLE) {
		return;
	}
	//the previous render's buffer may still be read by the frame in flight, so it goes to the
	//deletion queue of the slot that frame used - flushed once its fence has been waited on, at the
	//start of the frame after next
	const AllocatedBuffer old = buffer;
	engine->m_frames[(engine->m_frameNumber + 1) % FRAME_OVERLAP].deletionQueue.push_function([engine, old]() {
		engine->destroyBuffer(old);
	});
	buffer = {};
}

VkDeviceSize GpuRenderer::geometryUploadBytes(const RaytraceGeometry* geometry, VkDeviceSize alignment)
{
	const size_t nodes = geometry != nullptr ? geometry->nodes.size() : 0;
	const size_t triangles = geometry != nullptr ? geometry->triangles.size() : 0;
	const size_t attributes = geometry != nullptr ? geometry->attributes.size() : 0;
	const VkDeviceSize trianglesOffset = alignUp(sizeof(glm::uvec4) * std::max<size_t>(nodes, 1), alignment);
	const VkDeviceSize attributesOffset = alignUp(trianglesOffset + sizeof(BvhTriangle) * std::max<size_t>(triangles, 1), alignment);
	return attributesOffset + sizeof(GpuTriangleAttributes) * std::max<size_t>(attributes, 1);
}

void GpuRenderer::uploadGeometry(VulkanEngine* engine, const std::shared_ptr<const RaytraceGeometry>& geometry)
{
	if (m_uploadedGeometry == geometry && m_geometryBuffer.buffer != VK_NULL_HANDLE) {
		return;
	}

	//three ranges in one device-local buffer. The traversal reads nodes and triangles on every step
	//of every ray, so they belong in VRAM rather than in host-visible memory the GPU would fetch
	//across the bus; a zero-length range is invalid, so an empty one still gets one (unused) entry
	const VkDeviceSize alignment = std::max<VkDeviceSize>(engine->m_gpuProperties.limits.minStorageBufferOffsetAlignment, 16);
	const size_t nodes = geometry != nullptr ? geometry->nodes.size() : 0;
	const size_t triangles = geometry != nullptr ? geometry->triangles.size() : 0;
	const size_t attributes = geometry != nullptr ? geometry->attributes.size() : 0;
	const VkDeviceSize nodesOffset = 0;
	const VkDeviceSize nodesBytes = sizeof(glm::uvec4) * std::max<size_t>(nodes, 1);
	const VkDeviceSize trianglesOffset = alignUp(nodesOffset + nodesBytes, alignment);
	const VkDeviceSize trianglesBytes = sizeof(BvhTriangle) * std::max<size_t>(triangles, 1);
	const VkDeviceSize attributesOffset = alignUp(trianglesOffset + trianglesBytes, alignment);
	const VkDeviceSize attributesBytes = sizeof(GpuTriangleAttributes) * std::max<size_t>(attributes, 1);
	const VkDeviceSize totalBytes = attributesOffset + attributesBytes;

	AllocatedBuffer staging = engine->createBuffer(totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	uint8_t* mapped = (uint8_t*)staging.info.pMappedData;
	memset(mapped, 0, totalBytes);
	if (geometry != nullptr) {
		memcpy(mapped + nodesOffset, geometry->nodes.data(), sizeof(glm::uvec4) * nodes);
		memcpy(mapped + trianglesOffset, geometry->triangles.data(), sizeof(BvhTriangle) * triangles);
		memcpy(mapped + attributesOffset, geometry->attributes.data(), sizeof(GpuTriangleAttributes) * attributes);
	}
	vmaFlushAllocation(engine->m_memAllocator, staging.allocation, 0, totalBytes); //flush vma on MoltenVK

	retireBuffer(engine, m_geometryBuffer);
	m_geometryBuffer = engine->createBuffer(totalBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy copy {};
		copy.size = totalBytes;
		vkCmdCopyBuffer(cmd, staging.buffer, m_geometryBuffer.buffer, 1, &copy);
	});
	engine->destroyBuffer(staging);

	m_resources.setBuffer(CrtBinding::BlasNodes, m_geometryBuffer.buffer, nodesBytes, nodesOffset);
	m_resources.setBuffer(CrtBinding::BlasTriangles, m_geometryBuffer.buffer, trianglesBytes, trianglesOffset);
	m_resources.setBuffer(CrtBinding::TriangleAttributes, m_geometryBuffer.buffer, attributesBytes, attributesOffset);

	m_uploadedGeometry = geometry;
	fmt::println("GpuRenderer: BVH geometry uploaded - {} node words, {} triangles, {} attribute records, {:.1f} MB",
		nodes, triangles, attributes, double(totalBytes) / (1024.0 * 1024.0));
}

void GpuRenderer::uploadScene(VulkanEngine* engine)
{
	const std::shared_ptr<const RaytraceSceneAccel>& scene = m_snapshot.scene;
	uploadGeometry(engine, scene != nullptr ? scene->geometry : nullptr);

	//a render restarted by a camera drag arrives here every frame with the same scene; the scene
	//object is only ever rebuilt for a real change, so the pointer says whether anything moved
	if (m_uploadedScene == scene && m_sceneBuffer.buffer != VK_NULL_HANDLE) {
		return;
	}

	const size_t instances = scene != nullptr ? scene->instances.size() : 0;
	const size_t materials = scene != nullptr ? scene->materials.size() : 0;
	size_t materialWords = 0;
	if (scene != nullptr) {
		for (const PackedMaterial& material : scene->materials) {
			materialWords += material.params.size();
		}
	}
	const size_t instanceMaterials = scene != nullptr ? scene->instanceMaterials.size() : 0;
	const size_t tlasWords = scene != nullptr ? scene->tlas.bvh.nodes.size() : 0;
	//nothing is traced through a TLAS that failed to pack; the instance count is what the shader
	//checks. The unbounded shapes are outside it and traced regardless
	const bool traceable = scene != nullptr && scene->tlas.bvh.error.empty() && scene->tlas.bvh.nodeCount > 0;
	m_unboundedCount = scene != nullptr ? scene->unboundedCount : 0;
	m_tlasInstanceCount = traceable ? (uint32_t)instances - m_unboundedCount : 0;
	//what a traversal-free strategy scans: every record, TLAS or no TLAS
	m_instanceCount = (uint32_t)instances;

	const size_t lights = scene != nullptr ? scene->lights.lights.size() : 0;
	const size_t triangleLights = scene != nullptr ? scene->lights.triangleLights.size() : 0;

	const VkDeviceSize alignment = std::max<VkDeviceSize>(engine->m_gpuProperties.limits.minStorageBufferOffsetAlignment, 16);
	const VkDeviceSize instancesBytes = sizeof(GpuInstance) * std::max<size_t>(instances, 1);
	const VkDeviceSize materialsOffset = alignUp(instancesBytes, alignment);
	const VkDeviceSize materialsBytes = sizeof(CrtMaterialHeader) * std::max<size_t>(materials, 1);
	const VkDeviceSize materialParamsOffset = alignUp(materialsOffset + materialsBytes, alignment);
	const VkDeviceSize materialParamsBytes = sizeof(glm::vec4) * std::max<size_t>(materialWords, 1);
	const VkDeviceSize instanceMaterialsOffset = alignUp(materialParamsOffset + materialParamsBytes, alignment);
	const VkDeviceSize instanceMaterialsBytes = sizeof(uint32_t) * std::max<size_t>(instanceMaterials, 1);
	const VkDeviceSize tlasOffset = alignUp(instanceMaterialsOffset + instanceMaterialsBytes, alignment);
	const VkDeviceSize tlasBytes = sizeof(glm::uvec4) * std::max<size_t>(tlasWords, 4);
	//the light list's header, then its records - one record at least, so the array is never empty
	const VkDeviceSize lightsOffset = alignUp(tlasOffset + tlasBytes, alignment);
	const VkDeviceSize lightsBytes = sizeof(GpuLightHeader) + sizeof(GpuLight) * std::max<size_t>(lights, 1);
	const VkDeviceSize triangleLightsOffset = alignUp(lightsOffset + lightsBytes, alignment);
	const VkDeviceSize triangleLightsBytes = sizeof(uint32_t) * std::max<size_t>(triangleLights, 1);
	const VkDeviceSize totalBytes = triangleLightsOffset + triangleLightsBytes;

	retireBuffer(engine, m_sceneBuffer);
	m_sceneBuffer = engine->createBuffer(totalBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	uint8_t* mapped = (uint8_t*)m_sceneBuffer.info.pMappedData;
	memset(mapped, 0, totalBytes);
	if (scene != nullptr) {
		memcpy(mapped, scene->instances.data(), sizeof(GpuInstance) * instances);
		//each material's header in binding 7, its parameters one after the other in binding 26
		CrtMaterialHeader* headers = (CrtMaterialHeader*)(mapped + materialsOffset);
		glm::vec4* params = (glm::vec4*)(mapped + materialParamsOffset);
		uint32_t paramOffset = 0;
		for (size_t i = 0; i < materials; i++) {
			const PackedMaterial& material = scene->materials[i];
			headers[i] = CrtMaterialHeader { material.type, material.flags, paramOffset, (uint32_t)material.params.size() };
			std::copy(material.params.begin(), material.params.end(), params + paramOffset);
			paramOffset += (uint32_t)material.params.size();
		}
		memcpy(mapped + instanceMaterialsOffset, scene->instanceMaterials.data(), sizeof(uint32_t) * instanceMaterials);
		memcpy(mapped + tlasOffset, scene->tlas.bvh.nodes.data(), sizeof(glm::uvec4) * tlasWords);
		memcpy(mapped + lightsOffset, &scene->lights.header, sizeof(GpuLightHeader));
		memcpy(mapped + lightsOffset + sizeof(GpuLightHeader), scene->lights.lights.data(), sizeof(GpuLight) * lights);
		memcpy(mapped + triangleLightsOffset, scene->lights.triangleLights.data(), sizeof(uint32_t) * triangleLights);
	} else {
		//no scene at all still has a light list: an empty one
		const GpuLightHeader empty {};
		memcpy(mapped + lightsOffset, &empty, sizeof(GpuLightHeader));
	}
	vmaFlushAllocation(engine->m_memAllocator, m_sceneBuffer.allocation, 0, totalBytes); //flush vma on MoltenVK

	m_resources.setBuffer(CrtBinding::Instances, m_sceneBuffer.buffer, instancesBytes, 0);
	m_resources.setBuffer(CrtBinding::Materials, m_sceneBuffer.buffer, materialsBytes, materialsOffset);
	m_resources.setBuffer(CrtBinding::MaterialParams, m_sceneBuffer.buffer, materialParamsBytes, materialParamsOffset);
	m_resources.setBuffer(CrtBinding::InstanceMaterials, m_sceneBuffer.buffer, instanceMaterialsBytes, instanceMaterialsOffset);
	m_resources.setBuffer(CrtBinding::TlasNodes, m_sceneBuffer.buffer, tlasBytes, tlasOffset);
	m_resources.setBuffer(CrtBinding::Lights, m_sceneBuffer.buffer, lightsBytes, lightsOffset);
	m_resources.setBuffer(CrtBinding::TriangleLights, m_sceneBuffer.buffer, triangleLightsBytes, triangleLightsOffset);

	m_uploadedScene = scene;
	fmt::println("GpuRenderer: scene uploaded - {} instance(s) ({} shape(s), {} unbounded), {} material(s), {} TLAS node(s), {} light(s), {:.2f} MB",
		instances, scene != nullptr ? scene->shapeCount : 0, m_unboundedCount, materials, tlasWords / 4, scene != nullptr ? scene->lights.header.count : 0, double(totalBytes) / (1024.0 * 1024.0));
}

void GpuRenderer::uploadEnvironment(VulkanEngine* engine, const std::shared_ptr<const EnvironmentDistribution>& environment)
{
	if (m_hasEnvironmentBuffer && m_uploadedEnvironment == environment) {
		return;
	}

	//crt_common.glsl EnvironmentSamplingBuffer: a four-word header, then each table as its marginal
	//cdf, its row cdfs and its function. Without a map it is the header alone, all zero, which the
	//shaders read as "nothing to sample" - and nothing is in the light list to make them try
	struct Header {
		uint32_t width;
		uint32_t height;
		float integral;
		float compensatedIntegral;
	};
	std::vector<float> data;
	Header header {};
	if (environment != nullptr) {
		const PiecewiseConstant2D& full = environment->full;
		const PiecewiseConstant2D& compensated = environment->compensated;
		header = { full.width, full.height, full.integral, compensated.integral };
		for (const PiecewiseConstant2D* table : { &full, &compensated }) {
			data.insert(data.end(), table->marginalCdf.begin(), table->marginalCdf.end());
			data.insert(data.end(), table->rowCdf.begin(), table->rowCdf.end());
			data.insert(data.end(), table->func.begin(), table->func.end());
		}
	}
	data.resize(std::max<size_t>(data.size(), 1), 0.f);
	const VkDeviceSize environmentBytes = sizeof(Header) + sizeof(float) * data.size();

	AllocatedBuffer staging = engine->createBuffer(environmentBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	uint8_t* mapped = (uint8_t*)staging.info.pMappedData;
	memcpy(mapped, &header, sizeof(Header));
	memcpy(mapped + sizeof(Header), data.data(), sizeof(float) * data.size());
	vmaFlushAllocation(engine->m_memAllocator, staging.allocation, 0, environmentBytes); //flush vma on MoltenVK

	retireBuffer(engine, m_environmentBuffer);
	m_environmentBuffer = engine->createBuffer(environmentBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy copy {};
		copy.size = environmentBytes;
		vkCmdCopyBuffer(cmd, staging.buffer, m_environmentBuffer.buffer, 1, &copy);
	});
	engine->destroyBuffer(staging);
	m_resources.setBuffer(CrtBinding::EnvironmentSampling, m_environmentBuffer.buffer, environmentBytes);

	m_uploadedEnvironment = environment;
	m_hasEnvironmentBuffer = true;
	if (environment != nullptr) {
		fmt::println("GpuRenderer: environment sampling tables uploaded - {} x {}, {:.1f} MB", header.width, header.height, double(environmentBytes) / (1024.0 * 1024.0));
	}
}

void GpuRenderer::uploadSamplerTables(VulkanEngine* engine)
{
	std::string error;
	const std::filesystem::path directory = std::filesystem::path(engine->m_rootPath) / "assets" / "sampler";
	m_samplerTablesLoaded = loadSamplerTables(directory, m_samplerData, error);
	std::vector<uint32_t> words;
	if (m_samplerTablesLoaded) {
		words = packSamplerTablesForGpu(m_samplerData);
		m_pmjRegionWord = words[6];
	} else {
		//the Sobol samplers fall back to the independent one; the binding still has to name a buffer
		fmt::println("GpuRenderer: sampler tables not loaded ({}), the Sobol samplers are unavailable", error);
		words.assign(16, 0u);
	}
	const VkDeviceSize bytes = sizeof(uint32_t) * words.size();

	AllocatedBuffer staging = engine->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(staging.info.pMappedData, words.data(), bytes);
	vmaFlushAllocation(engine->m_memAllocator, staging.allocation, 0, bytes); //flush vma on MoltenVK

	m_samplerTableBuffer = engine->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy copy {};
		copy.size = bytes;
		vkCmdCopyBuffer(cmd, staging.buffer, m_samplerTableBuffer.buffer, 1, &copy);
	});
	engine->destroyBuffer(staging);
	m_resources.setBuffer(CrtBinding::SamplerTables, m_samplerTableBuffer.buffer, bytes);
	if (m_samplerTablesLoaded) {
		fmt::println("GpuRenderer: sampler tables uploaded - {:.2f} MB", double(bytes) / (1024.0 * 1024.0));
	}
}

void GpuRenderer::uploadPmjPixelSamples(VulkanEngine* engine, uint32_t samplesPerPixel)
{
	if (!m_samplerTablesLoaded || m_pmjUploadedSamples == samplesPerPixel) {
		return;
	}
	preparePmj02BnPixelSamples(m_samplerData, samplesPerPixel);
	const std::vector<uint32_t> words = packPmj02BnPixelSamples(m_samplerData);
	const VkDeviceSize bytes = sizeof(uint32_t) * words.size();

	AllocatedBuffer staging = engine->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(staging.info.pMappedData, words.data(), bytes);
	vmaFlushAllocation(engine->m_memAllocator, staging.allocation, 0, bytes); //flush vma on MoltenVK

	//a frame in flight may be reading the region for the previous count. Changing the count is a
	//settings change, rare enough for a device-wide wait
	vkDeviceWaitIdle(engine->m_device);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy copy {};
		copy.dstOffset = sizeof(uint32_t) * (VkDeviceSize)m_pmjRegionWord;
		copy.size = bytes;
		vkCmdCopyBuffer(cmd, staging.buffer, m_samplerTableBuffer.buffer, 1, &copy);
	});
	engine->destroyBuffer(staging);
	m_pmjUploadedSamples = samplesPerPixel;
}

void GpuRenderer::sampleParams(CrtParams& params) const
{
	const KernelVariant* sampler = m_snapshot.kernels.selected(KernelSlot::GenerateSamples);
	if (sampler == nullptr || !sampler->writesRecord) {
		return;
	}
	const RenderSettings& settings = m_snapshot.settings;
	const SamplerParams sampling = makeSamplerParams(sampler->sampler, settings.samplerRandomization, m_snapshot.seed, m_snapshot.width, m_snapshot.height, (uint32_t)std::max(settings.maxSamples, 1), settings.unlimitedSamples);
	const PackedSamplerParams packed = packSamplerParams(sampling);
	params.samplerOptions = packed.sampler;
	params.samplerHalton = packed.halton;
	params.samplerStrata = packed.strata;
	params.samplerInvSamples = packed.inverseSamples;
	params.samplerInvStrataX = packed.inverseStrataX;
	params.samplerInvStrataY = packed.inverseStrataY;
}

void GpuRenderer::publishEngineImages(VulkanEngine* engine)
{
	//with no map loaded (or the snapshot not using it) the escaped kernel never samples the
	//binding, but a combined image sampler still has to name a valid view
	const bool hasMap = m_snapshot.useEnvironmentMap && engine->m_environmentMap.image != VK_NULL_HANDLE;
	const AllocatedImage& map = hasMap ? engine->m_environmentMap : engine->m_greyImage;
	m_resources.setImage(CrtBinding::EnvironmentMap, map.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, engine->m_defaultSamplerLinear);
	//the array always exists, even for a scene with no textures at all, so the binding is never
	//left naming nothing
	m_resources.setImage(CrtBinding::MaterialTextures, engine->m_raytraceTextures.image().imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, engine->m_defaultSamplerLinear);
}

//---------------------------------------------------------------- a render

std::string samplerFallbackReason(const KernelVariant& sampler, const RenderSettings& settings, uint32_t width, uint32_t height, bool tablesLoaded)
{
	const bool usesTables = sampler.sampler == SamplerKind::Sobol || sampler.sampler == SamplerKind::PaddedSobol || sampler.sampler == SamplerKind::ZSobol
		|| sampler.sampler == SamplerKind::Pmj02Bn;
	const bool interleaves = usesTables || sampler.sampler == SamplerKind::Halton;
	if (sampler.needsSampleCount && settings.unlimitedSamples) {
		return fmt::format("{} needs a known number of samples, so a render with Unlimited samples runs the independent sampler.", sampler.name);
	}
	if (usesTables && !tablesLoaded) {
		return fmt::format("{} needs assets/sampler/, which did not load, so renders run the independent sampler.", sampler.name);
	}
	if (interleaves && std::max(width, height) > SAMPLER_MAX_IMAGE_SIDE) {
		return fmt::format("{} supports images up to {} pixels on a side, so this size renders with the independent sampler.", sampler.name, SAMPLER_MAX_IMAGE_SIDE);
	}
	if (sampler.sampler == SamplerKind::Pmj02Bn && (uint32_t)settings.maxSamples > PMJ02BN_MAX_SAMPLES_PER_PIXEL) {
		return fmt::format("{} supports up to {} samples per pixel, so this count renders with the independent sampler.", sampler.name, PMJ02BN_MAX_SAMPLES_PER_PIXEL);
	}
	return {};
}

FilmFallback filmFallback(const KernelSelection& kernels, const RenderSettings& settings, uint32_t width, uint32_t height)
{
	const KernelVariant* film = kernels.selected(KernelSlot::UpdateFilm);
	if (film == nullptr || !film->implemented) {
		return {};
	}
	if (film->keepsFilmRecord) {
		const uint64_t bytesPerPixel = sizeof(CrtFilmPixel) + (film->keepsFilmCascade ? FILM_CASCADE_BYTES_PER_PIXEL : 0);
		if ((uint64_t)width * height * bytesPerPixel > FILM_RECORD_BYTES_LIMIT) {
			return { fmt::format("The {} film keeps {} bytes a pixel, more than {} MB at {} x {}, so this size renders with the running mean.",
				film->name, bytesPerPixel, FILM_RECORD_BYTES_LIMIT >> 20, width, height), "running_mean" };
		}
		return {};
	}
	const KernelVariant* camera = kernels.selected(KernelSlot::GenerateCameraRays);
	const bool filtered = camera != nullptr && camera->settings == KernelSettings::PixelFilter && settings.antialiasing;
	if (filtered && pixelFilterHasNegativeLobes(settings.pixelFilter.kind)) {
		return { fmt::format("The {} filter has negative lobes, which the {} film cannot weigh, so renders run the compensated mean.",
			pixelFilterName(settings.pixelFilter.kind), film->name), "compensated_mean" };
	}
	if (settings.clampSamples) {
		return { fmt::format("The {} film does not clamp samples, so renders with the clamp on run the compensated mean.", film->name), "compensated_mean" };
	}
	return {};
}

void GpuRenderer::start(VulkanEngine* engine, GpuRenderSnapshot snapshot)
{
	m_snapshot = std::move(snapshot);

	//THE FILM THIS RENDER RUNS, decided like the sampler below: the selection is the user's and stays as
	//it is, and the render runs another when the selected one cannot do what the settings ask
	{
		const FilmFallback fallback = filmFallback(m_snapshot.kernels, m_snapshot.settings, m_snapshot.width, m_snapshot.height);
		if (!fallback.reason.empty()) {
			const int replacement = findKernelVariant(KernelSlot::UpdateFilm, fallback.variant);
			if (replacement >= 0) {
				m_snapshot.kernels[KernelSlot::UpdateFilm] = (uint32_t)replacement;
				fmt::println("GpuRenderer: {}", fallback.reason);
			}
		}
	}

	//THE SAMPLER THIS RENDER RUNS. The selection is the user's and stays as it is; what runs instead,
	//when the selected sampler cannot run this render, is the independent one
	if (const KernelVariant* sampler = m_snapshot.kernels.selected(KernelSlot::GenerateSamples)) {
		const std::string reason = samplerFallbackReason(*sampler, m_snapshot.settings, m_snapshot.width, m_snapshot.height, m_samplerTablesLoaded);
		if (!reason.empty()) {
			m_snapshot.kernels[KernelSlot::GenerateSamples] = (uint32_t)findKernelVariant(KernelSlot::GenerateSamples, "independent");
			fmt::println("GpuRenderer: {}", reason);
		}
	}
	if (const KernelVariant* sampler = m_snapshot.kernels.selected(KernelSlot::GenerateSamples); sampler != nullptr && sampler->sampler == SamplerKind::Pmj02Bn) {
		uploadPmjPixelSamples(engine, (uint32_t)std::max(m_snapshot.settings.maxSamples, 1));
	}

	//another integrator's schedule gives its buffers back first. The frames in flight may still
	//use them, and switching integrator is rare, so a device-wide wait is the simple way
	const IntegratorId integrator = m_snapshot.kernels.integrator;
	if (m_scheduleActive && integrator != m_activeIntegrator) {
		vkDeviceWaitIdle(engine->m_device);
		m_schedules[(size_t)m_activeIntegrator]->release(*this, engine);
		m_scheduleActive = false;
	}
	m_activeIntegrator = integrator;

	//THE OUTPUTS this render keeps beside its picture, decided like the film and the sampler: the settings are the
	//user's and stay as they are, and the render drops an output whose memory would pass the limit
	{
		const OutputsFallback fallback = outputsFallback(m_snapshot.settings.aov, m_snapshot.lightGroupCount, m_snapshot.width, m_snapshot.height);
		m_aov = m_snapshot.settings.aov;
		m_lightGroupCount = m_snapshot.lightGroupCount;
		if (!fallback.aov) {
			m_aov.outputs &= 1u << (uint32_t)AovOutput::SampleCount;
			m_aov.captureThroughSpecular = false;
		}
		if (!fallback.lightGroups) {
			m_lightGroupCount = 0;
		}
		m_outputsNote = fallback.reason;
		if (!fallback.reason.empty()) {
			fmt::println("GpuRenderer: {}", fallback.reason);
		}
		m_aovLayout = makeAovLayout(m_aov);
		m_spectralBuckets = spectralImageBuckets(m_snapshot.settings.spectral);
		if (lightGroupSumBytes(m_lightGroupCount, m_snapshot.width, m_snapshot.height, m_spectralBuckets) > OUTPUT_BYTES_LIMIT && m_spectralBuckets > 0) {
			m_outputsNote += fmt::format("{}The spectral images take {:.0f} MB at {} x {}, more than {} MB, so this size renders without them.", m_outputsNote.empty() ? "" : " ",
				double(lightGroupSumBytes(m_lightGroupCount, m_snapshot.width, m_snapshot.height, m_spectralBuckets)) / (1024.0 * 1024.0), m_snapshot.width, m_snapshot.height,
				OUTPUT_BYTES_LIMIT >> 20);
			m_spectralBuckets = 0;
		}
		m_outputFeatures = (m_aovLayout.count > 0 ? 2u : 0u) | (m_lightGroupCount > 0 ? 4u : 0u) | (m_spectralBuckets > 0 ? 8u : 0u);

		//the scene's size sets the ambient occlusion view's default ray length and the depth view's far end
		m_autoAoDistance = 1.f;
		m_depthScale = 1.f;
		if (m_snapshot.scene != nullptr) {
			const Aabb& bounds = m_snapshot.scene->tlas.bounds;
			if (bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z) {
				const float radius = std::max(0.5f * glm::length(bounds.max - bounds.min), 1e-3f);
				m_autoAoDistance = 0.1f * radius;
				m_depthScale = glm::length(0.5f * (bounds.min + bounds.max) - m_snapshot.camera.lookFrom) + radius;
			}
		}
	}

	ensureFilm(engine, m_snapshot.width, m_snapshot.height);
	ensureOutputs(engine, m_snapshot.width, m_snapshot.height);
	syncAovViews(engine);
	const KernelVariant* filmVariant = m_snapshot.kernels.selected(KernelSlot::UpdateFilm);
	ensureFilmRecords(engine, m_snapshot.width, m_snapshot.height, filmVariant != nullptr && filmVariant->implemented && filmVariant->keepsFilmRecord,
		filmVariant != nullptr && filmVariant->implemented && filmVariant->keepsFilmCascade);
	uploadFilterTable(engine, m_snapshot.settings.pixelFilter);
	uploadScene(engine);
	uploadEnvironment(engine, m_snapshot.environment);
	uploadSpectral(engine);
	preparePipelines(engine, m_snapshot.kernels);
	m_schedules[(size_t)m_activeIntegrator]->start(*this, engine);
	m_scheduleActive = true;

	m_renderSerial++;
	m_running = true;
	m_clearPending = true;
	m_samplesAccumulated = 0;
	m_maxSamples = m_snapshot.settings.unlimitedSamples ? 0u : (uint32_t)std::max(m_snapshot.settings.maxSamples, 1);
	m_lastFrameGpuMs = 0.f;
	m_totalGpuMs = 0.f;
	m_droppedSamples = 0;
	m_referenceError = {};
	m_convergence.clear();
	//the noise view compares the film with a copy of itself from when it held half as many samples
	m_noiseBaseSamples = 0;
	m_nextNoiseBase = 4;
}

void GpuRenderer::stop()
{
	m_running = false;
}

uint32_t GpuRenderer::samplesPerFrame() const
{
	return m_schedules[(size_t)m_activeIntegrator]->samplesPerFrame();
}

const TraversalWork& GpuRenderer::traversalWork() const
{
	return m_schedules[(size_t)m_activeIntegrator]->traversalWork();
}

std::span<const ScheduleCounter> GpuRenderer::counters() const
{
	return m_schedules[(size_t)m_activeIntegrator]->counters();
}

const IntegratorSchedule& GpuRenderer::schedule(IntegratorId id) const
{
	return *m_schedules[(size_t)id];
}

void GpuRenderer::beginFrame(VulkanEngine* engine)
{
	//draw() has just waited on this slot's fence, so what it recorded FRAME_OVERLAP frames ago is done
	collectReadbacks(engine, (uint32_t)(engine->m_frameNumber % FRAME_OVERLAP));
}

void GpuRenderer::captureReference()
{
	m_capturePending = true;
}

void GpuRenderer::clearReference(VulkanEngine* engine)
{
	m_capturePending = false;
	if (!m_hasReference) {
		return;
	}
	//a frame in flight may still read it, so it goes the way the scene buffers do
	const AllocatedImage old = m_reference;
	engine->m_frames[(engine->m_frameNumber + 1) % FRAME_OVERLAP].deletionQueue.push_function([engine, old]() {
		engine->destroyImage(old);
	});
	m_reference = {};
	m_hasReference = false;
	m_referenceSamples = 0;
	m_referenceError = {};
	m_convergence.clear();
}

void GpuRenderer::recordAnalysis(VkCommandBuffer cmd, VulkanEngine* engine, const AllocatedImage& displayImage)
{
	//a reference of another size is another picture; the accumulation stands in for the binding
	const bool hasReference = m_hasReference && m_reference.imageExtent.width == m_accumulation.imageExtent.width && m_reference.imageExtent.height == m_accumulation.imageExtent.height;
	const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, m_analysisPass.setLayout);
	DescriptorWriter writer;
	writer.writeImage(0, m_accumulation.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(1, hasReference ? m_reference.imageView : m_accumulation.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(2, m_noiseBase.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(3, m_samplesTaken.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(4, displayImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeBuffer(5, m_filmRecords.buffer, m_filmRecordBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.updateSet(engine->m_device, set);

	const AnalysisPushConstants constants { (uint32_t)analysisView, m_samplesAccumulated, m_noiseBaseSamples, hasReference ? 1u : 0u, m_filmRecordsLive ? 1u : 0u };
	//the tonemap wrote the display image, and the film and the reference were written before it
	vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	dispatchComputePassOver(cmd, m_analysisPass, set, &constants, displayImage.imageExtent);
}

//the image's pixels as a host RGB image: a copy to a staging buffer once the device is idle
static bool readLinearImage(VulkanEngine* engine, const AllocatedImage& source, Image& image)
{
	if (source.image == VK_NULL_HANDLE) {
		return false;
	}
	vkDeviceWaitIdle(engine->m_device);
	const VkExtent3D extent = source.imageExtent;
	const VkDeviceSize bytes = (VkDeviceSize)extent.width * extent.height * 4 * sizeof(float);
	const AllocatedBuffer staging = engine->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferImageCopy region {};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = extent;
		//both images stay in GENERAL for good
		vkCmdCopyImageToBuffer(cmd, source.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &region);
	});
	vmaInvalidateAllocation(engine->m_memAllocator, staging.allocation, 0, bytes);
	const float* rgba = (const float*)staging.info.pMappedData;
	image.width = extent.width;
	image.height = extent.height;
	image.rgb.resize((size_t)extent.width * extent.height * 3);
	for (size_t i = 0; i < (size_t)extent.width * extent.height; i++) {
		image.rgb[i * 3 + 0] = rgba[i * 4 + 0];
		image.rgb[i * 3 + 1] = rgba[i * 4 + 1];
		image.rgb[i * 3 + 2] = rgba[i * 4 + 2];
	}
	engine->destroyBuffer(staging);
	return true;
}

bool GpuRenderer::readAccumulation(VulkanEngine* engine, Image& image)
{
	return hasImage() && readLinearImage(engine, m_accumulation, image);
}

bool GpuRenderer::readReference(VulkanEngine* engine, Image& image)
{
	return m_hasReference && readLinearImage(engine, m_reference, image);
}

void GpuRenderer::loadReference(VulkanEngine* engine, const Image& image)
{
	std::vector<float> rgba(image.pixelCount() * 4, 1.f);
	for (size_t i = 0; i < image.pixelCount(); i++) {
		rgba[i * 4 + 0] = image.rgb[i * 3 + 0];
		rgba[i * 4 + 1] = image.rgb[i * 3 + 1];
		rgba[i * 4 + 2] = image.rgb[i * 3 + 2];
	}
	vkDeviceWaitIdle(engine->m_device);
	clearReference(engine);
	m_reference = engine->createImage(rgba.data(), { image.width, image.height, 1 }, TonemapPass::LINEAR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_reference.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	});
	m_hasReference = true;
	m_capturePending = false;
	m_referenceSamples = 0;
	m_referenceError = {};
	m_convergence.clear();
}

void GpuRenderer::recordReference(VkCommandBuffer cmd, VulkanEngine* engine, uint32_t slot)
{
	m_slotErrorGroups[slot] = 0;
	const VkExtent3D extent = m_accumulation.imageExtent;

	if (m_capturePending) {
		m_capturePending = false;
		if (m_hasReference) {
			clearReference(engine);
		}
		m_reference = engine->createImage(extent, TonemapPass::LINEAR_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		vkutil::transition_image(cmd, m_reference.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		//the mean as it stands after this frame's film update, both images in GENERAL for good
		VkImageCopy region {};
		region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.extent = extent;
		vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		vkCmdCopyImage(cmd, m_accumulation.image, VK_IMAGE_LAYOUT_GENERAL, m_reference.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
		vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
		m_hasReference = true;
		m_referenceSamples = m_samplesAccumulated;
		m_convergence.clear();
	}

	//measured only against a reference of the same size: another size is another picture
	if (!m_hasReference || m_reference.imageExtent.width != extent.width || m_reference.imageExtent.height != extent.height) {
		return;
	}

	const uint32_t groupsX = (extent.width + ERROR_WORKGROUP - 1) / ERROR_WORKGROUP;
	const uint32_t groupsY = (extent.height + ERROR_WORKGROUP - 1) / ERROR_WORKGROUP;
	const VkDeviceSize partialBytes = sizeof(glm::vec4) * (VkDeviceSize)groupsX * groupsY;
	if (partialBytes > m_errorPartialsBytes) {
		//grown in place: the old buffers may be in flight, so they retire like the scene buffers
		retireBuffer(engine, m_errorPartials);
		retireBuffer(engine, m_errorReadback);
		m_errorPartialsBytes = partialBytes;
		m_errorPartials = engine->createBuffer(partialBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		m_errorReadback = engine->createBuffer(partialBytes * FRAME_OVERLAP, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
		//a slot recorded before the growth would read the old layout
		std::fill(m_slotErrorGroups.begin(), m_slotErrorGroups.end(), 0u);
	}

	const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, m_errorPass.setLayout);
	DescriptorWriter writer;
	writer.writeImage(0, m_accumulation.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeImage(1, m_reference.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	writer.writeBuffer(2, m_errorPartials.buffer, partialBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.updateSet(engine->m_device, set);

	vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	dispatchComputePass(cmd, m_errorPass, set, nullptr, { groupsX, groupsY, 1 });
	vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	VkBufferCopy copy {};
	copy.dstOffset = m_errorPartialsBytes * slot;
	copy.size = partialBytes;
	vkCmdCopyBuffer(cmd, m_errorPartials.buffer, m_errorReadback.buffer, 1, &copy);

	m_slotErrorGroups[slot] = groupsX * groupsY;
	m_slotErrorSamples[slot] = m_samplesAccumulated;
}

void GpuRenderer::collectReadbacks(VulkanEngine* engine, uint32_t slot)
{
	//the reference measurement is read whether or not a render ran in the slot: a finished render
	//is still measured against a reference captured after it
	if (m_slotErrorGroups[slot] > 0) {
		const uint32_t groups = m_slotErrorGroups[slot];
		m_slotErrorGroups[slot] = 0;
		const VkDeviceSize offset = m_errorPartialsBytes * slot;
		vmaInvalidateAllocation(engine->m_memAllocator, m_errorReadback.allocation, offset, sizeof(glm::vec4) * groups);
		const glm::vec4* partials = (const glm::vec4*)((const uint8_t*)m_errorReadback.info.pMappedData + offset);
		double squared = 0.0;
		double relative = 0.0;
		double percent = 0.0;
		double difference = 0.0;
		for (uint32_t i = 0; i < groups; i++) {
			squared += partials[i].x;
			relative += partials[i].y;
			percent += partials[i].z;
			difference += partials[i].w;
		}
		const double pixels = (double)std::max(m_accumulation.imageExtent.width * m_accumulation.imageExtent.height, 1u);
		m_referenceError.rmse = std::sqrt(squared / (3.0 * pixels));
		m_referenceError.relativeMse = relative / pixels;
		m_referenceError.mape = percent / pixels;
		m_referenceError.meanDifference = difference / (3.0 * pixels);
		m_referenceError.samples = m_slotErrorSamples[slot];
		m_referenceError.valid = true;
		//one point per sample count: a frame measured again at the same count (the render
		//finished, the reference moved) replaces its point
		if (!m_convergence.empty() && m_convergence.back().samples == m_referenceError.samples) {
			m_convergence.back() = m_referenceError;
		} else if (m_referenceError.samples > 0 && (m_convergence.empty() || m_convergence.back().samples < m_referenceError.samples)) {
			m_convergence.push_back(m_referenceError);
		}
	}

	if (m_slotRender[slot] == 0) {
		return;
	}
	const bool currentRender = m_slotRender[slot] == m_renderSerial;
	m_slotRender[slot] = 0;

	if (m_hasTimestamps) {
		//value, availability pairs for the two queries
		uint64_t results[4] = {};
		const VkResult result = vkGetQueryPoolResults(engine->m_device, m_queryPool, slot * 2, 2, sizeof(results), results, sizeof(uint64_t) * 2, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		if (result == VK_SUCCESS && results[1] != 0 && results[3] != 0 && currentRender) {
			const double ms = double(results[2] - results[0]) * m_timestampPeriodNs / 1.0e6;
			m_lastFrameGpuMs = (float)ms;
			m_totalGpuMs += (float)ms;
		}
	}

	//a slot recorded for an earlier render - or by another integrator's schedule, since switching
	//starts a new render - is dropped
	if (!currentRender) {
		return;
	}
	const VkDeviceSize statsOffset = (VkDeviceSize)slot * TRAVERSAL_STATS_BYTES;
	vmaInvalidateAllocation(engine->m_memAllocator, m_statsReadback.allocation, statsOffset, TRAVERSAL_STATS_BYTES);
	const uint32_t* stats = (const uint32_t*)((const uint8_t*)m_statsReadback.info.pMappedData + statsOffset);
	//a running total over the render: a single dropped sample biases its pixel
	m_droppedSamples += stats[CRT_DROPPED_SAMPLES_STAT];
	m_schedules[(size_t)m_activeIntegrator]->collectReadbacks(engine, slot, stats);
}

CrtParams GpuRenderer::baseParams(VulkanEngine* engine) const
{
	//the RTIOW camera: origin, lower-left corner of the image plane, its two edges, and the lens basis
	const RTCameraSnapshot& camera = m_snapshot.camera;
	const float aspect = (float)m_snapshot.width / (float)m_snapshot.height;
	const float theta = glm::radians(camera.vfovDegrees);
	const float h = glm::tan(theta / 2.f);
	const float viewportHeight = 2.f * h;
	const float viewportWidth = aspect * viewportHeight;
	const glm::vec3 t = glm::normalize(camera.lookFrom - camera.lookAt);
	const glm::vec3 n = glm::normalize(glm::cross(camera.vUp, t));
	const glm::vec3 b = glm::cross(t, n);
	const glm::vec3 horizontal = camera.focusDistance * viewportWidth * n;
	const glm::vec3 vertical = camera.focusDistance * viewportHeight * b;
	const glm::vec3 lowerLeft = camera.lookFrom - horizontal / 2.f - vertical / 2.f - camera.focusDistance * t;

	const RenderSettings& settings = m_snapshot.settings;
	CrtParams params {};
	params.origin = glm::vec4(camera.lookFrom, 0.f);
	params.lowerLeft = glm::vec4(lowerLeft, 0.f);
	params.horizontal = glm::vec4(horizontal, 0.f);
	params.vertical = glm::vec4(vertical, 0.f);
	params.lensU = glm::vec4(n, camera.aperture / 2.f);
	params.lensV = glm::vec4(b, 0.f);
	params.width = m_snapshot.width;
	params.height = m_snapshot.height;
	params.seed = m_snapshot.seed;
	params.bounce = 0;
	params.rayDepth = (uint32_t)std::clamp(settings.rayDepth, 1, CRT_MAX_DEPTH);
	params.tlasInstanceCount = m_tlasInstanceCount;
	params.instanceCount = m_instanceCount;
	params.unboundedCount = m_unboundedCount;
	params.flags = (settings.antialiasing ? CRT_FLAG_JITTER : 0u)
		| (settings.russianRoulette ? CRT_FLAG_ROULETTE : 0u)
		| ((m_snapshot.useEnvironmentMap && engine->m_environmentMap.image != VK_NULL_HANDLE) ? CRT_FLAG_ENVIRONMENT_MAP : 0u)
		| (m_snapshot.solidBackground ? CRT_FLAG_SOLID_BACKGROUND : 0u)
		| (settings.errorBoundedOffsets ? CRT_FLAG_ERROR_BOUNDS : 0u)
		| (settings.watertightTriangles ? CRT_FLAG_WATERTIGHT : 0u);
	params.background = glm::vec4(m_snapshot.backgroundColor, 0.f);
	params.minBouncesBeforeRoulette = (uint32_t)std::max(settings.minBouncesBeforeRoulette, 0);
	params.debugView = (uint32_t)debugView;
	params.maxSamples = std::max(m_maxSamples, 1u);
	//the clamp is a feature of the film variants that keep a record; the others never see it
	params.filmClamp = (settings.clampSamples && m_filmRecordsLive) ? settings.clampValue : 0.f;
	params.outlierKappa = std::max(settings.outlierKappa, 1.f);
	//the auxiliary outputs and the light groups
	params.aovOutputs = m_aov.outputs | (m_aov.captureThroughSpecular ? AOV_PARAM_CAPTURE_THROUGH_SPECULAR : 0u);
	params.aovLayers = m_aovLayout.pack();
	params.lightGroupCount = m_lightGroupCount;
	params.spectralBuckets = m_spectralBuckets;
	params.environmentGroup = std::min(m_snapshot.environmentGroup, MAX_LIGHT_GROUPS - 1u);
	params.aoDistance = aoDistance > 0.f ? aoDistance : m_autoAoDistance;
	//the map is stored divided by its scale (environmentStorageScale()); the texture reads get
	//it back here. The light list prices the environment from the file's own values instead
	params.environmentIntensity = m_snapshot.environmentIntensity * m_snapshot.environmentMapScale;
	sampleParams(params);
	return params;
}

void GpuRenderer::record(VkCommandBuffer cmd, VulkanEngine* engine, const AllocatedImage& displayImage, float exposure)
{
	if (!m_hasFilm) {
		return;
	}

	const uint32_t slot = (uint32_t)(engine->m_frameNumber % FRAME_OVERLAP);

	if (m_running) {
		if (m_hasTimestamps) {
			vkCmdResetQueryPool(cmd, m_queryPool, slot * 2, 2);
			vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, slot * 2);
		}
		publishEngineImages(engine);

		const VkPipelineStageFlags2 computeStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
		const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
		const VkPipelineStageFlags2 transferStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
		const VkAccessFlags2 transferAccess = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

		if (m_clearPending) {
			m_clearPending = false;
			m_hasImage = true;

			//a fresh render: zero the mean and the counts
			const VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
			const VkClearColorValue zeroFloat { { 0.f, 0.f, 0.f, 0.f } };
			VkClearColorValue zeroUint {};
			zeroUint.uint32[0] = 0;
			vkCmdClearColorImage(cmd, m_accumulation.image, VK_IMAGE_LAYOUT_GENERAL, &zeroFloat, 1, &range);
			vkCmdClearColorImage(cmd, m_samplesTaken.image, VK_IMAGE_LAYOUT_GENERAL, &zeroUint, 1, &range);
			if (m_filmRecordsLive) {
				vkCmdFillBuffer(cmd, m_filmRecords.buffer, 0, VK_WHOLE_SIZE, 0);
			}
			if (m_filmCascadeLive) {
				vkCmdFillBuffer(cmd, m_filmCascade.buffer, 0, VK_WHOLE_SIZE, 0);
			}
			//the outputs accumulate from the first sample, like the film
			if (m_aovLayout.count > 0) {
				vkCmdClearColorImage(cmd, m_aovImage.image, VK_IMAGE_LAYOUT_GENERAL, &zeroFloat, 1, &range);
			}
			if (m_lightGroupCount > 0 || m_spectralBuckets > 0) {
				vkCmdFillBuffer(cmd, m_channelSums.buffer, 0, VK_WHOLE_SIZE, 0);
			}
		}
		//every kernel that traces adds its work to these, per frame
		vkCmdFillBuffer(cmd, m_traversalStats.buffer, 0, VK_WHOLE_SIZE, 0);
		vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);

		//the film as the frames before this one left it, kept each time its sample count has doubled,
		//so that the frame about to run takes it past the copy: the noise view needs the two apart
		if (m_samplesAccumulated > 0 && m_samplesAccumulated >= m_nextNoiseBase) {
			VkImageCopy region {};
			region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.extent = m_accumulation.imageExtent;
			vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
			vkCmdCopyImage(cmd, m_accumulation.image, VK_IMAGE_LAYOUT_GENERAL, m_noiseBase.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
			vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
			m_noiseBaseSamples = m_samplesAccumulated;
			m_nextNoiseBase = m_samplesAccumulated * 2u;
		}

		//THE INTEGRATOR. Everything above and below is the same for every one of them
		m_schedules[(size_t)m_activeIntegrator]->record(*this, engine, cmd, slot, baseParams(engine));

		vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);

		//the frame's counters, read back FRAME_OVERLAP frames later. After the film stage, whose
		//dropped-sample count is the last of them
		VkBufferCopy statsRegion {};
		statsRegion.dstOffset = (VkDeviceSize)slot * TRAVERSAL_STATS_BYTES;
		statsRegion.size = TRAVERSAL_STATS_BYTES;
		vkCmdCopyBuffer(cmd, m_traversalStats.buffer, m_statsReadback.buffer, 1, &statsRegion);

		m_slotRender[slot] = m_renderSerial;

		m_samplesAccumulated += samplesPerFrame();
		if (m_maxSamples > 0 && m_samplesAccumulated >= m_maxSamples) {
			m_running = false;
		}
	}

	if (m_hasImage) {
		recordReference(cmd, engine, slot);
		//Relight shows the picture the light groups' sums make, each times its gain, in place of the film's
		const bool relit = relight && m_lightGroupCount > 0;
		if (relit) {
			ensureRelitImage(engine);
			recordRelight(cmd, engine);
		}
		engine->m_tonemapPass.dispatch(cmd, engine->m_device, engine->getCurrentFrame().frameDescriptors, relit ? m_relit : m_accumulation, displayImage, exposure, tonemapDisplay());
		recordAovViews(cmd, engine);
		if (analysisView != CrtAnalysisView::None) {
			recordAnalysis(cmd, engine, displayImage);
		}
	}

	if (m_running && m_hasTimestamps) {
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, slot * 2 + 1);
	} else if (m_slotRender[slot] != 0 && m_hasTimestamps) {
		//the frame that just finished the render still needs its closing timestamp
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, slot * 2 + 1);
	}
}
