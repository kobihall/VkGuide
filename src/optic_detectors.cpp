#include <optic_detectors.h>

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

namespace {

constexpr double PI = 3.14159265358979323846;

uint32_t count(const SceneOptic& optic, std::string_view name)
{
	return (uint32_t)std::max(1.0, std::round(opticNumber(optic, name)));
}

// the first and second moments of the pixels inside a rectangle of half sizes (a, b) about (cx, cy), turned by `angle`;
// the whole image when a is 0
BeamMoments momentsWithin(const DetectorImage& image, double cx, double cy, double angle, double a, double b)
{
	BeamMoments m;
	const double c = std::cos(angle);
	const double s = std::sin(angle);
	double sum = 0.0;
	double sx = 0.0;
	double sy = 0.0;
	auto inside = [&](double x, double y) {
		if (a <= 0.0) {
			return true;
		}
		const double u = (x - cx) * c + (y - cy) * s;
		const double v = -(x - cx) * s + (y - cy) * c;
		return std::abs(u) <= a && std::abs(v) <= b;
	};
	for (uint32_t y = 0; y < image.height; y++) {
		for (uint32_t x = 0; x < image.width; x++) {
			const double px = image.centerX(x);
			const double py = image.centerY(y);
			if (!inside(px, py)) {
				continue;
			}
			const double w = image.at(x, y);
			sum += w;
			sx += w * px;
			sy += w * py;
		}
	}
	m.power = sum;
	if (!(sum > 0.0)) {
		return m;
	}
	m.centroidX = sx / sum;
	m.centroidY = sy / sum;
	double xx = 0.0;
	double yy = 0.0;
	double xy = 0.0;
	for (uint32_t y = 0; y < image.height; y++) {
		for (uint32_t x = 0; x < image.width; x++) {
			const double px = image.centerX(x);
			const double py = image.centerY(y);
			if (!inside(px, py)) {
				continue;
			}
			const double w = image.at(x, y);
			const double dx = px - m.centroidX;
			const double dy = py - m.centroidY;
			xx += w * dx * dx;
			yy += w * dy * dy;
			xy += w * dx * dy;
		}
	}
	// Sheppard's correction: a pixel's power counted at its center spreads the beam by the pixel's own second moment,
	// pitch^2 / 12, which comes off. ISO 11146 sums at the centers; with a film of exact pixel powers this measures the
	// beam itself, not the beam widened by the grid
	m.sigmaXX = std::max(xx / sum - image.pixelWidth * image.pixelWidth / 12.0, 0.0);
	m.sigmaYY = std::max(yy / sum - image.pixelHeight * image.pixelHeight / 12.0, 0.0);
	m.sigmaXY = xy / sum;
	// ISO 11146-1, 3.5 and 3.6: d = 2 sqrt(2) (sxx + syy +- gamma sqrt((sxx - syy)^2 + 4 sxy^2))^(1/2), gamma the sign
	// of sxx - syy, and the principal axis at atan2(2 sxy, sxx - syy) / 2
	m.diameterX = 4.0 * std::sqrt(std::max(m.sigmaXX, 0.0));
	m.diameterY = 4.0 * std::sqrt(std::max(m.sigmaYY, 0.0));
	const double sumSigma = m.sigmaXX + m.sigmaYY;
	const double root = std::sqrt((m.sigmaXX - m.sigmaYY) * (m.sigmaXX - m.sigmaYY) + 4.0 * m.sigmaXY * m.sigmaXY);
	m.diameterMajor = 2.0 * std::sqrt(2.0) * std::sqrt(std::max(sumSigma + root, 0.0));
	m.diameterMinor = 2.0 * std::sqrt(2.0) * std::sqrt(std::max(sumSigma - root, 0.0));
	m.angle = 0.5 * std::atan2(2.0 * m.sigmaXY, m.sigmaXX - m.sigmaYY);
	return m;
}

}

bool isDetectorKind(OpticKind kind)
{
	return kind >= OpticKind::PowerMeter && kind <= OpticKind::ProbePlane;
}

std::optional<CausticFit> fitCaustic(std::span<const double> z, std::span<const double> w)
{
	const size_t n = std::min(z.size(), w.size());
	if (n < 3) {
		return std::nullopt;
	}
	// about the positions' mean, so the normal equations stay well conditioned a meter down the table
	double mean = 0.0;
	for (size_t i = 0; i < n; i++) {
		mean += z[i] / (double)n;
	}
	// sums of t^k (k = 0..4) and of w^2 t^k (k = 0..2)
	double s[5] = {};
	double r[3] = {};
	for (size_t i = 0; i < n; i++) {
		const double t = z[i] - mean;
		double p = 1.0;
		for (int k = 0; k < 5; k++) {
			s[k] += p;
			if (k < 3) {
				r[k] += w[i] * w[i] * p;
			}
			p *= t;
		}
	}
	// [s0 s1 s2; s1 s2 s3; s2 s3 s4] (a, b, c) = r, by Cramer's rule
	auto det3 = [](double a, double b, double c, double d, double e, double f, double g, double h, double i) { return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g); };
	const double d = det3(s[0], s[1], s[2], s[1], s[2], s[3], s[2], s[3], s[4]);
	if (!(std::abs(d) > 0.0)) {
		return std::nullopt;
	}
	const double a = det3(r[0], s[1], s[2], r[1], s[2], s[3], r[2], s[3], s[4]) / d;
	const double b = det3(s[0], r[0], s[2], s[1], r[1], s[3], s[2], r[2], s[4]) / d;
	const double c = det3(s[0], s[1], r[0], s[1], s[2], r[1], s[2], s[3], r[2]) / d;
	const double waist2 = a - b * b / (4.0 * c);
	if (!(c > 0.0) || !(waist2 > 0.0)) {
		return std::nullopt;
	}
	CausticFit fit;
	fit.waist = std::sqrt(waist2);
	fit.position = mean - b / (2.0 * c);
	fit.halfAngle = std::sqrt(c);
	return fit;
}

SensorStack makeSensorStack(OpticKind kind)
{
	SensorStack stack;
	stack.sensor = makeOptic(kind);
	stack.end = stack.start + glm::dvec3(0.0, 0.0, 0.4);
	return stack;
}

glm::dvec3 sensorStackAxis(const SensorStack& stack)
{
	const glm::dvec3 d = stack.end - stack.start;
	const double length = glm::length(d);
	return length > 0.0 ? d / length : glm::dvec3(0.0);
}

glm::dquat sensorStackOrientation(const SensorStack& stack)
{
	const glm::dquat q = glm::normalize(stack.orientation);
	const glm::dvec3 z = sensorStackAxis(stack);
	if (!stack.alignToAxis || z == glm::dvec3(0.0)) {
		return q;
	}
	// the orientation's x across the axis; its y where its x is (nearly) along the axis
	glm::dvec3 x = q * glm::dvec3(1.0, 0.0, 0.0);
	x -= glm::dot(x, z) * z;
	if (glm::length(x) < 1e-6) {
		const glm::dvec3 y = q * glm::dvec3(0.0, 1.0, 0.0);
		x = glm::cross(y - glm::dot(y, z) * z, z);
	}
	x = glm::normalize(x);
	return glm::normalize(glm::quat_cast(glm::dmat3(x, glm::cross(z, x), z)));
}

glm::dvec3 sensorStackPosition(const SensorStack& stack, int member)
{
	if (stack.count <= 1) {
		return stack.start;
	}
	const double t = (double)member / (double)(stack.count - 1);
	return member == stack.count - 1 ? stack.end : stack.start + t * (stack.end - stack.start);
}

std::string sensorStackMemberName(const SensorStack& stack, int member)
{
	return fmt::format("{}_{}", stack.name, member);
}

DetectorGeometry detectorGeometry(const SceneOptic& optic)
{
	DetectorGeometry g;
	// a power meter, a spectrometer and a beam dump read power (or a spectrum) and nothing spatial: one pixel, the user's
	// choice on 2026-11-04
	auto disk = [&](const char* housing) {
		g.shape = SensorShape::Disk;
		g.halfWidth = g.halfHeight = 0.5 * opticNumber(optic, "diameter");
		g.pixelsX = g.pixelsY = g.nativeX = g.nativeY = 1;
		g.housing = housing;
	};
	// a sheet's size and its film's pixels, or a physical sensor's pixels and pitch, the film at most as fine
	auto sheet = [&](const char* housing) {
		g.pixelsX = g.nativeX = count(optic, "pixelsX");
		g.pixelsY = g.nativeY = count(optic, "pixelsY");
		g.halfWidth = 0.5 * opticNumber(optic, "width");
		g.halfHeight = 0.5 * opticNumber(optic, "height");
		if (findOpticParam(optic.kind, "physicalSensor") != nullptr && opticBool(optic, "physicalSensor")) {
			g.physical = true;
			g.nativeX = count(optic, "nativeX");
			g.nativeY = count(optic, "nativeY");
			const double pitch = opticNumber(optic, "pitch");
			g.halfWidth = 0.5 * pitch * g.nativeX;
			g.halfHeight = 0.5 * pitch * g.nativeY;
			g.pixelsX = std::min(g.pixelsX, g.nativeX);
			g.pixelsY = std::min(g.pixelsY, g.nativeY);
		}
		g.imaging = true;
		g.housing = housing;
	};
	switch (optic.kind) {
	case OpticKind::PowerMeter:
		disk("power_meter_head");
		break;
	case OpticKind::CameraSensor:
		sheet("camera");
		break;
	case OpticKind::BeamProfiler:
		sheet("beam_profiler");
		break;
	case OpticKind::AlignmentCard:
		sheet("alignment_card");
		g.action = DetectorAction::Scatter;
		break;
	case OpticKind::Screen:
		sheet(nullptr);
		g.action = DetectorAction::Scatter;
		break;
	case OpticKind::Spectrometer:
		disk(nullptr);
		g.spectrometer = true;
		g.lambdaMin = 1e9 * opticNumber(optic, "lambdaMin");
		g.lambdaMax = std::max(1e9 * opticNumber(optic, "lambdaMax"), g.lambdaMin + 1e-3);
		g.bins = count(optic, "bins");
		break;
	case OpticKind::BeamDump:
		disk("beam_dump");
		break;
	case OpticKind::ProbePlane:
		sheet(nullptr);
		g.action = DetectorAction::Transparent;
		break;
	default:
		break;
	}
	g.pixelWidth = 2.0 * g.halfWidth / (double)g.pixelsX;
	g.pixelHeight = 2.0 * g.halfHeight / (double)g.pixelsY;
	return g;
}

uint32_t detectorImageFloats(const DetectorGeometry& geometry, uint32_t channels)
{
	return geometry.pixelsX * geometry.pixelsY * channels;
}

uint32_t detectorSpectrumBins(const DetectorGeometry& geometry, bool spectral)
{
	return geometry.spectrometer && spectral ? geometry.bins : 0u;
}

uint32_t frameCopies(uint32_t floats)
{
	uint32_t copies = MAX_FRAME_COPIES;
	while (copies > 1 && (uint64_t)floats * copies > FRAME_COPY_FLOATS) {
		copies >>= 1;
	}
	return copies;
}

void buildDetector(const SceneOptic& optic, const OpticBuildContext&, OpticPrescription& p, OpticReadout& r)
{
	const DetectorGeometry g = detectorGeometry(optic);
	OpticSurface s;
	s.shape = SurfaceShape::Conic;
	// facing -z, toward the light arriving along +z: the turned frame's x is the body's x
	s.placement = frameFromNormal(glm::dvec3(0.0), glm::dvec3(0.0, 0.0, -1.0), glm::dvec3(1.0, 0.0, 0.0));
	if (g.shape == SensorShape::Disk) {
		s.aperture.kind = ApertureKind::Circle;
		s.aperture.a = g.halfWidth;
	} else {
		s.aperture.kind = ApertureKind::Rectangle;
		s.aperture.a = g.halfWidth;
		s.aperture.b = g.halfHeight;
	}
	s.mediumMinus = 0;
	s.mediumPlus = 0;
	s.materialSlot = 0;
	s.flags = 0;
	p.media = { OpticMedium {} };
	p.surfaces.push_back(s);

	if (g.physical) {
		r.lengths.push_back({ "sensor width", 2.0 * g.halfWidth });
		r.lengths.push_back({ "sensor height", 2.0 * g.halfHeight });
	}
	if (g.imaging) {
		r.lines.push_back({ "film", fmt::format("{} x {} pixels", g.pixelsX, g.pixelsY) });
		r.lengths.push_back({ "film pixel", g.pixelWidth });
	}
	if (g.physical) {
		r.lines.push_back({ "physical sensor", fmt::format("{} x {} pixels, {:.3g} x {:.3g} of them a film pixel", g.nativeX, g.nativeY,
			(double)g.nativeX / g.pixelsX, (double)g.nativeY / g.pixelsY) });
	}
	if (g.spectrometer) {
		r.lines.push_back({ "resolution", fmt::format("{:.3f} nm a bin", (g.lambdaMax - g.lambdaMin) / (double)g.bins) });
	}
}

//---------------------------------------------------------------- measurements

BeamMoments imageMoments(const DetectorImage& image)
{
	return momentsWithin(image, 0.0, 0.0, 0.0, 0.0, 0.0);
}

BeamMoments iso11146Moments(const DetectorImage& image, double tolerance, uint32_t maxIterations)
{
	BeamMoments m = imageMoments(image);
	if (!(m.power > 0.0)) {
		return m;
	}
	m.converged = false;
	for (uint32_t i = 1; i <= maxIterations; i++) {
		// three times the widths across: half sizes of 1.5 diameters about the centroid, along the principal axes
		BeamMoments next = momentsWithin(image, m.centroidX, m.centroidY, m.angle, 1.5 * m.diameterMajor, 1.5 * m.diameterMinor);
		next.iterations = i;
		if (!(next.power > 0.0)) {
			return m;
		}
		const bool settled = std::abs(next.diameterMajor - m.diameterMajor) <= tolerance * m.diameterMajor
			&& std::abs(next.diameterMinor - m.diameterMinor) <= tolerance * std::max(m.diameterMinor, 1e-30);
		m = next;
		if (settled) {
			m.converged = true;
			break;
		}
	}
	return m;
}

double peakIrradiance(const DetectorImage& image)
{
	double peak = 0.0;
	for (const double w : image.watts) {
		peak = std::max(peak, w);
	}
	const double area = image.pixelWidth * image.pixelHeight;
	return area > 0.0 ? peak / area : 0.0;
}

EncircledPower encircledPower(const DetectorImage& image, double centerX, double centerY, double maxRadius, uint32_t samples)
{
	EncircledPower out;
	samples = std::max(samples, 2u);
	std::vector<double> perBin(samples, 0.0);
	const double step = maxRadius / (double)(samples - 1);
	for (uint32_t y = 0; y < image.height; y++) {
		for (uint32_t x = 0; x < image.width; x++) {
			const double r = std::hypot(image.centerX(x) - centerX, image.centerY(y) - centerY);
			// the first radius at or beyond the pixel's center
			const double bin = step > 0.0 ? std::ceil(r / step) : 0.0;
			if (bin < (double)samples) {
				perBin[(size_t)bin] += image.at(x, y);
			}
		}
	}
	double running = 0.0;
	for (uint32_t i = 0; i < samples; i++) {
		running += perBin[i];
		out.radius.push_back(step * (double)i);
		out.power.push_back(running);
	}
	return out;
}

Profile lineProfile(const DetectorImage& image, double centerX, double centerY, double angle, uint32_t samples)
{
	Profile out;
	const double halfX = 0.5 * image.pixelWidth * image.width;
	const double halfY = 0.5 * image.pixelHeight * image.height;
	const double reach = std::hypot(halfX, halfY);
	const double c = std::cos(angle);
	const double s = std::sin(angle);
	const double area = image.pixelWidth * image.pixelHeight;
	samples = std::max(samples, 2u);
	for (uint32_t i = 0; i < samples; i++) {
		const double t = -reach + 2.0 * reach * (double)i / (double)(samples - 1);
		const double x = centerX + t * c;
		const double y = centerY + t * s;
		if (std::abs(x) > halfX || std::abs(y) > halfY) {
			continue;
		}
		// bilinear between the pixel centers
		const double fx = std::clamp((x + halfX) / image.pixelWidth - 0.5, 0.0, (double)image.width - 1.0);
		const double fy = std::clamp((y + halfY) / image.pixelHeight - 0.5, 0.0, (double)image.height - 1.0);
		const uint32_t x0 = (uint32_t)fx;
		const uint32_t y0 = (uint32_t)fy;
		const uint32_t x1 = std::min(x0 + 1, image.width - 1);
		const uint32_t y1 = std::min(y0 + 1, image.height - 1);
		const double ax = fx - x0;
		const double ay = fy - y0;
		const double w = (1 - ax) * (1 - ay) * image.at(x0, y0) + ax * (1 - ay) * image.at(x1, y0) + (1 - ax) * ay * image.at(x0, y1) + ax * ay * image.at(x1, y1);
		out.position.push_back(t);
		out.irradiance.push_back(area > 0.0 ? w / area : 0.0);
	}
	return out;
}

void FrameStatistics::add(double reading, double paths)
{
	if (!(paths > 0.0)) {
		return;
	}
	frames++;
	weight += paths;
	const double delta = reading - mean;
	mean += delta * paths / weight;
	m2 += paths * delta * (reading - mean);
}

double FrameStatistics::standardError() const
{
	// each reading is a mean over its paths, so its variance is the paths' over their count: the weighted sample
	// variance, over the paths, is the mean's
	if (frames < 2 || !(weight > 0.0)) {
		return 0.0;
	}
	return std::sqrt(std::max(m2, 0.0) / ((double)(frames - 1) * weight));
}
