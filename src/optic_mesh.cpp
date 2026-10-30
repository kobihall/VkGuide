#include <optic_mesh.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr double PI = 3.14159265358979323846;
// the most rings and spokes one face gets: the preview's cap on what the tolerance asks for
constexpr int MAX_RINGS = 96;
constexpr int MAX_SPOKES = 384;

// the step a curvature allows for a chord error of `tolerance`: c h^2 / 8 <= tolerance
double stepFor(double curvature, double tolerance)
{
	return std::abs(curvature) > 0.0 ? std::sqrt(8.0 * tolerance / std::abs(curvature)) : 1e30;
}

void emitVertex(OpticMesh& mesh, const OpticSurface& s, uint32_t surface, glm::dvec2 xy)
{
	const std::optional<double> sag = surfaceSag(s, xy);
	const glm::dvec3 local(xy, sag ? *sag : 0.0);
	const glm::dvec3 position = s.placement.rotation * local + s.placement.origin;
	const glm::dvec3 normal = s.placement.rotation * surfaceNormalLocal(s, local);
	mesh.vertices.push_back(OpticMeshVertex { glm::vec3(position), glm::vec3(normal), surface });
}

// a triangle wound counter-clockwise seen from the normal's side, whichever way the given order turns
void emitTriangle(OpticMesh& mesh, uint32_t a, uint32_t b, uint32_t c)
{
	const glm::vec3 pa = mesh.vertices[a].position;
	const glm::vec3 pb = mesh.vertices[b].position;
	const glm::vec3 pc = mesh.vertices[c].position;
	const glm::vec3 n = mesh.vertices[a].normal + mesh.vertices[b].normal + mesh.vertices[c].normal;
	if (glm::dot(glm::cross(pb - pa, pc - pa), n) < 0.f) {
		std::swap(b, c);
	}
	mesh.indices.insert(mesh.indices.end(), { a, b, c });
}

// a polar grid over a circle, an annulus or an ellipse (radii scaled per axis)
void tessellateRound(OpticMesh& mesh, const OpticSurface& s, uint32_t surface, double tolerance)
{
	const ApertureT<double>& ap = s.aperture;
	double inner = 0.0;
	double outer = ap.a;
	glm::dvec2 scale(1.0);
	if (ap.kind == ApertureKind::Annulus) {
		inner = ap.a;
		outer = ap.b;
	} else if (ap.kind == ApertureKind::Ellipse) {
		outer = std::max(ap.a, ap.b);
		scale = glm::dvec2(ap.a, ap.b) / outer;
	}
	const double h = stepFor(s.curvature, tolerance);
	const int rings = std::clamp((int)std::ceil((outer - inner) / h), 1, MAX_RINGS);
	const int spokes = std::clamp((int)std::ceil(2.0 * PI * outer / std::min(h, outer)), 24, MAX_SPOKES);
	const uint32_t base = (uint32_t)mesh.vertices.size();
	const bool center = inner <= 0.0;
	if (center) {
		emitVertex(mesh, s, surface, ap.center);
	}
	const int firstRing = center ? 1 : 0;
	for (int ring = firstRing; ring <= rings; ring++) {
		const double r = inner + (outer - inner) * ring / rings;
		for (int spoke = 0; spoke < spokes; spoke++) {
			const double phi = 2.0 * PI * spoke / spokes;
			emitVertex(mesh, s, surface, ap.center + r * scale * glm::dvec2(std::cos(phi), std::sin(phi)));
		}
	}
	auto at = [&](int ring, int spoke) { return base + (center ? 1u : 0u) + (uint32_t)((ring - firstRing) * spokes + (spoke % spokes)); };
	for (int spoke = 0; spoke < spokes && center; spoke++) {
		emitTriangle(mesh, base, at(1, spoke), at(1, spoke + 1));
	}
	for (int ring = firstRing; ring < rings; ring++) {
		for (int spoke = 0; spoke < spokes; spoke++) {
			emitTriangle(mesh, at(ring, spoke), at(ring + 1, spoke), at(ring + 1, spoke + 1));
			emitTriangle(mesh, at(ring, spoke), at(ring + 1, spoke + 1), at(ring, spoke + 1));
		}
	}
}

// a grid over a rectangle
void tessellateRectangle(OpticMesh& mesh, const OpticSurface& s, uint32_t surface, double tolerance)
{
	const ApertureT<double>& ap = s.aperture;
	const double h = stepFor(s.curvature, tolerance);
	const int nx = std::clamp((int)std::ceil(2.0 * ap.a / h), 1, MAX_RINGS);
	const int ny = std::clamp((int)std::ceil(2.0 * ap.b / h), 1, MAX_RINGS);
	const uint32_t base = (uint32_t)mesh.vertices.size();
	for (int j = 0; j <= ny; j++) {
		for (int i = 0; i <= nx; i++) {
			emitVertex(mesh, s, surface, ap.center + glm::dvec2(-ap.a + 2.0 * ap.a * i / nx, -ap.b + 2.0 * ap.b * j / ny));
		}
	}
	auto at = [&](int i, int j) { return base + (uint32_t)(j * (nx + 1) + i); };
	for (int j = 0; j < ny; j++) {
		for (int i = 0; i < nx; i++) {
			emitTriangle(mesh, at(i, j), at(i + 1, j), at(i + 1, j + 1));
			emitTriangle(mesh, at(i, j), at(i + 1, j + 1), at(i, j + 1));
		}
	}
}

// a fan over a convex polygon: the faces of prisms, cubes and wedges are flat
void tessellatePolygon(OpticMesh& mesh, const OpticSurface& s, uint32_t surface)
{
	const ApertureT<double>& ap = s.aperture;
	const int count = std::min((int)ap.a, (int)OPTIC_POLYGON_VERTICES);
	const uint32_t base = (uint32_t)mesh.vertices.size();
	for (int i = 0; i < count; i++) {
		emitVertex(mesh, s, surface, ap.center + ap.polygon[(size_t)i]);
	}
	for (int i = 1; i + 1 < count; i++) {
		emitTriangle(mesh, base, base + (uint32_t)i, base + (uint32_t)i + 1);
	}
}

// the rim between its two bounds: straight along z at each spoke
void tessellateCylinder(OpticMesh& mesh, const OpticSurface& s, uint32_t surface, double tolerance)
{
	// the chord error of a circle: r (1 - cos(d / 2)) <= tolerance
	const double step = 2.0 * std::acos(std::max(1.0 - tolerance / s.radius, -1.0));
	const int spokes = std::clamp((int)std::ceil(2.0 * PI / std::max(step, 1e-9)), 48, MAX_SPOKES);
	const uint32_t base = (uint32_t)mesh.vertices.size();
	for (int spoke = 0; spoke < spokes; spoke++) {
		const double phi = 2.0 * PI * spoke / spokes;
		const glm::dvec2 xy = s.radius * glm::dvec2(std::cos(phi), std::sin(phi));
		const glm::dvec3 normal = s.placement.rotation * glm::dvec3(std::cos(phi), std::sin(phi), 0.0);
		for (const CylinderBoundT<double>* bound : { &s.lower, &s.upper }) {
			const glm::dvec3 local(xy, boundZ(*bound, xy));
			mesh.vertices.push_back(OpticMeshVertex { glm::vec3(s.placement.rotation * local + s.placement.origin), glm::vec3(normal), surface });
		}
	}
	for (int spoke = 0; spoke < spokes; spoke++) {
		const uint32_t a = base + 2u * (uint32_t)spoke;
		const uint32_t b = base + 2u * (uint32_t)((spoke + 1) % spokes);
		emitTriangle(mesh, a, b, b + 1);
		emitTriangle(mesh, a, b + 1, a + 1);
	}
}

}

OpticMesh tessellateOptic(const OpticPrescription& body, double tolerance)
{
	OpticMesh mesh;
	for (uint32_t i = 0; i < (uint32_t)body.surfaces.size(); i++) {
		const OpticSurface& s = body.surfaces[i];
		if (s.shape == SurfaceShape::Cylinder) {
			tessellateCylinder(mesh, s, i, tolerance);
			continue;
		}
		switch (s.aperture.kind) {
		case ApertureKind::Circle:
		case ApertureKind::Annulus:
		case ApertureKind::Ellipse:
			tessellateRound(mesh, s, i, tolerance);
			break;
		case ApertureKind::Rectangle:
			tessellateRectangle(mesh, s, i, tolerance);
			break;
		case ApertureKind::Polygon:
			tessellatePolygon(mesh, s, i);
			break;
		case ApertureKind::None:
			break;
		}
	}
	return mesh;
}
