#pragma once

// LASER SOURCES (docs/plans/completed/physoptics-laser-sources.md), part of rt_optics: what a laser element's parameters
// (OpticKind::Laser, src/optic_catalog.h) make of its beam. The light list packs it for the path tracer's light paths
// (src/rt_lights.h), the panel shows its readouts, and the beam solver (physoptics-gaussian-beam-solver.md) takes its
// root segment from it.
//
// A beam is two independent transverse axes, x and y, each a Gaussian beam of its own waist, waist position and M^2
// (Siegman's embedded Gaussian). Along one axis the 1/e^2 intensity radius at a distance d from the waist is
//
//     w(d)^2 = w0^2 + theta^2 d^2,     theta = M^2 lambda / (pi w0), the far-field half angle at 1/e^2
//
// which is also its second-moment radius, twice the standard deviation of the intensity. Lengths are in meters and
// angles in radians; the wavelength is in meters, as every optic parameter is SI.

#include <optional>
#include <span>
#include <string>

#include <glm/glm.hpp>

#include <optic_catalog.h>
#include <spectral.h>
#include <spectrum_ref.h>

// one transverse axis of a beam
struct LaserAxisBeam {
	// the 1/e^2 intensity radius at the waist
	double waist { 0.0 };
	// where the waist is along the beam, from the aperture plane: negative inside the housing
	double waistPosition { 0.0 };
	double m2 { 1.0 };
	// M^2 lambda / (pi w0): the far-field half angle at 1/e^2, as a slope
	double halfAngle { 0.0 };

	// w0 / theta, the distance over which the radius grows by sqrt(2); infinite for a beam that does not diverge
	double rayleighRange() const;
	// the 1/e^2 radius at `z` along the beam from the aperture plane
	double radius(double z) const;
};

// what a laser's spectrum is
enum class LaserSpectrumKind : uint8_t {
	// a Lorentzian line: a single-frequency laser's homogeneous line
	LorentzianLine,
	// a Gaussian line or band: a Doppler-broadened gas laser's line, or a pulse's bandwidth
	GaussianLine
};

struct LaserBeam {
	// in the body frame: the beam's reference plane passes through `center` square to `axis`, the way the beam leaves,
	// and `xAxis` is the beam's own x axis in it. The pointing offsets move and turn them from the housing's output
	glm::dvec3 center { 0.0 };
	glm::dvec3 axis { 0.0, 0.0, 1.0 };
	glm::dvec3 xAxis { 1.0, 0.0, 0.0 };
	LaserAxisBeam x;
	LaserAxisBeam y;
	LaserSpectrumKind spectrumKind { LaserSpectrumKind::GaussianLine };
	// the line's center and its full width at half maximum
	double wavelength { 632.8e-9 };
	double linewidth { 0.0 };
	// average power, watts
	double power { 0.0 };
	// what a spectral build evaluates, a line integrating to 1 over nanometers; the light's power multiplies it
	SpectrumRef spectrum;
	// what exact RGB carries: linear sRGB whose three channels average to 1, so a beam of P watts is P in each channel
	// on average. The wavelength's display color (wavelengthDisplayColor(), src/spectral.h), which shows a line outside
	// the visible range in the display's false colors
	glm::dvec3 rgb { 1.0 };
	// the line's coherence length: c / (pi dnu) for a Lorentzian line, sqrt(2 ln 2 / pi) c / dnu for a Gaussian one.
	// Nothing reads it yet; the wave backend will
	double coherenceLength { 0.0 };
	// the output aperture's radius: the housing's, or three times the beam's larger radius at the aperture when that is
	// wider, so the disk holds all but 1.5e-8 of the power the light paths start with
	double apertureRadius { 0.0 };
};

// the beam of a laser element; a default beam for any other kind
LaserBeam laserBeam(const SceneOptic& optic);

// The waist radius and M^2 that give a 1/e^2 diameter `diameter` at the aperture and a full-angle divergence
// `divergence`, with the waist `waistPosition` from the aperture, at `wavelength`: what the panel's diameter and
// divergence fields set. Nothing when no Gaussian beam has them: a diameter the divergence alone exceeds at that
// distance, or an M^2 below 1
struct LaserAxisFit {
	double waist { 0.0 };
	double m2 { 1.0 };
};
std::optional<LaserAxisFit> fitLaserAxis(double diameter, double divergence, double waistPosition, double wavelength);

// The housings a laser can have: the placeholder model it stands for (mountModels()'s id, src/optic_models.h), whose
// size the Viewport outlines and which a STEP file replaces later, and the output aperture's diameter. No housing is
// geometry yet: the tracer sees the aperture alone
struct LaserHousing {
	const char* key;
	const char* label;
	const char* model;
	double apertureDiameter;
};
const LaserHousing& laserHousing(int index);

// the one laser of `optics`, or null when there are none or several
const SceneOptic* singleLaser(std::span<const SceneOptic> optics);
// THE DESIGN WAVELENGTH a render and the readouts use, nm: the settings', or the scene's one laser's while the settings
// follow it (SpectralSettings::designFollowsLaser), so exact RGB reads the glasses where the laser is
float designWavelength(const SpectralSettings& settings, std::span<const SceneOptic> optics);
