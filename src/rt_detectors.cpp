#include <rt_detectors.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <fmt/format.h>

#include <rt_gpu_types.h>
#include <vk_descriptors.h>
#include <vk_engine.h>
#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_readback.h>
#include <image_io.h>

namespace {

// binding 59's header: the count, the frame sums' floats, where the totals and the counts start, then eight vec4 of
// band widths (crt_common.glsl DetectorBuffer)
struct DetectorHeader {
	glm::uvec4 counts;
	glm::vec4 bandWidths[8];
};
static_assert(sizeof(DetectorHeader) == 144);

struct FoldPushConstants {
	uint32_t frameImage;
	uint32_t imageCount;
	uint32_t copies;
	uint32_t frameSpectrum;
	uint32_t bins;
	uint32_t running;
	uint32_t compensation;
	uint32_t channels;
	uint32_t totals;
};

struct ViewPushConstants {
	uint32_t first;
	uint32_t floats;
	uint32_t pixelsX;
	uint32_t pixelsY;
	uint32_t channels;
	uint32_t mode;
	float scale;
	uint32_t peakIndex;
	float decades;
	float normalization;
};

constexpr uint32_t TOTALS_PER_DETECTOR = 4;
constexpr VkDeviceSize COUNTER_FLOATS = (VkDeviceSize)MAX_DETECTORS * (TOTALS_PER_DETECTOR + 1);

VkDeviceSize floatsBytes(VkDeviceSize floats)
{
	return sizeof(float) * floats;
}

// what each band of a band representation integrates to over wavelength: a box's width, a Gaussian's sigma sqrt(2 pi)
// (crt_detector.glsl detectorLaneWatts())
std::array<float, 32> bandWidths(const SpectralSettings& spectral)
{
	std::array<float, 32> widths {};
	if (spectral.representation != SpectralRepresentation::SpectralBands && spectral.representation != SpectralRepresentation::RgbGaussians) {
		return widths;
	}
	const std::vector<Band> bands = buildBands(spectral);
	for (size_t i = 0; i < bands.size() && i < widths.size(); i++) {
		widths[i] = bands[i].sigma > 0.f ? bands[i].sigma * std::sqrt(2.f * 3.14159265f) : bands[i].hi - bands[i].lo;
	}
	return widths;
}

}

void DetectorFilms::init(VulkanEngine* engine, CrtResourceTable& resources)
{
	const std::string shaderDir = engine->m_rootPath + "shaders/rt/light/";
	m_foldPass = ComputePassBuilder(shaderDir + "detector_fold.comp.spv")
		.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.setPushConstants<FoldPushConstants>()
		.setWorkgroupSize(256)
		.build(engine->m_device);
	m_viewPass = ComputePassBuilder(shaderDir + "detector_view.comp.spv")
		.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
		.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
		.setPushConstants<ViewPushConstants>()
		.setWorkgroupSize(16, 16)
		.build(engine->m_device);
	m_slots.resize(FRAME_OVERLAP);
	for (SlotReadback& slot : m_slots) {
		slot.totals = engine->createBuffer(floatsBytes(COUNTER_FLOATS), VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	}
	//a scene without detectors binds 16-byte stubs
	m_records = engine->createBuffer(sizeof(DetectorHeader), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	std::memset(m_records.info.pMappedData, 0, sizeof(DetectorHeader));
	vmaFlushAllocation(engine->m_memAllocator, m_records.allocation, 0, sizeof(DetectorHeader)); //flush vma on MoltenVK
	m_recordBytes = sizeof(DetectorHeader);
	m_film = engine->createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_filmBytes = 16;
	m_peaks = engine->createBuffer(sizeof(uint32_t) * MAX_DETECTORS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	publish(resources);
}

void DetectorFilms::destroy(VulkanEngine* engine, CrtResourceTable& resources)
{
	destroyPictures(engine);
	for (SlotReadback& slot : m_slots) {
		for (AllocatedBuffer* buffer : { &slot.totals, &slot.film, &slot.sensor }) {
			if (buffer->buffer != VK_NULL_HANDLE) {
				engine->destroyBuffer(*buffer);
				*buffer = {};
			}
		}
	}
	m_slots.clear();
	for (AllocatedBuffer* buffer : { &m_records, &m_film, &m_peaks }) {
		if (buffer->buffer != VK_NULL_HANDLE) {
			engine->destroyBuffer(*buffer);
			*buffer = {};
		}
	}
	resources.clear(CrtBinding::Detectors);
	resources.clear(CrtBinding::DetectorFilm);
	m_foldPass.destroy(engine->m_device);
	m_viewPass.destroy(engine->m_device);
}

void DetectorFilms::publish(CrtResourceTable& resources)
{
	resources.setBuffer(CrtBinding::Detectors, m_records.buffer, m_recordBytes);
	resources.setBuffer(CrtBinding::DetectorFilm, m_film.buffer, m_filmBytes);
}

void DetectorFilms::discard(VulkanEngine* engine)
{
	destroyPictures(engine);
	m_entries.clear();
	m_scene.reset();
	m_serial++;
	m_pathsEmitted = 0.0;
	m_leftOut = 0;
	m_deposits = false;
	m_filmRequested = false;
}

void DetectorFilms::destroyPictures(VulkanEngine* engine)
{
	bool any = false;
	for (Entry& entry : m_entries) {
		if (entry.picture.image != VK_NULL_HANDLE) {
			engine->m_displayRegistry.unregisterImage(entry.pictureName);
			any = true;
		}
	}
	if (!any) {
		return;
	}
	vkDeviceWaitIdle(engine->m_device);
	for (Entry& entry : m_entries) {
		if (entry.picture.image != VK_NULL_HANDLE) {
			engine->destroyImage(entry.picture);
			entry.picture = {};
		}
	}
}

void DetectorFilms::start(VulkanEngine* engine, CrtResourceTable& resources, const std::shared_ptr<const RaytraceSceneAccel>& scene, const SpectralSettings& spectral,
	bool deposits, uint64_t cameraPathTarget)
{
	const bool depositsBefore = m_deposits;
	m_deposits = deposits;
	m_cameraPathTarget = cameraPathTarget;
	m_serial++;
	m_pathsEmitted = 0.0;
	m_filmRequested = false;
	//a reading made another way measures something else: it starts over, and the frames of earlier renders in flight are
	//not read into it
	auto clearReading = [&](Entry& entry) {
		entry.reading = {};
		entry.readingSerial = m_serial;
		entry.frames = {};
		entry.collectedWatts = 0.0;
		entry.collectedPaths = 0.0;
		std::fill(entry.image.watts.begin(), entry.image.watts.end(), 0.0);
		entry.channels.clear();
	};
	//a render restarted for the same scene (a camera drag, a setting) keeps its buffers, and its readings until collect()
	//reads a frame of the new render: while an edit restarts the render every frame, the frames in flight are the only
	//readings there are
	if (scene == m_scene && spectral == m_spectral) {
		for (Entry& entry : m_entries) {
			const bool cameraPaths = entry.opticId == cameraPathTarget;
			if (cameraPaths != entry.cameraPaths || deposits != depositsBefore) {
				clearReading(entry);
			}
			entry.cameraPaths = cameraPaths;
		}
		return;
	}
	const bool sameSpectral = spectral == m_spectral;
	m_scene = scene;
	m_spectral = spectral;

	//the detectors that stay keep their windows' settings and pictures, and (an edit of the scene, say an optic dragged,
	//with their films the same shape) their readings
	std::vector<Entry> previous = std::move(m_entries);
	m_entries.clear();
	m_leftOut = scene != nullptr ? scene->detectorsLeftOut : 0u;

	const uint32_t count = scene != nullptr ? (uint32_t)scene->detectors.size() : 0u;
	//each film's place: its frame sums' copies, then (after every detector's) its running sums, the image's then the
	//spectrum's bins
	std::vector<GpuDetector> records = scene != nullptr ? scene->detectors : std::vector<GpuDetector> {};
	std::vector<std::pair<uint32_t, uint32_t>> running;
	m_frameFloats = 0;
	m_runningFloats = 0;
	for (GpuDetector& d : records) {
		const uint32_t imageFloats = d.pixelsX * d.pixelsY * d.channels;
		const uint32_t copies = frameCopies(imageFloats);
		d.flags |= (uint32_t)std::countr_zero(copies) << GPU_DETECTOR_COPIES_SHIFT;
		d.imageOffset = m_frameFloats;
		m_frameFloats += imageFloats * copies;
		d.spectrumOffset = m_frameFloats;
		m_frameFloats += d.bins * SPECTRUM_COPIES;
		running.push_back({ m_runningFloats, m_runningFloats + imageFloats });
		m_runningFloats += imageFloats + d.bins;
	}
	const VkDeviceSize counters = (VkDeviceSize)m_frameFloats + 2ull * m_runningFloats;
	auto sameFilm = [](const GpuDetector& a, const GpuDetector& b) {
		return a.pixelsX == b.pixelsX && a.pixelsY == b.pixelsY && a.channels == b.channels && a.bins == b.bins;
	};
	//the frames in flight are read only into the layout they were recorded in: the same detectors in the same order,
	//their films the same shape
	bool sameLayout = sameSpectral && previous.size() == count;
	for (uint32_t i = 0; sameLayout && i < count; i++) {
		sameLayout = previous[i].opticId == scene->detectorOptics[i] && sameFilm(previous[i].record, records[i]);
	}
	if (!sameLayout) {
		m_layout++;
	}

	const VkDeviceSize recordBytes = sizeof(DetectorHeader) + sizeof(GpuDetector) * std::max<uint32_t>(count, 1);
	const VkDeviceSize filmBytes = count > 0 ? floatsBytes(counters + COUNTER_FLOATS) : 16;
	//the previous render's frames may still read the old ones
	vkDeviceWaitIdle(engine->m_device);
	engine->destroyBuffer(m_records);
	m_records = engine->createBuffer(recordBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	uint8_t* mapped = (uint8_t*)m_records.info.pMappedData;
	std::memset(mapped, 0, recordBytes);
	DetectorHeader header {};
	header.counts = glm::uvec4(count, m_frameFloats, (uint32_t)counters, (uint32_t)(counters + (VkDeviceSize)MAX_DETECTORS * TOTALS_PER_DETECTOR));
	const std::array<float, 32> widths = bandWidths(spectral);
	std::memcpy(header.bandWidths, widths.data(), sizeof(header.bandWidths));
	std::memcpy(mapped, &header, sizeof(header));
	if (count > 0) {
		std::memcpy(mapped + sizeof(DetectorHeader), records.data(), sizeof(GpuDetector) * count);
	}
	vmaFlushAllocation(engine->m_memAllocator, m_records.allocation, 0, recordBytes); //flush vma on MoltenVK
	m_recordBytes = recordBytes;
	if (filmBytes != m_filmBytes) {
		engine->destroyBuffer(m_film);
		m_film = engine->createBuffer(filmBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		m_filmBytes = filmBytes;
	}
	const VkDeviceSize runningBytes = floatsBytes(2ull * m_runningFloats);
	for (SlotReadback& slot : m_slots) {
		if (count > 0 && slot.filmBytes != runningBytes) {
			if (slot.film.buffer != VK_NULL_HANDLE) {
				engine->destroyBuffer(slot.film);
			}
			slot.film = engine->createBuffer(runningBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
			slot.filmBytes = runningBytes;
			slot.filmCopied = false;
		}
	}
	publish(resources);

	const std::vector<SceneOptic>& optics = engine->m_raytraceScene.optics();
	for (uint32_t i = 0; i < count; i++) {
		Entry entry;
		entry.opticId = scene->detectorOptics[i];
		entry.record = records[i];
		entry.runningImage = running[i].first;
		entry.runningSpectrum = running[i].second;
		const auto optic = std::find_if(optics.begin(), optics.end(), [&](const SceneOptic& o) { return o.id == entry.opticId; });
		if (optic != optics.end()) {
			entry.kind = optic->kind;
			entry.geometry = detectorGeometry(*optic);
		}
		entry.image.width = entry.geometry.pixelsX;
		entry.image.height = entry.geometry.pixelsY;
		entry.image.pixelWidth = entry.geometry.pixelWidth;
		entry.image.pixelHeight = entry.geometry.pixelHeight;
		entry.image.watts.assign((size_t)entry.image.width * entry.image.height, 0.0);
		entry.pictureName = fmt::format("detector {}", entry.opticId);
		entry.cameraPaths = entry.opticId == cameraPathTarget;
		entry.readingSerial = m_serial;
		const auto before = std::find_if(previous.begin(), previous.end(), [&](const Entry& e) { return e.opticId == entry.opticId; });
		if (before != previous.end()) {
			entry.view = before->view;
			entry.pictureWanted = before->pictureWanted;
			const bool film = sameFilm(before->record, entry.record);
			if (film && sameSpectral && before->cameraPaths == entry.cameraPaths && deposits == depositsBefore) {
				entry.reading = std::move(before->reading);
				entry.readingSerial = before->readingSerial;
				entry.frames = before->frames;
				entry.collectedWatts = before->collectedWatts;
				entry.collectedPaths = before->collectedPaths;
				entry.image.watts = std::move(before->image.watts);
				entry.channels = std::move(before->channels);
			}
			if (film && before->picture.image != VK_NULL_HANDLE) {
				entry.picture = before->picture;
				before->picture = {};
			}
		}
		m_entries.push_back(std::move(entry));
	}
	//the pictures no detector kept; the device is idle
	for (Entry& entry : previous) {
		if (entry.picture.image != VK_NULL_HANDLE) {
			engine->m_displayRegistry.unregisterImage(entry.pictureName);
			engine->destroyImage(entry.picture);
			entry.picture = {};
		}
	}
	for (Entry& entry : m_entries) {
		if (entry.pictureWanted && entry.picture.image == VK_NULL_HANDLE) {
			entry.pictureWanted = false;
			setPictureWanted(engine, entry.opticId, true);
		}
	}
}

bool DetectorFilms::adopt(Entry& entry, uint32_t serial)
{
	if (serial < entry.readingSerial) {
		return false;
	}
	if (serial > entry.readingSerial) {
		entry.readingSerial = serial;
		entry.frames = {};
		entry.collectedWatts = 0.0;
		entry.collectedPaths = 0.0;
		entry.reading.pathsReached = 0.0;
	}
	return true;
}

void DetectorFilms::recordClear(VkCommandBuffer cmd)
{
	if (m_entries.empty()) {
		return;
	}
	vkCmdFillBuffer(cmd, m_film.buffer, 0, VK_WHOLE_SIZE, 0);
}

void DetectorFilms::recordFrame(VkCommandBuffer cmd, VulkanEngine* engine, uint32_t slot, double paths, bool copyFilm)
{
	SlotReadback& readback = m_slots[slot];
	readback.serial = 0;
	if (!deposits()) {
		return;
	}
	m_pathsEmitted += paths;
	const VkPipelineStageFlags2 computeStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	const VkPipelineStageFlags2 transferStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
	const VkAccessFlags2 transferAccess = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

	//stage 26's adds, then each detector's fold
	vkutil::memory_barrier(cmd, computeStages, computeAccess, computeStages, computeAccess);
	const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, m_foldPass.setLayout);
	DescriptorWriter writer;
	writer.writeBuffer(0, m_film.buffer, m_filmBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	writer.updateSet(engine->m_device, set);
	const uint32_t counters = m_frameFloats + 2u * m_runningFloats;
	for (uint32_t i = 0; i < (uint32_t)m_entries.size(); i++) {
		const Entry& entry = m_entries[i];
		const GpuDetector& d = entry.record;
		FoldPushConstants constants {};
		constants.frameImage = d.imageOffset;
		constants.imageCount = d.pixelsX * d.pixelsY * d.channels;
		constants.copies = 1u << ((d.flags >> GPU_DETECTOR_COPIES_SHIFT) & 15u);
		constants.frameSpectrum = d.spectrumOffset;
		constants.bins = d.bins;
		constants.running = m_frameFloats + entry.runningImage;
		constants.compensation = m_runningFloats;
		constants.channels = d.channels;
		constants.totals = counters + i * TOTALS_PER_DETECTOR;
		dispatchComputePass(cmd, m_foldPass, set, &constants, { (constants.imageCount + constants.bins + 255u) / 256u, 1, 1 });
	}
	//the totals and counts back, and cleared for the next frame
	vkutil::memory_barrier(cmd, computeStages, computeAccess, transferStage, transferAccess);
	VkBufferCopy totals {};
	totals.srcOffset = floatsBytes(counters);
	totals.size = floatsBytes(COUNTER_FLOATS);
	vkCmdCopyBuffer(cmd, m_film.buffer, readback.totals.buffer, 1, &totals);
	readback.filmCopied = false;
	if (copyFilm && readback.film.buffer != VK_NULL_HANDLE) {
		VkBufferCopy running {};
		running.srcOffset = floatsBytes(m_frameFloats);
		running.size = floatsBytes(2ull * m_runningFloats);
		vkCmdCopyBuffer(cmd, m_film.buffer, readback.film.buffer, 1, &running);
		readback.filmCopied = true;
		readback.pathsAtCopy = m_pathsEmitted;
		m_filmRequested = false;
	}
	vkutil::memory_barrier(cmd, transferStage, transferAccess, transferStage, transferAccess);
	vkCmdFillBuffer(cmd, m_film.buffer, floatsBytes(counters), floatsBytes(COUNTER_FLOATS), 0);
	vkutil::memory_barrier(cmd, transferStage, transferAccess, computeStages, computeAccess);
	readback.serial = m_serial;
	readback.layout = m_layout;
	readback.paths = paths;
}

void DetectorFilms::readFilm(Entry& entry, const float* running, double paths)
{
	const GpuDetector& d = entry.record;
	if (!(paths > 0.0)) {
		return;
	}
	const size_t pixels = (size_t)d.pixelsX * d.pixelsY;
	entry.channels.assign(pixels * d.channels, 0.0);
	entry.image.watts.assign(pixels, 0.0);
	const float* sums = running;
	const float* compensations = running + m_runningFloats;
	for (size_t p = 0; p < pixels; p++) {
		double mean = 0.0;
		for (uint32_t c = 0; c < d.channels; c++) {
			const size_t f = entry.runningImage + p * d.channels + c;
			const double watts = ((double)sums[f] + (double)compensations[f]) / paths;
			entry.channels[p * d.channels + c] = watts;
			mean += watts;
		}
		entry.image.watts[p] = mean / (double)d.channels;
	}
	measure(entry);
	DetectorReading& reading = entry.reading;
	reading.spectrumWavelength.clear();
	reading.spectrum.clear();
	if (d.bins > 0) {
		const double width = (entry.geometry.lambdaMax - entry.geometry.lambdaMin) / (double)d.bins;
		for (uint32_t b = 0; b < d.bins; b++) {
			const size_t f = entry.runningSpectrum + b;
			const double sum = (double)sums[f] + (double)compensations[f];
			reading.spectrumWavelength.push_back(entry.geometry.lambdaMin + width * ((double)b + 0.5));
			reading.spectrum.push_back(sum / paths / width);
		}
	}
}

void DetectorFilms::measure(Entry& entry)
{
	if (entry.geometry.imaging) {
		entry.reading.moments = iso11146Moments(entry.image);
		entry.reading.peak = peakIrradiance(entry.image);
	}
}

void DetectorFilms::readSensorFilm(Entry& entry, const float* pixels, uint32_t stride, uint32_t width, uint32_t height)
{
	//a pixel's mean radiance over the front hemisphere times pi is its irradiance; times its area, its power. The film's
	//row 0 is the sensor's top (+y), the image's row 0 its bottom
	if (width != entry.image.width || height != entry.image.height) {
		return;
	}
	const double area = entry.geometry.pixelWidth * entry.geometry.pixelHeight;
	const double scale = 3.14159265358979323846 * area;
	entry.channels.assign((size_t)width * height * 3, 0.0);
	double power = 0.0;
	for (uint32_t row = 0; row < height; row++) {
		const uint32_t y = height - 1 - row;
		for (uint32_t x = 0; x < width; x++) {
			const float* p = pixels + stride * ((size_t)row * width + x);
			double mean = 0.0;
			for (uint32_t c = 0; c < 3; c++) {
				const double watts = scale * (double)p[c];
				entry.channels[((size_t)y * width + x) * 3 + c] = watts;
				mean += watts;
			}
			mean /= 3.0;
			// a round sensor's film is its square: the pixels outside the disk are not the sensor's
			if (entry.geometry.shape == SensorShape::Disk && width > 1
				&& std::hypot(entry.image.centerX(x), entry.image.centerY(y)) > entry.geometry.halfWidth) {
				mean = 0.0;
			}
			entry.image.watts[(size_t)y * width + x] = mean;
			power += mean;
		}
	}
	entry.reading.power = power;
	measure(entry);
}

void DetectorFilms::recordSensorCopy(VkCommandBuffer cmd, VulkanEngine* engine, uint32_t slot, const AllocatedImage& accumulation, bool copy)
{
	SlotReadback& readback = m_slots[slot];
	readback.sensorSerial = 0;
	if (m_cameraPathTarget == 0 || find(m_cameraPathTarget) == nullptr || !copy) {
		return;
	}
	const uint32_t width = accumulation.imageExtent.width;
	const uint32_t height = accumulation.imageExtent.height;
	const VkDeviceSize bytes = (VkDeviceSize)width * height * 4 * sizeof(float);
	if (readback.sensorBytes != bytes) {
		if (readback.sensor.buffer != VK_NULL_HANDLE) {
			engine->destroyBuffer(readback.sensor);
		}
		readback.sensor = engine->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
		readback.sensorBytes = bytes;
	}
	//kernel 09's resolve wrote the film before this
	vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	VkBufferImageCopy region {};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { width, height, 1 };
	vkCmdCopyImageToBuffer(cmd, accumulation.image, VK_IMAGE_LAYOUT_GENERAL, readback.sensor.buffer, 1, &region);
	vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	readback.sensorWidth = width;
	readback.sensorHeight = height;
	readback.sensorSerial = m_serial;
	readback.layout = m_layout;
}

void DetectorFilms::collect(VulkanEngine* engine, uint32_t slot)
{
	if (slot >= m_slots.size()) {
		return;
	}
	//a frame of an earlier render than the current one is read too, into the readings that have nothing newer: a render
	//restarted every frame by an edit has no frame of its own read back before the next restart
	SlotReadback& readback = m_slots[slot];
	const bool sameLayout = readback.layout == m_layout;
	if (readback.sensorSerial != 0) {
		const uint32_t serial = readback.sensorSerial;
		readback.sensorSerial = 0;
		Entry* entry = find(m_cameraPathTarget);
		if (entry != nullptr && entry->cameraPaths && sameLayout && adopt(*entry, serial)) {
			vmaInvalidateAllocation(engine->m_memAllocator, readback.sensor.allocation, 0, readback.sensorBytes);
			readSensorFilm(*entry, (const float*)readback.sensor.info.pMappedData, 4, readback.sensorWidth, readback.sensorHeight);
		}
	}
	if (readback.serial == 0) {
		return;
	}
	const uint32_t serial = readback.serial;
	const bool filmCopied = readback.filmCopied;
	readback.serial = 0;
	readback.filmCopied = false;
	if (!sameLayout) {
		return;
	}
	vmaInvalidateAllocation(engine->m_memAllocator, readback.totals.allocation, 0, floatsBytes(COUNTER_FLOATS));
	const float* counters = (const float*)readback.totals.info.pMappedData;
	for (uint32_t i = 0; i < (uint32_t)m_entries.size(); i++) {
		Entry& entry = m_entries[i];
		if (entry.cameraPaths || !adopt(entry, serial)) {
			continue;
		}
		const uint32_t channels = std::min(entry.record.channels, 3u);
		double watts = 0.0;
		for (uint32_t c = 0; c < channels; c++) {
			watts += counters[i * TOTALS_PER_DETECTOR + c];
		}
		watts /= (double)std::max(channels, 1u);
		entry.collectedWatts += watts;
		entry.collectedPaths += readback.paths;
		entry.frames.add(watts / readback.paths, readback.paths);
		entry.reading.pathsReached += counters[MAX_DETECTORS * TOTALS_PER_DETECTOR + i];
		entry.reading.pathsEmitted = entry.collectedPaths;
		entry.reading.frames = entry.frames.frames;
		entry.reading.power = entry.collectedPaths > 0.0 ? entry.collectedWatts / entry.collectedPaths : 0.0;
		entry.reading.powerError = entry.frames.standardError();
	}
	if (filmCopied) {
		vmaInvalidateAllocation(engine->m_memAllocator, readback.film.allocation, 0, readback.filmBytes);
		const float* running = (const float*)readback.film.info.pMappedData;
		for (Entry& entry : m_entries) {
			if (!entry.cameraPaths && entry.readingSerial == serial) {
				readFilm(entry, running, readback.pathsAtCopy);
			}
		}
	}
}

bool DetectorFilms::readNow(VulkanEngine* engine)
{
	if (m_entries.empty() || m_runningFloats == 0) {
		return false;
	}
	std::vector<uint8_t> bytes;
	if (!readbackBuffer(engine, m_film.buffer, floatsBytes((VkDeviceSize)m_frameFloats + 2ull * m_runningFloats), bytes)) {
		return false;
	}
	const float* running = (const float*)bytes.data() + m_frameFloats;
	for (Entry& entry : m_entries) {
		readFilm(entry, running, m_pathsEmitted);
		//the films hold every path recorded, the totals only the frames collected so far: the power is the films'
		double watts = 0.0;
		for (const double w : entry.image.watts) {
			watts += w;
		}
		entry.reading.power = watts;
		entry.reading.pathsEmitted = m_pathsEmitted;
		entry.readingSerial = m_serial;
	}
	return true;
}

void DetectorFilms::readSensorNow(const std::vector<float>& rgb, uint32_t width, uint32_t height)
{
	if (Entry* entry = find(m_cameraPathTarget); entry != nullptr && rgb.size() >= (size_t)width * height * 3) {
		readSensorFilm(*entry, rgb.data(), 3, width, height);
	}
}

DetectorFilms::Entry* DetectorFilms::find(uint64_t opticId)
{
	for (Entry& entry : m_entries) {
		if (entry.opticId == opticId) {
			return &entry;
		}
	}
	return nullptr;
}

const DetectorFilms::Entry* DetectorFilms::find(uint64_t opticId) const
{
	return const_cast<DetectorFilms*>(this)->find(opticId);
}

void DetectorFilms::setPictureWanted(VulkanEngine* engine, uint64_t opticId, bool wanted)
{
	Entry* entry = find(opticId);
	if (entry == nullptr || entry->pictureWanted == wanted) {
		return;
	}
	entry->pictureWanted = wanted;
	if (!wanted || entry->picture.image != VK_NULL_HANDLE) {
		return;
	}
	const VkExtent3D extent { entry->record.pixelsX, entry->record.pixelsY, 1 };
	entry->picture = engine->createImage(extent, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	AllocatedImage image = entry->picture;
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		const VkClearColorValue black { { 0.f, 0.f, 0.f, 1.f } };
		const VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
		vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
		vkutil::transition_image(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});
	engine->m_displayRegistry.registerTexture(entry->pictureName, entry->picture.imageView, { extent.width, extent.height });
}

ImTextureID DetectorFilms::pictureTexture(VulkanEngine* engine, uint64_t opticId) const
{
	const Entry* entry = find(opticId);
	if (entry == nullptr || entry->picture.image == VK_NULL_HANDLE) {
		return 0;
	}
	return engine->m_displayRegistry.textureId(entry->pictureName);
}

void DetectorFilms::recordViews(VkCommandBuffer cmd, VulkanEngine* engine)
{
	std::vector<uint32_t> shown;
	for (uint32_t i = 0; i < (uint32_t)m_entries.size(); i++) {
		if (m_entries[i].pictureWanted && m_entries[i].picture.image != VK_NULL_HANDLE) {
			shown.push_back(i);
		}
	}
	if (shown.empty()) {
		return;
	}
	const VkPipelineStageFlags2 computeStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	//the fold's sums were written before this, and the last frame's pictures read the peaks before they are zeroed
	vkutil::memory_barrier(cmd, computeStage | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
		computeStage | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
	vkCmdFillBuffer(cmd, m_peaks.buffer, 0, VK_WHOLE_SIZE, 0);
	vkutil::memory_barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, computeStage, computeAccess);

	//each picture twice: its film's peak, then the picture drawn by it, so a render's first frame draws by its own peak. A
	//picture normalized by hand is drawn once
	std::vector<VkDescriptorSet> sets;
	std::vector<ViewPushConstants> constants;
	for (const uint32_t i : shown) {
		const Entry& entry = m_entries[i];
		const VkDescriptorSet set = engine->getCurrentFrame().frameDescriptors.allocate(engine->m_device, m_viewPass.setLayout);
		DescriptorWriter writer;
		writer.writeBuffer(0, m_film.buffer, m_filmBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.writeImage(1, entry.picture.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		writer.writeBuffer(2, m_peaks.buffer, sizeof(uint32_t) * MAX_DETECTORS, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		writer.updateSet(engine->m_device, set);
		sets.push_back(set);
		const double area = entry.geometry.pixelWidth * entry.geometry.pixelHeight;
		ViewPushConstants c {};
		c.first = m_frameFloats + entry.runningImage;
		c.floats = m_runningFloats;
		c.pixelsX = entry.record.pixelsX;
		c.pixelsY = entry.record.pixelsY;
		c.channels = entry.record.channels;
		c.mode = (entry.view.logarithmic ? 1u : 0u) | (entry.view.falseColor ? 2u : 0u) | (entry.view.autoNormalize ? 0u : 8u);
		c.scale = m_pathsEmitted > 0.0 && area > 0.0 ? (float)(1.0 / (m_pathsEmitted * area)) : 0.f;
		c.peakIndex = i;
		c.decades = entry.view.decades;
		c.normalization = entry.view.normalization;
		constants.push_back(c);
		vkutil::transition_image(cmd, entry.picture.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	}
	for (size_t k = 0; k < shown.size(); k++) {
		if (!m_entries[shown[k]].view.autoNormalize) {
			continue;
		}
		ViewPushConstants peak = constants[k];
		peak.mode |= 4u;
		dispatchComputePassOver(cmd, m_viewPass, sets[k], &peak, { peak.pixelsX, peak.pixelsY, 1 });
	}
	vkutil::memory_barrier(cmd, computeStage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, computeStage, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	for (size_t k = 0; k < shown.size(); k++) {
		dispatchComputePassOver(cmd, m_viewPass, sets[k], &constants[k], { constants[k].pixelsX, constants[k].pixelsY, 1 });
		vkutil::transition_image(cmd, m_entries[shown[k]].picture.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
}

VkDeviceSize DetectorFilms::deviceBytes(VmaAllocator allocator) const
{
	VkDeviceSize bytes = 0;
	auto add = [&](const AllocatedBuffer& buffer) {
		if (buffer.allocation != nullptr) {
			VmaAllocationInfo info {};
			vmaGetAllocationInfo(allocator, buffer.allocation, &info);
			bytes += info.size;
		}
	};
	add(m_records);
	add(m_film);
	add(m_peaks);
	for (const SlotReadback& slot : m_slots) {
		add(slot.totals);
		add(slot.film);
		add(slot.sensor);
	}
	return bytes;
}

//---------------------------------------------------------------- the batch mode's --detectors

namespace {

std::string jsonNumber(double value)
{
	return std::isfinite(value) ? fmt::format("{:.17g}", value) : std::string("null");
}

std::string jsonVector(const glm::vec3& v)
{
	return fmt::format("[{},{},{}]", jsonNumber(v.x), jsonNumber(v.y), jsonNumber(v.z));
}

std::string jsonText(const std::string& text)
{
	std::string out = "\"";
	for (const char c : text) {
		if (c == '"' || c == '\\') {
			out += '\\';
		}
		out += c;
	}
	return out + "\"";
}

// a file name from a detector's name: letters, digits, '-' and '_'
std::string fileStem(const std::string& name, uint64_t id)
{
	std::string stem;
	for (const char c : name) {
		stem += std::isalnum((unsigned char)c) || c == '-' || c == '_' ? c : '_';
	}
	return stem.empty() ? fmt::format("detector_{}", id) : stem;
}

}

IoResult writeDetectorReadings(VulkanEngine* engine, const std::filesystem::path& directory)
{
	DetectorFilms& films = engine->m_raytracer.gpu().detectors();
	if (!films.any()) {
		return IoResult::success("no detectors");
	}
	//camera paths render only the target, from the render's own film; light tracing deposits on every detector
	const uint64_t cameraPathTarget = films.cameraPathTarget();
	if (cameraPathTarget != 0) {
		Image film;
		if (!engine->m_raytracer.gpu().readAccumulation(engine, film)) {
			return IoResult::failure("the render's film could not be read back");
		}
		films.readSensorNow(film.rgb, film.width, film.height);
	} else if (!films.deposits()) {
		return IoResult::failure("the detectors record under Light tracing, or as the render target under the path tracer");
	} else if (!films.readNow(engine)) {
		return IoResult::failure("the detectors' films could not be read back");
	}
	std::error_code ec;
	std::filesystem::create_directories(directory, ec);
	const bool monochromatic = films.spectral().representation == SpectralRepresentation::Monochromatic;
	std::string json = fmt::format("{{\n  \"pathsEmitted\": {},\n  \"cameraPaths\": {},\n  \"representation\": {},\n  \"unit\": {},\n  \"detectors\": [", jsonNumber(films.pathsEmitted()),
		cameraPathTarget != 0 ? "true" : "false",
		jsonText(spectralRepresentationKey(films.spectral().representation)), jsonText(monochromatic ? "W/nm" : "W"));
	bool first = true;
	for (const DetectorFilms::Entry& entry : films.entries()) {
		if (cameraPathTarget != 0 && entry.opticId != cameraPathTarget) {
			continue;
		}
		const SceneOptic* optic = engine->m_raytraceScene.findOptic(entry.opticId);
		const std::string name = optic != nullptr ? optic->name : fmt::format("detector_{}", entry.opticId);
		const std::string stem = fileStem(name, entry.opticId);
		const GpuDetector& d = entry.record;
		// the film, row 0 at the top: the film's last row
		ChannelImage image;
		image.width = d.pixelsX;
		image.height = d.pixelsY;
		const char* const rgb[] = { "R", "G", "B" };
		//a camera-path target's film is the render's, three channels in every mode
		const uint32_t channels = entry.cameraPaths ? 3u : d.channels;
		for (uint32_t c = 0; c < channels; c++) {
			ImageChannel channel;
			channel.name = channels == 3 ? rgb[c] : "P";
			channel.values.resize((size_t)d.pixelsX * d.pixelsY);
			for (uint32_t y = 0; y < d.pixelsY; y++) {
				for (uint32_t x = 0; x < d.pixelsX; x++) {
					const size_t film = ((size_t)(d.pixelsY - 1 - y) * d.pixelsX + x) * channels + c;
					channel.values[(size_t)y * d.pixelsX + x] = film < entry.channels.size() ? (float)entry.channels[film] : 0.f;
				}
			}
			image.channels.push_back(std::move(channel));
		}
		image.attributes.push_back(ImageAttribute::text("detector", name));
		image.attributes.push_back(ImageAttribute::text("units", monochromatic ? "W/nm per pixel" : "W per pixel"));
		image.attributes.push_back(ImageAttribute::real("pixelWidth", (float)entry.geometry.pixelWidth));
		image.attributes.push_back(ImageAttribute::real("pixelHeight", (float)entry.geometry.pixelHeight));
		image.attributes.push_back(ImageAttribute::real("pathsEmitted", (float)films.pathsEmitted()));
		const ImageIoStatus status = saveChannelImage(directory / (stem + ".exr"), image);
		if (!status.ok) {
			return IoResult::failure(status.message);
		}

		const DetectorReading& r = entry.reading;
		std::string detector = fmt::format("\n    {{\"name\": {}, \"kind\": {}, \"film\": {}, \"power\": {}, \"powerError\": {}, \"pathsReached\": {}, \"frames\": {}, "
			"\"pixels\": [{}, {}], \"pixelSize\": [{}, {}], \"origin\": {}, \"x\": {}, \"y\": {}, \"normal\": {}",
			jsonText(name), jsonText(opticTraits(entry.kind).name), jsonText(stem + ".exr"), jsonNumber(r.power), jsonNumber(r.powerError), jsonNumber(r.pathsReached), r.frames,
			d.pixelsX, d.pixelsY, jsonNumber(entry.geometry.pixelWidth), jsonNumber(entry.geometry.pixelHeight), jsonVector(glm::vec3(d.origin)), jsonVector(glm::vec3(d.uAxis)),
			jsonVector(glm::vec3(d.vAxis)), jsonVector(glm::vec3(d.normal)));
		if (channels == 3 && entry.channels.size() >= 3) {
			double sums[3] = {};
			for (size_t i = 0; i < entry.channels.size(); i++) {
				sums[i % 3] += entry.channels[i];
			}
			detector += fmt::format(", \"channels\": [{}, {}, {}]", jsonNumber(sums[0]), jsonNumber(sums[1]), jsonNumber(sums[2]));
		}
		if (r.moments && r.moments->power > 0.0) {
			const BeamMoments& m = *r.moments;
			detector += fmt::format(", \"centroid\": [{}, {}], \"sigma\": [{}, {}, {}], \"diameter\": [{}, {}], \"principal\": [{}, {}], \"angle\": {}, \"iterations\": {}, "
				"\"converged\": {}, \"peak\": {}",
				jsonNumber(m.centroidX), jsonNumber(m.centroidY), jsonNumber(m.sigmaXX), jsonNumber(m.sigmaYY), jsonNumber(m.sigmaXY), jsonNumber(m.diameterX), jsonNumber(m.diameterY),
				jsonNumber(m.diameterMajor), jsonNumber(m.diameterMinor), jsonNumber(m.angle), m.iterations, m.converged ? "true" : "false", jsonNumber(r.peak));
		}
		if (!r.spectrum.empty()) {
			std::string wavelengths;
			std::string values;
			for (size_t i = 0; i < r.spectrum.size(); i++) {
				wavelengths += (i > 0 ? "," : "") + jsonNumber(r.spectrumWavelength[i]);
				values += (i > 0 ? "," : "") + jsonNumber(r.spectrum[i]);
			}
			detector += fmt::format(", \"spectrum\": {{\"wavelength\": [{}], \"wattsPerNm\": [{}]}}", wavelengths, values);
		}
		json += (first ? "" : ",") + detector + "}";
		first = false;
	}
	json += "\n  ]\n}\n";
	std::FILE* file = std::fopen((directory / "detectors.json").string().c_str(), "wb");
	if (file == nullptr) {
		return IoResult::failure(fmt::format("cannot write {}", (directory / "detectors.json").string()));
	}
	std::fwrite(json.data(), 1, json.size(), file);
	std::fclose(file);
	return IoResult::success(fmt::format("{} detector readings to {}", films.entries().size(), directory.string()));
}
