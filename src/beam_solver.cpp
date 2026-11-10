#include <beam_solver.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <set>

#include <fmt/format.h>

#include <optic_detectors.h>
#include <optic_glass.h>
#include <optic_laser.h>
#include <optic_library.h>

const char* beamEndLabel(BeamEnd end)
{
	switch (end) {
	case BeamEnd::Surface:
		return "surface";
	case BeamEnd::Pass:
		return "crossed";
	case BeamEnd::Detector:
		return "detected";
	case BeamEnd::Absorbed:
		return "absorbed";
	case BeamEnd::Scattered:
		return "scattered";
	case BeamEnd::Laser:
		return "into a laser";
	case BeamEnd::Escaped:
		return "escaped";
	case BeamEnd::Truncated:
		return "truncated";
	}
	return "?";
}

bool BeamSegment::operator==(const BeamSegment& other) const
{
	// keys name segments within one solver's life and differ between a full solve and a selective one; the indices they
	// resolve to are what must agree
	BeamBranchState a = start;
	BeamBranchState b = other.start;
	a.entry = b.entry = 0;
	a.passage = b.passage = 0;
	return parent == other.parent && passage == other.passage && a == b && length == other.length && end == other.end && endOptic == other.endOptic
		&& endSurface == other.endSurface && endNormal == other.endNormal && endPower == other.endPower && endClipped == other.endClipped && dropped == other.dropped
		&& spent == other.spent && reading == other.reading;
}

BeamPowerBudget& BeamPowerBudget::operator+=(const BeamPowerBudget& other)
{
	detected += other.detected;
	absorbed += other.absorbed;
	glass += other.glass;
	clipped += other.clipped;
	scattered += other.scattered;
	escaped += other.escaped;
	backReflected += other.backReflected;
	belowThreshold += other.belowThreshold;
	truncated += other.truncated;
	return *this;
}

BeamProfile BeamTree::profile(uint32_t segment) const
{
	const BeamSegment& s = segments[segment];
	return beamProfile(s.start.rays, s.start.index, wavelength);
}

BeamCrossSection BeamTree::crossSection(uint32_t segment, double s) const
{
	const BeamSegment& seg = segments[segment];
	return ::crossSection(seg.start.rays, s, seg.start.index, wavelength);
}

const BeamNodeReport* BeamSolution::report(uint64_t axis, uint32_t node) const
{
	const auto found = reports.find(axis);
	if (found == reports.end()) {
		return nullptr;
	}
	for (const BeamNodeReport& r : found->second) {
		if (r.node == node) {
			return &r;
		}
	}
	return nullptr;
}

const BeamTree* BeamSolution::treeOf(uint64_t axis) const
{
	const auto found = axisTrees.find(axis);
	return found != axisTrees.end() && found->second < trees.size() ? &trees[found->second] : nullptr;
}

OpticPortId matchOpticPort(OpticKind kind, std::span<const PortEvent> events)
{
	const std::span<const OpticPort> ports = opticTraits(kind).ports;
	const auto same = [](const PortEvent& a, const PortEvent& b) { return a.surface == b.surface && a.reflect == b.reflect; };
	for (size_t p = 0; p < ports.size(); p++) {
		const std::span<const PortEvent> port = ports[p].events;
		if (port.size() != events.size()) {
			continue;
		}
		if (std::equal(port.begin(), port.end(), events.begin(), same) || std::equal(port.rbegin(), port.rend(), events.begin(), same)) {
			return (OpticPortId)p;
		}
	}
	return NO_PORT;
}

bool opticPortPrefix(OpticKind kind, std::span<const PortEvent> events)
{
	const std::span<const OpticPort> ports = opticTraits(kind).ports;
	const auto same = [](const PortEvent& a, const PortEvent& b) { return a.surface == b.surface && a.reflect == b.reflect; };
	for (const OpticPort& port : ports) {
		if (events.size() > port.events.size()) {
			continue;
		}
		if (std::equal(events.begin(), events.end(), port.events.begin(), same) || std::equal(events.begin(), events.end(), port.events.rbegin(), same)) {
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------- tracing

namespace {

constexpr double T_MIN = 1e-9;

// a segment while its tree is built: the segment and its parent's key
struct TracedSegment {
	BeamSegment segment;
	uint64_t parentKey { 0 };
};

bool slabHit(const glm::dvec3& lo, const glm::dvec3& hi, const glm::dvec3& o, const glm::dvec3& d, double tMax)
{
	double t0 = 0.0;
	double t1 = tMax;
	for (int i = 0; i < 3; i++) {
		if (std::abs(d[i]) < 1e-300) {
			if (o[i] < lo[i] || o[i] > hi[i]) {
				return false;
			}
			continue;
		}
		double a = (lo[i] - o[i]) / d[i];
		double b = (hi[i] - o[i]) / d[i];
		if (a > b) {
			std::swap(a, b);
		}
		t0 = std::max(t0, a);
		t1 = std::min(t1, b);
		if (t0 > t1) {
			return false;
		}
	}
	return true;
}

// the surface of an iris that holds its opening (the annulus of its blades)
std::optional<uint32_t> irisSurface(const OpticPrescription& body)
{
	for (uint32_t i = 0; i < body.surfaces.size(); i++) {
		if (body.surfaces[i].aperture.kind == ApertureKind::Annulus) {
			return i;
		}
	}
	return std::nullopt;
}

bool crossesOnly(OpticKind kind)
{
	return kind == OpticKind::ProbePlane;
}

} // namespace

struct BeamTracer {
	const BeamSolver& solver;
	const BeamSolverInput& input;
	BeamTree& tree;
	uint64_t& nextKey;
	std::vector<TracedSegment>& out;
	size_t treeSize { 0 };
	double lambdaNm { 632.8 };
	// per optic index: its media's real indices and absorption coefficients at the tree's wavelength
	std::vector<std::vector<double>> indices;
	std::vector<std::vector<double>> absorption;

	struct Hit {
		double t { INFINITY };
		size_t optic { SIZE_MAX };
		uint32_t surface { 0 };
		bool pass { false };
		glm::dvec3 local { 0.0 };
	};

	const BeamSolver::OpticEntry& entry(size_t i) const { return solver.m_optics[i]; }

	std::string glassOf(const OpticPrescription& body, uint8_t medium) const
	{
		const std::string& glass = medium < body.media.size() ? body.media[medium].glass : std::string();
		return glass.empty() ? input.build.surrounding : glass;
	}

	void prepareMedia()
	{
		lambdaNm = tree.wavelength * 1e9;
		indices.resize(solver.m_optics.size());
		absorption.resize(solver.m_optics.size());
		for (size_t i = 0; i < solver.m_optics.size(); i++) {
			const OpticPrescription& body = *entry(i).body;
			indices[i] = mediumIndices(body, input.build, lambdaNm);
			absorption[i].assign(body.media.size(), 0.0);
			for (size_t m = 0; m < body.media.size(); m++) {
				const OpticGlass* glass = input.build.library != nullptr ? input.build.library->findGlass(glassOf(body, (uint8_t)m)) : nullptr;
				absorption[i][m] = glass != nullptr ? glassAbsorption(*glass, lambdaNm) : 0.0;
			}
		}
	}

	double surroundingIndex() const
	{
		const OpticGlass* glass = input.build.library != nullptr ? input.build.library->findGlass(input.build.surrounding) : nullptr;
		return glass != nullptr ? glassIndex(*glass, lambdaNm) : 1.0;
	}

	// the nearest hit of one optic in [tMin, tMax], skipping its rims from outside (a beam that misses the optic goes on)
	Hit hitOptic(size_t i, const glm::dvec3& origin, const glm::dvec3& direction, bool outside, double tMax) const
	{
		Hit best;
		const BeamSolver::OpticEntry& e = entry(i);
		if (!slabHit(e.boundsMin, e.boundsMax, origin, direction, tMax)) {
			return best;
		}
		const glm::dvec3 o = glm::dvec3(e.worldToBody * glm::dvec4(origin, 1.0));
		const glm::dvec3 d = glm::dvec3(e.worldToBody * glm::dvec4(direction, 0.0));
		const OpticPrescription& body = *e.body;
		if (e.optic.kind == OpticKind::ProbePlane) {
			if (std::abs(d.z) > 1e-300) {
				const double t = -o.z / d.z;
				const glm::dvec3 p = o + t * d;
				const DetectorGeometry g = detectorGeometry(e.optic);
				const bool in = g.shape == SensorShape::Disk ? glm::length(glm::dvec2(p)) <= g.halfWidth
															 : std::abs(p.x) <= g.halfWidth && std::abs(p.y) <= g.halfHeight;
				if (in && t > T_MIN && t < tMax) {
					best = Hit { t, i, 0, true, p };
				}
			}
			return best;
		}
		double tMin = T_MIN;
		for (int attempt = 0; attempt < 8; attempt++) {
			OpticHitT<double> hit;
			if (!intersectOpticBody(body, o, d, tMin, tMax, hit)) {
				break;
			}
			if (outside && (body.surfaces[hit.surface].flags & OPTIC_SURFACE_EDGE) != 0) {
				tMin = hit.t + T_MIN;
				continue;
			}
			best = Hit { hit.t, i, hit.surface, false, hit.local };
			break;
		}
		// an iris's opening, crossed
		if (outside && e.optic.kind == OpticKind::Iris) {
			if (const std::optional<uint32_t> s = irisSurface(body)) {
				const OpticSurface& surface = body.surfaces[*s];
				const glm::dvec3 ol = toSurfacePoint(surface, o);
				const glm::dvec3 dl = toSurfaceVector(surface, d);
				if (std::abs(dl.z) > 1e-300) {
					const double t = -ol.z / dl.z;
					const glm::dvec3 p = ol + t * dl;
					if (t > T_MIN && t < std::min(tMax, best.t) && glm::length(glm::dvec2(p) - surface.aperture.center) < surface.aperture.a) {
						best = Hit { t, i, *s, true, p };
					}
				}
			}
		}
		return best;
	}

	Hit nearest(const BeamBranchState& s) const
	{
		if (s.inside != 0) {
			const auto found = solver.m_opticIndex.find(s.inside);
			return found != solver.m_opticIndex.end() ? hitOptic(found->second, s.origin, s.direction, false, INFINITY) : Hit {};
		}
		Hit best;
		for (size_t i = 0; i < solver.m_optics.size(); i++) {
			const Hit h = hitOptic(i, s.origin, s.direction, true, best.t);
			if (h.t < best.t) {
				best = h;
			}
		}
		return best;
	}

	// a world point on a surface's local frame
	glm::dvec3 localPoint(size_t optic, uint32_t surface, const glm::dvec3& world) const
	{
		const BeamSolver::OpticEntry& e = entry(optic);
		return toSurfacePoint(e.body->surfaces[surface], glm::dvec3(e.worldToBody * glm::dvec4(world, 1.0)));
	}

	// the affine map from a transverse offset of the beam at `point` to a surface's local xy, along the beam onto the
	// plane of `normal`: exact for the tangent plane, so one evaluation per hit stands in for a transform per test
	struct LocalMap {
		glm::dvec2 origin { 0.0 };
		glm::dmat2 linear { 1.0 };
		glm::dvec2 operator()(const glm::dvec2& x) const { return origin + linear * x; }
	};
	LocalMap localMap(size_t optic, uint32_t surface, const glm::dvec3& point, const glm::dvec3& normal, const BeamBranchState& s) const
	{
		LocalMap map;
		map.origin = glm::dvec2(localPoint(optic, surface, point));
		for (int j = 0; j < 2; j++) {
			const glm::dvec2 unit = j == 0 ? glm::dvec2(1.0, 0.0) : glm::dvec2(0.0, 1.0);
			map.linear[j] = glm::dvec2(localPoint(optic, surface, point + tangentPoint(normal, s.direction, s.e1, s.e2, unit))) - map.origin;
		}
		return map;
	}

	glm::dvec3 bodyToWorldVector(size_t optic, const glm::dvec3& v) const { return glm::normalize(glm::dvec3(entry(optic).optic.world * glm::dvec4(v, 0.0))); }

	// the beam's reading on a detector's sensor or a probe plane, in the body's x and y
	BeamDetectorReading reading(size_t optic, const glm::dvec3& point, const BeamBranchState& s, const glm::dmat2& w2, double power) const
	{
		const BeamSolver::OpticEntry& e = entry(optic);
		const glm::dvec3 u = bodyToWorldVector(optic, glm::dvec3(1.0, 0.0, 0.0));
		const glm::dvec3 v = bodyToWorldVector(optic, glm::dvec3(0.0, 1.0, 0.0));
		const glm::dvec3 n = bodyToWorldVector(optic, glm::dvec3(0.0, 0.0, 1.0));
		glm::dmat2 t;
		for (int j = 0; j < 2; j++) {
			const glm::dvec3 offset = tangentPoint(n, s.direction, s.e1, s.e2, j == 0 ? glm::dvec2(1.0, 0.0) : glm::dvec2(0.0, 1.0));
			t[j][0] = glm::dot(u, offset);
			t[j][1] = glm::dot(v, offset);
		}
		const glm::dmat2 plane = t * w2 * glm::transpose(t);
		const double a = plane[0][0];
		const double b = 0.5 * (plane[0][1] + plane[1][0]);
		const double c = plane[1][1];
		const double spread = std::sqrt(0.25 * (a - c) * (a - c) + b * b);
		BeamDetectorReading r;
		r.optic = e.optic.id;
		r.angle = 0.5 * std::atan2(2.0 * b, a - c);
		r.radii = glm::sqrt(glm::max(glm::dvec2(0.5 * (a + c) + spread, 0.5 * (a + c) - spread), glm::dvec2(0.0)));
		const glm::dvec3 local = glm::dvec3(e.worldToBody * glm::dvec4(point, 1.0));
		r.centroid = glm::dvec2(local);
		const DetectorGeometry g = detectorGeometry(e.optic);
		const glm::dvec3 bodyD = glm::dvec3(e.worldToBody * glm::dvec4(s.direction, 0.0));
		const glm::dvec3 bodyE1 = glm::dvec3(e.worldToBody * glm::dvec4(s.e1, 0.0));
		const glm::dvec3 bodyE2 = glm::dvec3(e.worldToBody * glm::dvec4(s.e2, 0.0));
		glm::dmat2 linear;
		for (int j = 0; j < 2; j++) {
			linear[j] = glm::dvec2(tangentPoint(glm::dvec3(0.0, 0.0, 1.0), bodyD, bodyE1, bodyE2, j == 0 ? glm::dvec2(1.0, 0.0) : glm::dvec2(0.0, 1.0)));
		}
		const glm::dvec2 center(local);
		const double fraction = fractionInside(w2,
			[&](const glm::dvec2& x) {
				const glm::dvec2 p = center + linear * x;
				return g.shape == SensorShape::Disk ? glm::length(p) <= g.halfWidth : std::abs(p.x) <= g.halfWidth && std::abs(p.y) <= g.halfHeight;
			},
			true);
		r.power = power * fraction;
		return r;
	}

	// traces a branch and everything it splits into, appending to `out` depth first, reflected before refracted
	void trace(const BeamBranchState& first, uint64_t firstParent)
	{
		struct Pending {
			BeamBranchState state;
			uint64_t parent;
		};
		std::vector<Pending> stack { { first, firstParent } };
		const double threshold = tree.threshold * tree.power;
		while (!stack.empty()) {
			Pending pending = std::move(stack.back());
			stack.pop_back();
			const BeamBranchState& s = pending.state;
			TracedSegment traced;
			BeamSegment& seg = traced.segment;
			seg.key = nextKey++;
			seg.start = s;
			traced.parentKey = pending.parent;
			treeSize++;

			if (s.depth >= BEAM_MAX_INTERACTIONS || treeSize >= BEAM_MAX_SEGMENTS) {
				seg.end = BeamEnd::Truncated;
				seg.length = 0.0;
				seg.endPower = s.power;
				seg.spent.truncated = s.power;
				tree.truncated = true;
				out.push_back(std::move(traced));
				continue;
			}

			const Hit hit = nearest(s);
			if (hit.optic == SIZE_MAX) {
				seg.end = BeamEnd::Escaped;
				seg.length = BEAM_ESCAPE_LENGTH;
				seg.endPower = s.power;
				seg.spent.escaped = s.power;
				out.push_back(std::move(traced));
				continue;
			}

			const BeamSolver::OpticEntry& e = entry(hit.optic);
			const OpticPrescription& body = *e.body;
			seg.length = hit.t;
			seg.endOptic = e.optic.id;
			seg.endSurface = hit.surface;
			// what the medium absorbed on the way
			double alpha = 0.0;
			if (s.inside != 0) {
				const auto found = solver.m_opticIndex.find(s.inside);
				if (found != solver.m_opticIndex.end() && s.medium < absorption[found->second].size()) {
					alpha = absorption[found->second][s.medium];
				}
			}
			const double power = s.power * std::exp(-alpha * hit.t);
			seg.spent.glass = s.power - power;
			seg.endPower = power;
			const glm::dvec3 point = s.origin + hit.t * s.direction;
			const glm::dmat2 w2 = ::crossSection(s.rays, hit.t, s.index, tree.wavelength).w2;
			// the beam at the end, carried there
			const ComplexRays arriving = driftTransfer(hit.t, s.index) * s.rays;

			BeamBranchState next = s;
			next.depth = s.depth + 1;
			next.opticalPath = s.opticalPath + s.index * hit.t;
			next.distance = s.distance + hit.t;
			next.origin = point;
			next.fromOptic = 0;
			next.passage = 0;
			next.port = NO_PORT;

			if (hit.pass) {
				// an iris's opening or a probe plane: through, less what the blades take
				seg.end = BeamEnd::Pass;
				const glm::dvec3 normal = e.optic.kind == OpticKind::ProbePlane ? bodyToWorldVector(hit.optic, glm::dvec3(0.0, 0.0, 1.0))
																			   : bodyToWorldVector(hit.optic, toBodyVector(body.surfaces[hit.surface], glm::dvec3(0.0, 0.0, 1.0)));
				seg.endNormal = normal;
				double fraction = 1.0;
				if (e.optic.kind == OpticKind::Iris) {
					const OpticSurface& surface = body.surfaces[hit.surface];
					const LocalMap map = localMap(hit.optic, hit.surface, point, normal, s);
					fraction = fractionInside(w2, [&](const glm::dvec2& x) { return glm::length(map(x) - surface.aperture.center) < surface.aperture.a; }, true);
				} else {
					seg.reading = reading(hit.optic, point, s, w2, power);
				}
				seg.endClipped = 1.0 - fraction;
				seg.spent.clipped = power * (1.0 - fraction);
				next.rays = arriving;
				next.power = power * fraction;
				next.clippedUpstream = 1.0 - (1.0 - s.clippedUpstream) * fraction;
				next.fromOptic = e.optic.id;
				next.passage = seg.key;
				next.port = 0;
				next.slot = 1;
				if (next.power < threshold) {
					seg.dropped = true;
					seg.spent.belowThreshold = next.power;
				} else {
					stack.push_back({ std::move(next), seg.key });
				}
				out.push_back(std::move(traced));
				continue;
			}

			const OpticSurface& surface = body.surfaces[hit.surface];
			const glm::dvec3 normal = bodyToWorldVector(hit.optic, toBodyVector(surface, surfaceNormalLocal(surface, hit.local)));
			seg.endNormal = normal;
			const MaterialDef* material = surface.materialSlot < e.slots.size() && e.slots[surface.materialSlot] ? &*e.slots[surface.materialSlot] : nullptr;
			const MaterialType type = material != nullptr ? material->type : MaterialType::OpticalInterface;
			bool ground = false;
			if (type == MaterialType::OpticalInterface && material != nullptr) {
				const ParamValue finish = materialValue(*material, "finish");
				const int* finishIndex = std::get_if<int>(&finish);
				ground = finishIndex != nullptr && *finishIndex == 1;
			}
			if (type == MaterialType::Detector || (isDetectorKind(e.optic.kind) && type != MaterialType::OpticalInterface)) {
				seg.end = BeamEnd::Detector;
				seg.reading = reading(hit.optic, point, s, w2, power);
				seg.spent.detected = power;
				out.push_back(std::move(traced));
				continue;
			}
			if (type == MaterialType::LaserAperture) {
				seg.end = BeamEnd::Laser;
				seg.spent.backReflected = power;
				out.push_back(std::move(traced));
				continue;
			}
			if (type != MaterialType::OpticalInterface) {
				seg.end = BeamEnd::Absorbed;
				seg.spent.absorbed = power;
				// an iris's blades: the chief ray ends there, and the share of the beam they take is what its visit reports
				if (e.optic.kind == OpticKind::Iris && surface.aperture.kind == ApertureKind::Annulus) {
					const LocalMap map = localMap(hit.optic, hit.surface, point, normal, s);
					const double through = fractionInside(w2, [&](const glm::dvec2& x) { return glm::length(map(x) - surface.aperture.center) < surface.aperture.a; }, true);
					seg.endClipped = 1.0 - through;
				}
				out.push_back(std::move(traced));
				continue;
			}
			if (ground) {
				seg.end = BeamEnd::Scattered;
				seg.spent.scattered = power;
				out.push_back(std::move(traced));
				continue;
			}
			seg.end = BeamEnd::Surface;

			// the clear aperture's share of the beam, on the surface's plane at the hit's obliquity
			double fraction = 1.0;
			if (surface.aperture.kind != ApertureKind::None) {
				const LocalMap map = localMap(hit.optic, hit.surface, point, normal, s);
				fraction = fractionInside(w2, [&](const glm::dvec2& x) { return insideAperture(surface.aperture, map(x)); }, surface.aperture.kind != ApertureKind::Annulus);
			}
			seg.endClipped = 1.0 - fraction;
			seg.spent.clipped = power * (1.0 - fraction);
			const double arrived = power * fraction;

			// the coating's split, unpolarized
			const bool fromPlus = glm::dot(s.direction, normal) < 0.0;
			const std::vector<double>& n = indices[hit.optic];
			const uint8_t mediumIn = fromPlus ? surface.mediumPlus : surface.mediumMinus;
			const uint8_t mediumOut = fromPlus ? surface.mediumMinus : surface.mediumPlus;
			const double nIn = n[mediumIn];
			const double nOut = n[mediumOut];
			const OpticCoating* coating = nullptr;
			if (input.build.library != nullptr && material != nullptr) {
				coating = input.build.library->findCoating(materialCoating(*material, "coating"));
			}
			static const OpticCoating uncoated;
			const double cosine = std::min(std::abs(glm::dot(s.direction, normal)), 1.0);
			CoatingResponse response;
			if (input.build.library != nullptr) {
				const IndexLookup lookup = input.build.library->indexLookup();
				response = evaluateCoating(coating != nullptr ? *coating : uncoated, lookup, lambdaNm, std::acos(cosine),
					input.build.library->complexIndex(glassOf(body, surface.mediumPlus), lambdaNm), input.build.library->complexIndex(glassOf(body, surface.mediumMinus), lambdaNm),
					fromPlus);
			} else {
				response.Rs = response.Rp = 0.04;
				response.Ts = response.Tp = 0.96;
			}
			glm::dvec3 refracted;
			const bool canRefract = refractDirection(s.direction, normal, nIn / nOut, refracted);
			const double reflectance = std::clamp(response.reflectance(), 0.0, 1.0);
			const double transmittance = canRefract ? std::clamp(response.transmittance(), 0.0, 1.0 - reflectance) : 0.0;
			seg.spent.absorbed = arrived * (1.0 - reflectance - transmittance);

			// the curvature at the hit, in world space
			const PrincipalCurvaturesT<double> curvature = principalCurvatures(surface, hit.local);
			SurfaceGeometry geometry;
			geometry.normal = normal;
			geometry.k1 = curvature.k1;
			geometry.k2 = curvature.k2;
			geometry.dir1 = bodyToWorldVector(hit.optic, toBodyVector(surface, curvature.d1));
			geometry.dir2 = bodyToWorldVector(hit.optic, toBodyVector(surface, curvature.d2));

			const auto branch = [&](bool reflect, double branchPower) -> std::optional<BeamBranchState> {
				if (branchPower <= 0.0) {
					return std::nullopt;
				}
				if (branchPower < threshold) {
					seg.dropped = true;
					seg.spent.belowThreshold += branchPower;
					return std::nullopt;
				}
				BeamBranchState b = next;
				glm::dvec3 d;
				if (reflect) {
					reflectDirection(s.direction, normal, d);
				} else {
					d = refracted;
				}
				b.direction = glm::normalize(d);
				transportFrame(s.direction, b.direction, normal, reflect, s.e1, s.e2, b.e1, b.e2);
				const double nAfter = reflect ? nIn : nOut;
				b.rays = surfaceTransfer(geometry, s.direction, s.e1, s.e2, nIn, b.direction, b.e1, b.e2, nAfter, reflect) * arriving;
				b.medium = reflect ? mediumIn : mediumOut;
				b.index = nAfter;
				b.power = branchPower;
				b.clippedUpstream = 1.0 - (1.0 - s.clippedUpstream) * fraction;
				b.slot = reflect ? 0 : 1;
				const PortEvent event { hit.surface, reflect };
				if (s.inside == 0) {
					b.events = { event };
					b.entry = seg.key;
				} else {
					b.events.push_back(event);
				}
				if (b.medium == 0) {
					// out of the body: the passage is complete, and its events name the port
					b.inside = 0;
					b.port = matchOpticPort(e.optic.kind, b.events);
					b.fromOptic = e.optic.id;
					b.passage = b.entry;
					b.ghost = s.ghost || b.port == NO_PORT;
					b.events.clear();
					b.entry = 0;
				} else {
					// still inside: a ghost already once its events can no longer become a port's, as a lens's internal
					// reflection cannot
					b.inside = e.optic.id;
					b.ghost = s.ghost || !opticPortPrefix(e.optic.kind, b.events);
				}
				return b;
			};
			std::optional<BeamBranchState> reflectedBranch = branch(true, arrived * reflectance);
			std::optional<BeamBranchState> refractedBranch = canRefract ? branch(false, arrived * transmittance) : std::nullopt;
			// depth first, reflected first: the refracted branch waits under it
			if (refractedBranch) {
				stack.push_back({ std::move(*refractedBranch), seg.key });
			}
			if (reflectedBranch) {
				stack.push_back({ std::move(*reflectedBranch), seg.key });
			}
			out.push_back(std::move(traced));
		}
	}
};

//---------------------------------------------------------------- solving

namespace {

// the tree's segments from the traced ones, in the order they came: parents and passages resolved from keys, the
// budget, the readings and the warnings summed in that order, so a selective solve sums exactly as a full one
void finishTree(BeamTree& tree, std::vector<TracedSegment>& traced, const BeamSolver& solver)
{
	std::map<uint64_t, uint32_t> indexOf;
	for (uint32_t i = 0; i < traced.size(); i++) {
		indexOf[traced[i].segment.key] = i;
	}
	tree.segments.clear();
	tree.segments.reserve(traced.size());
	tree.budget = {};
	tree.readings.clear();
	tree.warnings.clear();
	tree.truncated = false;
	for (uint32_t i = 0; i < traced.size(); i++) {
		BeamSegment seg = traced[i].segment;
		const auto parent = indexOf.find(traced[i].parentKey);
		seg.parent = parent != indexOf.end() ? parent->second : NO_BEAM_SEGMENT;
		const auto passage = indexOf.find(seg.start.passage);
		seg.passage = seg.start.passage != 0 && passage != indexOf.end() ? passage->second : NO_BEAM_SEGMENT;
		tree.budget += seg.spent;
		tree.truncated |= seg.end == BeamEnd::Truncated;
		if (seg.reading) {
			seg.reading->segment = i;
			tree.readings.push_back(*seg.reading);
		}
		if (seg.end == BeamEnd::Laser) {
			BeamWarning w;
			w.kind = BeamWarning::BackReflection;
			w.segment = i;
			w.optic = seg.endOptic;
			w.power = seg.endPower;
			if (const SceneOptic* laser = solver.optic(seg.endOptic)) {
				const glm::dvec3 axis = glm::normalize(glm::dvec3(laser->world * glm::dvec4(laserBeam(*laser).axis, 0.0)));
				w.value = std::acos(std::clamp(glm::dot(-seg.start.direction, axis), -1.0, 1.0));
			}
			tree.warnings.push_back(w);
		}
		tree.segments.push_back(std::move(seg));
	}
	// ghosts that focus on or inside an optic
	for (uint32_t i = 0; i < tree.segments.size(); i++) {
		const BeamSegment& seg = tree.segments[i];
		if (!seg.start.ghost || seg.end == BeamEnd::Escaped || seg.end == BeamEnd::Truncated || tree.chiefRayOnly) {
			continue;
		}
		const BeamProfile profile = tree.profile(i);
		for (const BeamAxisProfile& axis : profile.axes) {
			if (axis.waistAt < 0.0 || axis.waistAt > seg.length) {
				continue;
			}
			if (seg.start.inside == 0 && std::abs(seg.length - axis.waistAt) > axis.rayleighRange) {
				continue;
			}
			const BeamCrossSection at = tree.crossSection(i, axis.waistAt);
			BeamWarning w;
			w.kind = BeamWarning::GhostFocus;
			w.segment = i;
			w.optic = seg.start.inside != 0 ? seg.start.inside : seg.endOptic;
			w.power = seg.start.power;
			w.value = 2.0 * seg.start.power / (std::numbers::pi * std::max(at.radii.x * at.radii.y, 1e-30));
			tree.warnings.push_back(w);
			break;
		}
	}
}

// where a line meets an optic's main face plane, for a miss: the distance from its aperture's edge and the way back
struct MissGeometry {
	double distance { 0.0 };
	glm::dvec3 point { 0.0 };
	glm::dvec3 steer { 0.0 };
};

MissGeometry missGeometry(const SceneOptic& optic, const OpticPrescription& body, const glm::dvec3& origin, const glm::dvec3& direction)
{
	MissGeometry miss;
	const glm::dmat4 toBody = glm::inverse(optic.world);
	const glm::dvec3 o = glm::dvec3(toBody * glm::dvec4(origin, 1.0));
	const glm::dvec3 d = glm::dvec3(toBody * glm::dvec4(direction, 0.0));
	// the face: an iris's opening, a probe plane's sensor, else the first polished face
	OpticSurface face;
	bool opening = false;
	if (optic.kind == OpticKind::ProbePlane) {
		const DetectorGeometry g = detectorGeometry(optic);
		face.aperture.kind = g.shape == SensorShape::Disk ? ApertureKind::Circle : ApertureKind::Rectangle;
		face.aperture.a = g.halfWidth;
		face.aperture.b = g.halfHeight;
		finishSurface(face);
	} else if (std::optional<uint32_t> s = optic.kind == OpticKind::Iris ? irisSurface(body) : std::nullopt) {
		face = body.surfaces[*s];
		opening = true;
	} else if (!body.surfaces.empty()) {
		face = body.surfaces.front();
		for (const OpticSurface& surface : body.surfaces) {
			if ((surface.flags & OPTIC_SURFACE_OPTICAL) != 0) {
				face = surface;
				break;
			}
		}
	}
	const glm::dvec3 ol = toSurfacePoint(face, o);
	const glm::dvec3 dl = toSurfaceVector(face, d);
	// the plane's crossing, or where the line passes closest to the face's center when it runs along the plane
	const double t = std::abs(dl.z) > 1e-9 ? -ol.z / dl.z : glm::dot(-ol, dl) / std::max(glm::dot(dl, dl), 1e-300);
	const glm::dvec3 p = ol + t * dl;
	const ApertureT<double>& ap = face.aperture;
	const glm::dvec2 offset = glm::dvec2(p) - ap.center;
	switch (ap.kind) {
	case ApertureKind::Circle:
		miss.distance = glm::length(offset) - ap.a;
		break;
	case ApertureKind::Annulus:
		miss.distance = glm::length(offset) - (opening ? ap.a : ap.b);
		break;
	case ApertureKind::Rectangle:
		miss.distance = glm::length(glm::max(glm::abs(offset) - glm::dvec2(ap.a, ap.b), glm::dvec2(0.0)));
		break;
	case ApertureKind::Ellipse:
		miss.distance = (glm::length(offset / glm::dvec2(ap.a, ap.b)) - 1.0) * std::min(ap.a, ap.b);
		break;
	case ApertureKind::Polygon: {
		double radius = 0.0;
		for (int i = 0; i < (int)ap.a; i++) {
			radius = std::max(radius, glm::length(ap.polygon[i]));
		}
		miss.distance = glm::length(offset) - radius;
		break;
	}
	case ApertureKind::None:
		miss.distance = glm::length(offset) - face.radius;
		break;
	}
	const glm::dvec3 worldPoint = glm::dvec3(optic.world * glm::dvec4(toBodyPoint(face, p), 1.0));
	const glm::dvec3 worldCenter = glm::dvec3(optic.world * glm::dvec4(toBodyPoint(face, glm::dvec3(ap.center, 0.0)), 1.0));
	glm::dvec3 steer = worldCenter - worldPoint;
	steer -= direction * glm::dot(direction, steer);
	miss.point = worldPoint;
	miss.steer = glm::length(steer) > 0.0 ? glm::normalize(steer) : glm::dvec3(0.0);
	return miss;
}

} // namespace

void BeamSolver::reset()
{
	m_solution = {};
	m_optics.clear();
	m_opticIndex.clear();
	m_solved = false;
	m_revision++;
}

const OpticPrescription* BeamSolver::body(uint64_t optic) const
{
	const auto found = m_opticIndex.find(optic);
	return found != m_opticIndex.end() ? m_optics[found->second].body.get() : nullptr;
}

const SceneOptic* BeamSolver::optic(uint64_t optic) const
{
	const auto found = m_opticIndex.find(optic);
	return found != m_opticIndex.end() ? &m_optics[found->second].optic : nullptr;
}

bool BeamSolver::solve(const BeamSolverInput& input)
{
	const auto started = std::chrono::steady_clock::now();
	m_lastTraced = 0;

	//---- the optics as they are now, and which changed
	const std::string contextKey = fmt::format("{}|{}|{}|{}", (const void*)input.build.library, input.libraryRevision, input.build.surrounding, input.build.designWavelength);
	const bool global = !m_solved || contextKey != m_contextKey;
	if (contextKey != m_contextKey) {
		m_bodies.clear();
	}
	std::vector<OpticEntry> optics;
	optics.reserve(input.optics.size());
	std::map<std::string, std::shared_ptr<const OpticPrescription>> bodies;
	for (const SceneOptic& optic : input.optics) {
		OpticEntry e;
		e.optic = optic;
		const OpticTraits& traits = opticTraits(optic.kind);
		for (size_t s = 0; s < traits.slots.size() && s < MAX_OPTIC_MATERIAL_SLOTS; s++) {
			if (const MaterialDef* m = input.material ? input.material(optic.materials[s]) : nullptr) {
				e.slots[s] = *m;
			}
		}
		const std::string key = opticBodyKey(optic);
		auto found = m_bodies.find(key);
		if (found == m_bodies.end()) {
			OpticReadout readout;
			found = m_bodies.emplace(key, std::make_shared<const OpticPrescription>(buildOptic(optic, input.build, readout))).first;
		}
		bodies[key] = found->second;
		e.body = found->second;
		e.worldToBody = glm::inverse(optic.world);
		// the world box of the body's box, or of a probe plane's sensor
		OpticBoundsT<double> local = e.body->bounds;
		if (optic.kind == OpticKind::ProbePlane || local.empty()) {
			const DetectorGeometry g = isDetectorKind(optic.kind) ? detectorGeometry(optic) : DetectorGeometry {};
			local = {};
			local.grow(glm::dvec3(-g.halfWidth, -g.halfHeight, -1e-6));
			local.grow(glm::dvec3(g.halfWidth, g.halfHeight, 1e-6));
		}
		e.boundsMin = glm::dvec3(INFINITY);
		e.boundsMax = glm::dvec3(-INFINITY);
		for (int c = 0; c < 8; c++) {
			const glm::dvec3 corner((c & 1) ? local.max.x : local.min.x, (c & 2) ? local.max.y : local.min.y, (c & 4) ? local.max.z : local.min.z);
			const glm::dvec3 w = glm::dvec3(optic.world * glm::dvec4(corner, 1.0));
			e.boundsMin = glm::min(e.boundsMin, w);
			e.boundsMax = glm::max(e.boundsMax, w);
		}
		// a little slack, for the float of a sensor's zero thickness
		e.boundsMin -= glm::dvec3(1e-6);
		e.boundsMax += glm::dvec3(1e-6);
		optics.push_back(std::move(e));
	}
	m_bodies = std::move(bodies);

	// changed: added, removed or edited, with their old and new boxes
	std::set<uint64_t> changed;
	std::vector<std::pair<glm::dvec3, glm::dvec3>> changedBoxes;
	std::map<uint64_t, size_t> newIndex;
	for (size_t i = 0; i < optics.size(); i++) {
		newIndex[optics[i].optic.id] = i;
	}
	for (size_t i = 0; i < optics.size(); i++) {
		const auto old = m_opticIndex.find(optics[i].optic.id);
		if (old == m_opticIndex.end() || !(m_optics[old->second] == optics[i])) {
			changed.insert(optics[i].optic.id);
			changedBoxes.emplace_back(optics[i].boundsMin, optics[i].boundsMax);
			if (old != m_opticIndex.end()) {
				changedBoxes.emplace_back(m_optics[old->second].boundsMin, m_optics[old->second].boundsMax);
			}
		}
	}
	for (const auto& [id, index] : m_opticIndex) {
		if (!newIndex.contains(id)) {
			changed.insert(id);
			changedBoxes.emplace_back(m_optics[index].boundsMin, m_optics[index].boundsMax);
		}
	}
	m_optics = std::move(optics);
	m_opticIndex = std::move(newIndex);
	m_contextKey = contextKey;

	//---- the sources: every laser, then each camera an axis starts from
	struct Source {
		BeamSourceKind kind;
		uint64_t id;
		double threshold;
	};
	std::vector<Source> sources;
	const auto thresholdOf = [&](BeamSourceKind kind, uint64_t id) {
		double threshold = INFINITY;
		for (const BeamAxis& axis : input.axes) {
			if (axis.source.kind == kind && axis.source.id == id) {
				threshold = std::min(threshold, (double)axis.ghostThreshold.value_or(input.ghostThreshold));
			}
		}
		return std::isfinite(threshold) ? threshold : (double)input.ghostThreshold;
	};
	for (const OpticEntry& e : m_optics) {
		if (e.optic.kind == OpticKind::Laser) {
			sources.push_back({ BeamSourceKind::Laser, e.optic.id, thresholdOf(BeamSourceKind::Laser, e.optic.id) });
		}
	}
	for (const BeamAxis& axis : input.axes) {
		if (axis.source.kind != BeamSourceKind::Camera) {
			continue;
		}
		const bool known = std::any_of(input.cameras.begin(), input.cameras.end(), [&](const BeamCameraSource& c) { return c.id == axis.source.id; });
		const bool listed = std::any_of(sources.begin(), sources.end(), [&](const Source& s) { return s.kind == BeamSourceKind::Camera && s.id == axis.source.id; });
		if (known && !listed) {
			sources.push_back({ BeamSourceKind::Camera, axis.source.id, thresholdOf(BeamSourceKind::Camera, axis.source.id) });
		}
	}

	//---- each source's tree: kept, re-traced in part, or traced anew
	bool treesChanged = m_solution.trees.size() != sources.size();
	std::vector<BeamTree> trees;
	for (const Source& source : sources) {
		const BeamTree* old = nullptr;
		for (const BeamTree& t : m_solution.trees) {
			if (t.sourceKind == source.kind && t.source == source.id) {
				old = &t;
			}
		}
		BeamTree tree;
		tree.sourceKind = source.kind;
		tree.source = source.id;
		tree.threshold = source.threshold;
		BeamBranchState root;
		const BeamCameraSource* camera = nullptr;
		bool sourceChanged = false;
		if (source.kind == BeamSourceKind::Laser) {
			const OpticEntry& laser = m_optics[m_opticIndex.at(source.id)];
			const LaserBeam beam = laserBeam(laser.optic);
			tree.wavelength = beam.wavelength;
			tree.power = beam.power;
			tree.m2 = glm::dvec2(beam.x.m2, beam.y.m2);
			sourceChanged = changed.contains(source.id);
		} else {
			for (const BeamCameraSource& c : input.cameras) {
				if (c.id == source.id) {
					camera = &c;
				}
			}
			tree.wavelength = 550e-9;
			tree.power = 1.0;
			tree.chiefRayOnly = true;
			const auto previous = std::find_if(m_cameras.begin(), m_cameras.end(), [&](const BeamCameraSource& c) { return c.id == source.id; });
			sourceChanged = previous == m_cameras.end() || !(*previous == *camera);
		}

		std::vector<TracedSegment> traced;
		BeamTracer tracer { *this, input, tree, m_nextKey, traced };
		tracer.prepareMedia();
		const double n0 = tracer.surroundingIndex();
		if (source.kind == BeamSourceKind::Laser) {
			const SceneOptic& laser = m_optics[m_opticIndex.at(source.id)].optic;
			const LaserBeam beam = laserBeam(laser);
			root.origin = glm::dvec3(laser.world * glm::dvec4(beam.center, 1.0));
			root.direction = glm::normalize(glm::dvec3(laser.world * glm::dvec4(beam.axis, 0.0)));
			root.e1 = perpendicularTo(root.direction, glm::dvec3(laser.world * glm::dvec4(beam.xAxis, 0.0)));
			// the embedded beam's M^2, scaled so the readouts give the laser's own w0 in the surrounding medium
			const auto axisOf = [&](const LaserAxisBeam& a, double& zR, double& m2) {
				zR = std::min(a.rayleighRange(), 1e12);
				m2 = std::numbers::pi * n0 * a.waist * a.waist / (beam.wavelength * zR);
			};
			glm::dvec2 zR, m2;
			axisOf(beam.x, zR.x, m2.x);
			axisOf(beam.y, zR.y, m2.y);
			root.rays = simpleAstigmaticBeam(glm::dvec2(beam.x.waistPosition, beam.y.waistPosition), zR, n0, m2);
		} else {
			root.origin = camera->origin;
			root.direction = glm::normalize(camera->direction);
			root.e1 = perpendicularTo(root.direction, glm::cross(camera->up, -root.direction));
			// a notional collimated beam of 1 mm
			const double w0 = 1e-3;
			const double zR = std::numbers::pi * w0 * w0 / tree.wavelength;
			root.rays = simpleAstigmaticBeam(glm::dvec2(0.0), glm::dvec2(zR), n0, glm::dvec2(1.0));
		}
		root.e2 = glm::cross(root.direction, root.e1);
		root.power = tree.power;
		root.index = n0;
		root.slot = 0;

		const bool full = global || old == nullptr || sourceChanged || old->threshold != tree.threshold || old->segments.empty() || old->segments.front().start != root;
		if (full) {
			tracer.trace(root, 0);
			m_lastTraced += traced.size();
			treesChanged = true;
		} else {
			// the old tree, its segments valid where nothing changed near them, re-traced from the first invalid one of
			// each branch. A subtree is contiguous in depth-first order
			const std::vector<BeamSegment>& segs = old->segments;
			const auto touched = [&](const BeamSegment& s) {
				if (changed.contains(s.start.inside) || changed.contains(s.endOptic)) {
					return true;
				}
				if (s.end == BeamEnd::Escaped) {
					return !changed.empty();
				}
				const BeamCrossSection a = old->crossSection((uint32_t)(&s - segs.data()), 0.0);
				const BeamCrossSection b = old->crossSection((uint32_t)(&s - segs.data()), s.length);
				const double r = 3.0 * std::max({ a.radii.x, a.radii.y, b.radii.x, b.radii.y });
				const glm::dvec3 lo = glm::min(s.start.origin, s.endPoint()) - glm::dvec3(r);
				const glm::dvec3 hi = glm::max(s.start.origin, s.endPoint()) + glm::dvec3(r);
				for (const auto& [bmin, bmax] : changedBoxes) {
					if (glm::all(glm::lessThanEqual(lo, bmax)) && glm::all(glm::lessThanEqual(bmin, hi))) {
						return true;
					}
				}
				return false;
			};
			// the subtree's end of each segment
			std::vector<uint32_t> subtreeEnd(segs.size());
			for (size_t i = segs.size(); i-- > 0;) {
				subtreeEnd[i] = (uint32_t)i + 1;
			}
			for (size_t i = segs.size(); i-- > 0;) {
				if (segs[i].parent != NO_BEAM_SEGMENT) {
					subtreeEnd[segs[i].parent] = std::max(subtreeEnd[segs[i].parent], subtreeEnd[i]);
				}
			}
			tracer.treeSize = 0;
			bool any = false;
			for (uint32_t i = 0; i < segs.size();) {
				if (!touched(segs[i])) {
					TracedSegment keep;
					keep.segment = segs[i];
					keep.parentKey = segs[i].parent != NO_BEAM_SEGMENT ? segs[segs[i].parent].key : 0;
					traced.push_back(std::move(keep));
					tracer.treeSize++;
					i++;
					continue;
				}
				any = true;
				const size_t before = traced.size();
				tracer.trace(segs[i].start, segs[i].parent != NO_BEAM_SEGMENT ? segs[segs[i].parent].key : 0);
				m_lastTraced += traced.size() - before;
				i = subtreeEnd[i];
			}
			if (!any) {
				trees.push_back(*old);
				trees.back().threshold = tree.threshold;
				continue;
			}
			treesChanged = true;
		}
		finishTree(tree, traced, *this);
		trees.push_back(std::move(tree));
	}
	m_cameras = input.cameras;

	//---- the axes' statuses
	BeamSolution solution;
	solution.trees = std::move(trees);
	for (const BeamAxis& axis : input.axes) {
		std::vector<BeamNodeReport> reports;
		const std::vector<uint32_t> order = beamAxisOrder(axis);
		const std::vector<uint32_t> dangling = beamDanglingOrder(axis);
		const BeamTree* tree = nullptr;
		for (uint32_t t = 0; t < solution.trees.size(); t++) {
			if (axis.source.kind != BeamSourceKind::None && solution.trees[t].sourceKind == axis.source.kind && solution.trees[t].source == axis.source.id) {
				tree = &solution.trees[t];
				solution.axisTrees[axis.id] = t;
			}
		}
		std::map<uint32_t, uint32_t> portBeam;
		std::map<uint32_t, BeamNodeStatus> preset;
		const auto target = [this](uint64_t id) { return optic(id); };
		std::map<uint32_t, std::vector<uint32_t>> exits;
		if (tree != nullptr && !tree->segments.empty()) {
			for (uint32_t i = 0; i < tree->segments.size(); i++) {
				if (tree->segments[i].passage != NO_BEAM_SEGMENT) {
					exits[tree->segments[i].passage].push_back(i);
				}
			}
			if (const BeamAxisNode* first = beamNodeAt(axis, BEAM_SOURCE_NODE, NO_PORT)) {
				portBeam[first->id] = 0;
			}
		}
		for (uint32_t id : order) {
			const BeamAxisNode& node = *findBeamNode(axis, id);
			BeamNodeReport r;
			r.node = id;
			const auto beam = portBeam.find(id);
			if (tree == nullptr || beam == portBeam.end()) {
				const auto set = preset.find(id);
				r.status = set != preset.end() ? set->second : BeamNodeStatus::Unreached;
				reports.push_back(r);
				continue;
			}
			const BeamSegment& s = tree->segments[beam->second];
			r.segment = beam->second;
			r.from = s.start.origin;
			r.to = s.endPoint();
			r.power = s.endPower;
			if (s.start.power < axis.ghostThreshold.value_or(input.ghostThreshold) * tree->power) {
				r.status = BeamNodeStatus::BelowThreshold;
				reports.push_back(r);
				continue;
			}
			const bool reached = s.endOptic == node.optic && s.end != BeamEnd::Escaped && s.end != BeamEnd::Truncated;
			if (reached) {
				r.lostFraction = s.endClipped;
				// the beam on an iris's blades has ended there, whatever share they take
				const bool onBlades = s.end == BeamEnd::Absorbed && target(node.optic) != nullptr && target(node.optic)->kind == OpticKind::Iris;
				r.status = s.endClipped >= BEAM_CLIP_REPORT || onBlades ? BeamNodeStatus::Clipped : BeamNodeStatus::Hit;
				// the children leave by their ports' beams
				bool dropped = s.dropped;
				for (const BeamSegment& inner : tree->segments) {
					dropped |= inner.start.entry == s.key && inner.dropped;
				}
				const auto out = exits.find(beam->second);
				for (uint32_t child : beamNodeChildren(axis, id)) {
					const BeamAxisNode& c = *findBeamNode(axis, child);
					bool found = false;
					if (out != exits.end()) {
						for (uint32_t x : out->second) {
							if (tree->segments[x].start.port == c.port && tree->segments[x].start.fromOptic == node.optic) {
								portBeam[child] = x;
								found = true;
								break;
							}
						}
					}
					if (!found) {
						preset[child] = dropped ? BeamNodeStatus::BelowThreshold : BeamNodeStatus::Unreached;
					}
				}
				reports.push_back(r);
				continue;
			}
			// not reached: another optic first, or past it
			const SceneOptic* target = optic(node.optic);
			const OpticPrescription* targetBody = body(node.optic);
			if (target == nullptr || targetBody == nullptr) {
				r.status = BeamNodeStatus::Unreached;
				reports.push_back(r);
				continue;
			}
			std::vector<TracedSegment> none;
			BeamTree scratch;
			uint64_t key = 0;
			BeamTracer probe { *this, input, scratch, key, none };
			const BeamTracer::Hit h = probe.hitOptic(m_opticIndex.at(node.optic), s.start.origin, s.start.direction, s.start.inside == 0, INFINITY);
			if (h.optic != SIZE_MAX && s.end != BeamEnd::Escaped) {
				r.status = BeamNodeStatus::Blocked;
				const SceneOptic* blocker = optic(s.endOptic);
				r.blocker = blocker != nullptr ? blocker->name : std::string("?");
			} else {
				r.status = BeamNodeStatus::Missed;
				const MissGeometry miss = missGeometry(*target, *targetBody, s.start.origin, s.start.direction);
				r.missDistance = miss.distance;
				r.steer = miss.steer;
				r.to = miss.point;
			}
			reports.push_back(r);
		}
		for (uint32_t id : dangling) {
			BeamNodeReport r;
			r.node = id;
			reports.push_back(r);
		}
		solution.reports[axis.id] = std::move(reports);
	}

	const bool reportsChanged = solution.reports != m_solution.reports || solution.axisTrees != m_solution.axisTrees;
	m_solution = std::move(solution);
	m_axes.assign(input.axes.begin(), input.axes.end());
	m_solved = true;
	if (treesChanged || reportsChanged) {
		m_revision++;
	}
	m_lastSolveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	return treesChanged || reportsChanged;
}

//---------------------------------------------------------------- following the beam

std::vector<BeamFollowStep> followBeam(const BeamSolution& solution, const BeamAxis& axis, uint32_t node, std::span<const SceneOptic> optics, size_t limit)
{
	std::vector<BeamFollowStep> steps;
	const BeamTree* tree = solution.treeOf(axis.id);
	if (tree == nullptr || tree->segments.empty()) {
		return steps;
	}
	const auto visitable = [&](uint64_t id) {
		const auto found = std::find_if(optics.begin(), optics.end(), [&](const SceneOptic& o) { return o.id == id; });
		return found != optics.end() && found->stack == 0 && found->kind != OpticKind::Laser;
	};
	const auto reachedOptic = [&](const BeamSegment& s) -> uint64_t {
		const bool ends = s.end == BeamEnd::Surface || s.end == BeamEnd::Pass || s.end == BeamEnd::Detector || s.end == BeamEnd::Absorbed || s.end == BeamEnd::Scattered;
		return ends && visitable(s.endOptic) ? s.endOptic : 0;
	};
	std::map<uint32_t, std::vector<uint32_t>> exits;
	for (uint32_t i = 0; i < tree->segments.size(); i++) {
		if (tree->segments[i].passage != NO_BEAM_SEGMENT) {
			exits[tree->segments[i].passage].push_back(i);
		}
	}
	// (the parent as a step names it, the segment its visit was reached by, the ports already taken)
	struct Open {
		uint32_t parent;
		uint32_t reachedBy;
		std::vector<OpticPortId> taken;
	};
	std::vector<Open> open;
	if (node == BEAM_SOURCE_NODE) {
		if (beamNodeAt(axis, BEAM_SOURCE_NODE, NO_PORT) == nullptr) {
			if (const uint64_t first = reachedOptic(tree->segments[0])) {
				steps.push_back({ BEAM_SOURCE_NODE, NO_PORT, first });
				open.push_back({ BEAM_FOLLOW_STEP, 0, {} });
			}
		}
	} else {
		const BeamNodeReport* report = solution.report(axis.id, node);
		if (report != nullptr && (report->status == BeamNodeStatus::Hit || report->status == BeamNodeStatus::Clipped)) {
			Open start { node, report->segment, {} };
			for (uint32_t child : beamNodeChildren(axis, node)) {
				start.taken.push_back(findBeamNode(axis, child)->port);
			}
			open.push_back(std::move(start));
		}
	}
	while (!open.empty() && steps.size() < limit) {
		const Open current = std::move(open.front());
		open.erase(open.begin());
		const auto out = exits.find(current.reachedBy);
		if (out == exits.end()) {
			continue;
		}
		for (uint32_t x : out->second) {
			const BeamSegment& s = tree->segments[x];
			if (s.start.port == NO_PORT || std::find(current.taken.begin(), current.taken.end(), s.start.port) != current.taken.end()) {
				continue;
			}
			const uint64_t next = reachedOptic(s);
			if (next == 0 || steps.size() >= limit) {
				continue;
			}
			steps.push_back({ current.parent, s.start.port, next });
			open.push_back({ BEAM_FOLLOW_STEP | (uint32_t)(steps.size() - 1), x, {} });
		}
	}
	return steps;
}
