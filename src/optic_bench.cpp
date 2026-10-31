#include <optic_bench.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double INCH = 0.0254;

glm::dmat4 translation(const glm::dvec3& t)
{
	return glm::translate(glm::dmat4(1.0), t);
}

// about +y: 0 keeps +z, PI / 2 turns +z to +x
glm::dmat4 rotationY(double angle)
{
	return glm::rotate(glm::dmat4(1.0), angle, glm::dvec3(0.0, 1.0, 0.0));
}

glm::dmat4 rotationAbout(const glm::dvec3& axis, double angle)
{
	return glm::rotate(glm::dmat4(1.0), angle, axis);
}

glm::dvec3 transformPoint(const glm::dmat4& m, const glm::dvec3& p)
{
	return glm::dvec3(m * glm::dvec4(p, 1.0));
}

glm::dvec3 transformVector(const glm::dmat4& m, const glm::dvec3& v)
{
	return glm::dvec3(m * glm::dvec4(v, 0.0));
}

// an angle in (-PI, PI]
double wrapAngle(double a)
{
	a = std::fmod(a + PI, 2.0 * PI);
	if (a <= 0.0) {
		a += 2.0 * PI;
	}
	return a - PI;
}

glm::dmat3 skew(const glm::dvec3& v)
{
	return glm::dmat3(glm::dvec3(0.0, v.z, -v.y), glm::dvec3(-v.z, 0.0, v.x), glm::dvec3(v.y, -v.x, 0.0));
}

glm::dmat3 rotation3(const glm::dvec3& axis, double angle)
{
	return glm::dmat3(rotationAbout(axis, angle));
}

//---------------------------------------------------------------- the breadboard mesh

struct MeshBuilder {
	BenchMesh& mesh;
	// by surface: the skins, the side walls, the floors
	std::array<std::vector<uint32_t>, 3> surfaces {};
	// what the next triangles are, unless they are a floor's
	size_t current { 0 };

	// one triangle, wound so that its normal faces `outward`, with that face normal on its three corners
	void triangle(glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, const glm::dvec3& outward, bool floor = false)
	{
		glm::dvec3 n = glm::cross(b - a, c - a);
		const double length = glm::length(n);
		if (length <= 0.0) {
			return;
		}
		if (glm::dot(n, outward) < 0.0) {
			std::swap(b, c);
			n = -n;
		}
		n /= length;
		std::vector<uint32_t>& indices = surfaces[floor ? 2 : current];
		for (const glm::dvec3& p : { a, b, c }) {
			indices.push_back((uint32_t)mesh.positions.size());
			mesh.positions.push_back(glm::vec3(p));
			mesh.normals.push_back(glm::vec3(n));
			mesh.uvs.push_back(glm::vec2(100.0 * p.x, 100.0 * p.z));
		}
	}

	// a vertical quad over the segment a -> b (in x and z) between heights y0 and y1, facing `outward`
	void wall(glm::dvec2 a, glm::dvec2 b, double y0, double y1, const glm::dvec3& outward)
	{
		const glm::dvec3 p0(a.x, y0, a.y);
		const glm::dvec3 p1(b.x, y0, b.y);
		const glm::dvec3 p2(b.x, y1, b.y);
		const glm::dvec3 p3(a.x, y1, a.y);
		triangle(p0, p1, p2, outward);
		triangle(p0, p2, p3, outward);
	}
};

// a rounded rectangle's outline, counter-clockwise seen from above (+x toward +z), corners about the corner holes
std::vector<glm::dvec2> roundedOutline(const glm::dvec2& lo, const glm::dvec2& hi, double radius, int segments)
{
	const glm::dvec2 centers[4] = { { hi.x - radius, lo.y + radius }, { hi.x - radius, hi.y - radius }, { lo.x + radius, hi.y - radius },
		{ lo.x + radius, lo.y + radius } };
	std::vector<glm::dvec2> outline;
	for (int corner = 0; corner < 4; corner++) {
		const double start = -0.5 * PI + 0.5 * PI * corner;
		for (int i = 0; i <= segments; i++) {
			const double a = start + 0.5 * PI * i / segments;
			outline.push_back(centers[corner] + radius * glm::dvec2(std::cos(a), std::sin(a)));
		}
	}
	return outline;
}

// a closed outline's segments as vertical walls, facing away from `center`
void extrudeOutline(MeshBuilder& b, const std::vector<glm::dvec2>& outline, glm::dvec2 center, double y0, double y1)
{
	for (size_t i = 0; i < outline.size(); i++) {
		const glm::dvec2 p = outline[i];
		const glm::dvec2 q = outline[(i + 1) % outline.size()];
		if (glm::length(q - p) <= 0.0) {
			continue;
		}
		const glm::dvec2 d = q - p;
		glm::dvec2 n(d.y, -d.x);
		if (glm::dot(n, 0.5 * (p + q) - center) < 0.0) {
			n = -n;
		}
		b.wall(p, q, y0, y1, glm::dvec3(n.x, 0.0, n.y));
	}
}

// a closed outline's interior as a fan from its center, at height y, facing `up` (+1 or -1)
void capOutline(MeshBuilder& b, const std::vector<glm::dvec2>& outline, glm::dvec2 center, double y, double up)
{
	const glm::dvec3 c(center.x, y, center.y);
	for (size_t i = 0; i < outline.size(); i++) {
		const glm::dvec2 p = outline[i];
		const glm::dvec2 q = outline[(i + 1) % outline.size()];
		b.triangle(c, glm::dvec3(p.x, y, p.y), glm::dvec3(q.x, y, q.y), glm::dvec3(0.0, up, 0.0));
	}
}

// The side wall between the skins: set back BREADBOARD_PANEL_INSET along each side, with corner bumpers set back
// BREADBOARD_BUMPER_INSET that run BREADBOARD_BUMPER_RUN from each corner hole's center and wrap the corner about it
std::vector<glm::dvec2> wallOutline(const BenchGrid& grid, int arcSegments)
{
	const double bumperRadius = BREADBOARD_BORDER - BREADBOARD_BUMPER_INSET;
	const glm::dvec2 lo = grid.min;
	const glm::dvec2 hi = grid.max;
	std::vector<glm::dvec2> outline;
	// the sides in counter-clockwise order: +x (z rising), +z (x falling), -x (z falling), -z (x rising). Along each:
	// the corner center it starts at and ends at, and the outward normal
	struct Side {
		glm::dvec2 from;
		glm::dvec2 to;
		glm::dvec2 normal;
	};
	const double r = BREADBOARD_BORDER;
	const Side sides[4] = {
		{ { hi.x - r, lo.y + r }, { hi.x - r, hi.y - r }, { 1.0, 0.0 } },
		{ { hi.x - r, hi.y - r }, { lo.x + r, hi.y - r }, { 0.0, 1.0 } },
		{ { lo.x + r, hi.y - r }, { lo.x + r, lo.y + r }, { -1.0, 0.0 } },
		{ { lo.x + r, lo.y + r }, { hi.x - r, lo.y + r }, { 0.0, -1.0 } },
	};
	const double panel = r - BREADBOARD_PANEL_INSET;
	for (int s = 0; s < 4; s++) {
		const Side& side = sides[s];
		const glm::dvec2 along = glm::normalize(side.to - side.from);
		const double length = glm::length(side.to - side.from);
		auto at = [&](double t, double out) { return side.from + t * along + out * side.normal; };
		outline.push_back(at(0.0, bumperRadius));
		if (2.0 * BREADBOARD_BUMPER_RUN < length) {
			outline.push_back(at(BREADBOARD_BUMPER_RUN, bumperRadius));
			outline.push_back(at(BREADBOARD_BUMPER_RUN, panel));
			outline.push_back(at(length - BREADBOARD_BUMPER_RUN, panel));
			outline.push_back(at(length - BREADBOARD_BUMPER_RUN, bumperRadius));
		}
		outline.push_back(at(length, bumperRadius));
		// the corner: a quarter turn about side.to, from this side's normal to the next's
		const double start = std::atan2(side.normal.y, side.normal.x);
		for (int i = 1; i < arcSegments; i++) {
			const double a = start + 0.5 * PI * i / arcSegments;
			outline.push_back(side.to + bumperRadius * glm::dvec2(std::cos(a), std::sin(a)));
		}
	}
	return outline;
}

//---------------------------------------------------------------- mounts and seats

double opticDiameter(const SceneOptic& optic)
{
	return findOpticParam(optic.kind, "diameter") != nullptr ? opticNumber(optic, "diameter") : 0.0;
}

bool reflective(OpticKind kind)
{
	return kind == OpticKind::PlanarMirror || kind == OpticKind::ConcaveMirror || kind == OpticKind::OffAxisParabola;
}

// the half height of a prism standing on its cap: what lifts its datum above the platform
double prismHalfHeight(const SceneOptic& optic)
{
	switch (optic.kind) {
	case OpticKind::CubeBeamsplitter:
		return 0.5 * opticNumber(optic, "side");
	case OpticKind::RightAnglePrism: {
		const double height = opticNumber(optic, "height");
		return 0.5 * (height > 0.0 ? height : opticNumber(optic, "leg"));
	}
	case OpticKind::EquilateralPrism: {
		const double height = opticNumber(optic, "height");
		return 0.5 * (height > 0.0 ? height : opticNumber(optic, "side"));
	}
	default:
		return 0.0;
	}
}

// the mate an optic of a mount model sits on
const char* seatMateName(MountModelKind kind)
{
	switch (kind) {
	case MountModelKind::PrismMount:
		return "prism_seat";
	case MountModelKind::PlateHolder:
		return "plate_seat";
	case MountModelKind::Iris:
		return "aperture_center";
	default:
		return "optic_seat";
	}
}

struct Seat {
	// the body frame -> the mount's canonical frame, the seat's part at rest
	glm::dmat4 mountFromBody { 1.0 };
	// the part the seat moves with
	std::string part;
	// the optic's front where a retaining ring or a jaw meets it, along the mount's z
	double front { 0.0 };
	// the body's corners in the mount frame, for a platform's overhang
	OpticBoundsT<double> bounds;
};

// Where the body sits in its mount. A mirror faces the mount's front (its +z, toward the incoming light), its back on
// the seat; a round transmissive optic, an iris and a plate are turned half a turn about y, since their light travels
// along their own +z, through the mount from its front; a plate stands on its bottom edge. A prism stands on its cap,
// its body x up, so that it turns and disperses in the table's plane
std::optional<Seat> opticSeat(const SceneOptic& optic, const MountModel& model, const OpticPrescription& body)
{
	const MountMate* mate = model.findMate(seatMateName(model.info->kind));
	if (mate == nullptr || body.surfaces.empty()) {
		return std::nullopt;
	}
	const glm::dmat4 halfTurn = rotationY(PI);
	glm::dmat4 local(1.0);
	switch (model.info->kind) {
	case MountModelKind::PrismMount: {
		// body x -> mount y, body y -> mount x, body z -> mount -z
		const glm::dmat4 standing(glm::dvec4(0.0, 1.0, 0.0, 0.0), glm::dvec4(1.0, 0.0, 0.0, 0.0), glm::dvec4(0.0, 0.0, -1.0, 0.0), glm::dvec4(0.0, 0.0, 0.0, 1.0));
		local = translation(glm::dvec3(0.0, prismHalfHeight(optic), 0.0)) * standing;
		break;
	}
	case MountModelKind::PlateHolder: {
		const double half = 0.5 * opticNumber(optic, "side");
		// the back face's bottom edge, where it meets the holder's wall and floor: a plate wedge is thin at its bottom
		const double back = optic.kind == OpticKind::PlateWedge ? opticNumber(optic, "edgeThickness") : opticNumber(optic, "thickness");
		local = translation(glm::dvec3(0.0, half, back)) * halfTurn;
		break;
	}
	case MountModelKind::Iris:
		local = halfTurn;
		break;
	default: {
		// the back face's vertex: surface 1 of every round kind
		const double back = body.surfaces.size() > 1 ? body.surfaces[1].placement.origin.z : 0.0;
		local = reflective(optic.kind) ? translation(glm::dvec3(0.0, 0.0, -back)) : translation(glm::dvec3(0.0, 0.0, back)) * halfTurn;
		break;
	}
	}
	Seat seat;
	seat.mountFromBody = mateTransform(*mate) * local;
	seat.part = mate->part;
	// the front at the rim, from the edge cylinder's two bounds around it; a body without one (a plate, a prism), its box
	bool rim = false;
	seat.front = -1e30;
	for (const OpticSurface& surface : body.surfaces) {
		if (surface.shape != SurfaceShape::Cylinder) {
			continue;
		}
		for (int i = 0; i < 32; i++) {
			const double a = 2.0 * PI * i / 32.0;
			const glm::dvec2 xy = surface.radius * glm::dvec2(std::cos(a), std::sin(a));
			for (const CylinderBoundT<double>* bound : { &surface.lower, &surface.upper }) {
				const glm::dvec3 p(xy, boundZ(*bound, xy));
				const glm::dvec3 b = surface.placement.rotation * p + surface.placement.origin;
				seat.front = std::max(seat.front, transformPoint(local, b).z);
				rim = true;
			}
		}
	}
	for (int corner = 0; corner < 8; corner++) {
		const glm::dvec3 c((corner & 1) ? body.bounds.max.x : body.bounds.min.x, (corner & 2) ? body.bounds.max.y : body.bounds.min.y,
			(corner & 4) ? body.bounds.max.z : body.bounds.min.z);
		const glm::dvec3 m = transformPoint(seat.mountFromBody, c);
		seat.bounds.grow(m);
		if (!rim) {
			seat.front = std::max(seat.front, transformPoint(local, c).z);
		}
	}
	return seat;
}

const MountPart* findPart(const MountModel& model, std::string_view name)
{
	for (const MountPart& part : model.parts) {
		if (part.name == name) {
			return &part;
		}
	}
	return nullptr;
}

// a part's vertices' extent in its model's frame, or of those within `band` of the frame's y = 0 when `band` > 0 (a
// platform's top face). Every stack asks for the same few each frame, so each is measured once per model: the models
// stay where they are read until the library is cleared at shutdown
OpticBoundsT<double> partBounds(const MountModel& model, std::string_view name, double band = 0.0)
{
	static std::map<std::tuple<const MountModel*, std::string, double>, OpticBoundsT<double>> cache;
	const auto key = std::make_tuple(&model, std::string(name), band);
	if (const auto found = cache.find(key); found != cache.end()) {
		return found->second;
	}
	OpticBoundsT<double> bounds;
	if (const MountPart* part = findPart(model, name)) {
		for (const MountSurface& surface : part->surfaces) {
			for (const glm::vec3& p : surface.positions) {
				if (band <= 0.0 || std::abs(p.y) < band) {
					bounds.grow(glm::dvec3(p));
				}
			}
		}
	}
	cache[key] = bounds;
	return bounds;
}

// each part's motion, in its model's frame, at the optic's fine adjustment, with the parts that do not show
struct PartMotions {
	std::map<std::string, glm::dmat4> motion;
	std::vector<std::string> hidden;
	std::optional<KinematicReadout> kinematic;

	glm::dmat4 of(const std::string& part) const
	{
		const auto found = motion.find(part);
		return found == motion.end() ? glm::dmat4(1.0) : found->second;
	}
};

PartMotions partMotions(const MountModel& model, const SceneOptic& optic, const Seat& seat)
{
	PartMotions out;
	const FineAdjust& fine = optic.mount.fine;
	// the plate, and the adjusters that push it
	if (const std::optional<KinematicGeometry> geometry = kinematicGeometry(model)) {
		const glm::dvec2 limits = adjusterTurnLimits(*geometry);
		const double turnsX = std::clamp(fine.adjusterX, -limits.x, limits.x);
		const double turnsY = std::clamp(fine.adjusterY, -limits.y, limits.y);
		const KinematicPose pose = solveKinematic(*geometry, turnsX, turnsY);
		KinematicReadout readout;
		readout.angleX = pose.alpha;
		readout.angleY = pose.beta;
		readout.turnLimit = limits;
		readout.pitch = glm::dvec2(geometry->pitchX, geometry->pitchY);
		readout.anglePerRev = glm::dvec2(geometry->pitchX / std::max(std::abs(geometry->leverX), 1e-12), geometry->pitchY / std::max(std::abs(geometry->leverY), 1e-12));
		readout.tiltRange = geometry->tiltRange;
		readout.converged = pose.converged;
		out.kinematic = readout;
		for (const MountMotion& motion : model.motions) {
			if (motion.kind == MountMotionKind::Tilt) {
				out.motion[motion.part] = pose.plate;
			} else if (motion.kind == MountMotionKind::Screw) {
				const MountMate* tip = model.findMate(motion.mate);
				if (tip == nullptr) {
					continue;
				}
				const bool isX = motion.mate == "adjuster_x_tip";
				const double turns = isX ? turnsX : turnsY;
				const double advance = turns * (isX ? geometry->pitchX : geometry->pitchY);
				// the screw advances along its tip's z and turns about it
				out.motion[motion.part] = translation(tip->origin + advance * tip->z) * rotationAbout(tip->z, -2.0 * PI * turns) * translation(-tip->origin);
			}
		}
	}
	// a rotation mount's rotor
	for (const MountMotion& motion : model.motions) {
		if (motion.kind == MountMotionKind::Rotate && motion.drive.empty()) {
			if (const MountMate* axis = model.findMate(motion.mate)) {
				const double angle = std::clamp(fine.rotor, motion.rangeMin, motion.rangeMax);
				out.motion[motion.part] = translation(axis->origin) * rotationAbout(axis->z, angle) * translation(-axis->origin);
			}
		}
	}
	// an iris's lever and blades, by its opening
	for (const MountDrive& drive : model.drives) {
		if (drive.name != "aperture" || optic.kind != OpticKind::Iris) {
			continue;
		}
		const double opening = std::clamp(opticNumber(optic, "opening"), opticNumber(optic, "minOpening"), opticNumber(optic, "maxOpening"));
		for (const MountMotion& motion : model.motions) {
			if (motion.drive != drive.name) {
				continue;
			}
			if (const MountMate* axis = model.findMate(motion.mate)) {
				const double angle = driveMotionValue(drive, motion.part, opening);
				out.motion[motion.part] = translation(axis->origin) * rotationAbout(axis->z, angle) * translation(-axis->origin);
			}
		}
	}
	// what closes on the optic's front: a retaining ring, which rides on the seat's part, and a plate holder's jaw. A ring
	// that would have to stand proud of the mount comes out, as a 6 mm blank's does from KM05T/M
	for (const MountMotion& motion : model.motions) {
		if (motion.kind != MountMotionKind::Slide) {
			continue;
		}
		const MountMate* mate = model.findMate(motion.mate);
		if (mate == nullptr) {
			continue;
		}
		const double value = glm::dot(glm::dvec3(0.0, 0.0, seat.front) - mate->origin, mate->z);
		if (value > motion.rangeMax + 1e-6) {
			out.hidden.push_back(motion.part);
			continue;
		}
		const glm::dmat4 slide = translation(std::max(value, motion.rangeMin) * mate->z);
		out.motion[motion.part] = motion.part == seat.part ? slide : out.of(seat.part) * slide;
	}
	return out;
}

//---------------------------------------------------------------- the stack

struct StackFrames {
	const MountModel* mount { nullptr };
	const MountModel* holder { nullptr };
	const MountModel* fork { nullptr };
	std::optional<Seat> seat;
	PartMotions motions;
	// the optic's origin in the mount's frame at rest, and the post tap
	glm::dvec3 restOrigin { 0.0 };
	glm::dvec3 tap { 0.0 };
	// the holder's top face, and the post's bottom at the bore's mid-depth, above the table
	double entryHeight { 0.0 };
	double postBottom { 0.0 };
};

const MountModel* lookup(const BenchContext& context, const char* id)
{
	return context.models ? context.models(id) : nullptr;
}

StackFrames stackFrames(const SceneOptic& optic, const BenchContext& context, const OpticPrescription& body)
{
	StackFrames frames;
	const MountModelInfo* info = opticMountModel(optic);
	if (info == nullptr) {
		return frames;
	}
	frames.mount = lookup(context, info->id);
	frames.holder = lookup(context, POST_HOLDER_MODEL);
	frames.fork = lookup(context, CLAMPING_FORK_MODEL);
	if (frames.mount == nullptr) {
		return frames;
	}
	frames.seat = opticSeat(optic, *frames.mount, body);
	if (!frames.seat) {
		return frames;
	}
	frames.motions = partMotions(*frames.mount, optic, *frames.seat);
	frames.restOrigin = transformPoint(frames.seat->mountFromBody, opticOrigin(optic, body));
	if (const MountMate* tap = frames.mount->findMate("post_tap")) {
		frames.tap = tap->origin;
	}
	if (frames.holder != nullptr) {
		if (const MountMate* entry = frames.holder->findMate("post_entry")) {
			const auto depth = entry->params.find("depth");
			frames.entryHeight = entry->origin.y;
			frames.postBottom = entry->origin.y - 0.5 * (depth != entry->params.end() ? depth->second : 0.0);
		}
	}
	return frames;
}

OpticPrescription opticBody(const SceneOptic& optic, const BenchContext& context)
{
	OpticReadout readout;
	return buildOptic(optic, context.build, readout);
}

glm::dmat4 forkWorld(glm::dvec2 post, double angle)
{
	return translation(glm::dvec3(post.x, 0.0, post.y)) * rotationY(angle);
}

// the fork's outline on the table at an angle, from its part's extent
std::array<glm::dvec2, 4> forkOutlineAt(const MountModel* fork, glm::dvec2 post, double angle)
{
	OpticBoundsT<double> b = fork != nullptr ? partBounds(*fork, "fork") : OpticBoundsT<double> {};
	if (b.empty()) {
		b.min = glm::dvec3(-0.018, 0.0, -0.004);
		b.max = glm::dvec3(0.018, 0.011, 0.070);
	}
	const glm::dmat4 world = forkWorld(post, angle);
	std::array<glm::dvec2, 4> outline;
	const glm::dvec3 corners[4] = { { b.min.x, 0.0, b.min.z }, { b.max.x, 0.0, b.min.z }, { b.max.x, 0.0, b.max.z }, { b.min.x, 0.0, b.max.z } };
	for (int i = 0; i < 4; i++) {
		const glm::dvec3 w = transformPoint(world, corners[i]);
		outline[(size_t)i] = glm::dvec2(w.x, w.z);
	}
	return outline;
}

double holderRadiusOf(const MountModel* holder)
{
	const OpticBoundsT<double> b = holder != nullptr ? partBounds(*holder, "holder") : OpticBoundsT<double> {};
	return b.empty() ? 0.0159 : std::max(std::abs(b.min.x), std::abs(b.max.x));
}

// the slot's reach: from its near end to its far end
glm::dvec2 forkReachRange(const MountModel* fork)
{
	if (fork != nullptr) {
		if (const MountMate* slot = fork->findMate("slot")) {
			const auto length = slot->params.find("length");
			return glm::dvec2(slot->origin.z, slot->origin.z + (length != slot->params.end() ? length->second : 0.0));
		}
	}
	return glm::dvec2(0.026797, 0.058293);
}

// 2D separating axes: two convex quads, and a quad and a circle, shrunk by a margin so that touching is not a clash
bool quadsOverlap(const std::array<glm::dvec2, 4>& a, const std::array<glm::dvec2, 4>& b, double margin)
{
	for (const std::array<glm::dvec2, 4>* shape : { &a, &b }) {
		for (size_t i = 0; i < 4; i++) {
			const glm::dvec2 edge = (*shape)[(i + 1) % 4] - (*shape)[i];
			const glm::dvec2 axis = glm::normalize(glm::dvec2(-edge.y, edge.x));
			double aMin = 1e30, aMax = -1e30, bMin = 1e30, bMax = -1e30;
			for (const glm::dvec2& p : a) {
				aMin = std::min(aMin, glm::dot(p, axis));
				aMax = std::max(aMax, glm::dot(p, axis));
			}
			for (const glm::dvec2& p : b) {
				bMin = std::min(bMin, glm::dot(p, axis));
				bMax = std::max(bMax, glm::dot(p, axis));
			}
			if (aMax - margin <= bMin || bMax - margin <= aMin) {
				return false;
			}
		}
	}
	return true;
}

bool quadCircleOverlap(const std::array<glm::dvec2, 4>& quad, glm::dvec2 center, double radius, double margin)
{
	// the circle's distance from the quad: zero inside it
	bool inside = true;
	double nearest = 1e30;
	for (size_t i = 0; i < 4; i++) {
		const glm::dvec2 a = quad[i];
		const glm::dvec2 b = quad[(i + 1) % 4];
		const glm::dvec2 ab = b - a;
		const double t = std::clamp(glm::dot(center - a, ab) / glm::dot(ab, ab), 0.0, 1.0);
		nearest = std::min(nearest, glm::length(center - (a + t * ab)));
		const double side = ab.x * (center.y - a.y) - ab.y * (center.x - a.x);
		inside &= side >= 0.0;
	}
	// the outline winds either way: inside is all on one side
	if (!inside) {
		inside = true;
		for (size_t i = 0; i < 4; i++) {
			const glm::dvec2 ab = quad[(i + 1) % 4] - quad[i];
			inside &= ab.x * (center.y - quad[i].y) - ab.y * (center.x - quad[i].x) <= 0.0;
		}
	}
	return inside || nearest < radius - margin;
}

constexpr double CLASH_MARGIN = 1e-4;

bool forkClashes(const std::array<glm::dvec2, 4>& outline, std::span<const OpticStack> others)
{
	for (const OpticStack& other : others) {
		if (!other.post) {
			continue;
		}
		if (quadsOverlap(outline, other.forkOutline, CLASH_MARGIN) || quadCircleOverlap(outline, other.holderCenter, other.holderRadius, CLASH_MARGIN)) {
			return true;
		}
	}
	return false;
}

}

//---------------------------------------------------------------- mount kinds

const char* mountKindKey(MountKind kind)
{
	static const char* const KEYS[] = { "kinematic", "fixed", "plate_holder", "rotation", "prism", "iris", "none" };
	return KEYS[(size_t)kind];
}

const char* mountKindLabel(MountKind kind)
{
	static const char* const LABELS[] = { "Kinematic", "Fixed", "Plate holder", "Rotation", "Prism platform", "Iris housing", "None (unmounted)" };
	return LABELS[(size_t)kind];
}

std::optional<MountKind> findMountKind(std::string_view key)
{
	for (size_t i = 0; i < MOUNT_KIND_COUNT; i++) {
		if (key == mountKindKey((MountKind)i)) {
			return (MountKind)i;
		}
	}
	return std::nullopt;
}

//---------------------------------------------------------------- the grid

glm::dvec2 snapBenchSize(glm::dvec2 size)
{
	for (int axis = 0; axis < 2; axis++) {
		// in whole millimeters, so that 350 mm is 0.35 and not 14 x 0.025
		const double steps = std::round(std::clamp(size[axis], BREADBOARD_MIN_SIZE, BREADBOARD_MAX_SIZE) / BREADBOARD_PITCH);
		size[axis] = steps * (BREADBOARD_PITCH * 1e3) / 1e3;
	}
	return size;
}

BenchGrid benchGrid(const BenchSettings& bench)
{
	const glm::dvec2 size = snapBenchSize(bench.size);
	BenchGrid grid;
	for (int axis = 0; axis < 2; axis++) {
		const int count = std::max(2, (int)std::lround(size[axis] / BREADBOARD_PITCH));
		grid.count[axis] = count;
		// the hole at the origin: the center one, or just below the center for an even count
		const int origin = (count - 1) / 2;
		grid.first[axis] = -BREADBOARD_PITCH * origin;
		grid.min[axis] = grid.first[axis] - BREADBOARD_BORDER;
		grid.max[axis] = grid.first[axis] + BREADBOARD_PITCH * (count - 1) + BREADBOARD_BORDER;
	}
	return grid;
}

glm::dvec2 nearestHole(const BenchSettings& bench, glm::dvec2 point)
{
	const BenchGrid grid = benchGrid(bench);
	glm::dvec2 hole;
	for (int axis = 0; axis < 2; axis++) {
		const double i = std::clamp(std::round((point[axis] - grid.first[axis]) / BREADBOARD_PITCH), 0.0, (double)(grid.count[axis] - 1));
		hole[axis] = grid.first[axis] + BREADBOARD_PITCH * i;
	}
	return hole;
}

bool isHole(const BenchSettings& bench, glm::dvec2 point, double tolerance)
{
	return glm::length(nearestHole(bench, point) - point) <= tolerance;
}

//---------------------------------------------------------------- the breadboard

BenchMesh buildBreadboardMesh(const BenchSettings& bench)
{
	const BenchGrid grid = benchGrid(bench);
	BenchMesh mesh;
	MeshBuilder b { mesh };
	const double h = 0.5 * BREADBOARD_PITCH;
	const double r = 0.5 * BREADBOARD_HOLE_DIAMETER;
	const double skin = BREADBOARD_SKIN;
	constexpr int RING = 12;
	const double t30 = std::tan(PI / 6.0);
	const glm::dvec3 up(0.0, 1.0, 0.0);

	// every cell edge and hole center once, so neighbors share their boundary points exactly
	std::vector<double> edgeX, edgeZ, centerX, centerZ;
	for (int i = 0; i <= grid.count.x; i++) {
		edgeX.push_back(grid.first.x + BREADBOARD_PITCH * (i - 0.5));
	}
	for (int j = 0; j <= grid.count.y; j++) {
		edgeZ.push_back(grid.first.y + BREADBOARD_PITCH * (j - 0.5));
	}
	for (int i = 0; i < grid.count.x; i++) {
		centerX.push_back(grid.first.x + BREADBOARD_PITCH * i);
	}
	for (int j = 0; j < grid.count.y; j++) {
		centerZ.push_back(grid.first.y + BREADBOARD_PITCH * j);
	}
	// the top skin's rim, gathered cell by cell along the plate's edge: segments (a, b) and their outward normal
	struct RimSegment {
		glm::dvec2 a;
		glm::dvec2 b;
	};
	std::vector<RimSegment> rim;
	const glm::dvec2 plateCenter = 0.5 * (grid.min + grid.max);

	for (int i = 0; i < grid.count.x; i++) {
		for (int j = 0; j < grid.count.y; j++) {
			const double cx = centerX[(size_t)i];
			const double cz = centerZ[(size_t)j];
			const double x0 = edgeX[(size_t)i], x1 = edgeX[(size_t)i + 1];
			const double z0 = edgeZ[(size_t)j], z1 = edgeZ[(size_t)j + 1];
			// a corner cell's outer quadrant is rounded about its hole: which quadrant (by +x, +z), if any
			const bool atMaxX = i == grid.count.x - 1, atMinX = i == 0, atMaxZ = j == grid.count.y - 1, atMinZ = j == 0;
			auto rounded = [&](bool px, bool pz) { return (px ? atMaxX : atMinX) && (pz ? atMaxZ : atMinZ); };
			// the boundary, counter-clockwise in (x, z) from +x: the twelve ring directions' points and the four corners
			std::vector<glm::dvec2> boundary;
			std::vector<bool> onRing;
			for (int k = 0; k < RING; k++) {
				const double a = 2.0 * PI * k / RING;
				const int quadrant = (int)std::floor(a / (0.5 * PI));
				const bool px = quadrant == 0 || quadrant == 3;
				const bool pz = quadrant == 0 || quadrant == 1;
				glm::dvec2 p;
				// the square's point in each of the twelve directions, from the shared edges and centers
				switch (k) {
				case 0: p = { x1, cz }; break;
				case 1: p = { x1, cz + h * t30 }; break;
				case 2: p = { cx + h * t30, z1 }; break;
				case 3: p = { cx, z1 }; break;
				case 4: p = { cx - h * t30, z1 }; break;
				case 5: p = { x0, cz + h * t30 }; break;
				case 6: p = { x0, cz }; break;
				case 7: p = { x0, cz - h * t30 }; break;
				case 8: p = { cx - h * t30, z0 }; break;
				case 9: p = { cx, z0 }; break;
				case 10: p = { cx + h * t30, z0 }; break;
				default: p = { x1, cz - h * t30 }; break;
				}
				if (k % 3 != 0 && rounded(px, pz)) {
					p = glm::dvec2(cx, cz) + h * glm::dvec2(std::cos(a), std::sin(a));
				}
				boundary.push_back(p);
				onRing.push_back(true);
				// the corner after directions 1, 4, 7 and 10 (45, 135, 225 and 315 degrees)
				if (k % 3 == 1) {
					const double c = a + PI / 12.0;
					const bool cpx = std::cos(c) > 0.0, cpz = std::sin(c) > 0.0;
					const glm::dvec2 corner = rounded(cpx, cpz) ? glm::dvec2(cx, cz) + h * glm::dvec2(std::cos(c), std::sin(c))
															   : glm::dvec2(cpx ? x1 : x0, cpz ? z1 : z0);
					boundary.push_back(corner);
					onRing.push_back(false);
				}
			}
			// the top face between the hole's ring and the boundary: a fan from each ring vertex over the boundary points
			// up to the next ring direction
			std::vector<glm::dvec3> ring;
			for (int k = 0; k < RING; k++) {
				const double a = 2.0 * PI * k / RING;
				ring.push_back(glm::dvec3(cx + r * std::cos(a), 0.0, cz + r * std::sin(a)));
			}
			size_t bi = 0;
			for (int k = 0; k < RING; k++) {
				const glm::dvec3 rk = ring[(size_t)k];
				const glm::dvec3 rn = ring[(size_t)((k + 1) % RING)];
				// the boundary points from direction k to direction k + 1
				size_t start = bi;
				size_t end = (start + 1) % boundary.size();
				while (!onRing[end]) {
					end = (end + 1) % boundary.size();
				}
				for (size_t m = start; m != end; m = (m + 1) % boundary.size()) {
					const glm::dvec2 p = boundary[m];
					const glm::dvec2 q = boundary[(m + 1) % boundary.size()];
					b.triangle(rk, glm::dvec3(p.x, 0.0, p.y), glm::dvec3(q.x, 0.0, q.y), up);
				}
				const glm::dvec2 e = boundary[end];
				b.triangle(rk, glm::dvec3(e.x, 0.0, e.y), rn, up);
				bi = end;
			}
			// the hole's wall, facing its axis, and its floor
			for (int k = 0; k < RING; k++) {
				const glm::dvec3 p = ring[(size_t)k];
				const glm::dvec3 q = ring[(size_t)((k + 1) % RING)];
				const glm::dvec3 mid = 0.5 * (p + q);
				b.wall(glm::dvec2(p.x, p.z), glm::dvec2(q.x, q.z), -skin, 0.0, glm::dvec3(cx - mid.x, 0.0, cz - mid.z));
			}
			for (int k = 1; k + 1 < RING; k++) {
				const glm::dvec3 a0 = ring[0] - glm::dvec3(0.0, skin, 0.0);
				b.triangle(a0, ring[(size_t)k] - glm::dvec3(0.0, skin, 0.0), ring[(size_t)k + 1] - glm::dvec3(0.0, skin, 0.0), up, true);
			}
			// the boundary segments on the plate's edge, for the rim
			for (size_t m = 0; m < boundary.size(); m++) {
				const glm::dvec2 p = boundary[m];
				const glm::dvec2 q = boundary[(m + 1) % boundary.size()];
				const bool edge = (atMaxX && p.x >= x1 - 1e-12 && q.x >= x1 - 1e-12) || (atMinX && p.x <= x0 + 1e-12 && q.x <= x0 + 1e-12)
					|| (atMaxZ && p.y >= z1 - 1e-12 && q.y >= z1 - 1e-12) || (atMinZ && p.y <= z0 + 1e-12 && q.y <= z0 + 1e-12);
				// a rounded corner's arc is on the edge too: both points at the hole's border radius, outside the cell's
				// inner quadrants
				const glm::dvec2 c(cx, cz);
				const bool arc = std::abs(glm::length(p - c) - h) < 1e-12 && std::abs(glm::length(q - c) - h) < 1e-12 && rounded((p.x + q.x) > 2.0 * cx, (p.y + q.y) > 2.0 * cz)
					&& (p.x - cx) * (q.x - cx) >= -1e-18 && (p.y - cz) * (q.y - cz) >= -1e-18;
				if (edge || arc) {
					rim.push_back({ p, q });
				}
			}
		}
	}

	// the top skin's rim and underside
	std::vector<glm::dvec2> outline;
	for (const RimSegment& s : rim) {
		const glm::dvec2 d = s.b - s.a;
		glm::dvec2 n(d.y, -d.x);
		if (glm::dot(n, 0.5 * (s.a + s.b) - plateCenter) < 0.0) {
			n = -n;
		}
		b.wall(s.a, s.b, -skin, 0.0, glm::dvec3(n.x, 0.0, n.y));
		b.triangle(glm::dvec3(plateCenter.x, -skin, plateCenter.y), glm::dvec3(s.a.x, -skin, s.a.y), glm::dvec3(s.b.x, -skin, s.b.y), -up);
	}
	// the bottom skin
	const std::vector<glm::dvec2> bottom = roundedOutline(grid.min, grid.max, BREADBOARD_BORDER, 8);
	const double y0 = -BREADBOARD_THICKNESS;
	const double y1 = -BREADBOARD_THICKNESS + skin;
	extrudeOutline(b, bottom, plateCenter, y0, y1);
	capOutline(b, bottom, plateCenter, y1, 1.0);
	capOutline(b, bottom, plateCenter, y0, -1.0);
	// the side walls between the skins
	b.current = 1;
	extrudeOutline(b, wallOutline(grid, 8), plateCenter, y1, -skin);

	mesh.skinIndexCount = (uint32_t)b.surfaces[0].size();
	mesh.wallIndexCount = (uint32_t)b.surfaces[1].size();
	for (const std::vector<uint32_t>& surface : b.surfaces) {
		mesh.indices.insert(mesh.indices.end(), surface.begin(), surface.end());
	}
	return mesh;
}

//---------------------------------------------------------------- the stack

const MountModelInfo* opticMountModel(const SceneOptic& optic)
{
	static const char* const KINEMATIC[] = { "KM05T/M", "KM100", "KM200" };
	static const char* const FIXED[] = { "LMR05/M", "LMR1/M", "LMR2/M" };
	static const char* const ROTATION[] = { "RSP05/M", "RSP1/M", "RSP2/M" };
	static const char* const IRISES[] = { "ID8/M", "ID12/M", "ID15/M", "ID20/M", "ID25/M", "ID36/M", "ID50/M" };
	static const double IRIS_OPENINGS[] = { 0.008, 0.012, 0.015, 0.020, 0.025, 0.036, 0.050 };
	auto byDiameter = [&](const char* const (&ids)[3]) -> const MountModelInfo* {
		const double d = opticDiameter(optic);
		const double sizes[] = { 0.5 * INCH, INCH, 2.0 * INCH };
		for (int i = 0; i < 3; i++) {
			if (d > 0.0 && d <= sizes[i] + 1e-9) {
				return findMountModel(ids[i]);
			}
		}
		return nullptr;
	};
	switch (optic.mount.kind) {
	case MountKind::Kinematic:
		return byDiameter(KINEMATIC);
	case MountKind::Fixed:
		return byDiameter(FIXED);
	case MountKind::Rotation:
		return byDiameter(ROTATION);
	case MountKind::PrismMount:
		return findMountModel("KM200PM/M");
	case MountKind::PlateHolder:
		return findMountModel("FP01");
	case MountKind::IrisAssembly: {
		if (optic.kind != OpticKind::Iris) {
			return nullptr;
		}
		const double largest = opticNumber(optic, "maxOpening");
		for (size_t i = 0; i < std::size(IRISES); i++) {
			if (largest <= IRIS_OPENINGS[i] + 1e-9) {
				return findMountModel(IRISES[i]);
			}
		}
		return nullptr;
	}
	case MountKind::None:
		return nullptr;
	}
	return nullptr;
}

std::vector<MountKind> allowedMounts(const SceneOptic& optic)
{
	const std::span<const MountKind> mounts = opticTraits(optic.kind).mounts;
	std::vector<MountKind> out(mounts.begin(), mounts.end());
	out.push_back(MountKind::None);
	return out;
}

glm::dmat4 opticWorldTransform(const SceneOptic& optic, const BenchContext& context)
{
	return assembleStack(optic, context).opticToWorld;
}

OpticStack assembleStack(const SceneOptic& optic, const BenchContext& context)
{
	OpticStack stack;
	stack.opticToWorld = optic.mount.freeTransform();
	stack.mount = opticMountModel(optic);
	if (optic.mount.kind == MountKind::None) {
		return stack;
	}
	if (stack.mount == nullptr) {
		stack.notes.push_back(fmt::format("No {} mount holds this size: the optic is placed freely", mountKindLabel(optic.mount.kind)));
		return stack;
	}
	const OpticPrescription body = opticBody(optic, context);
	const StackFrames frames = stackFrames(optic, context, body);
	if (frames.mount == nullptr || !frames.seat) {
		stack.notes.push_back(fmt::format("{} has no seat for this optic: it is placed freely", stack.mount->id));
		return stack;
	}
	stack.kinematic = frames.motions.kinematic;
	// the optic in the mount's frame, through the motion of the part it rides on
	const glm::dmat4 opticInMount = frames.motions.of(frames.seat->part) * frames.seat->mountFromBody;
	glm::dmat4 mountWorld(1.0);
	if (optic.mount.freePlacement) {
		// the mount hangs around the free optic; no post, holder or fork
		mountWorld = optic.mount.freeTransform() * glm::inverse(opticInMount);
		stack.opticToWorld = optic.mount.freeTransform();
	} else {
		const MountPose& pose = optic.mount;
		stack.minimumHeight = frames.entryHeight + (frames.restOrigin.y - frames.tap.y);
		const double height = std::max(pose.height, stack.minimumHeight);
		const double postTop = height - (frames.restOrigin.y - frames.tap.y);
		// the post stands where the origin, turned about its vertical by the yaw, puts it
		const glm::dvec3 reach = transformVector(rotationY(pose.yaw), frames.restOrigin - frames.tap);
		const glm::dvec2 post = pose.origin - glm::dvec2(reach.x, reach.z);
		mountWorld = translation(glm::dvec3(post.x, postTop, post.y)) * rotationY(pose.yaw) * translation(-frames.tap);
		stack.opticToWorld = mountWorld * opticInMount;
		stack.post = true;
		stack.postAxis = post;
		stack.postBottom = glm::dvec3(post.x, frames.postBottom, post.y);
		stack.postTop = glm::dvec3(post.x, postTop, post.y);
		stack.holderTop = frames.entryHeight;
		// the holder faces away from the fork, its thumbscrew opposite the slot, as example mirror.FCStd has it
		if (frames.holder != nullptr) {
			const MountMate* base = frames.holder->findMate("base_contact");
			const glm::dmat4 holderWorld = translation(glm::dvec3(post.x, 0.0, post.y)) * rotationY(pose.forkAngle + PI)
				* translation(-(base != nullptr ? base->origin : glm::dvec3(0.0)));
			for (const MountPart& part : frames.holder->parts) {
				stack.parts.push_back(StackPart { frames.holder->info, part.name, StackRole::PostHolder, holderWorld, true });
			}
		}
		stack.holderCenter = post;
		stack.holderRadius = holderRadiusOf(frames.holder);
		if (frames.fork != nullptr) {
			const MountMate* seat = frames.fork->findMate("holder_seat");
			const MountMate* bolt = frames.fork->findMate("bolt");
			const glm::dmat4 world = forkWorld(post, pose.forkAngle) * translation(-(seat != nullptr ? seat->origin : glm::dvec3(0.0)));
			for (const MountPart& part : frames.fork->parts) {
				glm::dmat4 m = world;
				// the bolt slides along the slot to the reach
				if (bolt != nullptr && part.name == bolt->part) {
					m = world * translation(glm::dvec3(0.0, 0.0, pose.forkReach - bolt->origin.z));
				}
				stack.parts.push_back(StackPart { frames.fork->info, part.name, StackRole::Fork, m, true });
			}
		}
		stack.forkOutline = forkOutlineAt(frames.fork, post, pose.forkAngle);
		stack.bolt = post + pose.forkReach * glm::dvec2(std::sin(pose.forkAngle), std::cos(pose.forkAngle));
	}
	for (const MountPart& part : frames.mount->parts) {
		if (std::find(frames.motions.hidden.begin(), frames.motions.hidden.end(), part.name) != frames.motions.hidden.end()) {
			continue;
		}
		// an iris's blades are what its optic is: the tracer has their sheet, so their meshes are the preview's alone
		const bool traced = !(frames.mount->info->kind == MountModelKind::Iris && part.name.rfind("blade", 0) == 0);
		stack.parts.push_back(StackPart { frames.mount->info, part.name, StackRole::Mount, mountWorld * frames.motions.of(part.name), traced });
	}
	stack.origin = transformPoint(mountWorld, frames.restOrigin);
	if (stack.kinematic) {
		const glm::dmat4 rest = mountWorld * frames.seat->mountFromBody;
		stack.kinematic->datumShift = glm::dvec3(stack.opticToWorld[3]) - glm::dvec3(rest[3]);
	}
	// a prism wider than its platform overhangs it, which a real prism clamped on KM200PM/M can
	if (frames.mount->info->kind == MountModelKind::PrismMount) {
		const OpticBoundsT<double> platform = partBounds(*frames.mount, "plate", 1e-3);
		const OpticBoundsT<double>& b = frames.seat->bounds;
		if (!platform.empty() && (b.min.x < platform.min.x - 1e-4 || b.max.x > platform.max.x + 1e-4 || b.min.z < platform.min.z - 1e-4 || b.max.z > platform.max.z + 1e-4)) {
			stack.notes.push_back(fmt::format("The prism overhangs the {:.0f} x {:.0f} mm platform", (platform.max.x - platform.min.x) * 1e3, (platform.max.z - platform.min.z) * 1e3));
		}
	}
	return stack;
}

std::vector<OpticStack> assembleBench(std::span<const SceneOptic> optics, const BenchContext& context)
{
	std::vector<OpticStack> stacks;
	stacks.reserve(optics.size());
	for (const SceneOptic& optic : optics) {
		stacks.push_back(assembleStack(optic, context));
	}
	markForkClashes(stacks, context.settings);
	return stacks;
}

void markForkClashes(std::vector<OpticStack>& stacks, const BenchSettings& bench)
{
	for (size_t i = 0; i < stacks.size(); i++) {
		OpticStack& stack = stacks[i];
		if (!stack.post) {
			continue;
		}
		// the bolt must be in a hole, and the fork must clear every other stack's fork and holder
		stack.forkBlocked = !isHole(bench, stack.bolt, 1e-6);
		for (size_t j = 0; j < stacks.size() && !stack.forkBlocked; j++) {
			if (j != i && stacks[j].post
				&& (quadsOverlap(stack.forkOutline, stacks[j].forkOutline, CLASH_MARGIN)
					|| quadCircleOverlap(stack.forkOutline, stacks[j].holderCenter, stacks[j].holderRadius, CLASH_MARGIN))) {
				stack.forkBlocked = true;
			}
		}
	}
}

double minimumHeight(const SceneOptic& optic, const BenchContext& context)
{
	if (optic.mount.isFree()) {
		return 0.0;
	}
	return assembleStack(optic, context).minimumHeight;
}

ForkChoice chooseFork(glm::dvec2 post, double previousAngle, const BenchContext& context, std::span<const OpticStack> others)
{
	const MountModel* fork = lookup(context, CLAMPING_FORK_MODEL);
	const glm::dvec2 reach = forkReachRange(fork);
	const BenchGrid grid = benchGrid(context.settings);
	struct Candidate {
		bool axis;
		double distance;
		double offAxis;
		double fromPrevious;
		double angle;
	};
	std::vector<Candidate> candidates;
	const int span = (int)std::ceil(reach.y / BREADBOARD_PITCH) + 1;
	const glm::ivec2 center = glm::ivec2(glm::round((post - grid.first) / BREADBOARD_PITCH));
	for (int i = center.x - span; i <= center.x + span; i++) {
		for (int j = center.y - span; j <= center.y + span; j++) {
			if (i < 0 || j < 0 || i >= grid.count.x || j >= grid.count.y) {
				continue;
			}
			const glm::dvec2 hole = grid.first + BREADBOARD_PITCH * glm::dvec2(i, j);
			const glm::dvec2 d = hole - post;
			const double distance = glm::length(d);
			if (distance < reach.x - 1e-9 || distance > reach.y + 1e-9) {
				continue;
			}
			const double angle = std::atan2(d.x, d.y);
			double offAxis = 1e30;
			for (int k = -2; k <= 2; k++) {
				offAxis = std::min(offAxis, std::abs(wrapAngle(angle - 0.5 * PI * k)));
			}
			candidates.push_back(Candidate { std::abs(d.x) < 1e-9 || std::abs(d.y) < 1e-9, distance, offAxis, std::abs(wrapAngle(angle - previousAngle)), angle });
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		if (a.axis != b.axis) {
			return a.axis;
		}
		if (std::abs(a.distance - b.distance) > 1e-9) {
			return a.distance < b.distance;
		}
		if (std::abs(a.offAxis - b.offAxis) > 1e-9) {
			return a.offAxis < b.offAxis;
		}
		if (std::abs(a.fromPrevious - b.fromPrevious) > 1e-9) {
			return a.fromPrevious < b.fromPrevious;
		}
		return a.angle < b.angle;
	});
	ForkChoice choice;
	if (candidates.empty()) {
		return choice;
	}
	choice.found = true;
	for (const Candidate& c : candidates) {
		if (!forkClashes(forkOutlineAt(fork, post, c.angle), others)) {
			choice.angle = c.angle;
			choice.reach = c.distance;
			return choice;
		}
	}
	choice.angle = candidates.front().angle;
	choice.reach = candidates.front().distance;
	choice.blocked = true;
	return choice;
}

void placeOnBench(SceneOptic& optic, glm::dvec2 origin, double height, double yaw, bool snap, const BenchContext& context, std::span<const OpticStack> others)
{
	MountPose& pose = optic.mount;
	pose.origin = snap ? nearestHole(context.settings, origin) : origin;
	pose.yaw = wrapAngle(yaw);
	pose.height = std::max(height, minimumHeight(optic, context));
	const ForkChoice fork = chooseFork(assembleStack(optic, context).postAxis, pose.forkAngle, context, others);
	if (fork.found) {
		pose.forkAngle = fork.angle;
		pose.forkReach = fork.reach;
	}
}

bool refitFork(SceneOptic& optic, const BenchContext& context, std::span<const OpticStack> others)
{
	if (optic.mount.isFree()) {
		return false;
	}
	const OpticStack stack = assembleStack(optic, context);
	if (!stack.post || (isHole(context.settings, stack.bolt, 1e-9) && !forkClashes(stack.forkOutline, others))) {
		return false;
	}
	const ForkChoice fork = chooseFork(stack.postAxis, optic.mount.forkAngle, context, others);
	if (!fork.found || (fork.angle == optic.mount.forkAngle && fork.reach == optic.mount.forkReach)) {
		return false;
	}
	optic.mount.forkAngle = fork.angle;
	optic.mount.forkReach = fork.reach;
	return true;
}

void dropOntoBench(SceneOptic& optic, const BenchContext& context, std::span<const OpticStack> others)
{
	const glm::dmat4 world = optic.mount.freeTransform();
	MountPose& pose = optic.mount;
	pose.freePlacement = false;
	pose.fine = FineAdjust {};
	const MountModelInfo* info = opticMountModel(optic);
	const MountModel* model = info != nullptr ? lookup(context, info->id) : nullptr;
	if (model == nullptr) {
		return;
	}
	const OpticPrescription body = opticBody(optic, context);
	const std::optional<Seat> seat = opticSeat(optic, *model, body);
	if (!seat) {
		return;
	}
	// the mount's front in world space gives the yaw; the origin stays where it is
	const glm::dmat4 mountWorld = world * glm::inverse(seat->mountFromBody);
	const glm::dvec3 front = transformVector(mountWorld, glm::dvec3(0.0, 0.0, 1.0));
	if (std::hypot(front.x, front.z) > 1e-9) {
		pose.yaw = std::atan2(front.x, front.z);
	}
	const glm::dvec3 target = transformPoint(world, opticOrigin(optic, body));
	placeOnBench(optic, glm::dvec2(target.x, target.z), target.y, pose.yaw, false, context, others);
}

glm::dvec3 opticOrigin(const SceneOptic& optic, const OpticPrescription& body)
{
	switch (optic.kind) {
	// the front face's center: what the reflection is defined by
	case OpticKind::PlanarMirror:
	case OpticKind::ConcaveMirror:
	case OpticKind::OffAxisParabola:
	// the box center (a cube, a right-angle prism), the centroid (an equilateral prism), the blades' sheet
	case OpticKind::CubeBeamsplitter:
	case OpticKind::RightAnglePrism:
	case OpticKind::EquilateralPrism:
	case OpticKind::Iris:
		return glm::dvec3(0.0);
	default:
		// halfway from the front, at z = 0, to the back face's center on the axis: surface 1 of every such kind
		return glm::dvec3(0.0, 0.0, body.surfaces.size() > 1 ? 0.5 * body.surfaces[1].placement.origin.z : 0.0);
	}
}

glm::dvec3 opticOrigin(const SceneOptic& optic, const BenchContext& context)
{
	return opticOrigin(optic, opticBody(optic, context));
}

//---------------------------------------------------------------- the adjusters

std::optional<KinematicGeometry> kinematicGeometry(const MountModel& model)
{
	const MountMate* pivot = model.findMate("pivot");
	const MountMate* tipX = model.findMate("adjuster_x_tip");
	const MountMate* tipY = model.findMate("adjuster_y_tip");
	if (pivot == nullptr || tipX == nullptr || tipY == nullptr) {
		return std::nullopt;
	}
	KinematicGeometry g;
	g.pivot = pivot->origin;
	g.contactX = tipX->origin;
	g.contactY = tipY->origin;
	g.screwX = glm::normalize(tipX->z);
	g.screwY = glm::normalize(tipY->z);
	const glm::dvec3 axisX = glm::normalize(g.contactY - g.pivot);
	const glm::dvec3 axisY = glm::normalize(g.contactX - g.pivot);
	g.leverX = glm::dot(glm::cross(axisX, g.contactX - g.pivot), g.screwX);
	g.leverY = glm::dot(glm::cross(axisY, g.contactY - g.pivot), g.screwY);
	// Thorlabs' angle per revolution over the modeled lever; the thread's own pitch where the mates give no angle
	auto pitchOf = [](const MountMate& tip, double lever) {
		const auto angle = tip.params.find("angle_per_rev");
		if (angle != tip.params.end()) {
			return angle->second * std::abs(lever);
		}
		const auto pitch = tip.params.find("pitch");
		return pitch != tip.params.end() ? pitch->second : 0.0;
	};
	g.pitchX = pitchOf(*tipX, g.leverX);
	g.pitchY = pitchOf(*tipY, g.leverY);
	for (const MountMotion& motion : model.motions) {
		if (motion.kind == MountMotionKind::Tilt) {
			g.tiltRange = std::max(std::abs(motion.rangeMin), std::abs(motion.rangeMax));
		}
	}
	return g;
}

KinematicPose solveKinematic(const KinematicGeometry& g, double turnsX, double turnsY, glm::dvec2 start)
{
	const glm::dvec3 axisX = glm::normalize(g.contactY - g.pivot);
	const glm::dvec3 axisY = glm::normalize(g.contactX - g.pivot);
	const glm::dvec3 rx = g.contactX - g.pivot;
	const glm::dvec3 ry = g.contactY - g.pivot;
	const double sx = turnsX * g.pitchX;
	const double sy = turnsY * g.pitchY;
	KinematicPose pose;
	double alpha = start.x;
	double beta = start.y;
	pose.converged = false;
	for (int iteration = 0; iteration < 50; iteration++) {
		const glm::dmat3 ra = rotation3(axisX, alpha);
		const glm::dmat3 rb = rotation3(axisY, beta);
		const glm::dmat3 r = ra * rb;
		// each contact's travel along its screw, against the screw's advance
		const double fx = glm::dot(g.screwX, r * rx - rx) - sx;
		const double fy = glm::dot(g.screwY, r * ry - ry) - sy;
		const glm::dmat3 da = skew(axisX) * r;
		const glm::dmat3 db = ra * skew(axisY) * rb;
		const double j00 = glm::dot(g.screwX, da * rx);
		const double j01 = glm::dot(g.screwX, db * rx);
		const double j10 = glm::dot(g.screwY, da * ry);
		const double j11 = glm::dot(g.screwY, db * ry);
		const double det = j00 * j11 - j01 * j10;
		if (std::abs(det) < 1e-30) {
			break;
		}
		const double stepA = (fx * j11 - fy * j01) / det;
		const double stepB = (fy * j00 - fx * j10) / det;
		alpha -= stepA;
		beta -= stepB;
		if (std::abs(stepA) < 1e-16 && std::abs(stepB) < 1e-16) {
			pose.converged = true;
			break;
		}
		if (std::abs(fx) < 1e-18 && std::abs(fy) < 1e-18) {
			pose.converged = true;
			break;
		}
	}
	pose.alpha = alpha;
	pose.beta = beta;
	const glm::dmat3 r = rotation3(axisX, alpha) * rotation3(axisY, beta);
	pose.plate = translation(g.pivot) * glm::dmat4(r) * translation(-g.pivot);
	return pose;
}

glm::dvec2 adjusterTurnLimits(const KinematicGeometry& g)
{
	const glm::dvec3 axisX = glm::normalize(g.contactY - g.pivot);
	const glm::dvec3 axisY = glm::normalize(g.contactX - g.pivot);
	auto limit = [&](const glm::dvec3& axis, const glm::dvec3& contact, const glm::dvec3& screw, double pitch) {
		if (pitch <= 0.0) {
			return 0.0;
		}
		const glm::dvec3 r = contact - g.pivot;
		const double plus = glm::dot(screw, rotation3(axis, g.tiltRange) * r - r);
		const double minus = glm::dot(screw, rotation3(axis, -g.tiltRange) * r - r);
		return std::min(std::abs(plus), std::abs(minus)) / pitch;
	};
	return glm::dvec2(limit(axisX, g.contactX, g.screwX, g.pitchX), limit(axisY, g.contactY, g.screwY, g.pitchY));
}
