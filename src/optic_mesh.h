#pragma once

// The raster preview of an optical body (docs/plans/completed/physoptics-optical-surfaces.md), part of rt_optics: each surface
// tessellated over its aperture, sized so the sag error stays under a tolerance (1 um by default, within a cap on the
// triangle count), and each edge cylinder between its two bounds. It feeds the glass preview of the Viewport; the path
// tracer never sees it, and intersects the exact surfaces instead.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <optic_surface.h>

struct OpticMeshVertex {
	glm::vec3 position;
	glm::vec3 normal;
	// the surface the vertex is on, for a preview that tints faces apart
	uint32_t surface;
};

struct OpticMesh {
	std::vector<OpticMeshVertex> vertices;
	std::vector<uint32_t> indices;
};

OpticMesh tessellateOptic(const OpticPrescription& body, double sagTolerance = 1e-6);
