#pragma once

// The path tracer's acceleration structure, from the engine's side: which BVH settings the scene
// is built with, the bottom-level structures of the loaded meshes (built once per mesh and cached),
// and the per-edit top level over the placed objects and spheres - with the shading data that
// goes with them, laid out as the GPU reads it (shaders/crt_common.glsl).
//
// Two lifetimes, deliberately separate:
//  - RaytraceBlasSet: one BLAS per unique mesh of the loaded models. Depends only on the models and
//    the BVH settings, so it is built when a model is imported (or the settings change) and never
//    again - moving, adding or deleting an object does not touch it. This is where build time is
//    spent, and it is spent up front.
//  - RaytraceSceneAccel: the instances (one per visible mesh object, one per sphere), their
//    materials, and the TLAS over them. Cheap; rebuilt whenever an object or sphere changes.

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <bvh_scene.h>
#include <rt_scene_types.h>

struct MeshAsset;
class RaytraceTextureArray;

// how the raytracer's BLASes are built and laid out. Not saved with the scene: these are the knobs
// for comparing construction methods, and the defaults below are the choice
struct AccelSettings {
	BvhBuildOptions blas {};
	BvhLayout layout { BvhLayout::Binary };

	bool operator==(const AccelSettings&) const = default;
};

AccelSettings defaultAccelSettings();

// the builder options a layout actually gets: the wide layout's leaves hold at most three
// triangles, and letting the builder know avoids re-splitting its leaves after the fact
BvhBuildOptions effectiveBlasOptions(const AccelSettings& settings);

// per original triangle, what shading needs once it is hit (crt_common.glsl GpuTriangleAttributes):
// object-space vertex normals with the uvs in the spare components, and the surface it belongs to
struct GpuTriangleAttributes {
	// xyz normal, w that vertex's u
	glm::vec4 n0;
	glm::vec4 n1;
	glm::vec4 n2;
	// the three v's, and the surface index (uint bits) within the mesh
	glm::vec4 vAndSurface;
};
static_assert(sizeof(GpuTriangleAttributes) == 64);

// One mesh's BLAS plus its shading attributes
struct RaytraceBlas {
	Blas blas;
	std::vector<GpuTriangleAttributes> attributes;
	// the mesh's GeoSurfaces, which an instance's material table is indexed by
	uint32_t surfaceCount { 0 };
};

// Every mesh of the loaded models, by RaytraceMeshData::meshes index. Immutable once built, and
// shared with every render snapshot taken while it is current
struct RaytraceBlasSet {
	AccelSettings settings;
	std::vector<std::shared_ptr<const RaytraceBlas>> blases;

	// totals over the unique meshes, for the panel
	size_t triangles { 0 };
	size_t triangleRefs { 0 };
	size_t nodes { 0 };
	size_t bytes { 0 };
	double buildMs { 0.0 };
	// triangle-weighted mean of the BLASes' SAH costs
	float meanSahCost { 0.f };
	uint32_t maxDepth { 0 };
	// meshes whose BLAS failed validation or packing; they are not traced
	std::vector<std::string> errors;
};

// Builds (or reuses) a BLAS per mesh. Meshes keep their BLAS across calls for as long as they stay
// loaded and the settings do not change, so importing a second model builds only its own meshes.
class RaytraceBlasCache {
public:
	std::shared_ptr<const RaytraceBlasSet> build(const RaytraceMeshData& meshData, const AccelSettings& settings);
	void clear() { m_cache.clear(); }

private:
	AccelSettings m_settings;
	std::unordered_map<const MeshAsset*, std::shared_ptr<const RaytraceBlas>> m_cache;
};

// one instance as the GPU reads it (crt_common.glsl GpuInstance), stored in TLAS slot order
struct GpuInstance {
	// rows of the world-to-object transform. A sphere: row0 = centre xyz, radius w
	glm::vec4 row0;
	glm::vec4 row1;
	glm::vec4 row2;
	// the BLAS's first node, or GPU_INSTANCE_SPHERE for a sphere
	uint32_t nodeBase;
	// the BLAS's first triangle (BvhTriangle) and first attribute record
	uint32_t triangleBase;
	uint32_t attributeBase;
	// a mesh: into instanceMaterials[], indexed by surface. A sphere: its material directly
	uint32_t materialBase;
};
static_assert(sizeof(GpuInstance) == 64);

inline constexpr uint32_t GPU_INSTANCE_SPHERE = 0xFFFFFFFFu;

// Where each BLAS sits in the shared geometry buffers: nodes (in nodes, not words), triangles and
// attributes, all concatenated in RaytraceBlasSet order
struct BlasPlacement {
	uint32_t nodeBase { 0 };
	uint32_t triangleBase { 0 };
	uint32_t attributeBase { 0 };
};

// The geometry every BLAS contributes, concatenated for upload. Built once per RaytraceBlasSet
struct RaytraceGeometry {
	std::vector<glm::uvec4> nodes;
	std::vector<BvhTriangle> triangles;
	std::vector<GpuTriangleAttributes> attributes;
	std::vector<BlasPlacement> placements;
};

RaytraceGeometry packGeometry(const RaytraceBlasSet& blases);

// The scene as traced: the BLAS set it was built over, the TLAS, and the instances, materials and
// per-surface material table the extend and shade stages read
struct RaytraceSceneAccel {
	std::shared_ptr<const RaytraceBlasSet> blases;
	std::shared_ptr<const RaytraceGeometry> geometry;
	Tlas tlas;
	// in TLAS slot order: tlas.bvh leaves index this directly
	std::vector<GpuInstance> instances;
	std::vector<uint32_t> instanceMaterials;
	// every material: the spheres' first (a sphere's material index is its sphere index), then the
	// mesh objects'
	std::vector<RaytraceTriMaterial> materials;
	uint32_t sphereCount { 0 };
	uint32_t meshInstanceCount { 0 };
	// triangles the placed instances add up to, counting every placement
	size_t placedTriangles { 0 };
};

// Builds the top level over the visible mesh objects and the spheres. `geometry` must be
// packGeometry(*blases); it is passed in so it is packed once per BLAS set, not per edit
std::shared_ptr<const RaytraceSceneAccel> buildSceneAccel(std::shared_ptr<const RaytraceBlasSet> blases, std::shared_ptr<const RaytraceGeometry> geometry,
	const RaytraceMeshData& meshData, const std::vector<SceneMeshObject>& objects, const std::vector<SceneSphere>& spheres, const RaytraceTextureArray& textures);
