#include <beam_preview.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>

#include <beam_solver.h>

namespace {

// one axis's three-section profile: its breaks and the radius at each, linear between
struct AxisSections {
	std::vector<double> s;
	std::vector<double> radius;
	double at(double x) const
	{
		if (x <= s.front()) {
			return radius.front();
		}
		for (size_t i = 1; i < s.size(); i++) {
			if (x <= s[i]) {
				const double span = s[i] - s[i - 1];
				const double t = span > 0.0 ? (x - s[i - 1]) / span : 1.0;
				return radius[i - 1] + t * (radius[i] - radius[i - 1]);
			}
		}
		return radius.back();
	}
};

AxisSections axisSections(const BeamAxisProfile& axis, double length, BeamWaistRegion region)
{
	AxisSections out;
	const double half = region == BeamWaistRegion::Confocal ? axis.rayleighRange : 0.5 * axis.rayleighRange;
	const double lo = std::max(0.0, axis.waistAt - half);
	const double hi = std::min(length, axis.waistAt + half);
	out.s.push_back(0.0);
	out.radius.push_back(axis.radius(0.0));
	if (lo < hi) {
		// the cylinder at w0, and the cones to it where they have a length
		if (lo > 0.0) {
			out.s.push_back(lo);
			out.radius.push_back(axis.waist);
		} else {
			out.radius.back() = axis.waist;
		}
		if (hi < length) {
			out.s.push_back(hi);
			out.radius.push_back(axis.waist);
		} else {
			out.s.push_back(length);
			out.radius.push_back(axis.waist);
			return out;
		}
	}
	out.s.push_back(length);
	out.radius.push_back(axis.radius(length));
	return out;
}

} // namespace

std::vector<BeamRing> beamRings(const ComplexRays& rays, double index, double lambda0, double length, BeamWaistRegion region)
{
	const BeamProfile profile = beamProfile(rays, index, lambda0);
	const AxisSections first = axisSections(profile.axes[0], length, region);
	const AxisSections second = axisSections(profile.axes[1], length, region);
	std::vector<double> breaks = first.s;
	breaks.insert(breaks.end(), second.s.begin(), second.s.end());
	std::sort(breaks.begin(), breaks.end());
	const double tolerance = 1e-12 * std::max(length, 1e-9);
	breaks.erase(std::unique(breaks.begin(), breaks.end(), [&](double a, double b) { return b - a <= tolerance; }), breaks.end());
	std::vector<BeamRing> rings;
	for (double s : breaks) {
		BeamRing ring;
		ring.s = s;
		ring.radii = glm::dvec2(first.at(s), second.at(s));
		// a twisted beam's axes turn along the segment
		ring.angle = profile.twisted ? crossSection(rays, s, index, lambda0).angle : profile.angle;
		rings.push_back(ring);
	}
	return rings;
}

std::vector<BeamSection> buildBeamSections(const BeamSolution& solution, const BeamPreviewSettings& settings, const SpectralDisplay& display)
{
	std::vector<BeamSection> out;
	if (settings.display == BeamDisplay::Off) {
		return out;
	}
	// after every tube, so a segment's tubes stay consecutive
	std::vector<BeamSection> disks;
	for (const BeamTree& tree : solution.trees) {
		if (tree.chiefRayOnly || tree.power <= 0.0) {
			continue;
		}
		const glm::vec3 base = wavelengthDisplayColor((float)(tree.wavelength * 1e9), display);
		// the parents of the segments a cap cut off are dashed
		std::vector<uint8_t> dashed(tree.segments.size(), 0);
		for (const BeamSegment& s : tree.segments) {
			if (s.end == BeamEnd::Truncated && s.parent != NO_BEAM_SEGMENT) {
				dashed[s.parent] = 1;
			}
		}
		const auto opacityOf = [&](const BeamSegment& s) {
			const double relative = s.start.power / tree.power;
			return (float)std::clamp(1.0 + std::log10(std::max(relative, 1e-300)) / settings.brightnessDecades, 0.0, 1.0);
		};
		const auto drawn = [&](const BeamSegment& s) { return s.length > 0.0 && (!s.start.ghost || settings.ghosts) && opacityOf(s) > 0.f; };
		// whether a beam going `before` and then `after` crosses the surface with this normal, rather than turning back from it
		const auto crosses = [](const glm::dvec3& before, const glm::dvec3& after, const glm::dvec3& normal) { return glm::dot(before, normal) * glm::dot(after, normal) > 0.0; };
		// the segments with a drawn child across the surface they end on: their glow goes on in that child
		std::vector<uint8_t> carriesOn(tree.segments.size(), 0);
		for (const BeamSegment& s : tree.segments) {
			if (s.parent != NO_BEAM_SEGMENT && drawn(s) && crosses(tree.segments[s.parent].start.direction, s.start.direction, tree.segments[s.parent].endNormal)) {
				carriesOn[s.parent] = 1;
			}
		}
		// where a segment ends on a surface, its normal there: the plane its last ring lies on, and its children's first
		const auto meetsSurface = [](const BeamSegment& s) {
			return s.end == BeamEnd::Surface || s.end == BeamEnd::Pass || s.end == BeamEnd::Detector || s.end == BeamEnd::Absorbed || s.end == BeamEnd::Scattered
				|| s.end == BeamEnd::Laser;
		};
		for (uint32_t i = 0; i < tree.segments.size(); i++) {
			const BeamSegment& seg = tree.segments[i];
			if (!drawn(seg)) {
				continue;
			}
			const float opacity = opacityOf(seg);
			// toward red by the fraction clipping took upstream, on a square-root scale so a percent shows
			const float tint = (float)std::min(1.0, std::sqrt(std::max(seg.start.clippedUpstream, 0.0)));
			const glm::vec3 color = glm::mix(base, glm::vec3(1.f, 0.08f, 0.05f), tint);
			const glm::dvec3& d = seg.start.direction;
			const glm::dvec3& e1 = seg.start.e1;
			const glm::dvec3& e2 = seg.start.e2;
			const uint32_t flags = dashed[i] ? BEAM_SECTION_DASHED : 0u;

			if (settings.display == BeamDisplay::SpotsOnly) {
				const bool meets = seg.end == BeamEnd::Surface || seg.end == BeamEnd::Pass || seg.end == BeamEnd::Detector || seg.end == BeamEnd::Absorbed
					|| seg.end == BeamEnd::Scattered;
				if (!meets) {
					continue;
				}
				// the footprint: the beam's ellipse at the end, carried along the beam onto the surface's tangent plane
				const BeamCrossSection at = tree.crossSection(i, seg.length);
				const double c = std::cos(at.angle);
				const double sn = std::sin(at.angle);
				const glm::dvec3 a = tangentPoint(seg.endNormal, d, e1, e2, settings.radiusScale * at.radii.x * glm::dvec2(c, sn));
				const glm::dvec3 b = tangentPoint(seg.endNormal, d, e1, e2, settings.radiusScale * at.radii.y * glm::dvec2(-sn, c));
				BeamSection section {};
				const glm::vec3 center = glm::vec3(seg.endPoint());
				section.center0 = glm::vec4(center, (float)(seg.start.distance + seg.length));
				section.axisA0 = glm::vec4(glm::vec3(a), std::bit_cast<float>(flags | BEAM_SECTION_SPOT));
				section.axisB0 = glm::vec4(glm::vec3(b), 0.f);
				section.center1 = section.center0;
				section.axisA1 = glm::vec4(0.f);
				section.axisB1 = glm::vec4(0.f);
				section.color = glm::vec4(color, opacity);
				out.push_back(section);
				continue;
			}

			const std::vector<BeamRing> rings = beamRings(seg.start.rays, seg.start.index, tree.wavelength, seg.length, settings.waistRegion);
			// the planes the end rings lie on: the surface the parent ended on, and the one this segment ends on; none at a
			// laser's output or an escape, and none at grazing incidence, where the footprint runs away
			const auto usable = [&](const glm::dvec3& n) { return glm::dot(n, n) > 0.5 && std::abs(glm::dot(n, d)) > 0.05; };
			std::optional<glm::dvec3> startPlane;
			if (seg.parent != NO_BEAM_SEGMENT && meetsSurface(tree.segments[seg.parent]) && usable(tree.segments[seg.parent].endNormal)) {
				startPlane = tree.segments[seg.parent].endNormal;
			}
			std::optional<glm::dvec3> endPlane;
			if (meetsSurface(seg) && usable(seg.endNormal)) {
				endPlane = seg.endNormal;
			}
			const auto ringAxes = [&](const BeamRing& ring, const std::optional<glm::dvec3>& plane, glm::vec3& a, glm::vec3& b) {
				const double c = std::cos(ring.angle);
				const double s = std::sin(ring.angle);
				glm::dvec3 da = settings.radiusScale * ring.radii.x * (c * e1 + s * e2);
				glm::dvec3 db = settings.radiusScale * ring.radii.y * (-s * e1 + c * e2);
				if (plane) {
					// along the beam onto the plane
					da -= d * (glm::dot(*plane, da) / glm::dot(*plane, d));
					db -= d * (glm::dot(*plane, db) / glm::dot(*plane, d));
				}
				a = glm::vec3(da);
				b = glm::vec3(db);
			};
			for (size_t r = 1; r < rings.size(); r++) {
				BeamSection section {};
				glm::vec3 a0, b0, a1, b1;
				ringAxes(rings[r - 1], r - 1 == 0 ? startPlane : std::nullopt, a0, b0);
				ringAxes(rings[r], r + 1 == rings.size() ? endPlane : std::nullopt, a1, b1);
				section.center0 = glm::vec4(glm::vec3(seg.start.origin + rings[r - 1].s * d), (float)(seg.start.distance + rings[r - 1].s));
				uint32_t sectionFlags = flags;
				if (r == 1 && startPlane && !crosses(tree.segments[seg.parent].start.direction, d, *startPlane)) {
					sectionFlags |= BEAM_SECTION_STOP_AT_START;
				}
				if (r + 1 == rings.size() && endPlane && !carriesOn[i]) {
					sectionFlags |= BEAM_SECTION_STOP_AT_END;
				}
				section.axisA0 = glm::vec4(a0, std::bit_cast<float>(sectionFlags));
				section.axisB0 = glm::vec4(b0, 0.f);
				section.center1 = glm::vec4(glm::vec3(seg.start.origin + rings[r].s * d), (float)(seg.start.distance + rings[r].s));
				section.axisA1 = glm::vec4(a1, 0.f);
				section.axisB1 = glm::vec4(b1, 0.f);
				section.color = glm::vec4(color, opacity);
				out.push_back(section);
			}

			// the disks that close the tube, each on its own ring: at the laser's output, and on the surface the segment ends
			// on. Never at an iris's opening or a probe plane, where the beam meets nothing, or past the grazing limit above
			const auto addDisk = [&](const BeamRing& ring, const std::optional<glm::dvec3>& plane, double s) {
				glm::vec3 a, b;
				ringAxes(ring, plane, a, b);
				BeamSection disk {};
				disk.center0 = glm::vec4(glm::vec3(seg.start.origin + s * d), (float)(seg.start.distance + s));
				disk.axisA0 = glm::vec4(a, std::bit_cast<float>(BEAM_SECTION_SPOT));
				disk.axisB0 = glm::vec4(b, 0.f);
				disk.center1 = disk.center0;
				disk.color = glm::vec4(color, opacity);
				disks.push_back(disk);
			};
			if (seg.parent == NO_BEAM_SEGMENT) {
				addDisk(rings.front(), std::nullopt, 0.0);
			}
			if (endPlane && seg.end != BeamEnd::Pass) {
				addDisk(rings.back(), endPlane, seg.length);
			}
		}
	}
	out.insert(out.end(), disks.begin(), disks.end());
	return out;
}

namespace {

// the surface an optic's distance label belongs on, when its kind or its origin names one
std::optional<uint32_t> distanceSurface(const SceneOptic& optic)
{
	if (optic.kind == OpticKind::CubeBeamsplitter) {
		return CUBE_DIAGONAL_SURFACE;
	}
	if (optic.kind == OpticKind::SphericalLens) {
		// the choices of "originAt" that name a surface: the front vertex is surface 0, the back vertex surface 1
		const int originAt = opticEnum(optic, "originAt");
		if (originAt == 0 || originAt == 4) {
			return originAt == 0 ? 0u : 1u;
		}
	}
	return std::nullopt;
}

} // namespace

std::vector<BeamDistanceMark> beamDistanceMarks(const BeamTree& tree, const BeamSolver& solver)
{
	const std::vector<BeamSegment>& segments = tree.segments;
	// each main segment's child across the surface it ends on, into or out of a body
	std::vector<uint32_t> through(segments.size(), NO_BEAM_SEGMENT);
	for (uint32_t i = 0; i < segments.size(); i++) {
		const BeamSegment& seg = segments[i];
		if (!seg.start.ghost && seg.parent != NO_BEAM_SEGMENT && seg.start.inside != segments[seg.parent].start.inside) {
			through[seg.parent] = i;
		}
	}

	const auto markAlong = [&](uint32_t segment, double s) { return BeamDistanceMark { segments[segment].start.origin + s * segments[segment].start.direction, segments[segment].start.distance + s }; };
	// the hit on `surface` of the passage the beam arriving along `arrival` makes through `optic`, if it reaches it
	const auto onSurface = [&](uint32_t arrival, const SceneOptic& optic, uint32_t surface) -> std::optional<BeamDistanceMark> {
		for (const uint32_t k : { arrival, through[arrival] }) {
			if (k != NO_BEAM_SEGMENT && segments[k].end == BeamEnd::Surface && segments[k].endOptic == optic.id && segments[k].endSurface == surface) {
				return markAlong(k, segments[k].length);
			}
		}
		return std::nullopt;
	};
	// where the beam arriving along `arrival` crosses the plane of a lens's origin, if on this passage
	const auto onOriginPlane = [&](uint32_t arrival, const SceneOptic& lens, const OpticPrescription& body) -> std::optional<BeamDistanceMark> {
		const uint32_t inside = through[arrival];
		const glm::dvec3 point = glm::dvec3(lens.world * glm::dvec4(0.0, 0.0, body.originZ, 1.0));
		const glm::dvec3 normal = glm::dvec3(lens.world[2]);
		for (const uint32_t k : { arrival, inside, inside != NO_BEAM_SEGMENT ? through[inside] : NO_BEAM_SEGMENT }) {
			if (k == NO_BEAM_SEGMENT) {
				continue;
			}
			const double facing = glm::dot(segments[k].start.direction, normal);
			const double s = std::abs(facing) > 1e-9 ? glm::dot(point - segments[k].start.origin, normal) / facing : -1.0;
			if (s >= 0.0 && s <= segments[k].length) {
				return markAlong(k, s);
			}
		}
		return std::nullopt;
	};

	std::vector<BeamDistanceMark> marks;
	for (uint32_t i = 0; i < segments.size(); i++) {
		const BeamSegment& seg = segments[i];
		if (seg.start.ghost || seg.length <= 0.0 || seg.start.inside != 0 || seg.end == BeamEnd::Escaped) {
			continue;
		}
		std::optional<BeamDistanceMark> mark;
		const SceneOptic* optic = seg.end == BeamEnd::Surface ? solver.optic(seg.endOptic) : nullptr;
		const OpticPrescription* body = optic != nullptr ? solver.body(seg.endOptic) : nullptr;
		if (body != nullptr) {
			if (const std::optional<uint32_t> surface = distanceSurface(*optic)) {
				mark = onSurface(i, *optic, *surface);
			} else if (optic->kind == OpticKind::SphericalLens) {
				mark = onOriginPlane(i, *optic, *body);
			}
		}
		marks.push_back(mark.value_or(markAlong(i, seg.length)));
	}
	return marks;
}
