#pragma once

// DETECTORS (docs/plans/completed/physoptics-detectors.md), part of rt_optics: what each detector kind records and where,
// and the measurements a lab reports from what it recorded. A detector is an optical element (OpticKind::PowerMeter and
// the kinds after it) whose body is one flat sensor facing the incoming light: its face at z = 0 of the body frame, its
// front toward -z, so light travelling along +z reaches it. Light paths of the light integrator deposit their power on
// the sensor's film as they reach its front (stage 26, shaders/rt/2601_deposit_detector.comp); this file says how large
// that film is and turns it back into watts, centroids, widths and spectra.
//
// THE FILM'S AXES are the body frame's: column u along +x, row v along +y, row 0 at y = -half height. A reading's
// positions are in meters from the sensor's center in that frame.
//
// Everything here is CPU-only and double, so bin/optics_test checks the measurements against closed forms.

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optic_catalog.h>

// what a sensor does with the light after recording it
enum class DetectorAction : uint8_t {
	// ends every path (a power meter, a camera, a beam dump): its slot is a `detector` material
	Absorb,
	// scatters it as its slot's material does, a diffuse white by default (an alignment card, a screen)
	Scatter,
	// lets it through unchanged and is not geometry at all (a probe plane)
	Transparent
};

enum class SensorShape : uint8_t {
	Disk,
	Rectangle
};

// the most detectors a scene's films hold; more are left out of the render and the panel says so
inline constexpr uint32_t MAX_DETECTORS = 32;
// A FRAME'S SUMS ARE KEPT IN COPIES, one per (path index mod the copies): a frame of light tracing can add a million paths'
// power to the few pixels a beam lands on, and a float that many equal terms are added to loses about 1e-3 of them (a
// power meter read 4.995 mW of 5 mW). The fold adds the copies, each that much shorter a sum, into the compensated running
// sum. A film keeps as many copies, up to 4096 (a one-pixel meter's), as fit in 2^20 floats; a spectrum keeps 8
inline constexpr uint32_t MAX_FRAME_COPIES = 4096;
inline constexpr uint32_t FRAME_COPY_FLOATS = 1u << 20;
inline constexpr uint32_t SPECTRUM_COPIES = 8;
// the copies of a film of `floats` a frame keeps: a power of two
uint32_t frameCopies(uint32_t floats);

// What a detector element records: from its kind and parameters, whatever the render mode
struct DetectorGeometry {
	SensorShape shape { SensorShape::Rectangle };
	DetectorAction action { DetectorAction::Absorb };
	// the sensor's half extent along x and y; a disk's radius in both
	double halfWidth { 0.0 };
	double halfHeight { 0.0 };
	// the film: its pixels along x and y and the size of one
	uint32_t pixelsX { 1 };
	uint32_t pixelsY { 1 };
	double pixelWidth { 0.0 };
	double pixelHeight { 0.0 };
	// a physical sensor's own pixels (a camera sensor's or a profiler's, when it names one), which set the size with
	// their pitch and which the film downsamples; the film's otherwise
	bool physical { false };
	uint32_t nativeX { 1 };
	uint32_t nativeY { 1 };
	// the image is a reading (cameras, profilers, cards, screens, probe planes); a power meter, a spectrometer and a beam
	// dump are one pixel
	bool imaging { false };
	// a spectrometer: its bins over [lambdaMin, lambdaMax] nm
	bool spectrometer { false };
	double lambdaMin { 0.0 };
	double lambdaMax { 0.0 };
	uint32_t bins { 0 };
	// the placeholder model the Viewport outlines behind the sensor (src/optic_models.h), or null
	const char* housing { nullptr };
};

bool isDetectorKind(OpticKind kind);

// A SENSOR STACK: `count` identical copies of one detector spaced evenly from `start` to `end`, all turned alike, as one
// detector slid along a rail or a translation stage reads a beam at each stop. Every member has the sensor's size and
// film wherever it is along the axis (a size that follows the beam is the beam solver's, physoptics-gaussian-beam-solver.md).
// The editor makes each member an ordinary detector (SceneOptic::stack), so the render, the readings and the render
// target treat it as one; the scene file saves only the stack
struct SensorStack {
	// shares the editor's id space with the optics; never saved
	uint64_t id { 0 };
	std::string name;
	// the detector every member copies: its kind, its parameters and its slot materials, which the stack owns. Its name,
	// mount and transform are not used
	SceneOptic sensor;
	// the first member's and the last member's sensor centers
	glm::dvec3 start { 0.0, 0.1, 0.0 };
	glm::dvec3 end { 0.0, 0.1, 0.1 };
	int count { 5 };
	// every member's body frame -> world rotation. With alignToAxis, only its turn about the axis is kept: each sensor
	// faces along start -> end, light travelling that way reaching its front
	glm::dquat orientation { 1.0, 0.0, 0.0, 0.0 };
	bool alignToAxis { true };

	bool operator==(const SensorStack&) const = default;
};

// a stack's members: as many as a scene's films hold
inline constexpr int MAX_STACK_SENSORS = (int)MAX_DETECTORS;

// a new stack of `kind`, five sensors 100 mm apart along +z
SensorStack makeSensorStack(OpticKind kind);
// the unit direction from start to end; zero when they coincide
glm::dvec3 sensorStackAxis(const SensorStack& stack);
// the members' rotation: `orientation`, or with alignToAxis the rotation that takes +z to the axis with +x as near the
// orientation's own x as it can be (its y when its x lies along the axis). `orientation` as it is when start = end
glm::dquat sensorStackOrientation(const SensorStack& stack);
// a member's sensor center: start to end in count - 1 equal steps, the one member of a stack of one at start
glm::dvec3 sensorStackPosition(const SensorStack& stack, int member);
// "<stack>_<member>", from 0
std::string sensorStackMemberName(const SensorStack& stack, int member);
DetectorGeometry detectorGeometry(const SceneOptic& optic);
// the film's floats in a render mode with `channels` per pixel (3 in exact RGB, 1 in a spectral mode), and the
// spectrum's bins (a spectrometer in a spectral mode), each once: the running sums'
uint32_t detectorImageFloats(const DetectorGeometry& geometry, uint32_t channels);
uint32_t detectorSpectrumBins(const DetectorGeometry& geometry, bool spectral);

// The catalog's builder for every detector kind: one flat sensor at z = 0 facing -z, of the kind's shape
void buildDetector(const SceneOptic& optic, const OpticBuildContext& context, OpticPrescription& prescription, OpticReadout& readout);

//---------------------------------------------------------------- measurements

// A film as watts per pixel, one channel (the reading's: exact RGB's mean of three, a spectral mode's one). Row-major,
// row 0 at y = -half height
struct DetectorImage {
	uint32_t width { 0 };
	uint32_t height { 0 };
	double pixelWidth { 0.0 };
	double pixelHeight { 0.0 };
	std::vector<double> watts;

	double at(uint32_t x, uint32_t y) const { return watts[(size_t)y * width + x]; }
	// a pixel's center, from the sensor's center
	double centerX(uint32_t x) const { return ((double)x + 0.5 - 0.5 * (double)width) * pixelWidth; }
	double centerY(uint32_t y) const { return ((double)y + 0.5 - 0.5 * (double)height) * pixelHeight; }
};

// ISO 11146-1's first and second moments of an image's power density, and the beam widths they give
struct BeamMoments {
	double power { 0.0 };
	double centroidX { 0.0 };
	double centroidY { 0.0 };
	double sigmaXX { 0.0 };
	double sigmaYY { 0.0 };
	double sigmaXY { 0.0 };
	// the second-moment (D4 sigma) diameters along x and y, and along the principal axes, the first at `angle` from +x
	double diameterX { 0.0 };
	double diameterY { 0.0 };
	double diameterMajor { 0.0 };
	double diameterMinor { 0.0 };
	double angle { 0.0 };
	// the ISO 11146-3 iterations the integration area took to settle (0 for the whole image)
	uint32_t iterations { 0 };
	bool converged { true };
};

// the moments over the whole image
BeamMoments imageMoments(const DetectorImage& image);
// ISO 11146-3's iterative integration area for a noisy image: the moments within a rectangle three times the widths
// across, centered on the centroid and aligned with the principal axes, recomputed until the widths settle to `tolerance`
BeamMoments iso11146Moments(const DetectorImage& image, double tolerance = 1e-4, uint32_t maxIterations = 50);

// ISO 11146-1's fit of a beam's caustic: second-moment radii w at positions z along the beam, fitted by least squares as
// w^2 = a + b z + c z^2, which a beam of any M^2 follows in free space. The waist radius, its position and the far-field
// half angle; nullopt for fewer than three radii or a caustic that does not open (c <= 0, or a waist below zero)
struct CausticFit {
	double waist { 0.0 };
	double position { 0.0 };
	double halfAngle { 0.0 };

	double radius(double z) const { return std::sqrt(waist * waist + halfAngle * halfAngle * (z - position) * (z - position)); }
	// pi w0 theta / lambda
	double m2(double wavelength) const { return 3.14159265358979323846 * waist * halfAngle / wavelength; }
};
std::optional<CausticFit> fitCaustic(std::span<const double> z, std::span<const double> w);

// the brightest pixel's power over its area, W/m^2
double peakIrradiance(const DetectorImage& image);

// the power within each radius of a point, at `samples` radii from 0 to `maxRadius` (pixels count by their centers)
struct EncircledPower {
	std::vector<double> radius;
	std::vector<double> power;
};
EncircledPower encircledPower(const DetectorImage& image, double centerX, double centerY, double maxRadius, uint32_t samples = 64);

// the irradiance (W/m^2) along a line through a point at an angle from +x, bilinear, at `samples` points over the image
struct Profile {
	std::vector<double> position;
	std::vector<double> irradiance;
};
Profile lineProfile(const DetectorImage& image, double centerX, double centerY, double angle, uint32_t samples = 256);

// What the Raytraced Output window and the batch mode report for a detector
struct DetectorReading {
	// watts in exact RGB and the spectral modes that integrate over wavelength; a monochromatic render's W/nm
	double power { 0.0 };
	// from the spread of the per-frame readings; 0 until two frames have been read
	double powerError { 0.0 };
	// light paths emitted over the render, and how many of them deposited on this detector
	double pathsEmitted { 0.0 };
	double pathsReached { 0.0 };
	uint32_t frames { 0 };
	// present for a detector that keeps an image
	std::optional<BeamMoments> moments;
	double peak { 0.0 };
	// a spectrometer's spectrum in W/nm at its bins' centers
	std::vector<double> spectrumWavelength;
	std::vector<double> spectrum;
};

// the mean and standard error of per-frame readings, each of n paths: Welford's running sums weighted by the paths
struct FrameStatistics {
	double weight { 0.0 };
	double mean { 0.0 };
	double m2 { 0.0 };
	uint32_t frames { 0 };

	void add(double reading, double paths);
	double standardError() const;
};
