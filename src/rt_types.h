#pragma once

// Plain data shared by the CPU raytracer. Ported from the sibling project at
// /Users/kobihall/Documents/Code/RayTracingInAWeekend, which works in double precision
// throughout - kept as-is here rather than narrowed to float, so the ported intersection
// and scattering math behaves identically.

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

class material;

inline constexpr double RT_INFINITY = std::numeric_limits<double>::infinity();
inline constexpr double RT_PI = 3.1415926535897932385;

inline double degrees_to_radians(double degrees)
{
	return degrees * RT_PI / 180.0;
}

class ray {
public:
	ray() {}
	ray(const glm::dvec3& origin, const glm::dvec3& direction) : orig(origin), dir(direction) {}

	glm::dvec3 origin() const { return orig; }
	glm::dvec3 direction() const { return dir; }

	glm::dvec3 at(double t) const { return orig + t * dir; }

	glm::dvec3 orig { 0.0 };
	glm::dvec3 dir { 0.0 };
};

struct hit_record {
	glm::dvec3 p;
	glm::dvec3 normal;
	// non-owning: the sphere that was hit owns its material, and the render's RaytraceScene owns
	// the sphere for the whole render. A shared_ptr here paid an atomic refcount bump on every
	// hit_record copy in the innermost intersection loop
	const material* mat_ptr { nullptr };
	double t;
	bool front_face;

	// glTF and this raytracer both allow rays to start inside geometry, so the stored normal
	// always faces the incoming ray and front_face records which side was hit
	void set_face_normal(const ray& r, const glm::dvec3& outward_normal)
	{
		front_face = glm::dot(r.direction(), outward_normal) < 0;
		normal = front_face ? outward_normal : -outward_normal;
	}
};

struct MeshAsset;

// One glTF GeoSurface placed in the world: which retained mesh it indexes into, that surface's
// index range, the owning node's world transform, and the raw material factors the surface was
// authored with. Geometry stays in object space - a consumer that needs world-space triangles
// applies worldTransform itself. Per surface rather than per node because a surface is the
// finest granularity that has exactly one material.
//
// Deliberately without an intersection routine: the CPU raytracer traces spheres only. This is
// the bottom-level/top-level split a future BVH or GPU mesh path would want anyway.
struct RTMeshInstance {
	// the node's name, suffixed with the surface index when the node's mesh has several surfaces
	std::string name;
	// into RaytraceMeshData::meshes
	size_t meshIndex { 0 };
	// into that mesh's cpuIndices
	uint32_t firstIndex { 0 };
	uint32_t indexCount { 0 };
	glm::mat4 worldTransform { 1.f };
	glm::vec4 colorFactors { 1.f };
	glm::vec2 metalRoughFactors { 0.f };
};

// The loaded glTF scene as the raytracer sees it: every unique mesh once, plus one instance per
// mesh-bearing node and surface. Built once per scene load by buildRaytraceMeshData() and shared
// immutably (shared_ptr<const>) with every render snapshot taken afterwards, instead of being
// rebuilt on every Render click.
//
// The MeshAssets are held alive here past a scene replacement, but LoadedGLTF::clearAll() will
// already have destroyed their GPU buffers by then. Raytracer code must NEVER read
// MeshAsset::meshBuffers through this - only cpuVertices and cpuIndices, which stay valid.
struct RaytraceMeshData {
	std::vector<std::shared_ptr<const MeshAsset>> meshes;
	std::vector<RTMeshInstance> instances;
	size_t triangleCount { 0 };
};

// The raster camera's state at the instant Render was clicked. Captured by value so the worker
// thread never reads VulkanEngine::m_mainCamera, which keeps moving while the render runs.
struct RTCameraSnapshot {
	glm::dvec3 lookFrom { 0.0, 0.0, 0.0 };
	glm::dvec3 lookAt { 0.0, 0.0, -1.0 };
	glm::dvec3 vUp { 0.0, 1.0, 0.0 };
	double vfovDegrees { 70.0 };
	// no UI drives these yet; a zero aperture makes the lens sampling a no-op
	double aperture { 0.0 };
	double focusDistance { 1.0 };
};

// the render resolution is picked from this fixed list rather than typed in or dragged: every
// entry is a sane, recognisable size, and there is no way to land on a degenerate one
struct ResolutionPreset {
	const char* label;
	int width;
	int height;
};

inline constexpr ResolutionPreset RESOLUTION_PRESETS[] = {
	{ "320 x 180", 320, 180 },
	{ "640 x 360", 640, 360 },
	{ "800 x 600", 800, 600 },
	{ "960 x 540", 960, 540 },
	{ "1280 x 720", 1280, 720 },
	{ "1600 x 900", 1600, 900 },
	{ "1920 x 1080", 1920, 1080 },
};

inline constexpr int DEFAULT_RESOLUTION_PRESET = 1;

struct RenderSettings {
	int width { RESOLUTION_PRESETS[DEFAULT_RESOLUTION_PRESET].width };
	int height { RESOLUTION_PRESETS[DEFAULT_RESOLUTION_PRESET].height };
	// jitter each primary ray within its pixel and average samplesPerPixel of them - supersampling,
	// not Vulkan MSAA. Off traces a single un-jittered ray per pixel, as the sibling project did
	bool antialiasing { true };
	int samplesPerPixel { 8 };
	int rayDepth { 8 };
	// seeds the worker's generator with `seed` instead of std::random_device, so the same scene
	// and settings reproduce the same image - useful when comparing renders or debugging
	bool useFixedSeed { false };
	uint32_t seed { 1 };
};
