#include <optic_laser.h>

#include <algorithm>
#include <cmath>
#include <limits>


namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double SPEED_OF_LIGHT = 299792458.0;
// the narrowest line a spectrum is given: a line's profile divides by its width
constexpr double MIN_LINEWIDTH = 1e-15;

// Thorlabs' parts the placeholders stand for (src/optic_models.cpp). The apertures are nominal: the beam's own size
// widens one that is too small
constexpr LaserHousing HOUSINGS[] = {
	{ "laser_head", "Laser head (HNL050LB)", "laser_head", 2e-3 },
	{ "diode_module", "Diode module (CPS635R)", "diode_module", 5e-3 },
	{ "fiber_collimator", "Fiber collimator (F220APC-633)", "fiber_collimator", 5.5e-3 },
};

LaserAxisBeam axisBeam(double waist, double waistPosition, double m2, double wavelength)
{
	LaserAxisBeam a;
	a.waist = std::max(waist, 1e-9);
	a.waistPosition = waistPosition;
	a.m2 = std::max(m2, 1.0);
	a.halfAngle = a.m2 * wavelength / (PI * a.waist);
	return a;
}

}

double LaserAxisBeam::rayleighRange() const
{
	return halfAngle > 0.0 ? waist / halfAngle : std::numeric_limits<double>::infinity();
}

double LaserAxisBeam::radius(double z) const
{
	const double d = z - waistPosition;
	return std::sqrt(waist * waist + halfAngle * halfAngle * d * d);
}

const LaserHousing& laserHousing(int index)
{
	return HOUSINGS[std::clamp(index, 0, (int)std::size(HOUSINGS) - 1)];
}

LaserBeam laserBeam(const SceneOptic& optic)
{
	LaserBeam beam;
	if (optic.kind != OpticKind::Laser) {
		return beam;
	}
	beam.wavelength = std::max(opticNumber(optic, "wavelength"), 1e-9);
	beam.linewidth = std::max(opticNumber(optic, "linewidth"), MIN_LINEWIDTH);
	beam.spectrumKind = opticEnum(optic, "spectrum") == 0 ? LaserSpectrumKind::LorentzianLine : LaserSpectrumKind::GaussianLine;
	beam.power = std::max(opticNumber(optic, "power"), 0.0);

	// the y axis is the x axis's twin unless it has its own waist, waist position and M^2
	const bool astigmatic = opticBool(optic, "astigmatic");
	beam.x = axisBeam(opticNumber(optic, "waistRadius"), opticNumber(optic, "waistPosition"), opticNumber(optic, "m2"), beam.wavelength);
	beam.y = astigmatic ? axisBeam(opticNumber(optic, "waistRadiusY"), opticNumber(optic, "waistPositionY"), opticNumber(optic, "m2Y"), beam.wavelength) : beam.x;

	// the housing's output is the body frame's origin and +z; the pointing turns the beam toward +x and +y by its angles
	// and moves it across the output
	beam.axis = glm::normalize(glm::dvec3(std::tan(opticNumber(optic, "pointingX")), std::tan(opticNumber(optic, "pointingY")), 1.0));
	const double ellipse = astigmatic ? opticNumber(optic, "ellipseAngle") : 0.0;
	const glm::dvec3 x(std::cos(ellipse), std::sin(ellipse), 0.0);
	beam.xAxis = glm::normalize(x - glm::dot(x, beam.axis) * beam.axis);
	beam.center = glm::dvec3(opticNumber(optic, "offsetX"), opticNumber(optic, "offsetY"), 0.0);

	const double nm = beam.wavelength * 1e9;
	beam.spectrum = lineRef((float)nm, (float)(beam.linewidth * 1e9), beam.spectrumKind == LaserSpectrumKind::LorentzianLine ? LineProfile::Lorentzian : LineProfile::Gaussian, 1.f);
	const glm::dvec3 color = glm::dvec3(wavelengthDisplayColor((float)nm, SpectralDisplay {}));
	const double mean = (color.r + color.g + color.b) / 3.0;
	beam.rgb = mean > 0.0 ? color / mean : glm::dvec3(1.0);

	const double frequencyWidth = SPEED_OF_LIGHT * beam.linewidth / (beam.wavelength * beam.wavelength);
	beam.coherenceLength = beam.spectrumKind == LaserSpectrumKind::LorentzianLine ? SPEED_OF_LIGHT / (PI * frequencyWidth)
	                                                                             : std::sqrt(2.0 * std::log(2.0) / PI) * SPEED_OF_LIGHT / frequencyWidth;

	const double housing = 0.5 * laserHousing(opticEnum(optic, "housing")).apertureDiameter;
	beam.apertureRadius = std::max(housing, 3.0 * std::max(beam.x.radius(0.0), beam.y.radius(0.0)));
	return beam;
}

std::optional<LaserAxisFit> fitLaserAxis(double diameter, double divergence, double waistPosition, double wavelength)
{
	const double radius = 0.5 * diameter;
	const double halfAngle = 0.5 * divergence;
	const double waist2 = radius * radius - halfAngle * halfAngle * waistPosition * waistPosition;
	if (!(radius > 0.0) || !(halfAngle > 0.0) || !(waist2 > 0.0) || !(wavelength > 0.0)) {
		return std::nullopt;
	}
	LaserAxisFit fit;
	fit.waist = std::sqrt(waist2);
	fit.m2 = PI * fit.waist * halfAngle / wavelength;
	// a hair under 1 is a data sheet's rounding; well under is no Gaussian beam
	if (fit.m2 < 1.0 - 1e-3) {
		return std::nullopt;
	}
	fit.m2 = std::max(fit.m2, 1.0);
	return fit;
}

const SceneOptic* singleLaser(std::span<const SceneOptic> optics)
{
	const SceneOptic* found = nullptr;
	for (const SceneOptic& optic : optics) {
		if (optic.kind == OpticKind::Laser) {
			if (found != nullptr) {
				return nullptr;
			}
			found = &optic;
		}
	}
	return found;
}

float designWavelength(const SpectralSettings& settings, std::span<const SceneOptic> optics)
{
	const SceneOptic* laser = settings.designFollowsLaser ? singleLaser(optics) : nullptr;
	return laser != nullptr ? (float)(laserBeam(*laser).wavelength * 1e9) : settings.designWavelength;
}
