#pragma once

// Triangle meshes of the analytic shapes' unit primitives (shape.h), for the raster viewport's
// preview: each is drawn under the same SceneShape::objectToWorld() the path tracer places the
// primitive with, so the preview and the render agree on where and how big every shape is.
//
// The infinite plane cannot be meshed; its preview is a square large enough to read as a floor.

#include <cstdint>
#include <vector>

#include <shape.h>
#include <vk_types.h>

// the half-width of the plane's preview square, well inside the raster camera's far plane
inline constexpr float PLANE_PREVIEW_HALF_EXTENT = 500.f;

struct ShapeMeshData {
	// vertex colours are white, so the per-object material's colour factor is the colour
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
};

ShapeMeshData buildShapeMesh(ShapeKind kind);
