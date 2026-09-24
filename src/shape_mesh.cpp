#include <shape_mesh.h>

#include <glm/gtc/constants.hpp>

namespace {

constexpr uint32_t SPHERE_RINGS = 24;
constexpr uint32_t ROUND_SEGMENTS = 32;

uint32_t addVertex(ShapeMeshData& mesh, const glm::vec3& position, const glm::vec3& normal, const glm::vec2& uv)
{
	Vertex vertex;
	vertex.position = position;
	vertex.normal = normal;
	vertex.uv_x = uv.x;
	vertex.uv_y = uv.y;
	vertex.color = glm::vec4(1.f);
	mesh.vertices.push_back(vertex);
	return (uint32_t)mesh.vertices.size() - 1;
}

// a flat face with its own four vertices, so it shades flat: centre c, edge half-vectors u and v,
// facing u x v
void addFace(ShapeMeshData& mesh, const glm::vec3& c, const glm::vec3& u, const glm::vec3& v)
{
	const glm::vec3 normal = glm::normalize(glm::cross(u, v));
	const uint32_t a = addVertex(mesh, c - u - v, normal, { 0.f, 0.f });
	const uint32_t b = addVertex(mesh, c + u - v, normal, { 1.f, 0.f });
	const uint32_t d = addVertex(mesh, c + u + v, normal, { 1.f, 1.f });
	const uint32_t e = addVertex(mesh, c - u + v, normal, { 0.f, 1.f });
	mesh.indices.insert(mesh.indices.end(), { a, b, d, a, d, e });
}

void buildSphere(ShapeMeshData& mesh)
{
	for (uint32_t ring = 0; ring <= SPHERE_RINGS; ring++) {
		const float theta = glm::pi<float>() * (float)ring / (float)SPHERE_RINGS;
		for (uint32_t segment = 0; segment <= ROUND_SEGMENTS; segment++) {
			const float phi = glm::two_pi<float>() * (float)segment / (float)ROUND_SEGMENTS;
			//on a unit sphere centred on the origin the position is also the normal
			const glm::vec3 p(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
			addVertex(mesh, p, p, { (float)segment / (float)ROUND_SEGMENTS, (float)ring / (float)SPHERE_RINGS });
		}
	}
	for (uint32_t ring = 0; ring < SPHERE_RINGS; ring++) {
		for (uint32_t segment = 0; segment < ROUND_SEGMENTS; segment++) {
			const uint32_t a = ring * (ROUND_SEGMENTS + 1) + segment;
			const uint32_t b = a + ROUND_SEGMENTS + 1;
			mesh.indices.insert(mesh.indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
		}
	}
}

void buildBox(ShapeMeshData& mesh)
{
	for (int axis = 0; axis < 3; axis++) {
		for (const float side : { -0.5f, 0.5f }) {
			glm::vec3 c(0.f);
			glm::vec3 u(0.f);
			glm::vec3 v(0.f);
			c[axis] = side;
			u[(axis + 1) % 3] = 0.5f;
			v[(axis + 2) % 3] = 0.5f;
			//the next two axes in cyclic order cross to +axis, so the negative face swaps them
			addFace(mesh, c, side > 0.f ? u : v, side > 0.f ? v : u);
		}
	}
}

void buildCylinder(ShapeMeshData& mesh)
{
	//the side: a ring of vertices at each end, normals radial so it shades round
	const uint32_t sideBase = (uint32_t)mesh.vertices.size();
	for (uint32_t segment = 0; segment <= ROUND_SEGMENTS; segment++) {
		const float phi = glm::two_pi<float>() * (float)segment / (float)ROUND_SEGMENTS;
		const glm::vec3 radial(std::cos(phi), 0.f, std::sin(phi));
		const float u = (float)segment / (float)ROUND_SEGMENTS;
		addVertex(mesh, radial + glm::vec3(0.f, -0.5f, 0.f), radial, { u, 0.f });
		addVertex(mesh, radial + glm::vec3(0.f, 0.5f, 0.f), radial, { u, 1.f });
	}
	for (uint32_t segment = 0; segment < ROUND_SEGMENTS; segment++) {
		const uint32_t a = sideBase + segment * 2;
		mesh.indices.insert(mesh.indices.end(), { a, a + 1, a + 2, a + 2, a + 1, a + 3 });
	}

	//the caps: a fan each, with the cap's own flat normal
	for (const float y : { -0.5f, 0.5f }) {
		const glm::vec3 normal(0.f, y > 0.f ? 1.f : -1.f, 0.f);
		const uint32_t centre = addVertex(mesh, { 0.f, y, 0.f }, normal, { 0.5f, 0.5f });
		for (uint32_t segment = 0; segment <= ROUND_SEGMENTS; segment++) {
			const float phi = glm::two_pi<float>() * (float)segment / (float)ROUND_SEGMENTS;
			addVertex(mesh, { std::cos(phi), y, std::sin(phi) }, normal, { 0.5f + 0.5f * std::cos(phi), 0.5f + 0.5f * std::sin(phi) });
		}
		for (uint32_t segment = 0; segment < ROUND_SEGMENTS; segment++) {
			mesh.indices.insert(mesh.indices.end(), { centre, centre + 1 + segment, centre + 2 + segment });
		}
	}
}

}

ShapeMeshData buildShapeMesh(ShapeKind kind)
{
	//the raster pipeline draws both sides of every triangle, as the tracer does, so no winding here
	//decides visibility
	ShapeMeshData mesh;
	switch (kind) {
	case ShapeKind::Sphere:
		buildSphere(mesh);
		break;
	case ShapeKind::Plane:
		addFace(mesh, glm::vec3(0.f), { 0.f, 0.f, PLANE_PREVIEW_HALF_EXTENT }, { PLANE_PREVIEW_HALF_EXTENT, 0.f, 0.f });
		break;
	case ShapeKind::Quad:
		addFace(mesh, glm::vec3(0.f), { 0.f, 0.f, 0.5f }, { 0.5f, 0.f, 0.f });
		break;
	case ShapeKind::Box:
		buildBox(mesh);
		break;
	case ShapeKind::Cylinder:
		buildCylinder(mesh);
		break;
	}
	return mesh;
}
