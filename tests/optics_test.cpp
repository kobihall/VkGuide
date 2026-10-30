// CPU verification of the optical bodies, glasses, coatings and elements (rt_optics; docs/plans/completed/physoptics-optical-
// surfaces.md, -materials.md and -elements.md). No Vulkan, no GPU.
//
//  - surfaces: every shape's float hit against a long double reference (the same roots, then Newton's method on the
//    implicit function) over random and grazing rays and curvatures from 1e-6 to 1e3 per meter; normals against a
//    finite-difference gradient; principal curvatures against finite differences of the normal
//  - closed bodies: random lines through every element's body cross it an even number of times
//  - the lensmaker's equation against a real ray at 1e-4 of the aperture
//  - glasses: the bundled catalog against its own nd and vd, N-BK7 against Schott's data sheet, air at 633 nm
//  - coatings: uncoated against frDielectric(), Brewster's angle, total internal reflection, a quarter-wave MgF2 layer,
//    quarter-wave stacks, energy and reciprocity of lossless stacks, bare silver, and the GPU table's interpolation
//  - elements: every kind at every size builds and closes, its bounds hold it, the off-axis parabola focuses, the wedges
//    deviate, the lens readout matches a real ray, the right-angle prism turns and retro-reflects
//
// Run from the repository root:  ./bin/optics_test [--rays N] [--root dir] [--selfcheck]

#include <optic_catalog.h>
#include <optic_coating.h>
#include <optic_glass.h>
#include <optic_library.h>
#include <optic_mesh.h>
#include <optic_surface.h>

#include <bxdf.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>

#include <fmt/format.h>

namespace {

constexpr double PI = 3.14159265358979323846;

int g_failures = 0;
// --selfcheck: errors planted in the references, which every check below must catch
bool g_plant = false;

std::mt19937_64 g_rng(20261029);

double uniform()
{
	return std::uniform_real_distribution<double>(0.0, 1.0)(g_rng);
}

void check(bool ok, const std::string& what)
{
	if (!ok) {
		g_failures++;
		fmt::println("  FAIL {}", what);
	}
}

glm::dvec3 randomDirection()
{
	const double z = 2.0 * uniform() - 1.0;
	const double phi = 2.0 * PI * uniform();
	const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
	return glm::dvec3(r * std::cos(phi), r * std::sin(phi), z);
}

OpticalLibrary g_library;
bool g_verboseWalk = false;

OpticBuildContext context()
{
	OpticBuildContext c;
	c.library = &g_library;
	return c;
}

//---------------------------------------------------------------- surfaces

template <typename To, typename From>
OpticSurfaceT<To> castSurface(const OpticSurfaceT<From>& s)
{
	OpticSurfaceT<To> t;
	t.shape = s.shape;
	t.curvature = (To)s.curvature;
	t.conic = (To)s.conic;
	for (size_t i = 0; i < OPTIC_ASPHERE_TERMS; i++) {
		t.asphere[i] = (To)s.asphere[i];
	}
	t.radius = (To)s.radius;
	auto bound = [](const CylinderBoundT<From>& b) {
		CylinderBoundT<To> o;
		o.z0 = (To)b.z0;
		o.slope = Vec2T<To>(b.slope);
		o.curvature = (To)b.curvature;
		o.conic = (To)b.conic;
		o.center = Vec2T<To>(b.center);
		return o;
	};
	t.lower = bound(s.lower);
	t.upper = bound(s.upper);
	t.aperture.kind = s.aperture.kind;
	t.aperture.a = (To)s.aperture.a;
	t.aperture.b = (To)s.aperture.b;
	t.aperture.center = Vec2T<To>(s.aperture.center);
	for (size_t i = 0; i < OPTIC_POLYGON_VERTICES; i++) {
		t.aperture.polygon[i] = Vec2T<To>(s.aperture.polygon[i]);
	}
	for (size_t i = 0; i < 3; i++) {
		t.rows[i] = Vec4T<To>(s.rows[i]);
	}
	return t;
}

// the implicit function a surface's points zero, in long double
long double implicitF(const OpticSurfaceT<long double>& s, const Vec3T<long double>& p)
{
	if (s.shape == SurfaceShape::Cylinder) {
		return p.x * p.x + p.y * p.y - s.radius * s.radius;
	}
	if (s.shape == SurfaceShape::EvenAsphere) {
		return p.z - *surfaceSag(s, Vec2T<long double>(p.x, p.y));
	}
	const long double kz = 1.0L + s.conic;
	return s.curvature * (p.x * p.x + p.y * p.y + kz * p.z * p.z) - 2.0L * p.z;
}

// Newton's method on F(o + t d) from t, in long double
long double polishRoot(const OpticSurfaceT<long double>& s, const Vec3T<long double>& o, const Vec3T<long double>& d, long double t)
{
	for (int i = 0; i < 30; i++) {
		const long double h = 1e-9L * std::max(1.0L, std::abs(t));
		const long double f = implicitF(s, o + t * d);
		const long double df = (implicitF(s, o + (t + h) * d) - implicitF(s, o + (t - h) * d)) / (2.0L * h);
		if (df == 0.0L) {
			break;
		}
		const long double step = f / df;
		t -= step;
		if (std::abs(step) < 1e-18L * std::max(1.0L, std::abs(t))) {
			break;
		}
	}
	return t;
}

void testSurfaceHits(int rays)
{
	fmt::println("surfaces: float hits against a long double reference");
	struct Case {
		const char* name;
		OpticSurface surface;
	};
	std::vector<Case> cases;
	for (const double c : { 1e-6, 1e-3, 1.0, 20.0, 1e3 }) {
		for (const double k : { 0.0, -1.0, -2.5, 0.5 }) {
			OpticSurface s;
			s.curvature = c;
			s.conic = k;
			// an aperture the conic reaches: within its vertex branch
			const double limit = (1.0 + k) > 0.0 ? 0.9 / (c * std::sqrt(1.0 + k)) : 1.0;
			s.aperture.kind = ApertureKind::Circle;
			s.aperture.a = std::min({ 0.025, limit, 0.9 / c });
			finishSurface(s);
			cases.push_back({ "conic", s });
		}
	}
	{
		OpticSurface s;
		s.curvature = 10.0;
		s.shape = SurfaceShape::EvenAsphere;
		s.asphere[0] = 50.0;
		s.asphere[1] = -2e4;
		s.aperture.kind = ApertureKind::Circle;
		s.aperture.a = 0.02;
		finishSurface(s);
		cases.push_back({ "even asphere", s });
	}
	{
		OpticSurface s;
		s.shape = SurfaceShape::Cylinder;
		s.radius = 0.0127;
		s.lower.z0 = -0.003;
		s.upper.z0 = 0.004;
		s.upper.curvature = 20.0;
		s.aperture.kind = ApertureKind::None;
		finishSurface(s);
		cases.push_back({ "cylinder", s });
	}

	for (const Case& c : cases) {
		const OpticSurfaceT<float> sf = castSurface<float>(c.surface);
		const OpticSurfaceT<long double> sl = castSurface<long double>(c.surface);
		// the surface's size: its aperture, or its height where that is larger (a steep paraboloid)
		double size = c.surface.shape == SurfaceShape::Cylinder ? c.surface.radius : c.surface.aperture.a;
		if (c.surface.shape != SurfaceShape::Cylinder) {
			size = std::max(size, std::abs(*surfaceSag(c.surface, glm::dvec2(c.surface.aperture.a, 0.0))));
		}
		int mismatches = 0;
		int compared = 0;
		double worst = 0.0;
		for (int i = 0; i < rays; i++) {
			// a target on the surface inside 0.98 of the aperture, and a ray at it from a random direction; one in eight
			// grazing, within 2 degrees of the tangent plane
			glm::dvec3 target;
			if (c.surface.shape == SurfaceShape::Cylinder) {
				const double phi = 2.0 * PI * uniform();
				const glm::dvec2 xy = c.surface.radius * glm::dvec2(std::cos(phi), std::sin(phi));
				const double lo = boundZ(c.surface.lower, xy);
				const double hi = boundZ(c.surface.upper, xy);
				target = glm::dvec3(xy, lo + (0.02 + 0.96 * uniform()) * (hi - lo));
			} else {
				const double r = 0.98 * size * std::sqrt(uniform());
				const double phi = 2.0 * PI * uniform();
				const glm::dvec2 xy = r * glm::dvec2(std::cos(phi), std::sin(phi));
				target = glm::dvec3(xy, *surfaceSag(c.surface, xy));
			}
			const glm::dvec3 normal = surfaceNormalLocal(c.surface, target);
			glm::dvec3 d = randomDirection();
			if (i % 8 == 0) {
				const glm::dvec3 tangent = glm::normalize(glm::cross(normal, randomDirection()));
				const double angle = (2.0 * uniform()) * PI / 180.0;
				d = glm::normalize(std::cos(angle) * tangent - std::sin(angle) * normal * (uniform() < 0.5 ? 1.0 : -1.0));
			}
			const double distance = 0.5 * size + size * uniform();
			const glm::dvec3 o = target - distance * d;
			// the reference: the long double roots of the float-rounded ray, polished on the implicit function, so the
			// ray's own rounding is not charged to the intersector
			const glm::vec3 of(o);
			const glm::vec3 df(d);
			long double tl;
			const Vec3T<long double> ol(of);
			const Vec3T<long double> dl(df);
			if (!intersectSurfaceLocal(sl, ol, dl, 1e-12L, 1e30L, tl)) {
				continue;
			}
			tl = polishRoot(sl, ol, dl, tl);
			float tf;
			compared++;
			const bool hitF = intersectSurfaceLocal(sf, of, df, 1e-12f, 1e30f, tf);
			// A grazing ray (within 3 degrees of the tangent plane at the reference hit, or within 1e-4 of the size of the
			// aperture's or the bounds' edge) is where float runs out: it may miss, or take a root further on. Its hits, when
			// found, must still be on the surface
			const glm::dvec3 referencePoint(ol + tl * dl);
			const glm::dvec3 referenceNormal = surfaceNormalLocal(c.surface, referencePoint);
			bool grazing = std::abs(glm::dot(glm::normalize(glm::dvec3(df)), referenceNormal)) < std::sin(3.0 * PI / 180.0);
			if (c.surface.shape == SurfaceShape::Cylinder) {
				const glm::dvec2 xy(referencePoint);
				grazing |= std::min(referencePoint.z - boundZ(c.surface.lower, xy), boundZ(c.surface.upper, xy) - referencePoint.z) < 1e-4 * size;
			} else {
				grazing |= c.surface.aperture.a - glm::length(glm::dvec2(referencePoint)) < 1e-4 * size;
			}
			// how far off the surface the float hit lands, |F| / |grad F| in long double at the float's point. Two roots
			// close together (a near-tangent ray) are both on the surface: taking the other one is no error as long as it is
			// within 1e-3 of the size along the ray
			double error = 1e30;
			if (hitF) {
				const Vec3T<long double> pf = ol + (long double)tf * dl;
				const long double h = 1e-9L * (long double)size;
				const Vec3T<long double> gradient((implicitF(sl, pf + Vec3T<long double>(h, 0, 0)) - implicitF(sl, pf - Vec3T<long double>(h, 0, 0))) / (2 * h),
					(implicitF(sl, pf + Vec3T<long double>(0, h, 0)) - implicitF(sl, pf - Vec3T<long double>(0, h, 0))) / (2 * h),
					(implicitF(sl, pf + Vec3T<long double>(0, 0, h)) - implicitF(sl, pf - Vec3T<long double>(0, 0, h))) / (2 * h));
				const long double norm = std::sqrt(gradient.x * gradient.x + gradient.y * gradient.y + gradient.z * gradient.z);
				error = (double)(std::abs(implicitF(sl, pf)) / norm);
				if (std::abs((double)tf - (double)tl) * glm::length(glm::dvec3(df)) > 1e-3 * size && !grazing) {
					error = 1e30;
				}
			}
			const double planted = g_plant && i == 0 ? 1e-3 * size : 0.0;
			const double relative = (error + planted) / size;
			if (relative > 1e-6 && !(grazing && error >= 1e30)) {
				mismatches++;
				if (mismatches <= 3 && !g_plant) {
					fmt::println("    {} c={} k={}: off the surface by {:.2e} of the size, hit {}, grazing {}", c.name, c.surface.curvature, c.surface.conic, relative, hitF, grazing);
				}
			} else {
				worst = std::max(worst, relative);
			}
		}
		check(mismatches == 0 && compared > rays / 2, fmt::format("{} c={} k={}: {} of {} float hits off by more than 1e-6 of the size", c.name, c.surface.curvature, c.surface.conic, mismatches, compared));
		(void)worst;
	}
}

void testNormalsAndCurvatures()
{
	fmt::println("surfaces: normals and principal curvatures against finite differences");
	for (const double c : { 1e-3, 2.0, 50.0 }) {
		for (const double k : { 0.0, -1.0, -3.0, 0.7 }) {
			for (const bool asphere : { false, true }) {
				OpticSurface s;
				s.curvature = c;
				s.conic = k;
				if (asphere) {
					s.shape = SurfaceShape::EvenAsphere;
					s.asphere[0] = 3.0;
					s.asphere[1] = -40.0;
				}
				finishSurface(s);
				const double limit = (1.0 + k) > 0.0 ? 0.8 / (c * std::sqrt(1.0 + k)) : 0.02;
				for (int i = 0; i < 64; i++) {
					const double r = std::min(limit, 0.02) * std::sqrt(uniform());
					const double phi = 2.0 * PI * uniform();
					const glm::dvec2 xy = r * glm::dvec2(std::cos(phi), std::sin(phi));
					const glm::dvec3 p(xy, *surfaceSag(s, xy));
					const double h = 1e-6;
					auto sagAt = [&](glm::dvec2 q) { return *surfaceSag(s, q); };
					const glm::dvec2 grad((sagAt(xy + glm::dvec2(h, 0)) - sagAt(xy - glm::dvec2(h, 0))) / (2 * h), (sagAt(xy + glm::dvec2(0, h)) - sagAt(xy - glm::dvec2(0, h))) / (2 * h));
					const glm::dvec3 fd = glm::normalize(glm::dvec3(-grad, 1.0));
					const glm::dvec3 n = surfaceNormalLocal(s, p);
					const double planted = g_plant && i == 0 ? 1e-3 : 0.0;
					check(glm::length(fd - n) + planted < 1e-6, fmt::format("normal c={} k={} asphere={} at r={}: off by {}", c, k, asphere, r, glm::length(fd - n)));
					// the curvature along each principal direction: kappa = -(dn/ds) . t
					const PrincipalCurvaturesT<double> pc = principalCurvatures(s, p);
					for (const auto& [kappa, dir] : { std::pair { pc.k1, pc.d1 }, std::pair { pc.k2, pc.d2 } }) {
						const double step = 1e-5 * std::min(0.02, 1.0 / c);
						auto onSurface = [&](double sStep) {
							const glm::dvec2 q = xy + sStep * glm::dvec2(dir.x, dir.y) / std::max(glm::length(glm::dvec2(dir.x, dir.y)), 1e-12);
							return glm::dvec3(q, sagAt(q));
						};
						const glm::dvec3 a = onSurface(-step);
						const glm::dvec3 b = onSurface(step);
						const double ds = glm::length(b - a);
						const glm::dvec3 dn = (surfaceNormalLocal(s, b) - surfaceNormalLocal(s, a)) / ds;
						const glm::dvec3 t = glm::normalize(b - a);
						const double fdKappa = -glm::dot(dn, t);
						check(std::abs(fdKappa - kappa) <= 1e-4 * std::max(1.0, std::abs(kappa)), fmt::format("curvature c={} k={} asphere={} at r={}: {} against {}", c, k, asphere, r, kappa, fdKappa));
					}
				}
			}
		}
	}
	// a cylinder bends away from its outward normal around its axis
	OpticSurface cylinder;
	cylinder.shape = SurfaceShape::Cylinder;
	cylinder.radius = 0.01;
	const PrincipalCurvaturesT<double> pc = principalCurvatures(cylinder, glm::dvec3(0.01, 0.0, 0.0));
	check(std::abs(pc.k1 + 100.0) < 1e-9 && pc.k2 == 0.0, "cylinder curvatures");
}

// Walks the line o + t d through a body, keeping whether it is inside: each hit enters (against the outward normal) or
// leaves, and an interface between two media of the body (a cube's diagonal) is not its boundary. Each surface is asked
// for its next hit past its own last one only, so no step can pass over a neighbor's hit. Hits within 1e-6 of the size of
// each other are one event at an edge: both faces of a rim answer, or the line clips a corner (enters and leaves), or it
// passes just outside an edge within the apertures' slack (leaves and enters); an event with both senses changes nothing.
// False when the line ends inside: a gap in the body
template <typename Real>
bool walksThrough(const OpticPrescriptionT<Real>& body, const Vec3T<Real>& o, const Vec3T<Real>& d, Real size, bool& entered, Real* shortestChord = nullptr)
{
	std::vector<Real> last(body.surfaces.size(), -std::numeric_limits<Real>::infinity());
	const Real advance = Real(4) * std::numeric_limits<Real>::epsilon();
	struct Event {
		Real t;
		bool entering;
		size_t surface;
		// within the apertures' slack band outside the true aperture: a phantom hit the slack makes by design, at most
		// a fraction of a micron from an edge, which only counts with another hit of the same edge
		bool band;
	};
	std::vector<Event> events;
	Real t = Real(0);
	for (int guard = 0; guard < 64; guard++) {
		Real best = std::numeric_limits<Real>::infinity();
		size_t which = 0;
		Vec3T<Real> local(0);
		for (size_t i = 0; i < body.surfaces.size(); i++) {
			const OpticSurfaceT<Real>& s = body.surfaces[i];
			const Vec3T<Real> lo = toSurfacePoint(s, o);
			const Vec3T<Real> ld = toSurfaceVector(s, d);
			const Real tMin = std::max(t, last[i] + advance * (size + std::abs(last[i])));
			Real hit;
			if (intersectSurfaceLocal(s, lo, ld, tMin, best, hit) && hit < best) {
				best = hit;
				which = i;
				local = lo + hit * ld;
			}
		}
		if (!(best < std::numeric_limits<Real>::infinity())) {
			break;
		}
		t = best;
		last[which] = best;
		const OpticSurfaceT<Real>& s = body.surfaces[which];
		if ((s.flags & OPTIC_SURFACE_INTERNAL) == 0) {
			bool band = false;
			if (s.shape == SurfaceShape::Cylinder) {
				const Vec2T<Real> xy(local.x, local.y);
				band = local.z < boundZ(s.lower, xy) || local.z > boundZ(s.upper, xy);
			} else {
				band = !insideAperture(s.aperture, Vec2T<Real>(local.x, local.y), Real(0));
			}
			events.push_back({ best, glm::dot(toBodyVector(s, surfaceNormalLocal(s, local)), d) < Real(0), which, band });
			if (g_verboseWalk) {
				fmt::println("      {} s{} t {:.12g} {} local ({:.9g}, {:.9g}, {:.9g})", sizeof(Real) == 4 ? "float " : "double", which, (double)best, events.back().entering ? "enter" : "leave", (double)local.x, (double)local.y, (double)local.z);
			}
		}
	}
	// the shortest chord a curved surface cuts from the line: two hits on it, entering then leaving
	if (shortestChord != nullptr) {
		*shortestChord = std::numeric_limits<Real>::infinity();
		for (size_t i = 0; i + 1 < events.size(); i++) {
			if (events[i].surface == events[i + 1].surface) {
				*shortestChord = std::min(*shortestChord, events[i + 1].t - events[i].t);
			}
		}
	}
	bool inside = false;
	entered = false;
	for (size_t i = 0; i < events.size();) {
		size_t j = i;
		bool enters = false;
		bool leaves = false;
		bool allBand = true;
		while (j < events.size() && events[j].t - events[i].t <= Real(1e-5) * size) {
			enters |= events[j].entering;
			leaves |= !events[j].entering;
			allBand &= events[j].band;
			j++;
		}
		if (allBand && j == i + 1) {
			i = j;
			continue;
		}
		if (enters && !leaves) {
			inside = true;
			entered = true;
		} else if (leaves && !enters) {
			inside = false;
		}
		i = j;
	}
	return !inside;
}

void testClosedBodies(int rays)
{
	fmt::println("bodies: every element at every size builds, closes, and lies in its bounds");
	const OpticBuildContext ctx = context();
	for (size_t k = 0; k < OPTIC_KIND_COUNT; k++) {
		const OpticKind kind = (OpticKind)k;
		const OpticTraits& traits = opticTraits(kind);
		for (int size = 0; size < (int)traits.sizes.size(); size++) {
			SceneOptic optic = makeOptic(kind);
			optic.name = traits.name;
			setOpticSize(optic, size);
			if (kind == OpticKind::SphericalLens) {
				// bi-convex, the plan's case, with radii every size can take
				setOpticValue(optic, "form", 1);
				setOpticValue(optic, "r1", 0.1);
				setOpticValue(optic, "r2", 0.1);
			}
			OpticReadout readout;
			const OpticPrescription body = buildOptic(optic, ctx, readout);
			const std::string label = fmt::format("{} {}", traits.name, traits.sizes[(size_t)size].key);
			check(readout.error.empty() && !body.surfaces.empty(), fmt::format("{}: does not build ({})", label, readout.error));
			if (body.surfaces.empty()) {
				continue;
			}
			// the bounds hold every surface: the preview mesh's points
			const OpticMesh mesh = tessellateOptic(body);
			bool inside = !mesh.vertices.empty();
			for (const OpticMeshVertex& v : mesh.vertices) {
				const glm::dvec3 p(v.position);
				inside &= glm::all(glm::greaterThanEqual(p, body.bounds.min)) && glm::all(glm::lessThanEqual(p, body.bounds.max));
			}
			check(inside, fmt::format("{}: a surface leaves the bounds", label));
			if (!traits.closed) {
				continue;
			}
			const OpticPrescriptionF bodyF = toFloat(body);
			const glm::dvec3 center = 0.5 * (body.bounds.min + body.bounds.max);
			const glm::dvec3 extent = body.bounds.max - body.bounds.min;
			const double scale = glm::length(extent);
			int odd = 0;
			int entered = 0;
			for (int i = 0; i < rays; i++) {
				const glm::dvec3 through = center + (glm::dvec3(uniform(), uniform(), uniform()) - 0.5) * extent;
				const glm::dvec3 d = randomDirection();
				const glm::dvec3 o = through - 2.0 * scale * d;
				bool enteredD = false;
				bool enteredF = false;
				double chord = 0.0;
				const bool okD = walksThrough(body, o, d, scale, enteredD, &chord);
				// float resolves a near-tangent chord only down to about sqrt(epsilon) of the surface's radius: its two roots
				// merge into one. A line the double walk finds cutting a chord shorter than 1e-3 of the size is not asked of float
				const bool okF = chord < 1e-3 * scale || walksThrough(bodyF, glm::vec3(o), glm::vec3(d), (float)scale, enteredF);
				odd += (okD ? 0 : 1) + (okF ? 0 : 1);
				if ((!okD || !okF) && std::getenv("OPTICS_TEST_VERBOSE") != nullptr) {
					fmt::println("    {} line {}: double {} float {}, o ({:.9g}, {:.9g}, {:.9g}) d ({:.9g}, {:.9g}, {:.9g})", label, i, okD, okF, o.x, o.y, o.z, d.x, d.y, d.z);
					g_verboseWalk = true;
					walksThrough(body, o, d, scale, enteredD);
					walksThrough(bodyF, glm::vec3(o), glm::vec3(d), (float)scale, enteredF);
					g_verboseWalk = false;
				}
				entered += enteredD ? 1 : 0;
			}
			if (g_plant) {
				odd++;
			}
			check(odd == 0 && entered > 0, fmt::format("{}: {} of {} lines ended inside the body", label, odd, 2 * rays));
		}
	}
}

//---------------------------------------------------------------- the lensmaker

void testLensmaker()
{
	fmt::println("lens: the lensmaker's equation against a real ray");
	SceneOptic lens = makeOptic(OpticKind::SphericalLens);
	OpticReadout readout;
	const OpticBuildContext ctx = context();
	const OpticPrescription body = buildOptic(lens, ctx, readout);
	const std::vector<double> n = mediumIndices(body, ctx, ctx.designWavelength);
	const double relative = n[1] / n[0];
	const double r1 = opticNumber(lens, "r1");
	// plano-convex: f = R1 / (n - 1), whatever the thickness
	const double paraxial = r1 / (relative - 1.0) * (g_plant ? 1.001 : 1.0);
	const double h = 1e-4 * 0.5 * opticNumber(lens, "diameter");
	OpticRay ray { glm::dvec3(0.0, h, -0.01), glm::dvec3(0.0, 0.0, 1.0) };
	const bool ok = traceEvents(body, n, opticTraits(OpticKind::SphericalLens).ports[0].events, ray);
	const double efl = -h * ray.direction.z / ray.direction.y;
	check(ok && std::abs(efl / paraxial - 1.0) < 1e-9, fmt::format("plano-convex N-BK7: real ray f = {:.12g} m, lensmaker {:.12g} m", efl, paraxial));
	// the readout's paraxial focal length matches a real ray to 1e-4
	double readoutEfl = 0.0;
	for (const auto& [label, value] : readout.lengths) {
		if (label == "effective focal length") {
			readoutEfl = value;
		}
	}
	check(std::abs(readoutEfl / efl - 1.0) < 1e-4, fmt::format("lens readout {} m against the real ray's {} m", readoutEfl, efl));
	// and a bi-concave one diverges as its readout says
	SceneOptic negative = makeOptic(OpticKind::SphericalLens);
	setOpticValue(negative, "form", 3);
	OpticReadout negativeReadout;
	const OpticPrescription negativeBody = buildOptic(negative, ctx, negativeReadout);
	OpticRay negativeRay { glm::dvec3(0.0, h, -0.01), glm::dvec3(0.0, 0.0, 1.0) };
	traceEvents(negativeBody, n, opticTraits(OpticKind::SphericalLens).ports[0].events, negativeRay);
	const double negativeEfl = -h * negativeRay.direction.z / negativeRay.direction.y;
	double negativeReadoutEfl = 0.0;
	for (const auto& [label, value] : negativeReadout.lengths) {
		if (label == "effective focal length") {
			negativeReadoutEfl = value;
		}
	}
	check(negativeEfl < 0.0 && std::abs(negativeReadoutEfl / negativeEfl - 1.0) < 1e-4, fmt::format("bi-concave readout {} m against the real ray's {} m", negativeReadoutEfl, negativeEfl));
}

//---------------------------------------------------------------- glasses

void testGlasses()
{
	fmt::println("glasses: the bundled catalog, N-BK7 and air");
	const OpticGlass* bk7 = g_library.findGlass("N-BK7");
	check(bk7 != nullptr, "N-BK7 is in the bundled catalog");
	if (bk7 != nullptr) {
		const double nd = glassIndex(*bk7, LAMBDA_D) + (g_plant ? 1e-4 : 0.0);
		check(std::abs(nd - 1.51680) < 1e-5, fmt::format("N-BK7 nd = {:.6f}, Schott 1.51680", nd));
		check(std::abs(abbeNumber(*bk7) - 64.17) < 0.005, fmt::format("N-BK7 vd = {:.3f}, Schott 64.17", abbeNumber(*bk7)));
		check(glassAbsorption(*bk7, 550.0) > 0.0 && glassAbsorption(*bk7, 310.0) > 10.0 * glassAbsorption(*bk7, 550.0), "N-BK7 absorbs in the UV, from its IT lines");
	}
	int checked = 0;
	for (const GlassCatalog& catalog : g_library.catalogs()) {
		if (catalog.name != CATALOG_BUNDLED) {
			continue;
		}
		for (const OpticGlass& glass : catalog.glasses) {
			checked++;
			const double nd = glassIndex(glass, LAMBDA_D);
			check(std::abs(nd - glass.catalogNd) < 1e-5, fmt::format("{}: nd {:.6f} against the catalog's {:.5f}", glass.name, nd, glass.catalogNd));
			check(std::abs(abbeNumber(glass) - glass.catalogVd) < 0.01, fmt::format("{}: vd {:.3f} against the catalog's {:.2f}", glass.name, abbeNumber(glass), glass.catalogVd));
		}
	}
	check(checked >= 17, fmt::format("{} bundled glasses read, expected 17", checked));
	const OpticGlass* air = g_library.findGlass(GLASS_AIR);
	check(air != nullptr && std::abs(glassIndex(*air, 633.0) - 1.000276) < 2e-6, fmt::format("air at 633 nm: {:.7f}", air != nullptr ? glassIndex(*air, 633.0) : 0.0));
	check(std::abs(ciddorAirIndex(633.0) - 1.000276) < 2e-6, "Ciddor's air at 633 nm");
	// the ND test glass: an optical density of 1 through 2 mm
	const OpticGlass* nd = g_library.findGlass("ND-OD1");
	check(nd != nullptr && std::abs(std::exp(-glassAbsorption(*nd, 550.0) * 2e-3) - 0.1) < 1e-6, "ND-OD1 transmits 0.1 through 2 mm");
	// a UTF-16 catalog reads as the ASCII one does
	std::string ascii = "NM TEST 2 0 1.5 60 0 1 0\nCD 1.03961212 0.00600069867 0.231792344 0.0200179144 1.01046945 103.560653\n";
	std::string utf16 = "\xFF\xFE";
	for (const char c : ascii) {
		utf16 += c;
		utf16 += '\0';
	}
	std::vector<std::string> warnings;
	const std::optional<GlassCatalog> wide = parseAgf(utf16, "wide", warnings);
	check(wide && wide->glasses.size() == 1 && std::abs(glassIndex(wide->glasses[0], LAMBDA_D) - 1.5168) < 1e-4, "a UTF-16 AGF catalog reads");
}

//---------------------------------------------------------------- coatings

void testCoatings()
{
	fmt::println("coatings: Fresnel, thin films, metals and the GPU table");
	const OpticCoating uncoated;
	const IndexLookup index = g_library.indexLookup();
	// uncoated equals frDielectric from both sides at every angle, and has R = 1 past the critical angle
	double worst = 0.0;
	for (int a = 0; a < 90; a++) {
		const double theta = a * PI / 180.0;
		for (const bool fromPlus : { true, false }) {
			const CoatingResponse r = evaluateCoating(uncoated, index, 550.0, theta, { 1.0, 0.0 }, { 1.5, 0.0 }, fromPlus);
			// frDielectric's eta is inside over outside, its cosine positive from outside
			const float reference = frDielectric((float)std::cos(theta) * (fromPlus ? 1.f : -1.f), 1.5f);
			worst = std::max(worst, std::abs(r.reflectance() - (double)reference));
		}
	}
	check(worst + (g_plant ? 1e-4 : 0.0) < 1e-6, fmt::format("uncoated against frDielectric: worst difference {:.2e}", worst));
	const double brewster = std::atan(1.5);
	const CoatingResponse atBrewster = evaluateCoating(uncoated, index, 550.0, brewster, { 1.0, 0.0 }, { 1.5, 0.0 }, true);
	check(atBrewster.Rp < 1e-12, fmt::format("Rp at Brewster's angle: {:.2e}", atBrewster.Rp));
	const CoatingResponse pastCritical = evaluateCoating(uncoated, index, 550.0, std::asin(1.0 / 1.5) + 0.01, { 1.0, 0.0 }, { 1.5, 0.0 }, false);
	check(pastCritical.Rs > 1.0 - 1e-12 && pastCritical.Rp > 1.0 - 1e-12 && pastCritical.transmittance() == 0.0, "total internal reflection past the critical angle");

	// a quarter-wave MgF2 layer on N-BK7 at its design wavelength, normal incidence
	OpticCoating mgf2;
	mgf2.kind = CoatingKind::Stack;
	mgf2.designWavelength = 550.0;
	mgf2.layers = { CoatingLayer { "MGF2", 0.0, 1.0 } };
	const double n0 = 1.0;
	const double n1 = index("MGF2", 550.0).real();
	const double ns = index("N-BK7", 550.0).real();
	const CoatingResponse ar = evaluateCoating(mgf2, index, 550.0, 0.0, { n0, 0.0 }, index("N-BK7", 550.0), true);
	const double expected = std::pow((n0 * ns - n1 * n1) / (n0 * ns + n1 * n1), 2.0);
	check(std::abs(ar.reflectance() - expected) < 1e-9 + (g_plant ? -1.0 : 0.0), fmt::format("quarter-wave MgF2 on N-BK7: R {:.9f}, closed form {:.9f}", ar.reflectance(), expected));

	// quarter-wave stacks of N pairs reach the closed-form peak. The closed form is for (L H)^N from the air side, the high
	// index against the substrate
	for (int pairs = 1; pairs <= 8; pairs++) {
		OpticCoating mirror;
		mirror.kind = CoatingKind::Stack;
		mirror.designWavelength = 600.0;
		for (int i = 0; i < pairs; i++) {
			mirror.layers.push_back({ "SIO2", 0.0, 1.0 });
			mirror.layers.push_back({ "TIO2", 0.0, 1.0 });
		}
		const double nH = index("TIO2", 600.0).real();
		const double nL = index("SIO2", 600.0).real();
		const double s = index("N-BK7", 600.0).real();
		const double ratio = std::pow(nH / nL, 2.0 * pairs);
		const double closed = std::pow((s - n0 * ratio) / (s + n0 * ratio), 2.0);
		const CoatingResponse r = evaluateCoating(mirror, index, 600.0, 0.0, { 1.0, 0.0 }, { s, 0.0 }, true);
		check(std::abs(r.reflectance() - closed) < 1e-9, fmt::format("{} quarter-wave pairs: R {:.9f}, closed form {:.9f}", pairs, r.reflectance(), closed));
	}

	// every lossless stack conserves energy, and T is the same from either side
	for (const OpticCoating& coating : presetCoatings()) {
		if (coating.kind != CoatingKind::Stack) {
			continue;
		}
		double energy = 0.0;
		double reciprocity = 0.0;
		for (int i = 0; i < 200; i++) {
			const double lambda = 400.0 + 900.0 * uniform();
			const double theta = 1.4 * uniform();
			// real indices: the films' and the glass's absorption off, as a lossless stack has
			auto lossless = [&](std::string_view m, double l) { return std::complex<double>(index(m, l).real(), 0.0); };
			const std::complex<double> glass = lossless("N-BK7", lambda);
			const CoatingResponse a = evaluateCoating(coating, lossless, lambda, theta, { 1.0, 0.0 }, glass, true);
			energy = std::max({ energy, std::abs(a.Rs + a.Ts - 1.0), std::abs(a.Rp + a.Tp - 1.0) });
			// the same ray backwards: from the glass at the refracted angle
			const double back = std::asin(std::sin(theta) / glass.real());
			const CoatingResponse b = evaluateCoating(coating, lossless, lambda, back, { 1.0, 0.0 }, glass, false);
			reciprocity = std::max({ reciprocity, std::abs(a.Ts - b.Ts), std::abs(a.Tp - b.Tp) });
		}
		check(energy < 1e-9, fmt::format("{}: R + T - 1 up to {:.2e}", coating.key, energy));
		check(reciprocity < 1e-9, fmt::format("{}: T forward and back differ by {:.2e}", coating.key, reciprocity));
	}

	// bare silver from pbrt's eta and k
	OpticCoating silver;
	silver.kind = CoatingKind::Metal;
	silver.metal = "Ag";
	const double rAg = evaluateCoating(silver, index, 633.0, 0.0, { 1.0, 0.0 }, { 1.5, 0.0 }, true).reflectance();
	check(rAg >= 0.95 && rAg <= 0.97, fmt::format("bare silver at 633 nm reflects {:.4f}", rAg));

	// the GPU table against the exact transfer matrix between its samples
	const OpticCoating* bsw = g_library.findCoating("thorlabs_BSW");
	check(bsw != nullptr, "the BSW preset");
	if (bsw != nullptr) {
		std::vector<float> lambdas;
		for (int l = 400; l <= 800; l++) {
			lambdas.push_back((float)l);
		}
		const CoatingTable table = coatingTable(*bsw, index, "AIR", "N-BK7", lambdas);
		double tableError = 0.0;
		for (int i = 0; i < 2000; i++) {
			const double lambda = 400.0 + 400.0 * uniform();
			const double degreesIn = 85.0 * uniform();
			const bool fromPlus = uniform() < 0.5;
			const double nIncident = index(fromPlus ? "AIR" : "N-BK7", lambda).real();
			const double nOther = index(fromPlus ? "N-BK7" : "AIR", lambda).real();
			const glm::vec4 looked = lookupCoatingTable(table, fromPlus ? 0u : 1u, (float)(lambda - 400.0), (float)std::cos(degreesIn * PI / 180.0), (float)(nIncident / nOther));
			const CoatingResponse exact = evaluateCoating(*bsw, index, lambda, degreesIn * PI / 180.0, index("AIR", lambda), index("N-BK7", lambda), fromPlus);
			tableError = std::max(tableError, std::abs(0.5 * (looked.x + looked.y) - exact.reflectance()));
		}
		check(tableError + (g_plant ? 1e-2 : 0.0) < 1e-3, fmt::format("the GPU table's R against the transfer matrix: worst {:.2e}", tableError));
	}
	// an ideal splitter splits evenly
	const OpticCoating* half = g_library.findCoating("ideal_50_50");
	check(half != nullptr && half->rs == 0.5 && half->tp == 0.5, "the ideal 50:50 splitter");
	// a CSV curve in percent
	std::string error;
	const std::optional<OpticCoating> csv = readCoatingCsv("Wavelength (nm),%R\n400,10\n500,50\n600,90\n", false, 0.0, &error);
	check(csv && std::abs(evaluateCoating(*csv, index, 550.0, 0.0, { 1, 0 }, { 1.5, 0 }, true).reflectance() - 0.7) < 1e-9, "a CSV reflectance curve in percent");
}

//---------------------------------------------------------------- elements

void testElements()
{
	fmt::println("elements: focus, deviation, turning and retro-reflection");
	const OpticBuildContext ctx = context();
	// the off-axis parabola focuses a collimated bundle along -z at RFL from its datum, at theta to the parent axis
	for (int angle = 0; angle < 5; angle++) {
		for (int size = 0; size < 3; size++) {
			SceneOptic oap = makeOptic(OpticKind::OffAxisParabola);
			setOpticSize(oap, size);
			setOpticValue(oap, "offAxisAngle", angle);
			OpticReadout readout;
			const OpticPrescription body = buildOptic(oap, ctx, readout);
			const double theta = std::array { 15.0, 30.0, 45.0, 60.0, 90.0 }[(size_t)angle] * PI / 180.0;
			const double rfl = opticNumber(oap, "reflectedFocalLength");
			const glm::dvec3 focus(-rfl * std::sin(theta), 0.0, rfl * std::cos(theta));
			const double radius = 0.5 * opticNumber(oap, "diameter");
			double worst = 0.0;
			const std::vector<double> n = mediumIndices(body, ctx, ctx.designWavelength);
			for (int i = 0; i < 200; i++) {
				const double r = 0.95 * radius * std::sqrt(uniform());
				const double phi = 2.0 * PI * uniform();
				OpticRay ray { glm::dvec3(r * std::cos(phi), r * std::sin(phi), 0.5), glm::dvec3(0.0, 0.0, -1.0) };
				if (!traceEvents(body, n, opticTraits(OpticKind::OffAxisParabola).ports[0].events, ray)) {
					worst = 1e30;
					break;
				}
				// the reflected ray's closest approach to the focus
				const glm::dvec3 toFocus = focus - ray.origin;
				const double miss = glm::length(toFocus - glm::dot(toFocus, ray.direction) * ray.direction);
				worst = std::max(worst, miss);
			}
			check(worst + (g_plant ? 1e-5 : 0.0) < 1e-6, fmt::format("OAP {} deg size {}: rays miss the focus by up to {:.3e} m", theta * 180.0 / PI, size, worst));
		}
	}
	// every wedge deviates a normally incident ray by its nominal deviation at the design wavelength
	for (const OpticKind kind : { OpticKind::RoundWedge, OpticKind::PlateWedge }) {
		for (int deviation = 0; deviation < 4; deviation++) {
			SceneOptic wedge = makeOptic(kind);
			setOpticValue(wedge, "deviation", deviation);
			OpticReadout readout;
			const OpticPrescription body = buildOptic(wedge, ctx, readout);
			OpticRay ray { glm::dvec3(0.0, 0.0, -0.01), glm::dvec3(0.0, 0.0, 1.0) };
			const bool ok = traceEvents(body, mediumIndices(body, ctx, ctx.designWavelength), opticTraits(kind).ports[0].events, ray);
			const double nominal = std::array { 2.0, 4.0, 6.0, 10.0 }[(size_t)deviation] * PI / 180.0;
			const double measured = std::acos(std::clamp(ray.direction.z, -1.0, 1.0));
			check(ok && std::abs(measured - nominal) + (g_plant ? 1e-5 : 0.0) < 1e-6, fmt::format("{} {} deg: deviates {:.9f} deg", opticTraits(kind).name, nominal * 180 / PI, measured * 180 / PI));
		}
	}
	// the right-angle prism's Turned port leaves at 90 degrees and its Retro port at 180
	{
		SceneOptic prism = makeOptic(OpticKind::RightAnglePrism);
		OpticReadout readout;
		const OpticPrescription body = buildOptic(prism, ctx, readout);
		const std::vector<double> n = mediumIndices(body, ctx, ctx.designWavelength);
		OpticRay turned { glm::dvec3(0.0, -0.003, -0.05), glm::dvec3(0.0, 0.0, 1.0) };
		const bool turnedOk = traceEvents(body, n, opticTraits(OpticKind::RightAnglePrism).ports[0].events, turned);
		const double turnedAngle = std::acos(std::clamp(turned.direction.z, -1.0, 1.0));
		check(turnedOk && std::abs(turnedAngle - 0.5 * PI) + (g_plant ? 1e-8 : 0.0) < 1e-9, fmt::format("right-angle prism Turned leaves at {:.12f} deg", turnedAngle * 180 / PI));
		// into the hypotenuse along its inward normal, on the half that meets the y leg first
		const glm::dvec3 inward = -glm::normalize(glm::dvec3(0.0, 1.0, 1.0));
		const glm::dvec3 start = glm::dvec3(0.0, -0.004, 0.004) - 0.05 * inward;
		OpticRay retro { start, inward };
		const bool retroOk = traceEvents(body, n, opticTraits(OpticKind::RightAnglePrism).ports[1].events, retro);
		const double retroAngle = std::acos(std::clamp(glm::dot(retro.direction, inward), -1.0, 1.0));
		check(retroOk && std::abs(retroAngle - PI) < 1e-9, fmt::format("right-angle prism Retro leaves at {:.12f} deg", retroAngle * 180 / PI));
	}
	// the cube's ports, and the equilateral prism refracts toward its base
	{
		SceneOptic cube = makeOptic(OpticKind::CubeBeamsplitter);
		OpticReadout readout;
		const OpticPrescription body = buildOptic(cube, ctx, readout);
		const std::vector<double> n = mediumIndices(body, ctx, ctx.designWavelength);
		OpticRay reflected { glm::dvec3(0.001, 0.002, -0.05), glm::dvec3(0.0, 0.0, 1.0) };
		check(traceEvents(body, n, opticTraits(OpticKind::CubeBeamsplitter).ports[0].events, reflected) && glm::length(reflected.direction - glm::dvec3(0.0, -1.0, 0.0)) < 1e-12, "cube Reflected leaves along -y");
		OpticRay transmitted { glm::dvec3(0.001, 0.002, -0.05), glm::dvec3(0.0, 0.0, 1.0) };
		check(traceEvents(body, n, opticTraits(OpticKind::CubeBeamsplitter).ports[1].events, transmitted) && glm::length(transmitted.direction - glm::dvec3(0.0, 0.0, 1.0)) < 1e-12, "cube Transmitted goes straight on");
		// a dispersing prism is used at minimum deviation: the ray enters at asin(n sin 30) and crosses parallel to the base
		SceneOptic prism = makeOptic(OpticKind::EquilateralPrism);
		OpticReadout prismReadout;
		const OpticPrescription prismBody = buildOptic(prism, ctx, prismReadout);
		const std::vector<double> prismN = mediumIndices(prismBody, ctx, ctx.designWavelength);
		const double incidence = std::asin(prismN[1] / prismN[0] * 0.5);
		const double tilt = incidence - PI / 6.0;
		const glm::dvec3 in(0.0, std::sin(tilt), std::cos(tilt));
		OpticRay refracted { prismBody.surfaces[0].placement.origin - 0.05 * in, in };
		const bool through = traceEvents(prismBody, prismN, opticTraits(OpticKind::EquilateralPrism).ports[0].events, refracted);
		double minimum = 0.0;
		for (const auto& [label, value] : prismReadout.angles) {
			if (label == "minimum deviation") {
				minimum = value;
			}
		}
		const double deviation = std::acos(std::clamp(glm::dot(refracted.direction, in), -1.0, 1.0));
		check(through && refracted.direction.y < 0.0 && std::abs(deviation - minimum) < 1e-9,
			fmt::format("equilateral prism at minimum deviation: deviates {:.9f} deg, readout {:.9f} deg", deviation * 180 / PI, minimum * 180 / PI));
	}
	// parameters: a lens whose edge would be below zero is refused, a filter too thick for an FP01 is refused
	{
		SceneOptic lens = makeOptic(OpticKind::SphericalLens);
		setOpticValue(lens, "centerThickness", 0.2e-3);
		OpticReadout readout;
		check(buildOptic(lens, ctx, readout).surfaces.empty() && !readout.error.empty(), "a lens with a negative edge thickness is refused");
		SceneOptic filter = makeOptic(OpticKind::SquareFilter);
		filter.params["thickness"] = 20e-3;
		OpticReadout filterReadout;
		check(buildOptic(filter, ctx, filterReadout).surfaces.empty(), "a square filter thicker than 14.7 mm is refused");
		// a size preset's value edited makes the size custom
		SceneOptic mirror = makeOptic(OpticKind::PlanarMirror);
		setOpticValue(mirror, "diameter", 0.03);
		check(mirror.sizePreset == -1 && std::abs(opticNumber(mirror, "diameter") - 0.03) < 1e-15 && std::abs(opticNumber(mirror, "thickness") - 6e-3) < 1e-15,
			"editing a preset's diameter makes the size custom and keeps its thickness");
		// values set once are kept by name across a form change
		SceneOptic keeps = makeOptic(OpticKind::SphericalLens);
		setOpticValue(keeps, "r2", 0.07);
		setOpticValue(keeps, "form", 0);
		setOpticValue(keeps, "form", 1);
		check(std::abs(opticNumber(keeps, "r2") - 0.07) < 1e-15, "a lens keeps R2 across a form change");
	}
}

}

int main(int argc, char** argv)
{
	int rays = 200000;
	std::string root = ".";
	bool selfcheck = false;
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--rays") == 0 && i + 1 < argc) {
			rays = std::atoi(argv[++i]);
		} else if (std::strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
			root = argv[++i];
		} else if (std::strcmp(argv[i], "--selfcheck") == 0) {
			selfcheck = true;
		} else {
			fmt::println("usage: optics_test [--rays N] [--root dir] [--selfcheck]");
			return 2;
		}
	}
	std::vector<std::string> warnings;
	g_library.loadBundled(root, warnings);
	for (const std::string& warning : warnings) {
		fmt::println("  warning: {}", warning);
	}

	auto runAll = [&]() {
		testSurfaceHits(rays);
		testNormalsAndCurvatures();
		testClosedBodies(rays / 4);
		testLensmaker();
		testGlasses();
		testCoatings();
		testElements();
	};
	runAll();
	const int failures = g_failures;
	if (selfcheck) {
		// every check above plants an error under g_plant; each must fail
		g_failures = 0;
		g_plant = true;
		fmt::println("selfcheck: planting errors");
		runAll();
		const int caught = g_failures;
		g_plant = false;
		g_failures = failures;
		if (caught < 20) {
			fmt::println("optics_test: --selfcheck FAILED, only {} planted errors were caught", caught);
			return 1;
		}
		fmt::println("selfcheck: {} planted errors caught", caught);
	}
	if (g_failures == 0) {
		fmt::println("optics_test: all checks passed");
		return 0;
	}
	fmt::println("optics_test: {} check(s) FAILED", g_failures);
	return 1;
}
