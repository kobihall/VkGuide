#include <optic_catalog.h>

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

#include <optic_detectors.h>
#include <optic_glass.h>
#include <optic_laser.h>
#include <optic_library.h>

// The kinds of docs/plans/completed/physoptics-optical-elements.md, with the defaults the user settled on 2026-10-29: mirrors are
// N-BK7 with a fine-ground back and a -P01 protected silver front; CM127, CM254 and CM508 are 3.0, 6.0 and 9.0 mm at the
// center; an off-axis parabola's thin edge is 0.75 of its diameter; the equilateral prism is F2, the right-angle prism
// N-BK7 uncoated, the non-polarizing cube N-BK7 and the polarizing cube N-SF1 with the cement folded into the diagonal's
// coating; a round wedge's thin edge is 2, 3 and 6 mm at 1/2, 1 and 2 in, a plate wedge's 3 mm. A laser's one preset is
// Thorlabs' HNL050LB from the data sheet the user gave on 2026-11-02 (docs/plans/completed/physoptics-laser-sources.md).
//
// THE BODY FRAMES. A transmissive element takes light along +z, entering its front face at z = 0. A mirror's
// reflecting face is at z = 0 facing +z, and light arrives along -z. A cube's and a prism's origin is the center of
// its box. Every outer face's normal points out of the body, so its plus side is the surrounding medium and its minus
// side the glass, and a coating's layers are listed from the air side.

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double INCH = 0.0254;
constexpr double MM = 1e-3;

double degrees(double d)
{
	return d * PI / 180.0;
}

//---------------------------------------------------------------- building blocks

// a conic (or plane, c = 0) face with its normal along `normal` from `origin`. A face whose outward normal is -z is
// the same surface in a frame turned half a turn about x, where its curvature changes sign
OpticSurface conicFace(const glm::dvec3& origin, bool facesMinusZ, double curvature, double conic, const ApertureT<double>& aperture)
{
	OpticSurface s;
	s.shape = SurfaceShape::Conic;
	s.placement = frameFromNormal(origin, glm::dvec3(0.0, 0.0, facesMinusZ ? -1.0 : 1.0), glm::dvec3(1.0, 0.0, 0.0));
	s.curvature = facesMinusZ ? -curvature : curvature;
	s.conic = conic;
	s.aperture = aperture;
	if (facesMinusZ) {
		// the turned frame's y is the body's -y: an offset center turns with it
		s.aperture.center.y = -s.aperture.center.y;
	}
	return s;
}

ApertureT<double> circle(double radius, glm::dvec2 center = glm::dvec2(0.0))
{
	ApertureT<double> a;
	a.kind = ApertureKind::Circle;
	a.a = radius;
	a.center = center;
	return a;
}

ApertureT<double> ellipse(double rx, double ry)
{
	ApertureT<double> a;
	a.kind = ApertureKind::Ellipse;
	a.a = rx;
	a.b = ry;
	return a;
}

// a flat face through the given points (a convex polygon of 3 to 6 corners, in the body frame), its normal outward
// along `normal`: the frame sits at the points' centroid with its x along the first edge
OpticSurface polygonFace(const std::vector<glm::dvec3>& points, const glm::dvec3& normal)
{
	glm::dvec3 centroid(0.0);
	for (const glm::dvec3& p : points) {
		centroid += p;
	}
	centroid /= (double)points.size();
	OpticSurface s;
	s.shape = SurfaceShape::Conic;
	s.placement = frameFromNormal(centroid, normal, points[1] - points[0]);
	s.aperture.kind = ApertureKind::Polygon;
	s.aperture.a = (double)points.size();
	const glm::dmat3 toLocal = glm::transpose(s.placement.rotation);
	std::vector<glm::dvec2> local;
	for (const glm::dvec3& p : points) {
		const glm::dvec3 q = toLocal * (p - centroid);
		local.emplace_back(q.x, q.y);
	}
	// counter-clockwise seen from the normal's side
	double area = 0.0;
	for (size_t i = 0; i < local.size(); i++) {
		const glm::dvec2& a = local[i];
		const glm::dvec2& b = local[(i + 1) % local.size()];
		area += a.x * b.y - a.y * b.x;
	}
	if (area < 0.0) {
		std::reverse(local.begin(), local.end());
	}
	for (size_t i = 0; i < local.size() && i < OPTIC_POLYGON_VERTICES; i++) {
		s.aperture.polygon[i] = local[i];
	}
	return s;
}

CylinderBoundT<double> flatBound(double z, glm::dvec2 slope = glm::dvec2(0.0))
{
	CylinderBoundT<double> b;
	b.z0 = z;
	b.slope = slope;
	return b;
}

CylinderBoundT<double> conicBound(double z, double curvature, double conic = 0.0, glm::dvec2 center = glm::dvec2(0.0))
{
	CylinderBoundT<double> b;
	b.z0 = z;
	b.curvature = curvature;
	b.conic = conic;
	b.center = center;
	return b;
}

OpticSurface edgeCylinder(double radius, const CylinderBoundT<double>& lower, const CylinderBoundT<double>& upper)
{
	OpticSurface s;
	s.shape = SurfaceShape::Cylinder;
	s.radius = radius;
	s.lower = lower;
	s.upper = upper;
	s.aperture.kind = ApertureKind::None;
	return s;
}

void add(OpticPrescription& p, OpticSurface s, uint8_t minus, uint8_t plus, uint16_t slot, uint16_t flags)
{
	s.mediumMinus = minus;
	s.mediumPlus = plus;
	s.materialSlot = slot;
	s.flags = flags;
	p.surfaces.push_back(s);
}

// one glass medium (index 1)
void glassMedia(OpticPrescription& p, const std::string& glass)
{
	p.media = { OpticMedium {}, OpticMedium { glass } };
}

std::string lengthText(double meters)
{
	return fmt::format("{:.4g} mm", meters * 1e3);
}

// a wedge's angle from its deviation at normal incidence on the front: delta = asin(n sin a) - a, by Newton's method
double wedgeAngle(double deviation, double n)
{
	double a = deviation / std::max(n - 1.0, 1e-6);
	for (int i = 0; i < 50; i++) {
		const double s = n * std::sin(a);
		if (s >= 1.0) {
			a *= 0.5;
			continue;
		}
		const double f = std::asin(s) - a - deviation;
		const double df = n * std::cos(a) / std::sqrt(1.0 - s * s) - 1.0;
		const double step = f / df;
		a -= step;
		if (std::abs(step) < 1e-15) {
			break;
		}
	}
	return a;
}

//---------------------------------------------------------------- parameters

const char* const OFF_AXIS_LABELS[] = { "15 deg", "30 deg", "45 deg", "60 deg", "90 deg" };
const char* const OFF_AXIS_KEYS[] = { "15", "30", "45", "60", "90" };
constexpr double OFF_AXIS_DEGREES[] = { 15.0, 30.0, 45.0, 60.0, 90.0 };

const char* const DEVIATION_LABELS[] = { "2 deg", "4 deg", "6 deg", "10 deg" };
const char* const DEVIATION_KEYS[] = { "2", "4", "6", "10" };
constexpr double DEVIATION_DEGREES[] = { 2.0, 4.0, 6.0, 10.0 };

const char* const LENS_FORM_LABELS[] = { "Plano-convex", "Bi-convex", "Plano-concave", "Bi-concave" };
const char* const LENS_FORM_KEYS[] = { "plano_convex", "bi_convex", "plano_concave", "bi_concave" };

bool lensHasSecondRadius(const OpticParams& params)
{
	const auto found = params.find("form");
	const int form = found != params.end() && std::holds_alternative<int>(found->second) ? std::get<int>(found->second) : 0;
	return form == 1 || form == 3;
}

const OpticSizeValue MIRROR_HALF[] = { { "diameter", 0.5 * INCH }, { "thickness", 6 * MM } };
const OpticSizeValue MIRROR_ONE[] = { { "diameter", INCH }, { "thickness", 6 * MM } };
const OpticSizeValue MIRROR_TWO[] = { { "diameter", 2 * INCH }, { "thickness", 12 * MM } };
const OpticSize MIRROR_SIZES[] = { { "half_inch", "1/2 in (PF05-03)", MIRROR_HALF }, { "1in", "1 in (PF10-03)", MIRROR_ONE }, { "2in", "2 in (PF20-03)", MIRROR_TWO } };

const OpticSizeValue CONCAVE_HALF[] = { { "diameter", 0.5 * INCH }, { "centerThickness", 3 * MM } };
const OpticSizeValue CONCAVE_ONE[] = { { "diameter", INCH }, { "centerThickness", 6 * MM } };
const OpticSizeValue CONCAVE_TWO[] = { { "diameter", 2 * INCH }, { "centerThickness", 9 * MM } };
const OpticSize CONCAVE_SIZES[] = { { "half_inch", "1/2 in (CM127)", CONCAVE_HALF }, { "1in", "1 in (CM254)", CONCAVE_ONE }, { "2in", "2 in (CM508)", CONCAVE_TWO } };

const OpticSizeValue OAP_HALF[] = { { "diameter", 0.5 * INCH }, { "edgeThickness", 0.75 * 0.5 * INCH }, { "reflectedFocalLength", 2 * INCH } };
const OpticSizeValue OAP_ONE[] = { { "diameter", INCH }, { "edgeThickness", 0.75 * INCH }, { "reflectedFocalLength", 4 * INCH } };
const OpticSizeValue OAP_TWO[] = { { "diameter", 2 * INCH }, { "edgeThickness", 0.75 * 2 * INCH }, { "reflectedFocalLength", 4 * INCH } };
const OpticSize OAP_SIZES[] = { { "half_inch", "1/2 in (MPD0x)", OAP_HALF }, { "1in", "1 in (MPD1x)", OAP_ONE }, { "2in", "2 in (MPD2x)", OAP_TWO } };

const OpticSizeValue ROUND_HALF[] = { { "diameter", 0.5 * INCH } };
const OpticSizeValue ROUND_ONE[] = { { "diameter", INCH } };
const OpticSizeValue ROUND_TWO[] = { { "diameter", 2 * INCH } };
const OpticSize ROUND_SIZES[] = { { "half_inch", "1/2 in", ROUND_HALF }, { "1in", "1 in", ROUND_ONE }, { "2in", "2 in", ROUND_TWO } };

const OpticSizeValue LENS_HALF[] = { { "diameter", 0.5 * INCH }, { "centerThickness", 2.8 * MM } };
const OpticSizeValue LENS_ONE[] = { { "diameter", INCH }, { "centerThickness", 3.6 * MM } };
const OpticSizeValue LENS_TWO[] = { { "diameter", 2 * INCH }, { "centerThickness", 9.7 * MM } };
const OpticSize LENS_SIZES[] = { { "half_inch", "1/2 in", LENS_HALF }, { "1in", "1 in", LENS_ONE }, { "2in", "2 in", LENS_TWO } };

const OpticSizeValue PLATE_BS_HALF[] = { { "diameter", 0.5 * INCH }, { "thickness", 3 * MM } };
const OpticSizeValue PLATE_BS_ONE[] = { { "diameter", INCH }, { "thickness", 5 * MM } };
const OpticSizeValue PLATE_BS_TWO[] = { { "diameter", 2 * INCH }, { "thickness", 8 * MM } };
const OpticSize PLATE_BS_SIZES[] = { { "half_inch", "1/2 in", PLATE_BS_HALF }, { "1in", "1 in", PLATE_BS_ONE }, { "2in", "2 in", PLATE_BS_TWO } };

const OpticSizeValue WEDGE_HALF[] = { { "diameter", 0.5 * INCH }, { "edgeThickness", 2 * MM } };
const OpticSizeValue WEDGE_ONE[] = { { "diameter", INCH }, { "edgeThickness", 3 * MM } };
const OpticSizeValue WEDGE_TWO[] = { { "diameter", 2 * INCH }, { "edgeThickness", 6 * MM } };
const OpticSize WEDGE_SIZES[] = { { "half_inch", "1/2 in", WEDGE_HALF }, { "1in", "1 in (PS81x)", WEDGE_ONE }, { "2in", "2 in", WEDGE_TWO } };

const OpticSizeValue CUBE_HALF[] = { { "side", 12.7 * MM } };
const OpticSizeValue CUBE_ONE[] = { { "side", 25.4 * MM } };
const OpticSizeValue CUBE_TWO[] = { { "side", 50.8 * MM } };
const OpticSize CUBE_SIZES[] = { { "12.7mm", "12.7 mm", CUBE_HALF }, { "25.4mm", "25.4 mm", CUBE_ONE }, { "50.8mm", "50.8 mm", CUBE_TWO } };

const OpticSizeValue RA_10[] = { { "leg", 10 * MM } };
const OpticSizeValue RA_25[] = { { "leg", 25 * MM } };
const OpticSizeValue RA_60[] = { { "leg", 60 * MM } };
const OpticSize RA_SIZES[] = { { "10mm", "10 mm (PS910)", RA_10 }, { "25mm", "25 mm (PS911)", RA_25 }, { "60mm", "60 mm (PS913)", RA_60 } };

const OpticSizeValue EQ_10[] = { { "side", 10 * MM } };
const OpticSizeValue EQ_25[] = { { "side", 25 * MM } };
const OpticSizeValue EQ_50[] = { { "side", 50 * MM } };
const OpticSize EQ_SIZES[] = { { "10mm", "10 mm (PS850)", EQ_10 }, { "25mm", "25 mm (PS852)", EQ_25 }, { "50mm", "50 mm", EQ_50 } };

const OpticSizeValue SQUARE_50[] = { { "side", 50 * MM } };
const OpticSize SQUARE_SIZES[] = { { "50mm", "50 mm", SQUARE_50 } };
const OpticSize PLATE_WEDGE_SIZES[] = { { "50mm", "50 mm", SQUARE_50 } };

// Thorlabs' ID-series irises: the largest opening, and the range it closes over
const OpticSizeValue ID8[] = { { "maxOpening", 8 * MM }, { "minOpening", 0.8 * MM } };
const OpticSizeValue ID12[] = { { "maxOpening", 12 * MM }, { "minOpening", 1 * MM } };
const OpticSizeValue ID15[] = { { "maxOpening", 15 * MM }, { "minOpening", 1.2 * MM } };
const OpticSizeValue ID20[] = { { "maxOpening", 20 * MM }, { "minOpening", 1.2 * MM } };
const OpticSizeValue ID25[] = { { "maxOpening", 25 * MM }, { "minOpening", 1.4 * MM } };
const OpticSizeValue ID36[] = { { "maxOpening", 36 * MM }, { "minOpening", 1.9 * MM } };
const OpticSizeValue ID50[] = { { "maxOpening", 50 * MM }, { "minOpening", 2.5 * MM } };
const OpticSize IRIS_SIZES[] = { { "ID8", "ID8", ID8 }, { "ID12", "ID12", ID12 }, { "ID15", "ID15", ID15 }, { "ID20", "ID20", ID20 }, { "ID25", "ID25", ID25 },
	{ "ID36", "ID36", ID36 }, { "ID50", "ID50", ID50 } };

#define LENGTH(name, label, value, lo, hi, tip) OpticParam { name, label, OpticParamKind::Length, OpticValue((double)(value)), lo, hi, {}, {}, tip, nullptr }

const OpticParam PLANAR_PARAMS[] = {
	LENGTH("diameter", "diameter", INCH, 1 * MM, 0.3, nullptr),
	LENGTH("thickness", "thickness", 6 * MM, 0.1 * MM, 0.1, nullptr),
	{ "glass", "substrate", OpticParamKind::Glass, std::string("N-BK7"), 0, 0, {}, {}, "What a beam the front lets through travels in", nullptr },
};
const OpticParam CONCAVE_PARAMS[] = {
	LENGTH("diameter", "diameter", INCH, 1 * MM, 0.3, nullptr),
	LENGTH("radiusOfCurvature", "radius of curvature", 0.2, 5 * MM, 10.0, "Positive: concave toward the incoming light. The focal length is half of it"),
	LENGTH("centerThickness", "center thickness", 6 * MM, 0.1 * MM, 0.1, nullptr),
	{ "glass", "substrate", OpticParamKind::Glass, std::string("N-BK7"), 0, 0, {}, {}, nullptr, nullptr },
};
const OpticParam OAP_PARAMS[] = {
	LENGTH("diameter", "diameter", INCH, 1 * MM, 0.3, nullptr),
	{ "offAxisAngle", "off-axis angle", OpticParamKind::Enum, 4, 0, 4, OFF_AXIS_LABELS, OFF_AXIS_KEYS, "Thorlabs' set: the angle between the incoming axis and the focused beam", nullptr },
	LENGTH("reflectedFocalLength", "reflected focal length", 4 * INCH, 5 * MM, 2.0, "From the aperture's center to the focus"),
	LENGTH("edgeThickness", "thin edge", 0.75 * INCH, 0.5 * MM, 0.2, "The substrate at its thinnest point, the rim nearest the parent axis"),
	{ "glass", "substrate", OpticParamKind::Glass, std::string("N-BK7"), 0, 0, {}, {}, nullptr, nullptr },
};
const OpticParam CIRCULAR_FILTER_PARAMS[] = {
	LENGTH("diameter", "diameter", INCH, 1 * MM, 0.3, nullptr),
	LENGTH("thickness", "thickness", 3 * MM, 0.05 * MM, 0.05, nullptr),
	{ "glass", "glass", OpticParamKind::Glass, std::string("N-BK7"), 0, 0, {}, {}, "A neutral density or colored filter is a glass that absorbs: its catalog's internal transmittance", nullptr },
};
const OpticParam SQUARE_FILTER_PARAMS[] = {
	LENGTH("side", "side", 50 * MM, 1 * MM, 0.5, "No upper size: a square filter sits in an FP01 plate holder"),
	LENGTH("thickness", "thickness", 3 * MM, 0.05 * MM, 14.7 * MM, "At most 14.7 mm, what an FP01 holds"),
	{ "glass", "glass", OpticParamKind::Glass, std::string("N-BK7"), 0, 0, {}, {}, nullptr, nullptr },
};
const OpticParam IRIS_PARAMS[] = {
	LENGTH("opening", "opening", 5 * MM, 0.1 * MM, 0.1, "The diameter the blades leave open, within the iris's range"),
	LENGTH("maxOpening", "largest opening", 25 * MM, 1 * MM, 0.1, nullptr),
	LENGTH("minOpening", "smallest opening", 1.4 * MM, 0.1 * MM, 0.1, nullptr),
};
const OpticParam PLATE_BS_PARAMS[] = {
	LENGTH("diameter", "diameter", INCH, 1 * MM, 0.3, nullptr),
	LENGTH("thickness", "thickness", 5 * MM, 0.2 * MM, 0.05, "At the center"),
	{ "wedge", "wedge", OpticParamKind::Angle, degrees(0.5), 0.0, degrees(10.0), {}, {}, "Between the front and the tilted back: Thorlabs' 30 arcmin keeps the back's ghost out of the beam", nullptr },
	{ "polarizing", "polarizing", OpticParamKind::Bool, false, 0, 0, {}, {}, "A polarizing splitter's front coating; the renderer uses the average of s and p until polarization exists", nullptr },
	{ "glass", "glass", OpticParamKind::Glass, std::string("N-BK7"), 0, 0, {}, {}, nullptr, nullptr },
};
const OpticParam CUBE_PARAMS[] = {
	LENGTH("side", "side", 25.4 * MM, 1 * MM, 0.2, nullptr),
	{ "polarizing", "polarizing", OpticParamKind::Bool, false, 0, 0, {}, {}, "A polarizing cube (N-SF1, Thorlabs PBS) or a non-polarizing one (N-BK7, Thorlabs BS)", nullptr },
	{ "glass", "glass", OpticParamKind::Glass, std::string(""), 0, 0, {}, {}, "Empty: N-SF1 for a polarizing cube, N-BK7 otherwise", nullptr },
};
const OpticParam LENS_PARAMS[] = {
	LENGTH("diameter", "diameter", INCH, 1 * MM, 0.3, nullptr),
	{ "form", "form", OpticParamKind::Enum, 0, 0, 3, LENS_FORM_LABELS, LENS_FORM_KEYS, "The form fixes the radii's signs and which face is flat", nullptr },
	LENGTH("r1", "R1 (front)", 51.5 * MM, 1 * MM, 100.0, "The front face's radius, as a magnitude"),
	{ "r2", "R2 (back)", OpticParamKind::Length, 51.5 * MM, 1 * MM, 100.0, {}, {}, "The back face's radius, as a magnitude", lensHasSecondRadius },
	LENGTH("centerThickness", "center thickness", 3.6 * MM, 0.1 * MM, 0.1, nullptr),
	{ "glass", "glass", OpticParamKind::Glass, std::string("N-BK7"), 0, 0, {}, {}, nullptr, nullptr },
};
const OpticParam RA_PARAMS[] = {
	LENGTH("leg", "leg", 25 * MM, 1 * MM, 0.3, nullptr),
	LENGTH("height", "height", 0.0, 0.0, 0.3, "0 for as tall as the leg"),
	{ "glass", "glass", OpticParamKind::Glass, std::string("N-BK7"), 0, 0, {}, {}, nullptr, nullptr },
};
const OpticParam EQ_PARAMS[] = {
	LENGTH("side", "side", 25 * MM, 1 * MM, 0.3, nullptr),
	LENGTH("height", "height", 0.0, 0.0, 0.3, "0 for as tall as the side"),
	{ "glass", "glass", OpticParamKind::Glass, std::string("F2"), 0, 0, {}, {}, nullptr, nullptr },
};
const OpticParam PLATE_WEDGE_PARAMS[] = {
	LENGTH("side", "side", 50 * MM, 1 * MM, 0.5, nullptr),
	{ "deviation", "beam deviation", OpticParamKind::Enum, 0, 0, 3, DEVIATION_LABELS, DEVIATION_KEYS, "At normal incidence on the front, at the design wavelength", nullptr },
	LENGTH("edgeThickness", "thin edge", 3 * MM, 0.1 * MM, 0.05, nullptr),
	{ "glass", "glass", OpticParamKind::Glass, std::string("N-BK7"), 0, 0, {}, {}, nullptr, nullptr },
};
const OpticParam ROUND_WEDGE_PARAMS[] = {
	LENGTH("diameter", "diameter", INCH, 1 * MM, 0.3, nullptr),
	{ "deviation", "beam deviation", OpticParamKind::Enum, 0, 0, 3, DEVIATION_LABELS, DEVIATION_KEYS, "At normal incidence on the front, at the design wavelength", nullptr },
	LENGTH("edgeThickness", "thin edge", 3 * MM, 0.1 * MM, 0.05, nullptr),
	{ "glass", "glass", OpticParamKind::Glass, std::string("N-BK7"), 0, 0, {}, {}, nullptr, nullptr },
};

const char* const HOUSING_LABELS[] = { "Laser head (HNL050LB)", "Diode module (CPS635R)", "Fiber collimator (F220APC-633)" };
const char* const HOUSING_KEYS[] = { "laser_head", "diode_module", "fiber_collimator" };
const char* const LASER_SPECTRUM_LABELS[] = { "Lorentzian line", "Gaussian line or band" };
const char* const LASER_SPECTRUM_KEYS[] = { "lorentzian", "gaussian" };

bool laserAstigmatic(const OpticParams& params)
{
	const auto found = params.find("astigmatic");
	const bool* b = found != params.end() ? std::get_if<bool>(&found->second) : nullptr;
	return b != nullptr && *b;
}

#define WAVELENGTH(name, label, value, lo, hi, tip) OpticParam { name, label, OpticParamKind::Wavelength, OpticValue((double)(value)), lo, hi, {}, {}, tip, nullptr }
#define NUMBER(name, label, value, lo, hi, tip, visible) OpticParam { name, label, OpticParamKind::Number, OpticValue((double)(value)), lo, hi, {}, {}, tip, visible }

// HNL050LB: 0.81 mm 1/e^2 diameter and 1.0 mrad full divergence, its waist at the output, which is M^2 = 1.005
constexpr double HNL_WAIST = 0.405 * MM;
constexpr double HNL_HALF_ANGLE = 0.5e-3;
constexpr double HNL_WAVELENGTH = 632.8e-9;
const OpticSizeValue HNL050LB[] = { { "housing", 0.0 }, { "wavelength", HNL_WAVELENGTH }, { "spectrum", 1.0 }, { "linewidth", 0.002e-9 }, { "power", 5e-3 },
	{ "waistRadius", HNL_WAIST }, { "waistPosition", 0.0 }, { "m2", PI * HNL_WAIST * HNL_HALF_ANGLE / HNL_WAVELENGTH } };
const OpticSize LASER_PRESETS[] = { { "HNL050LB", "HNL050LB HeNe, 632.8 nm, 5 mW", HNL050LB } };

const OpticParam LASER_PARAMS[] = {
	{ "housing", "housing", OpticParamKind::Enum, 0, 0, 2, HOUSING_LABELS, HOUSING_KEYS,
		"What the laser stands for. Only its output aperture is geometry; the Viewport outlines the housing", nullptr },
	WAVELENGTH("wavelength", "wavelength", HNL_WAVELENGTH, 1e-9, 20e-6, "The line's center. Exact RGB reads the glasses here when the scene has one laser"),
	{ "spectrum", "spectrum", OpticParamKind::Enum, 1, 0, 1, LASER_SPECTRUM_LABELS, LASER_SPECTRUM_KEYS,
		"Lorentzian: a single-frequency laser's line. Gaussian: a Doppler-broadened gas laser's line, or a pulse's bandwidth", nullptr },
	WAVELENGTH("linewidth", "linewidth", 0.002e-9, 1e-15, 1e-6, "Full width at half maximum: the spectral modes draw a laser's wavelengths from it"),
	NUMBER("power", "power (W)", 5e-3, 0.0, 1e3, "Average power", nullptr),
	LENGTH("waistRadius", "waist radius", HNL_WAIST, 1e-7, 0.05, "The 1/e^2 intensity radius at the waist (along the beam's x axis when x and y differ)"),
	LENGTH("waistPosition", "waist position", 0.0, -2.0, 2.0, "From the output aperture along the beam: negative inside the housing"),
	NUMBER("m2", "M^2", PI * HNL_WAIST * HNL_HALF_ANGLE / HNL_WAVELENGTH, 1.0, 100.0, "The beam quality factor: the divergence over a perfect Gaussian's of the same waist", nullptr),
	{ "astigmatic", "separate x and y", OpticParamKind::Bool, false, 0, 0, {}, {}, "An elliptical or astigmatic beam, such as a diode's: y gets a waist, waist position and M^2 of its own", nullptr },
	{ "waistRadiusY", "waist radius y", OpticParamKind::Length, HNL_WAIST, 1e-7, 0.05, {}, {}, nullptr, laserAstigmatic },
	{ "waistPositionY", "waist position y", OpticParamKind::Length, 0.0, -2.0, 2.0, {}, {}, nullptr, laserAstigmatic },
	NUMBER("m2Y", "M^2 y", PI * HNL_WAIST * HNL_HALF_ANGLE / HNL_WAVELENGTH, 1.0, 100.0, nullptr, laserAstigmatic),
	{ "ellipseAngle", "x axis angle", OpticParamKind::Angle, 0.0, -PI, PI, {}, {}, "The beam's x axis about the output axis, from the housing's x", laserAstigmatic },
	{ "pointingX", "pointing x", OpticParamKind::Angle, 0.0, -0.1, 0.1, {}, {}, "The beam's angle toward the housing's +x: a laser that does not leave its housing straight", nullptr },
	{ "pointingY", "pointing y", OpticParamKind::Angle, 0.0, -0.1, 0.1, {}, {}, "The beam's angle toward the housing's +y", nullptr },
	LENGTH("offsetX", "offset x", 0.0, -0.01, 0.01, "The beam's center across the output aperture, along the housing's x"),
	LENGTH("offsetY", "offset y", 0.0, -0.01, 0.01, "The beam's center along the housing's y"),
};

// THE DETECTORS (docs/plans/completed/physoptics-detectors.md): a sensor's size and the film pixels the renderer records,
// and for a camera sensor or a beam profiler, optionally, the physical sensor it stands for (its own pixels and their
// pitch, which then set the size, the film downsampling them). No kind has presets: the user chose plain defaults on
// 2026-11-04, with named sensor models to come
bool physicalSensor(const OpticParams& params)
{
	const auto found = params.find("physicalSensor");
	const bool* b = found != params.end() ? std::get_if<bool>(&found->second) : nullptr;
	return b != nullptr && *b;
}
bool sizedByHand(const OpticParams& params)
{
	return !physicalSensor(params);
}

#define SHEET(name, w, h, px, py) const OpticParam name[] = { \
	OpticParam { "width", "width", OpticParamKind::Length, OpticValue((double)(w)), 0.1 * MM, 2.0, {}, {}, nullptr, nullptr }, \
	OpticParam { "height", "height", OpticParamKind::Length, OpticValue((double)(h)), 0.1 * MM, 2.0, {}, {}, nullptr, nullptr }, \
	NUMBER("pixelsX", "pixels x", px, 1, 4096, "The film the renderer records: its pixels across", nullptr), \
	NUMBER("pixelsY", "pixels y", py, 1, 4096, nullptr, nullptr), \
}
SHEET(CARD_PARAMS, 85.6 * MM, 54 * MM, 428, 270);
SHEET(SCREEN_PARAMS, 100 * MM, 100 * MM, 400, 400);
SHEET(PROBE_PARAMS, 10 * MM, 10 * MM, 256, 256);
#undef SHEET

const OpticParam SENSOR_PARAMS[] = {
	{ "width", "width", OpticParamKind::Length, 10 * MM, 0.1 * MM, 0.2, {}, {}, nullptr, sizedByHand },
	{ "height", "height", OpticParamKind::Length, 10 * MM, 0.1 * MM, 0.2, {}, {}, nullptr, sizedByHand },
	NUMBER("pixelsX", "pixels x", 512, 1, 16384, "The film the renderer records: its pixels across", nullptr),
	NUMBER("pixelsY", "pixels y", 512, 1, 16384, nullptr, nullptr),
	{ "physicalSensor", "physical sensor", OpticParamKind::Bool, false, 0, 0, {}, {},
		"A real sensor's own pixels and pitch, which set the size: the film then records at most as many pixels, downsampling them", nullptr },
	{ "nativeX", "sensor pixels x", OpticParamKind::Number, 2048.0, 1, 16384, {}, {}, nullptr, physicalSensor },
	{ "nativeY", "sensor pixels y", OpticParamKind::Number, 2048.0, 1, 16384, {}, {}, nullptr, physicalSensor },
	{ "pitch", "pixel pitch", OpticParamKind::Length, 5e-6, 0.1e-6, 1 * MM, {}, {}, nullptr, physicalSensor },
};
const OpticParam POWER_METER_PARAMS[] = {
	LENGTH("diameter", "sensor aperture", 10 * MM, 0.1 * MM, 0.1, "The diameter of the sensor the meter reads: its power, with no image"),
};
const OpticParam SPECTROMETER_PARAMS[] = {
	LENGTH("diameter", "entrance", 3 * MM, 0.01 * MM, 0.05, "The entrance a fiber or a slit would collect from: its spectrum, with no image"),
	WAVELENGTH("lambdaMin", "from", 350e-9, 1e-9, 20e-6, nullptr),
	WAVELENGTH("lambdaMax", "to", 700e-9, 1e-9, 20e-6, nullptr),
	NUMBER("bins", "bins", 3500, 1, 16384, "The spectrum's pixels over the range", nullptr),
};
const OpticParam DUMP_PARAMS[] = {
	LENGTH("diameter", "entrance", 10 * MM, 0.5 * MM, 0.1, "An ideal dump: whatever enters is absorbed and its power counted"),
};

#undef NUMBER
#undef WAVELENGTH
#undef LENGTH

//---------------------------------------------------------------- slots, mounts, ports

const MaterialSlot MIRROR_SLOTS[] = { { "front", SlotDefault::Polished, "thorlabs_P01" }, { "back", SlotDefault::Ground, "uncoated" }, { "edge", SlotDefault::Ground, "uncoated" } };
const MaterialSlot TRANSMISSIVE_SLOTS[] = { { "front", SlotDefault::Polished, "uncoated" }, { "back", SlotDefault::Polished, "uncoated" }, { "edge", SlotDefault::Ground, "uncoated" } };
const MaterialSlot IRIS_SLOTS[] = { { "blades", SlotDefault::Black, "uncoated" } };
const MaterialSlot LASER_SLOTS[] = { { "aperture", SlotDefault::LaserAperture, "uncoated" } };
const MaterialSlot SENSOR_SLOTS[] = { { "sensor", SlotDefault::Sensor, "uncoated" } };
const MaterialSlot FACE_SLOTS[] = { { "face", SlotDefault::WhiteDiffuse, "uncoated" } };
const MaterialSlot PLATE_BS_SLOTS[] = { { "front", SlotDefault::Polished, "thorlabs_BSW" }, { "back", SlotDefault::Polished, "thorlabs_A" }, { "edge", SlotDefault::Ground, "uncoated" } };
const MaterialSlot CUBE_SLOTS[] = { { "faces", SlotDefault::Polished, "thorlabs_A" }, { "diagonal", SlotDefault::Polished, "ideal_50_50" } };
const MaterialSlot RA_SLOTS[] = { { "legs", SlotDefault::Polished, "uncoated" }, { "hypotenuse", SlotDefault::Polished, "uncoated" }, { "caps", SlotDefault::Ground, "uncoated" } };
const MaterialSlot PRISM_SLOTS[] = { { "faces", SlotDefault::Polished, "uncoated" }, { "caps", SlotDefault::Ground, "uncoated" } };

const MountKind KINEMATIC_MOUNTS[] = { MountKind::Kinematic, MountKind::Fixed };
const MountKind FIXED_MOUNTS[] = { MountKind::Fixed, MountKind::Kinematic };
const MountKind PLATE_MOUNTS[] = { MountKind::PlateHolder };
const MountKind ROTATION_MOUNTS[] = { MountKind::Rotation, MountKind::Kinematic };
const MountKind PRISM_MOUNTS[] = { MountKind::PrismMount };
const MountKind IRIS_MOUNTS[] = { MountKind::IrisAssembly };

const PortEvent REFLECT_FRONT[] = { { 0, true } };
const OpticPort MIRROR_PORTS[] = { { "Reflected", REFLECT_FRONT } };
const PortEvent THROUGH_TWO[] = { { 0, false }, { 1, false } };
const OpticPort TRANSMITTED_PORTS[] = { { "Transmitted", THROUGH_TWO } };
const OpticPort SPLITTER_PORTS[] = { { "Reflected", REFLECT_FRONT }, { "Transmitted", THROUGH_TWO } };
const OpticPort IRIS_PORTS[] = { { "Transmitted", {} } };
const OpticPort LASER_PORTS[] = { { "Output", {} } };
// the cube's surfaces: 0 z-, 1 y-, 2 z+, 3 y+, 4 to 7 the two triangles of each x face, 8 the diagonal
const PortEvent CUBE_REFLECTED[] = { { 0, false }, { 8, true }, { 1, false } };
const PortEvent CUBE_TRANSMITTED[] = { { 0, false }, { 8, false }, { 2, false } };
const OpticPort CUBE_PORTS[] = { { "Reflected", CUBE_REFLECTED }, { "Transmitted", CUBE_TRANSMITTED } };
// the right-angle prism's surfaces: 0 the leg at z-, 1 the leg at y-, 2 the hypotenuse, 3 and 4 the caps
const PortEvent RA_TURNED[] = { { 0, false }, { 2, true }, { 1, false } };
const PortEvent RA_RETRO[] = { { 2, false }, { 1, true }, { 0, true }, { 2, false } };
const OpticPort RA_PORTS[] = { { "Turned", RA_TURNED }, { "Retro", RA_RETRO } };
const OpticPort PRISM_PORTS[] = { { "Refracted", THROUGH_TWO } };

//---------------------------------------------------------------- the builders

void buildPlanarMirror(const SceneOptic& o, const OpticBuildContext&, OpticPrescription& p, OpticReadout& r)
{
	const double radius = 0.5 * opticNumber(o, "diameter");
	const double thickness = opticNumber(o, "thickness");
	glassMedia(p, opticString(o, "glass"));
	add(p, conicFace(glm::dvec3(0.0), false, 0.0, 0.0, circle(radius)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, conicFace(glm::dvec3(0.0, 0.0, -thickness), true, 0.0, 0.0, circle(radius)), 1, 0, 1, 0);
	add(p, edgeCylinder(radius, flatBound(-thickness), flatBound(0.0)), 1, 0, 2, OPTIC_SURFACE_EDGE);
	r.lengths.push_back({ "thickness", thickness });
}

void buildConcaveMirror(const SceneOptic& o, const OpticBuildContext&, OpticPrescription& p, OpticReadout& r)
{
	const double radius = 0.5 * opticNumber(o, "diameter");
	const double roc = opticNumber(o, "radiusOfCurvature");
	const double center = opticNumber(o, "centerThickness");
	if (roc <= radius) {
		r.error = "The radius of curvature must exceed the mirror's radius";
		return;
	}
	const double c = 1.0 / roc;
	const double sag = *conicSag(c, 0.0, radius * radius);
	glassMedia(p, opticString(o, "glass"));
	add(p, conicFace(glm::dvec3(0.0), false, c, 0.0, circle(radius)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, conicFace(glm::dvec3(0.0, 0.0, -center), true, 0.0, 0.0, circle(radius)), 1, 0, 1, 0);
	add(p, edgeCylinder(radius, flatBound(-center), conicBound(0.0, c)), 1, 0, 2, OPTIC_SURFACE_EDGE);
	r.lengths.push_back({ "focal length", 0.5 * roc });
	r.lengths.push_back({ "edge thickness", center + sag });
}

void buildOffAxisParabola(const SceneOptic& o, const OpticBuildContext&, OpticPrescription& p, OpticReadout& r)
{
	const double diameter = opticNumber(o, "diameter");
	const double radius = 0.5 * diameter;
	const double theta = degrees(OFF_AXIS_DEGREES[std::clamp(opticEnum(o, "offAxisAngle"), 0, 4)]);
	const double rfl = opticNumber(o, "reflectedFocalLength");
	const double thin = opticNumber(o, "edgeThickness");
	// the parent paraboloid: z = rho^2 / (4 PFL), focus at (0, 0, PFL), the aperture centered at the off-axis distance
	const double pfl = 0.5 * rfl * (1.0 + std::cos(theta));
	const double oad = rfl * std::sin(theta);
	const double c = 1.0 / (2.0 * pfl);
	const double sagDatum = oad * oad / (4.0 * pfl);
	// the substrate is a cylinder about the datum, parallel to the parent axis, with a back face perpendicular to it
	const double nearest = std::max(0.0, oad - radius);
	const double sagLow = nearest * nearest / (4.0 * pfl) - sagDatum;
	const double back = sagLow - thin;
	glassMedia(p, opticString(o, "glass"));
	OpticSurface front;
	front.shape = SurfaceShape::Conic;
	front.placement = translationFrame(glm::dvec3(-oad, 0.0, -sagDatum));
	front.curvature = c;
	front.conic = -1.0;
	front.aperture = circle(radius, glm::dvec2(oad, 0.0));
	add(p, front, 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, conicFace(glm::dvec3(0.0, 0.0, back), true, 0.0, 0.0, circle(radius)), 1, 0, 1, 0);
	add(p, edgeCylinder(radius, flatBound(back), conicBound(-sagDatum, c, -1.0, glm::dvec2(-oad, 0.0))), 1, 0, 2, OPTIC_SURFACE_EDGE);
	r.lengths.push_back({ "parent focal length", pfl });
	r.lengths.push_back({ "off-axis distance", oad });
	r.lengths.push_back({ "center thickness", -back });
	r.lines.push_back({ "focus", fmt::format("RFL from the datum, {:.0f} deg from +z toward -x", OFF_AXIS_DEGREES[std::clamp(opticEnum(o, "offAxisAngle"), 0, 4)]) });
}

void buildCircularFilter(const SceneOptic& o, const OpticBuildContext&, OpticPrescription& p, OpticReadout&)
{
	const double radius = 0.5 * opticNumber(o, "diameter");
	const double thickness = opticNumber(o, "thickness");
	glassMedia(p, opticString(o, "glass"));
	add(p, conicFace(glm::dvec3(0.0), true, 0.0, 0.0, circle(radius)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, conicFace(glm::dvec3(0.0, 0.0, thickness), false, 0.0, 0.0, circle(radius)), 1, 0, 1, OPTIC_SURFACE_OPTICAL);
	add(p, edgeCylinder(radius, flatBound(0.0), flatBound(thickness)), 1, 0, 2, OPTIC_SURFACE_EDGE);
}

// a box of the given half extents in x and y and z from z0 to z1, its six faces as polygons: front (z0), back (z1),
// then -x, +x, -y, +y
void boxFaces(OpticPrescription& p, double hx, double hy, double z0, double z1, uint16_t frontSlot, uint16_t backSlot, uint16_t edgeSlot)
{
	using V = glm::dvec3;
	add(p, polygonFace({ V(-hx, -hy, z0), V(hx, -hy, z0), V(hx, hy, z0), V(-hx, hy, z0) }, V(0, 0, -1)), 1, 0, frontSlot, OPTIC_SURFACE_OPTICAL);
	add(p, polygonFace({ V(-hx, -hy, z1), V(hx, -hy, z1), V(hx, hy, z1), V(-hx, hy, z1) }, V(0, 0, 1)), 1, 0, backSlot, OPTIC_SURFACE_OPTICAL);
	add(p, polygonFace({ V(-hx, -hy, z0), V(-hx, hy, z0), V(-hx, hy, z1), V(-hx, -hy, z1) }, V(-1, 0, 0)), 1, 0, edgeSlot, OPTIC_SURFACE_EDGE);
	add(p, polygonFace({ V(hx, -hy, z0), V(hx, hy, z0), V(hx, hy, z1), V(hx, -hy, z1) }, V(1, 0, 0)), 1, 0, edgeSlot, OPTIC_SURFACE_EDGE);
	add(p, polygonFace({ V(-hx, -hy, z0), V(hx, -hy, z0), V(hx, -hy, z1), V(-hx, -hy, z1) }, V(0, -1, 0)), 1, 0, edgeSlot, OPTIC_SURFACE_EDGE);
	add(p, polygonFace({ V(-hx, hy, z0), V(hx, hy, z0), V(hx, hy, z1), V(-hx, hy, z1) }, V(0, 1, 0)), 1, 0, edgeSlot, OPTIC_SURFACE_EDGE);
}

void buildSquareFilter(const SceneOptic& o, const OpticBuildContext&, OpticPrescription& p, OpticReadout& r)
{
	const double half = 0.5 * opticNumber(o, "side");
	const double thickness = opticNumber(o, "thickness");
	if (thickness > 14.7 * MM + 1e-12) {
		r.error = "An FP01 plate holder takes filters up to 14.7 mm thick";
		return;
	}
	glassMedia(p, opticString(o, "glass"));
	boxFaces(p, half, half, 0.0, thickness, 0, 1, 2);
}

void buildIris(const SceneOptic& o, const OpticBuildContext&, OpticPrescription& p, OpticReadout& r)
{
	const double largest = opticNumber(o, "maxOpening");
	const double opening = std::clamp(opticNumber(o, "opening"), opticNumber(o, "minOpening"), largest);
	// the blades: an annulus from the opening out past the largest one, where the housing (the iris's own model) takes over
	ApertureT<double> annulus;
	annulus.kind = ApertureKind::Annulus;
	annulus.a = 0.5 * opening;
	annulus.b = 0.5 * largest + 3 * MM;
	p.media = { OpticMedium {} };
	add(p, conicFace(glm::dvec3(0.0), true, 0.0, 0.0, annulus), 0, 0, 0, 0);
	r.lengths.push_back({ "opening", opening });
}

void buildPlateBeamsplitter(const SceneOptic& o, const OpticBuildContext&, OpticPrescription& p, OpticReadout& r)
{
	const double radius = 0.5 * opticNumber(o, "diameter");
	const double thickness = opticNumber(o, "thickness");
	const double wedge = opticNumber(o, "wedge");
	const double slope = std::tan(wedge);
	if (thickness - radius * slope <= 0.0) {
		r.error = "The wedge makes the plate's thin edge thinner than nothing";
		return;
	}
	glassMedia(p, opticString(o, "glass"));
	add(p, conicFace(glm::dvec3(0.0), true, 0.0, 0.0, circle(radius)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	OpticSurface back;
	back.shape = SurfaceShape::Conic;
	back.placement = frameFromNormal(glm::dvec3(0.0, 0.0, thickness), glm::dvec3(0.0, -std::sin(wedge), std::cos(wedge)), glm::dvec3(1.0, 0.0, 0.0));
	back.aperture = ellipse(radius, radius / std::cos(wedge));
	add(p, back, 1, 0, 1, OPTIC_SURFACE_OPTICAL);
	add(p, edgeCylinder(radius, flatBound(0.0), flatBound(thickness, glm::dvec2(0.0, slope))), 1, 0, 2, OPTIC_SURFACE_EDGE);
	r.lengths.push_back({ "thin edge", thickness - radius * slope });
	r.lengths.push_back({ "thick edge", thickness + radius * slope });
}

std::string cubeGlass(const SceneOptic& o)
{
	const std::string glass = opticString(o, "glass");
	if (!glass.empty()) {
		return glass;
	}
	return opticBool(o, "polarizing") ? "N-SF1" : "N-BK7";
}

void buildCubeBeamsplitter(const SceneOptic& o, const OpticBuildContext&, OpticPrescription& p, OpticReadout& r)
{
	using V = glm::dvec3;
	const double h = 0.5 * opticNumber(o, "side");
	const std::string glass = cubeGlass(o);
	// prism A on the side y + z < 0 of the diagonal, prism B on the other: two media of one glass, so the diagonal is an
	// interface between them, which only its coating makes reflect
	p.media = { OpticMedium {}, OpticMedium { glass }, OpticMedium { glass } };
	add(p, polygonFace({ V(-h, -h, -h), V(h, -h, -h), V(h, h, -h), V(-h, h, -h) }, V(0, 0, -1)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, polygonFace({ V(-h, -h, -h), V(h, -h, -h), V(h, -h, h), V(-h, -h, h) }, V(0, -1, 0)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, polygonFace({ V(-h, -h, h), V(h, -h, h), V(h, h, h), V(-h, h, h) }, V(0, 0, 1)), 2, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, polygonFace({ V(-h, h, -h), V(h, h, -h), V(h, h, h), V(-h, h, h) }, V(0, 1, 0)), 2, 0, 0, OPTIC_SURFACE_OPTICAL);
	for (const double x : { -h, h }) {
		add(p, polygonFace({ V(x, -h, -h), V(x, h, -h), V(x, -h, h) }, V(x < 0 ? -1 : 1, 0, 0)), 1, 0, 0, 0);
		add(p, polygonFace({ V(x, h, -h), V(x, h, h), V(x, -h, h) }, V(x < 0 ? -1 : 1, 0, 0)), 2, 0, 0, 0);
	}
	add(p, polygonFace({ V(-h, h, -h), V(h, h, -h), V(h, -h, h), V(-h, -h, h) }, V(0, 1, 1)), 1, 2, 1, OPTIC_SURFACE_OPTICAL | OPTIC_SURFACE_INTERNAL);
	r.lines.push_back({ "glass", glass });
}

void buildSphericalLens(const SceneOptic& o, const OpticBuildContext& context, OpticPrescription& p, OpticReadout& r)
{
	const double radius = 0.5 * opticNumber(o, "diameter");
	const int form = std::clamp(opticEnum(o, "form"), 0, 3);
	const double r1 = opticNumber(o, "r1");
	const double r2 = opticNumber(o, "r2");
	const double center = opticNumber(o, "centerThickness");
	// the signs and the flat face the form fixes, whatever was typed
	double c1 = 0.0;
	double c2 = 0.0;
	switch (form) {
	case 0:
		c1 = 1.0 / r1;
		break;
	case 1:
		c1 = 1.0 / r1;
		c2 = -1.0 / r2;
		break;
	case 2:
		c1 = -1.0 / r1;
		break;
	default:
		c1 = -1.0 / r1;
		c2 = 1.0 / r2;
		break;
	}
	const std::optional<double> sag1 = conicSag(c1, 0.0, radius * radius);
	const std::optional<double> sag2 = conicSag(c2, 0.0, radius * radius);
	if (!sag1 || !sag2) {
		r.error = "A radius is smaller than the lens's semi-diameter";
		return;
	}
	const double edge = center + *sag2 - *sag1;
	if (edge <= 0.0) {
		r.error = fmt::format("The edge would be {:.3f} mm thick: increase the center thickness or the radii", edge * 1e3);
		return;
	}
	const std::string glass = opticString(o, "glass");
	glassMedia(p, glass);
	add(p, conicFace(glm::dvec3(0.0), true, c1, 0.0, circle(radius)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, conicFace(glm::dvec3(0.0, 0.0, center), false, c2, 0.0, circle(radius)), 1, 0, 1, OPTIC_SURFACE_OPTICAL);
	add(p, edgeCylinder(radius, conicBound(0.0, c1), conicBound(center, c2)), 1, 0, 2, OPTIC_SURFACE_EDGE);
	// the thick lens, paraxially, at the design wavelength: P = (n - 1)(c1 - c2) + (n - 1)^2 t c1 c2 / n
	const double n = context.relativeIndex(glass);
	const double power = (n - 1.0) * (c1 - c2) + (n - 1.0) * (n - 1.0) * center * c1 * c2 / n;
	r.lengths.push_back({ "edge thickness", edge });
	if (power != 0.0) {
		const double f = 1.0 / power;
		r.lengths.push_back({ "effective focal length", f });
		r.lengths.push_back({ "back focal length", f * (1.0 - (n - 1.0) * center * c1 / n) });
		r.lengths.push_back({ "front focal length", f * (1.0 + (n - 1.0) * center * c2 / n) });
	}
	r.lines.push_back({ "index", fmt::format("{:.6f} at {:.1f} nm", n, context.designWavelength) });
}

double prismHeight(const SceneOptic& o, const char* dimension)
{
	const double height = opticNumber(o, "height");
	return height > 0.0 ? height : opticNumber(o, dimension);
}

void buildRightAnglePrism(const SceneOptic& o, const OpticBuildContext&, OpticPrescription& p, OpticReadout& r)
{
	using V = glm::dvec3;
	const double h = 0.5 * opticNumber(o, "leg");
	const double hx = 0.5 * prismHeight(o, "leg");
	glassMedia(p, opticString(o, "glass"));
	// the triangle in (y, z): the right angle at (-h, -h), the hypotenuse from (h, -h) to (-h, h)
	add(p, polygonFace({ V(-hx, -h, -h), V(hx, -h, -h), V(hx, h, -h), V(-hx, h, -h) }, V(0, 0, -1)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, polygonFace({ V(-hx, -h, -h), V(hx, -h, -h), V(hx, -h, h), V(-hx, -h, h) }, V(0, -1, 0)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, polygonFace({ V(-hx, h, -h), V(hx, h, -h), V(hx, -h, h), V(-hx, -h, h) }, V(0, 1, 1)), 1, 0, 1, OPTIC_SURFACE_OPTICAL);
	for (const double x : { -hx, hx }) {
		add(p, polygonFace({ V(x, -h, -h), V(x, h, -h), V(x, -h, h) }, V(x < 0 ? -1 : 1, 0, 0)), 1, 0, 2, 0);
	}
	r.lengths.push_back({ "hypotenuse", 2.0 * h * std::sqrt(2.0) });
}

void buildEquilateralPrism(const SceneOptic& o, const OpticBuildContext& context, OpticPrescription& p, OpticReadout& r)
{
	using V = glm::dvec3;
	const double side = opticNumber(o, "side");
	const double hx = 0.5 * prismHeight(o, "side");
	// apex up (+y), the centroid at the origin; light along +z enters the face on the -z side
	const double top = side * std::sqrt(3.0) / 3.0;
	const double bottom = -side * std::sqrt(3.0) / 6.0;
	const glm::dvec2 apex(top, 0.0);
	const glm::dvec2 left(bottom, -0.5 * side);
	const glm::dvec2 right(bottom, 0.5 * side);
	auto at = [](double x, glm::dvec2 yz) { return V(x, yz.x, yz.y); };
	const std::string glass = opticString(o, "glass");
	glassMedia(p, glass);
	auto outward = [](glm::dvec2 a, glm::dvec2 b) {
		const glm::dvec2 mid = 0.5 * (a + b);
		return V(0.0, mid.x, mid.y);
	};
	add(p, polygonFace({ at(-hx, left), at(hx, left), at(hx, apex), at(-hx, apex) }, outward(left, apex)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, polygonFace({ at(-hx, apex), at(hx, apex), at(hx, right), at(-hx, right) }, outward(apex, right)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, polygonFace({ at(-hx, left), at(hx, left), at(hx, right), at(-hx, right) }, outward(left, right)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	for (const double x : { -hx, hx }) {
		add(p, polygonFace({ at(x, left), at(x, apex), at(x, right) }, V(x < 0 ? -1 : 1, 0, 0)), 1, 0, 1, 0);
	}
	// the minimum deviation, 2 asin(n sin 30) - 60 degrees
	const double n = context.relativeIndex(glass);
	if (n * 0.5 < 1.0) {
		r.angles.push_back({ "minimum deviation", 2.0 * std::asin(0.5 * n) - degrees(60.0) });
		r.angles.push_back({ "its angle of incidence", std::asin(0.5 * n) });
	}
}

void buildPlateWedge(const SceneOptic& o, const OpticBuildContext& context, OpticPrescription& p, OpticReadout& r)
{
	using V = glm::dvec3;
	const double h = 0.5 * opticNumber(o, "side");
	const double thin = opticNumber(o, "edgeThickness");
	const double deviation = degrees(DEVIATION_DEGREES[std::clamp(opticEnum(o, "deviation"), 0, 3)]);
	const std::string glass = opticString(o, "glass");
	const double alpha = wedgeAngle(deviation, context.relativeIndex(glass));
	const double thick = thin + 2.0 * h * std::tan(alpha);
	glassMedia(p, glass);
	// thin at y = -h, thick at y = +h
	auto zBack = [&](double y) { return y < 0 ? thin : thick; };
	add(p, polygonFace({ V(-h, -h, 0), V(h, -h, 0), V(h, h, 0), V(-h, h, 0) }, V(0, 0, -1)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	add(p, polygonFace({ V(-h, -h, thin), V(h, -h, thin), V(h, h, thick), V(-h, h, thick) }, V(0, -std::sin(alpha), std::cos(alpha))), 1, 0, 1, OPTIC_SURFACE_OPTICAL);
	for (const double x : { -h, h }) {
		add(p, polygonFace({ V(x, -h, 0), V(x, h, 0), V(x, h, zBack(h)), V(x, -h, zBack(-h)) }, V(x < 0 ? -1 : 1, 0, 0)), 1, 0, 2, OPTIC_SURFACE_EDGE);
	}
	add(p, polygonFace({ V(-h, -h, 0), V(h, -h, 0), V(h, -h, thin), V(-h, -h, thin) }, V(0, -1, 0)), 1, 0, 2, OPTIC_SURFACE_EDGE);
	add(p, polygonFace({ V(-h, h, 0), V(h, h, 0), V(h, h, thick), V(-h, h, thick) }, V(0, 1, 0)), 1, 0, 2, OPTIC_SURFACE_EDGE);
	r.angles.push_back({ "wedge angle", alpha });
	r.lengths.push_back({ "thick edge", thick });
}

void buildRoundWedge(const SceneOptic& o, const OpticBuildContext& context, OpticPrescription& p, OpticReadout& r)
{
	const double radius = 0.5 * opticNumber(o, "diameter");
	const double thin = opticNumber(o, "edgeThickness");
	const double deviation = degrees(DEVIATION_DEGREES[std::clamp(opticEnum(o, "deviation"), 0, 3)]);
	const std::string glass = opticString(o, "glass");
	const double alpha = wedgeAngle(deviation, context.relativeIndex(glass));
	const double slope = std::tan(alpha);
	const double center = thin + radius * slope;
	glassMedia(p, glass);
	add(p, conicFace(glm::dvec3(0.0), true, 0.0, 0.0, circle(radius)), 1, 0, 0, OPTIC_SURFACE_OPTICAL);
	OpticSurface back;
	back.shape = SurfaceShape::Conic;
	back.placement = frameFromNormal(glm::dvec3(0.0, 0.0, center), glm::dvec3(0.0, -std::sin(alpha), std::cos(alpha)), glm::dvec3(1.0, 0.0, 0.0));
	back.aperture = ellipse(radius, radius / std::cos(alpha));
	add(p, back, 1, 0, 1, OPTIC_SURFACE_OPTICAL);
	add(p, edgeCylinder(radius, flatBound(0.0), flatBound(center, glm::dvec2(0.0, slope))), 1, 0, 2, OPTIC_SURFACE_EDGE);
	r.angles.push_back({ "wedge angle", alpha });
	r.lengths.push_back({ "center thickness", center });
	r.lengths.push_back({ "thick edge", thin + 2.0 * radius * slope });
}

// the output aperture: a disk square to the beam where it leaves, emitting toward +z (crt_laser.glsl)
void buildLaser(const SceneOptic& o, const OpticBuildContext&, OpticPrescription& p, OpticReadout& r)
{
	const LaserBeam beam = laserBeam(o);
	OpticSurface s;
	s.shape = SurfaceShape::Conic;
	s.placement = frameFromNormal(beam.center, beam.axis, beam.xAxis);
	s.aperture = circle(beam.apertureRadius);
	p.media = { OpticMedium {} };
	add(p, s, 0, 0, 0, 0);

	// the output diameter and divergence are the panel's own fields (they set the waist and M^2)
	const bool astigmatic = opticBool(o, "astigmatic");
	r.lengths.push_back({ astigmatic ? "Rayleigh range x" : "Rayleigh range", beam.x.rayleighRange() });
	if (astigmatic) {
		r.lengths.push_back({ "Rayleigh range y", beam.y.rayleighRange() });
	}
	r.lengths.push_back({ "coherence length", beam.coherenceLength });
	r.lengths.push_back({ "aperture diameter", 2.0 * beam.apertureRadius });
}

//---------------------------------------------------------------- the table

// by OpticKind
const OpticTraits OPTIC_TRAITS[] = {
	{ "planar_mirror", "Planar mirror", "Planar mirrors", MIRROR_SIZES, PLANAR_PARAMS, MIRROR_SLOTS, KINEMATIC_MOUNTS, MIRROR_PORTS, true, buildPlanarMirror, true },
	{ "concave_mirror", "Spherical concave mirror", "Concave mirrors", CONCAVE_SIZES, CONCAVE_PARAMS, MIRROR_SLOTS, KINEMATIC_MOUNTS, MIRROR_PORTS, false, buildConcaveMirror, true },
	{ "off_axis_parabola", "Off-axis parabolic mirror", "Off-axis parabolas", OAP_SIZES, OAP_PARAMS, MIRROR_SLOTS, KINEMATIC_MOUNTS, MIRROR_PORTS, false, buildOffAxisParabola, true },
	{ "circular_filter", "Circular filter", "Circular filters", ROUND_SIZES, CIRCULAR_FILTER_PARAMS, TRANSMISSIVE_SLOTS, FIXED_MOUNTS, TRANSMITTED_PORTS, false, buildCircularFilter, true },
	{ "square_filter", "Square filter", "Square filters", SQUARE_SIZES, SQUARE_FILTER_PARAMS, TRANSMISSIVE_SLOTS, PLATE_MOUNTS, TRANSMITTED_PORTS, false, buildSquareFilter, true },
	{ "iris", "Iris", "Irises", IRIS_SIZES, IRIS_PARAMS, IRIS_SLOTS, IRIS_MOUNTS, IRIS_PORTS, false, buildIris, false },
	{ "plate_beamsplitter", "Plate beamsplitter", "Plate beamsplitters", PLATE_BS_SIZES, PLATE_BS_PARAMS, PLATE_BS_SLOTS, ROTATION_MOUNTS, SPLITTER_PORTS, false, buildPlateBeamsplitter, true },
	{ "cube_beamsplitter", "Cube beamsplitter", "Cube beamsplitters", CUBE_SIZES, CUBE_PARAMS, CUBE_SLOTS, PRISM_MOUNTS, CUBE_PORTS, false, buildCubeBeamsplitter, true },
	{ "spherical_lens", "Spherical lens", "Lenses", LENS_SIZES, LENS_PARAMS, TRANSMISSIVE_SLOTS, FIXED_MOUNTS, TRANSMITTED_PORTS, false, buildSphericalLens, true },
	{ "right_angle_prism", "Right-angle prism", "Right-angle prisms", RA_SIZES, RA_PARAMS, RA_SLOTS, PRISM_MOUNTS, RA_PORTS, false, buildRightAnglePrism, true },
	{ "equilateral_prism", "Equilateral dispersing prism", "Equilateral prisms", EQ_SIZES, EQ_PARAMS, PRISM_SLOTS, PRISM_MOUNTS, PRISM_PORTS, false, buildEquilateralPrism, true },
	{ "plate_wedge", "Plate wedge prism", "Plate wedges", PLATE_WEDGE_SIZES, PLATE_WEDGE_PARAMS, TRANSMISSIVE_SLOTS, PLATE_MOUNTS, TRANSMITTED_PORTS, false, buildPlateWedge, true },
	{ "round_wedge", "Round wedge prism", "Round wedges", WEDGE_SIZES, ROUND_WEDGE_PARAMS, TRANSMISSIVE_SLOTS, ROTATION_MOUNTS, TRANSMITTED_PORTS, false, buildRoundWedge, true },
	{ "laser", "Laser", "Lasers", LASER_PRESETS, LASER_PARAMS, LASER_SLOTS, {}, LASER_PORTS, false, buildLaser, false, "preset", true },
	{ "power_meter", "Power meter", "Power meters", {}, POWER_METER_PARAMS, SENSOR_SLOTS, {}, {}, false, buildDetector, false, "size", true },
	{ "camera_sensor", "Camera sensor", "Camera sensors", {}, SENSOR_PARAMS, SENSOR_SLOTS, {}, {}, false, buildDetector, false, "size", true },
	{ "beam_profiler", "Beam profiler", "Beam profilers", {}, SENSOR_PARAMS, SENSOR_SLOTS, {}, {}, false, buildDetector, false, "size", true },
	{ "alignment_card", "Alignment card", "Alignment cards", {}, CARD_PARAMS, FACE_SLOTS, {}, {}, false, buildDetector, false, "size", true },
	{ "screen", "Screen", "Screens", {}, SCREEN_PARAMS, FACE_SLOTS, {}, {}, false, buildDetector, false, "size", true },
	{ "spectrometer", "Spectrometer", "Spectrometers", {}, SPECTROMETER_PARAMS, SENSOR_SLOTS, {}, {}, false, buildDetector, false, "size", true },
	{ "beam_dump", "Beam dump", "Beam dumps", {}, DUMP_PARAMS, SENSOR_SLOTS, {}, {}, false, buildDetector, false, "size", true },
	{ "probe_plane", "Probe plane", "Probe planes", {}, PROBE_PARAMS, {}, {}, {}, false, buildDetector, false, "size", true },
};
static_assert(std::size(OPTIC_TRAITS) == OPTIC_KIND_COUNT);

// the size preset's value for a parameter, if the optic is at a preset that sets it
const double* sizeValue(const SceneOptic& optic, std::string_view name)
{
	const OpticTraits& traits = opticTraits(optic.kind);
	if (optic.sizePreset < 0 || optic.sizePreset >= (int)traits.sizes.size()) {
		return nullptr;
	}
	for (const OpticSizeValue& value : traits.sizes[(size_t)optic.sizePreset].values) {
		if (name == value.param) {
			return &value.value;
		}
	}
	return nullptr;
}

}

double OpticBuildContext::relativeIndex(const std::string& glass) const
{
	if (library == nullptr) {
		return 1.5;
	}
	const OpticGlass* g = library->findGlass(glass);
	const OpticGlass* s = library->findGlass(surrounding);
	const double n = g != nullptr ? glassIndex(*g, designWavelength) : 1.5;
	const double ns = s != nullptr ? glassIndex(*s, designWavelength) : 1.0;
	return n / ns;
}

std::span<const OpticTraits> opticTraitsTable()
{
	return OPTIC_TRAITS;
}

const OpticTraits& opticTraits(OpticKind kind)
{
	return OPTIC_TRAITS[(size_t)kind];
}

std::optional<OpticKind> findOpticKind(std::string_view name)
{
	for (size_t i = 0; i < OPTIC_KIND_COUNT; i++) {
		if (name == OPTIC_TRAITS[i].name) {
			return (OpticKind)i;
		}
	}
	return std::nullopt;
}

const OpticParam* findOpticParam(OpticKind kind, std::string_view name)
{
	for (const OpticParam& param : opticTraits(kind).params) {
		if (name == param.name) {
			return &param;
		}
	}
	return nullptr;
}

OpticValue opticValue(const SceneOptic& optic, std::string_view name)
{
	// a size preset decides the values it sets; a custom size keeps what was last set
	if (const double* value = sizeValue(optic, name)) {
		return *value;
	}
	if (const auto found = optic.params.find(name); found != optic.params.end()) {
		return found->second;
	}
	if (const OpticParam* param = findOpticParam(optic.kind, name)) {
		return param->defaultValue;
	}
	return 0.0;
}

double opticNumber(const SceneOptic& optic, std::string_view name)
{
	const OpticValue value = opticValue(optic, name);
	if (const double* d = std::get_if<double>(&value)) {
		return *d;
	}
	if (const int* i = std::get_if<int>(&value)) {
		return (double)*i;
	}
	return 0.0;
}

int opticEnum(const SceneOptic& optic, std::string_view name)
{
	const OpticValue value = opticValue(optic, name);
	if (const int* i = std::get_if<int>(&value)) {
		return *i;
	}
	if (const double* d = std::get_if<double>(&value)) {
		return (int)*d;
	}
	return 0;
}

bool opticBool(const SceneOptic& optic, std::string_view name)
{
	const OpticValue value = opticValue(optic, name);
	const bool* b = std::get_if<bool>(&value);
	return b != nullptr && *b;
}

std::string opticString(const SceneOptic& optic, std::string_view name)
{
	const OpticValue value = opticValue(optic, name);
	const std::string* s = std::get_if<std::string>(&value);
	return s != nullptr ? *s : std::string();
}

bool setOpticValue(SceneOptic& optic, std::string_view name, OpticValue value)
{
	const OpticParam* param = findOpticParam(optic.kind, name);
	if (param == nullptr) {
		return false;
	}
	if (double* d = std::get_if<double>(&value); d != nullptr && param->max > param->min) {
		*d = std::clamp(*d, param->min, param->max);
	}
	if (int* i = std::get_if<int>(&value); i != nullptr && param->max > param->min) {
		*i = std::clamp(*i, (int)param->min, (int)param->max);
	}
	// editing a value a size preset sets makes the size custom, keeping the preset's other values
	if (sizeValue(optic, name) != nullptr) {
		setOpticSize(optic, -1);
	}
	if (opticValue(optic, name) == value) {
		return false;
	}
	optic.params[std::string(name)] = std::move(value);
	return true;
}

void setOpticSize(SceneOptic& optic, int preset)
{
	const OpticTraits& traits = opticTraits(optic.kind);
	if (preset < 0) {
		// custom keeps the preset's values as its own
		if (optic.sizePreset >= 0 && optic.sizePreset < (int)traits.sizes.size()) {
			for (const OpticSizeValue& value : traits.sizes[(size_t)optic.sizePreset].values) {
				optic.params[value.param] = value.value;
			}
		}
		optic.sizePreset = -1;
		return;
	}
	optic.sizePreset = std::min(preset, (int)traits.sizes.size() - 1);
}

SceneOptic makeOptic(OpticKind kind)
{
	SceneOptic optic;
	optic.kind = kind;
	// the first preset; a 1 in part where the kind has one, as most benches use
	optic.sizePreset = opticTraits(kind).sizes.size() >= 2 ? 1 : 0;
	// a kind without presets is its parameters' own size
	if (opticTraits(kind).sizes.empty()) {
		optic.sizePreset = -1;
	}
	if (kind == OpticKind::Iris) {
		optic.sizePreset = 4;
	}
	// in the kind's default mount, until the editor stands it on the bench; free at the old default place before that
	const std::span<const MountKind> mounts = opticTraits(kind).mounts;
	optic.mount.kind = mounts.empty() ? MountKind::None : mounts[0];
	optic.world = optic.mount.freeTransform();
	return optic;
}

std::string opticBodyKey(const SceneOptic& optic)
{
	std::string key = fmt::format("{}|{}", (int)optic.kind, optic.sizePreset);
	for (const auto& [name, value] : optic.params) {
		key += "|" + name + "=";
		if (const double* d = std::get_if<double>(&value)) {
			key += fmt::format("{:.17g}", *d);
		} else if (const int* i = std::get_if<int>(&value)) {
			key += fmt::format("i{}", *i);
		} else if (const bool* b = std::get_if<bool>(&value)) {
			key += *b ? "t" : "f";
		} else {
			key += std::get<std::string>(value);
		}
	}
	return key;
}

OpticPrescription buildOptic(const SceneOptic& optic, const OpticBuildContext& context, OpticReadout& readout)
{
	OpticPrescription prescription;
	opticTraits(optic.kind).build(optic, context, prescription, readout);
	if (!readout.error.empty()) {
		return OpticPrescription {};
	}
	finishPrescription(prescription);
	return prescription;
}

std::string defaultSlotCoating(const SceneOptic& optic, size_t slot)
{
	const OpticTraits& traits = opticTraits(optic.kind);
	if (slot >= traits.slots.size()) {
		return "uncoated";
	}
	// a polarizing splitter's coating: the cube's diagonal, the plate's front
	if (opticBool(optic, "polarizing") && ((optic.kind == OpticKind::CubeBeamsplitter && slot == 1) || (optic.kind == OpticKind::PlateBeamsplitter && slot == 0))) {
		return "ideal_pbs";
	}
	return traits.slots[slot].coating;
}

MaterialDef defaultSlotMaterial(const SceneOptic& optic, size_t slot)
{
	const OpticTraits& traits = opticTraits(optic.kind);
	MaterialDef material;
	material.name = fmt::format("{} / {}", optic.name, slot < traits.slots.size() ? traits.slots[slot].name : "slot");
	material.opticOrigin = OpticSlotOrigin { optic.id, (uint32_t)slot };
	const SlotDefault kind = slot < traits.slots.size() ? traits.slots[slot].kind : SlotDefault::Polished;
	if (kind == SlotDefault::LaserAperture) {
		material.type = MaterialType::LaserAperture;
		return material;
	}
	if (kind == SlotDefault::Sensor) {
		material.type = MaterialType::Detector;
		return material;
	}
	if (kind == SlotDefault::WhiteDiffuse) {
		// a card's face is a paper white; a screen is the ideal white of the screen checks (tools/verify/beam_probe.py)
		material.type = MaterialType::Diffuse;
		material.params["albedo"] = rgbSpectrum(glm::vec3(optic.kind == OpticKind::AlignmentCard ? 0.8f : 1.f));
		return material;
	}
	if (kind == SlotDefault::Black) {
		// black anodized aluminum, as the mounts' blades are (physoptics-bench-and-mounts.md)
		material.type = MaterialType::Diffuse;
		material.params["albedo"] = rgbSpectrum(glm::vec3(0.04f));
		return material;
	}
	material.type = MaterialType::OpticalInterface;
	material.params["coating"] = CoatingRef { kind == SlotDefault::Ground ? std::string("uncoated") : defaultSlotCoating(optic, slot) };
	material.params["finish"] = kind == SlotDefault::Ground ? 1 : 0;
	return material;
}

std::vector<double> mediumIndices(const OpticPrescription& body, const OpticBuildContext& context, double lambda)
{
	std::vector<double> out;
	for (const OpticMedium& medium : body.media) {
		const std::string& name = medium.glass.empty() ? context.surrounding : medium.glass;
		const OpticGlass* glass = context.library != nullptr ? context.library->findGlass(name) : nullptr;
		out.push_back(glass != nullptr ? glassIndex(*glass, lambda) : (medium.glass.empty() ? 1.0 : 1.5));
	}
	return out;
}

bool traceEvents(const OpticPrescription& body, const std::vector<double>& indices, std::span<const PortEvent> events, OpticRay& ray)
{
	for (const PortEvent& event : events) {
		OpticHitT<double> hit;
		if (!intersectOpticBody(body, ray.origin, ray.direction, 1e-12, 1e30, hit) || hit.surface != event.surface) {
			return false;
		}
		const OpticSurface& s = body.surfaces[hit.surface];
		const glm::dvec3 n = toBodyVector(s, surfaceNormalLocal(s, hit.local));
		const glm::dvec3 p = ray.origin + hit.t * ray.direction;
		// arriving against the normal: from the plus side, into the minus medium
		const bool fromPlus = glm::dot(ray.direction, n) < 0.0;
		const double nIncident = indices[fromPlus ? s.mediumPlus : s.mediumMinus];
		const double nTransmitted = indices[fromPlus ? s.mediumMinus : s.mediumPlus];
		glm::dvec3 out;
		if (event.reflect) {
			reflectDirection(ray.direction, n, out);
		} else if (!refractDirection(ray.direction, n, nIncident / nTransmitted, out)) {
			return false;
		}
		ray.origin = p;
		ray.direction = glm::normalize(out);
	}
	return true;
}
