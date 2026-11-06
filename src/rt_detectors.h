#pragma once

// THE DETECTOR FILMS on the GPU (docs/plans/completed/physoptics-detectors.md): bindings 59 (the records, src/rt_accel.h
// GpuDetector, after a header) and 60 (the films), the per-frame fold into compensated sums
// (shaders/rt/light/detector_fold.comp), the readbacks the readings come from, and the pictures the Raytraced Output window
// shows (shaders/rt/light/detector_view.comp). GpuRenderer owns one and drives it: start() with each render, the fold and
// the copies after the light integrator's frame, collect() FRAME_OVERLAP frames later. Light tracing's stage 26 adds the
// frame's light paths to the films; no other integrator deposits.
//
// A restart (an edit while the render runs) keeps the readings until a frame of the new render is read back: the
// readings are always the newest render's that has reached the CPU, which while an edit goes on every frame is one a
// frame or two behind. The pictures are drawn on the GPU from the films each frame, the frame's own.
//
// BINDING 60: every detector's frame sums, each image in as many copies as frameCopies() gives it (src/optic_detectors.h)
// and a spectrum in SPECTRUM_COPIES, then every detector's R running sums and their R Neumaier compensations, then
// MAX_DETECTORS x 4 frame totals (an image's per channel) and MAX_DETECTORS counts of the paths that reached each
// detector in the frame. The totals and counts are copied back every frame, the running sums
// twice a second and when a render ends. Everything is the light paths' power unnormalized: the readings divide by the
// paths emitted, a pixel sample's one light path each.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <imgui.h>

#include <optic_detectors.h>
#include <rt_accel.h>
#include <spectral.h>
#include <vk_compute.h>
#include <vk_types.h>

class VulkanEngine;
class CrtResourceTable;
struct IoResult;

// how a detector's window shows its film
struct DetectorViewSettings {
	bool logarithmic { false };
	// decades below the peak a logarithmic picture spans
	float decades { 4.f };
	bool falseColor { true };
	// the irradiance at the picture's top, W/m^2, above which it clips; with `autoNormalize` the film's peak each frame
	bool autoNormalize { true };
	float normalization { 0.f };
	bool operator==(const DetectorViewSettings&) const = default;
};

class DetectorFilms {
public:
	struct Entry {
		uint64_t opticId { 0 };
		OpticKind kind { OpticKind::PowerMeter };
		GpuDetector record;
		// where its running sums start, the image's and the spectrum's, among every detector's
		uint32_t runningImage { 0 };
		uint32_t runningSpectrum { 0 };
		DetectorGeometry geometry;
		DetectorReading reading;
		// the render (start()'s serial) the reading, the frames and the totals below are of
		uint32_t readingSerial { 0 };
		FrameStatistics frames;
		// the frames' totals and counts so far, which the power is
		double collectedWatts { 0.0 };
		double collectedPaths { 0.0 };
		// the last film read back, in watts: the image (the channels' mean in exact RGB) and every channel apart
		DetectorImage image;
		std::vector<double> channels;
		// the render target, rendered by camera paths from its pixels (CameraKind::Sensor) rather than by light tracing's
		// deposits: its readings come from the render's own film
		bool cameraPaths { false };
		// the window's picture
		DetectorViewSettings view;
		AllocatedImage picture {};
		std::string pictureName;
		bool pictureWanted { false };
	};

	void init(VulkanEngine* engine, CrtResourceTable& resources);
	void destroy(VulkanEngine* engine, CrtResourceTable& resources);

	// a render's detectors: the scene's records, the films sized for its mode, cleared on its first frame. `deposits` is
	// whether its integrator runs stage 26
	void start(VulkanEngine* engine, CrtResourceTable& resources, const std::shared_ptr<const RaytraceSceneAccel>& scene, const SpectralSettings& spectral,
		bool deposits, uint64_t cameraPathTarget);
	// stage 26 runs this render
	bool deposits() const { return m_deposits && !m_entries.empty(); }
	// the scene's detectors, whatever the integrator
	bool any() const { return !m_entries.empty(); }

	// forgets every reading and picture: the scene they belonged to is gone
	void discard(VulkanEngine* engine);
	// zeroes the films, on the render's first frame
	void recordClear(VkCommandBuffer cmd);
	// after the frame's schedule: the fold, the totals' copy, and the running sums' copy when `copyFilm`. `paths` is the
	// light paths the frame emitted
	void recordFrame(VkCommandBuffer cmd, VulkanEngine* engine, uint32_t slot, double paths, bool copyFilm);
	// the pictures of the windows that want one, from the running sums
	void recordViews(VkCommandBuffer cmd, VulkanEngine* engine);
	// a detector render target that camera paths render (CameraKind::Sensor): the render's film is its film, copied back
	// twice a second and when the render ends (`copy`) to make its readings
	void recordSensorCopy(VkCommandBuffer cmd, VulkanEngine* engine, uint32_t slot, const AllocatedImage& accumulation, bool copy);
	uint64_t cameraPathTarget() const { return m_cameraPathTarget; }
	// draw() has waited on this slot's fence
	void collect(VulkanEngine* engine, uint32_t slot);
	// the films read now, with the device idle: what the batch mode writes after a render. A camera-path target's film is
	// the render's, read by the caller (GpuRenderer::readAccumulation(): rgb, row 0 at the top)
	bool readNow(VulkanEngine* engine);
	void readSensorNow(const std::vector<float>& rgb, uint32_t width, uint32_t height);

	std::span<const Entry> entries() const { return m_entries; }
	Entry* find(uint64_t opticId);
	const Entry* find(uint64_t opticId) const;
	double pathsEmitted() const { return m_pathsEmitted; }
	// the render's spectral mode: a monochromatic render reads W/nm, a spectral one has no RGB channels
	const SpectralSettings& spectral() const { return m_spectral; }
	uint32_t leftOut() const { return m_leftOut; }
	// asks for the running sums' copy on the next frame
	void requestFilm() { m_filmRequested = true; }
	bool filmRequested() const { return m_filmRequested; }

	// a picture for the detector's window, made each frame while `wanted`; 0 until there is one
	void setPictureWanted(VulkanEngine* engine, uint64_t opticId, bool wanted);
	ImTextureID pictureTexture(VulkanEngine* engine, uint64_t opticId) const;

	VkDeviceSize deviceBytes(VmaAllocator allocator) const;

private:
	void publish(CrtResourceTable& resources);
	void readFilm(Entry& entry, const float* running, double paths);
	void readSensorFilm(Entry& entry, const float* pixels, uint32_t stride, uint32_t width, uint32_t height);
	void measure(Entry& entry);
	// whether a readback of render `serial` may change the entry's reading: one of a newer render than the reading's starts
	// it over, one of an older render may not
	static bool adopt(Entry& entry, uint32_t serial);
	void destroyPictures(VulkanEngine* engine);

	ComputePass m_foldPass;
	ComputePass m_viewPass;
	AllocatedBuffer m_records {};
	AllocatedBuffer m_film {};
	// the pictures' peaks, MAX_DETECTORS uints (detector_view.comp)
	AllocatedBuffer m_peaks {};
	VkDeviceSize m_recordBytes { 0 };
	VkDeviceSize m_filmBytes { 0 };
	// the floats of every detector's frame sums with their copies, and of their running sums
	uint32_t m_frameFloats { 0 };
	uint32_t m_runningFloats { 0 };
	std::vector<Entry> m_entries;
	// what the records and films were built for
	std::shared_ptr<const RaytraceSceneAccel> m_scene;
	SpectralSettings m_spectral;
	bool m_deposits { false };
	uint64_t m_cameraPathTarget { 0 };
	uint32_t m_leftOut { 0 };
	uint32_t m_serial { 0 };
	// changes when the entries or the places of their sums do: a readback of another layout is not read
	uint32_t m_layout { 0 };
	double m_pathsEmitted { 0.0 };
	bool m_filmRequested { false };

	// per frame slot: the totals and counts, and the running sums when copied, with what the frame recorded
	struct SlotReadback {
		AllocatedBuffer totals {};
		AllocatedBuffer film {};
		VkDeviceSize filmBytes { 0 };
		uint32_t serial { 0 };
		uint32_t layout { 0 };
		double paths { 0.0 };
		double pathsAtCopy { 0.0 };
		bool filmCopied { false };
		// the camera-path target's film
		AllocatedBuffer sensor {};
		VkDeviceSize sensorBytes { 0 };
		uint32_t sensorWidth { 0 };
		uint32_t sensorHeight { 0 };
		uint32_t sensorSerial { 0 };
	};
	std::vector<SlotReadback> m_slots;
};

// The batch mode's --detectors (docs/plans/completed/physoptics-detectors.md): every detector of the last render read now,
// with the device idle, to <directory>/detectors.json (the readings, each detector's frame, its moments and spectrum)
// and its film to <directory>/<name>.exr, in watts per pixel: R, G and B in exact RGB, P in a spectral mode, row 0 at
// the sensor's top (+y), with the pixel size and the paths as attributes
IoResult writeDetectorReadings(VulkanEngine* engine, const std::filesystem::path& directory);
