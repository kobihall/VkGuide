// The auxiliary outputs' albedo self-test (docs/plans/completed/pbrt-aovs-and-image-output.md):
// shaders/test/aov_albedo_parity.comp runs the 16-sample estimate of crt_surface.glsl aovCapture() on a batch of
// surfaces, and this compares it with bsdfAlbedoEstimate() in src/bsdf.cpp, the mirror bin/bsdf_test checks
// against the properties a sampling routine must have. A batch is every material over random normals and
// incident directions, with the sample record off and on: the estimate must answer from pbrt's table in both.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include <fmt/format.h>

#include <aov.h>
#include <bsdf.h>
#include <gpu_selftest.h>
#include <vk_compute.h>
#include <vk_engine.h>
#include <vk_images.h>

namespace {

// shaders/test/aov_albedo_parity.comp AlbedoCase
struct AlbedoCase {
	glm::vec4 albedoParam;
	glm::vec4 inDir;
	glm::vec4 normal;
	glm::vec4 materialValues;
};
static_assert(sizeof(AlbedoCase) == 64);

struct NamedMaterial {
	std::string name;
	BsdfMaterial material;
};

std::vector<NamedMaterial> materials()
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
	list.push_back({ "phong 0.6", make(BsdfType::Phong, 0.6f) });
	for (float fuzz : { 0.f, 0.3f, 1.0f }) {
		list.push_back({ fmt::format("metal fuzz {:g}", fuzz), make(BsdfType::Metal, fuzz) });
	}
	list.push_back({ "dielectric 1.5", make(BsdfType::Dielectric, 1.5f) });
	for (float roughness : { 0.2f, 0.6f }) {
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

}

SelfTestResult runAovAlbedoTest(VulkanEngine* engine)
{
	SelfTestResult result;
	const std::vector<NamedMaterial> list = materials();
	constexpr size_t CASES_PER_MATERIAL = 512;

	std::mt19937 rng(20261021u);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	std::vector<AlbedoCase> cases;
	std::vector<size_t> materialOf;
	for (size_t m = 0; m < list.size(); m++) {
		for (size_t i = 0; i < CASES_PER_MATERIAL; i++) {
			const glm::vec3 normal = randomUnitVector(rng);
			glm::vec3 above = randomUnitVector(rng);
			if (glm::dot(above, normal) < 0.f) {
				above = -above;
			}
			AlbedoCase c {};
			c.albedoParam = glm::vec4(list[m].material.albedo, list[m].material.param);
			c.inDir = glm::vec4(-above, 0.f);
			c.normal = glm::vec4(normal, 0.f);
			c.materialValues = glm::vec4(list[m].material.metallic, list[m].material.roughness, unit(rng) < 0.5f ? 1.f : 0.f, (float)list[m].material.type);
			cases.push_back(c);
			materialOf.push_back(m);
		}
	}

	const VkDeviceSize caseBytes = sizeof(AlbedoCase) * cases.size();
	const VkDeviceSize resultBytes = sizeof(glm::vec4) * cases.size();
	const AllocatedBuffer caseBuffer = engine->createBuffer(caseBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	const AllocatedBuffer resultBuffer = engine->createBuffer(resultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	//the record's binding is declared by the shaders the includes name, and unused: a stub stands in
	const AllocatedBuffer recordBuffer = engine->createBuffer(sizeof(glm::vec4) * 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	std::memcpy(caseBuffer.info.pMappedData, cases.data(), caseBytes);
	vmaFlushAllocation(engine->m_memAllocator, caseBuffer.allocation, 0, caseBytes);
	std::memset(recordBuffer.info.pMappedData, 0, sizeof(glm::vec4) * 3);
	vmaFlushAllocation(engine->m_memAllocator, recordBuffer.allocation, 0, sizeof(glm::vec4) * 3);

	// The GPU's sin, cos, pow and sqrt are not the C++ library's, so two values agree to a few ulps and a sample
	// kept or not, a lobe picked or a refraction taken that sits within rounding of its threshold can flip: a flip
	// moves the mean by up to a sixteenth of the attenuation, and is not a disagreement
	constexpr double TOLERANCE = 2e-3;
	constexpr double MAX_FLIP_FRACTION = 5e-3;

	bool passed = true;
	for (const bool record : { false, true }) {
		ComputePass pass = ComputePassBuilder(engine->m_rootPath + "shaders/test/aov_albedo_parity.comp.spv")
			.addBinding(23, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addBinding(90, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.addBinding(91, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			.setWorkgroupSize(64, 1, 1)
			.addSpecialization(0, record ? 1u : 0u)
			.addSpecialization(1, 1u)
			.build(engine->m_device);
		const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, pass.setLayout);
		DescriptorWriter writer;
		writer.writeBuffer(23, recordBuffer.buffer, sizeof(glm::vec4) * 3, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.writeBuffer(90, caseBuffer.buffer, caseBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.writeBuffer(91, resultBuffer.buffer, resultBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.updateSet(engine->m_device, set);
		std::memset(resultBuffer.info.pMappedData, 0, resultBytes);
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			dispatchComputePassOver(cmd, pass, set, nullptr, { (uint32_t)cases.size(), 1, 1 });
		});
		vmaInvalidateAllocation(engine->m_memAllocator, resultBuffer.allocation, 0, resultBytes);
		const glm::vec4* gpu = (const glm::vec4*)resultBuffer.info.pMappedData;

		std::vector<size_t> flipped(list.size(), 0);
		std::vector<double> worst(list.size(), 0.0);
		size_t totalFlips = 0;
		for (size_t i = 0; i < cases.size(); i++) {
			const AlbedoCase& c = cases[i];
			const glm::vec3 mirror = bsdfAlbedoEstimate(list[materialOf[i]].material, glm::vec3(c.inDir), glm::vec3(c.normal), c.materialValues.z > 0.5f);
			double difference = 0.0;
			for (int k = 0; k < 3; k++) {
				difference = std::max(difference, (double)std::abs(mirror[k] - gpu[i][k]) / std::max({ (double)std::abs(mirror[k]), (double)std::abs(gpu[i][k]), 0.05 }));
			}
			if (difference > TOLERANCE) {
				flipped[materialOf[i]]++;
				totalFlips++;
			} else {
				worst[materialOf[i]] = std::max(worst[materialOf[i]], difference);
			}
		}
		pass.destroy(engine->m_device);

		double worstAll = 0.0;
		for (size_t m = 0; m < list.size(); m++) {
			worstAll = std::max(worstAll, worst[m]);
			if (flipped[m] > CASES_PER_MATERIAL * MAX_FLIP_FRACTION) {
				result.lines.push_back(fmt::format("[{}] {}: {} of {} cases differ by more than {:g}", record ? "record" : "stream", list[m].name, flipped[m], CASES_PER_MATERIAL, TOLERANCE));
				passed = false;
			}
		}
		const bool modePassed = totalFlips <= (size_t)((double)cases.size() * MAX_FLIP_FRACTION);
		passed &= modePassed;
		result.lines.insert(result.lines.begin(), fmt::format("{} mode: {} cases, {} past the tolerance (decisions that rounding flipped), worst difference of the rest {:.2g} (limit {:g})",
			record ? "record" : "stream", cases.size(), totalFlips, worstAll, TOLERANCE));
	}

	engine->destroyBuffer(caseBuffer);
	engine->destroyBuffer(resultBuffer);
	engine->destroyBuffer(recordBuffer);
	result.passed = passed;
	result.lines.insert(result.lines.begin(), passed ? "the shader and the mirror agree" : "the shader and the mirror disagree");
	return result;
}

//---------------------------------------------------------------- the views

// shaders/aov_view.comp: each view of an output as the window shows it, over a synthetic batch of pixel sums made
// by the mirror (src/aov.cpp), against what the view is defined to show: the output resolved as an exported file
// resolves it, then mapped to a color. Includes the id color, whose GLSL mirror is crt_aov_color.glsl
SelfTestResult runAovViewTest(VulkanEngine* engine)
{
	SelfTestResult result;
	constexpr uint32_t WIDTH = 16;
	constexpr uint32_t HEIGHT = 8;
	constexpr uint32_t PIXELS = WIDTH * HEIGHT;
	constexpr float DEPTH_SCALE = 30.f;
	constexpr uint32_t MAX_SAMPLES = 20;

	AovSettings settings;
	for (uint32_t i = 0; i < (uint32_t)AovOutput::Count; i++) {
		settings.set((AovOutput)i, true);
	}
	const AovLayout layout = makeAovLayout(settings);

	// every pixel: a handful of samples that met a surface, or not, with a spread of everything the views read
	std::mt19937 rng(77);
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	std::vector<AovPixel> pixels(PIXELS);
	std::vector<uint32_t> taken(PIXELS);
	for (uint32_t p = 0; p < PIXELS; p++) {
		const uint32_t samples = 1 + p % 9;
		taken[p] = samples;
		const bool misses = p % 5 == 0;
		for (uint32_t s = 0; s < samples; s++) {
			AovSample sample;
			sample.hit = !misses && !(p % 7 == 0 && s % 2 == 1);
			if (sample.hit) {
				sample.position = glm::vec3(unit(rng) * 8.f - 2.f, unit(rng) * 4.f, unit(rng) * 8.f - 4.f);
				sample.depth = 1.f + unit(rng) * 25.f;
				sample.normal = glm::normalize(glm::vec3(unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f));
				sample.shadingNormal = sample.normal;
				sample.uv = glm::vec2(unit(rng), unit(rng));
				sample.albedo = glm::vec3(unit(rng), unit(rng), unit(rng));
				sample.instance = p;
				sample.material = (p * 7) % 33;
			}
			pixels[p].add(sample, 1.f, glm::vec3(unit(rng) * 2.f, unit(rng), unit(rng) * 0.2f));
		}
	}

	// the layers as the image array holds them, layer after layer
	std::vector<glm::vec4> layers((size_t)layout.count * PIXELS);
	for (uint32_t layer = 0; layer < AOV_LAYER_COUNT; layer++) {
		for (uint32_t p = 0; p < PIXELS; p++) {
			layers[(size_t)layout.layer[layer] * PIXELS + p] = pixels[p].layers[layer];
		}
	}
	const VkExtent3D extent { WIDTH, HEIGHT, 1 };
	const VkDeviceSize layerBytes = sizeof(glm::vec4) * layers.size();
	const AllocatedBuffer staging = engine->createBuffer(layerBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	std::memcpy(staging.info.pMappedData, layers.data(), layerBytes);
	vmaFlushAllocation(engine->m_memAllocator, staging.allocation, 0, layerBytes);
	const AllocatedImage aovImage = engine->createImageArray(extent, layout.count, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	const AllocatedImage takenImage = engine->createImage(extent, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	const AllocatedBuffer takenStaging = engine->createBuffer(sizeof(uint32_t) * PIXELS, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	std::memcpy(takenStaging.info.pMappedData, taken.data(), sizeof(uint32_t) * PIXELS);
	vmaFlushAllocation(engine->m_memAllocator, takenStaging.allocation, 0, sizeof(uint32_t) * PIXELS);
	const AllocatedImage output = engine->createImage(extent, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	const AllocatedBuffer readback = engine->createBuffer(sizeof(uint32_t) * PIXELS, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, aovImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkBufferImageCopy region {};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layout.count };
		region.imageExtent = extent;
		vkCmdCopyBufferToImage(cmd, staging.buffer, aovImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		vkutil::transition_image(cmd, aovImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, takenImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkBufferImageCopy takenRegion {};
		takenRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		takenRegion.imageExtent = extent;
		vkCmdCopyBufferToImage(cmd, takenStaging.buffer, takenImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &takenRegion);
		vkutil::transition_image(cmd, takenImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	});

	struct ViewPush {
		uint32_t view;
		uint32_t layers;
		uint32_t maxSamples;
		float depthScale;
	};
	ComputePass pass = ComputePassBuilder(engine->m_rootPath + "shaders/aov_view.comp.spv")
		.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.setPushConstants<ViewPush>()
		.build(engine->m_device);

	// what each view shows of a resolved pixel, as the shader's comment defines it
	auto expectedColor = [&](uint32_t view, uint32_t p) {
		const AovResolved r = resolveAov(pixels[p]);
		const bool hit = pixels[p].layers[(size_t)AovLayer::Weights].x != 0.f;
		glm::vec3 color(0.f);
		switch (view) {
		case 0:
			color = hit ? glm::fract(r.position) : glm::vec3(0.f);
			break;
		case 1:
			color = hit ? glm::vec3(1.f - r.depth / DEPTH_SCALE) : glm::vec3(0.f);
			break;
		case 2:
			color = glm::dot(r.normal, r.normal) > 0.f ? r.normal * 0.5f + 0.5f : glm::vec3(0.f);
			break;
		case 3:
			color = glm::dot(r.shadingNormal, r.shadingNormal) > 0.f ? r.shadingNormal * 0.5f + 0.5f : glm::vec3(0.f);
			break;
		case 4:
			color = hit ? glm::vec3(r.uv, 0.f) : glm::vec3(0.f);
			break;
		case 5:
			color = glm::sqrt(glm::clamp(r.albedo, 0.f, 1.f));
			break;
		case 6:
			color = r.instance >= 0.f ? aovIdColor((uint32_t)r.instance) : glm::vec3(0.f);
			break;
		case 7:
			color = r.material >= 0.f ? aovIdColor((uint32_t)r.material) : glm::vec3(0.f);
			break;
		case 8:
			color = r.variance / (1.f + r.variance);
			break;
		case 9:
			color = glm::vec3((float)taken[p] / (float)MAX_SAMPLES);
			break;
		}
		return glm::clamp(color, 0.f, 1.f);
	};

	bool passed = true;
	const char* names[] = { "position", "depth", "normal", "shading normal", "uv", "albedo", "instance id", "material id", "variance", "sample count" };
	for (uint32_t view = 0; view < 10; view++) {
		const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, pass.setLayout);
		DescriptorWriter writer;
		writer.writeImage(0, aovImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		writer.writeImage(1, takenImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		writer.writeImage(2, output.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		writer.updateSet(engine->m_device, set);
		const ViewPush push { view, layout.pack(), MAX_SAMPLES, DEPTH_SCALE };
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			dispatchComputePassOver(cmd, pass, set, &push, extent);
			vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
			VkBufferImageCopy region {};
			region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.imageExtent = extent;
			vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &region);
		});
		vmaInvalidateAllocation(engine->m_memAllocator, readback.allocation, 0, sizeof(uint32_t) * PIXELS);
		const uint8_t* bytes = (const uint8_t*)readback.info.pMappedData;
		double worst = 0.0;
		for (uint32_t p = 0; p < PIXELS; p++) {
			const glm::vec3 expected = expectedColor(view, p);
			for (int c = 0; c < 3; c++) {
				worst = std::max(worst, (double)std::abs(expected[c] * 255.f - (float)bytes[p * 4 + c]));
			}
		}
		// the shader and the mirror round a color to 8 bits and a fract or a sqrt may land either side of a boundary
		const bool ok = worst <= 1.5;
		passed &= ok;
		result.lines.push_back(fmt::format("{}: the {} view is within {:.2f} of 255 of what it is defined to show{}", ok ? "ok" : "FAILED", names[view], worst, ok ? "" : " (limit 1.5)"));
	}
	pass.destroy(engine->m_device);
	for (const AllocatedBuffer& buffer : { staging, takenStaging, readback }) {
		engine->destroyBuffer(buffer);
	}
	engine->destroyImage(aovImage);
	engine->destroyImage(takenImage);
	engine->destroyImage(output);
	result.passed = passed;
	result.lines.insert(result.lines.begin(), passed ? "every view shows what it is defined to show" : "a view differs from its definition");
	return result;
}
