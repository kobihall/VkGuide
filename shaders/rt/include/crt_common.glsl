// Shared by every kernel of the GPU path tracer (shaders/rt/NNVV_*.comp): the buffer contracts,
// the BINDING NUMBERS every kernel agrees on, the push-constant block, the queue set and the
// workgroup-aggregated queue allocator. The C++ side is src/rt_gpu.h and src/rt_kernels.h, which
// mirror the structs with static_asserts - std430 pads every vec3 to 16 bytes, so each struct
// carries an explicit fourth component.
//
// BINDING NUMBERS ARE GLOBAL AND STABLE. Every kernel declares only the bindings it actually
// uses, at the number given here, and src/rt_kernels.cpp builds that kernel's descriptor set
// layout from the same subset (CrtBinding). A layout with gaps is legal; what is NOT legal is
// two kernels using the same number for different resources. Adding a resource means adding a
// number at the end here and a CrtBinding enumerator in the same position - never renumbering.
//
// Requires GL_GOOGLE_include_directive to be enabled by the including shader.

#define CRT_WORKGROUP 64

#define CRT_T_MIN 0.001
#define CRT_INFINITY 1e30

#define CRT_FLAG_JITTER 1u
#define CRT_FLAG_ROULETTE 2u
#define CRT_FLAG_ENVIRONMENT_MAP 4u
#define CRT_FLAG_SOLID_BACKGROUND 8u

#define CRT_DEBUG_NONE 0u
#define CRT_DEBUG_PRIMARY_DIRECTION 1u
#define CRT_DEBUG_HIT_MISS 2u
#define CRT_DEBUG_NORMAL 3u
#define CRT_DEBUG_BOUNCE_HEAT 4u
#define CRT_DEBUG_SAMPLE_COUNT_HEAT 5u
#define CRT_DEBUG_TRAVERSAL_COST 6u

#define CRT_MATERIAL_LAMBERTIAN 0u
#define CRT_MATERIAL_METAL 1u
#define CRT_MATERIAL_PHONG 2u
#define CRT_MATERIAL_DIELECTRIC 3u
#define CRT_MATERIAL_EMISSIVE 4u

// 48 bytes, by path index (== the pool slot the path was spawned in)
struct PathState {
	vec3 origin;
	uint radianceSlot;
	vec3 direction;
	uint rngState;
	vec3 throughput;
	uint bounce;
};

// 48 bytes, by QUEUE POSITION, not path index - extend writes hits[i] for queue entry i and
// shade reads the same. t < 0 is a miss. Geometry-agnostic: a shape and a triangle write the
// same record, and shade never learns which one it came from. A shape leaves uv at zero, which
// is harmless because a shape's material never carries a texture
struct HitRecord {
	vec3 position;
	float t;
	vec3 normal;
	// material index << 1 | front face
	uint materialAndFace;
	vec2 uv;
	// x = BVH nodes visited, y = primitives tested finding this hit (the traversal-cost debug view)
	vec2 pad;
};

// 48 bytes: one triangle of one BLAS, in its mesh's object space, as the traversal intersects it -
// the first vertex and the two edges from it. v0.w is the triangle's index within its mesh (uint
// bits), which finds its GpuTriangleAttributes after leaves re-ordered it and spatial splits
// duplicated it. src/bvh_layout.h BvhTriangle
struct BvhTriangle {
	vec4 v0;
	vec4 e1;
	vec4 e2;
};

// 64 bytes, per triangle of a mesh in its original order: what shading needs once the triangle is
// the closest hit. Object space. src/rt_accel.h GpuTriangleAttributes
struct GpuTriangleAttributes {
	// xyz normal (un-normalised), w that vertex's u
	vec4 n0;
	vec4 n1;
	vec4 n2;
	// xyz the three v's, w the surface index within the mesh (uint bits)
	vec4 vAndSurface;
};

// 64 bytes: a placed mesh (its BLAS, its per-surface material table) or a placed shape, either
// under its world-to-object transform. The unbounded shapes first, then TLAS slot order.
// src/rt_accel.h GpuInstance
struct GpuInstance {
	// rows of the world-to-object transform
	vec4 row0;
	vec4 row1;
	vec4 row2;
	// the BLAS's first node, or CRT_INSTANCE_SHAPE
	uint nodeBase;
	// a mesh: the BLAS's first triangle. A shape: its CRT_SHAPE_* kind
	uint triangleBase;
	uint attributeBase;
	// a mesh: into instanceMaterials[], by surface. A shape: its material index
	uint materialBase;
	// a mesh: how many BvhTriangles its BLAS owns, which is what a traversal-free variant of
	// kernel 02 scans. A shape: 0
	uint triangleCount;
	uint pad0;
	uint pad1;
	uint pad2;
};

#define CRT_INSTANCE_SHAPE 0xFFFFFFFFu

struct GpuMaterial {
	vec3 albedo;
	// fuzz | smoothness | ir | strength, by type
	float param;
	uint type;
	// layer of albedoTextures modulating the albedo, or -1 for an untextured material. Shapes'
	// are always -1
	int albedoLayer;
	uint pad0;
	uint pad1;
};

// == VkDispatchIndirectCommand followed by the count. The allocator keeps groupCountX equal to
// ceil(rayCount / CRT_WORKGROUP); nothing derives one from the other later
struct QueueHeader {
	uint groupCountX;
	uint groupCountY;
	uint groupCountZ;
	uint rayCount;
};

//---------------------------------------------------------------- the queue set
//
// One flat allocation holds CRT_QUEUE_COUNT queues of poolSize entries each, addressed by
// queueSlot(); headers[] carries one QueueHeader per queue. Two of them are the ping-ponging
// RAY queues, which hold PATH INDICES; the rest are the classification queues written by
// kernel 02, which hold the RAY QUEUE POSITION of the path - that is also the index of its
// HitRecord, so a consumer finds both the hit and the path from one number.

#define CRT_QUEUE_RAY_A 0u
#define CRT_QUEUE_RAY_B 1u
// written by 02 Intersect Closest, drained by 03 / 04 / 06 respectively
#define CRT_QUEUE_ESCAPED 2u
#define CRT_QUEUE_EMISSIVE 3u
#define CRT_QUEUE_SURFACE 4u
// written by 06 (and later 07), drained by 08 Trace Shadow Rays. Allocated but unused until a
// next-event-estimation variant of 06 exists - see docs/plans/shadow-rays-nee.md
#define CRT_QUEUE_SHADOW 5u
#define CRT_QUEUE_COUNT 6u

// the ray queue this bounce reads; the next bounce writes the other one
uint rayQueueFor(uint bounce)
{
	return bounce & 1u;
}

layout (std430, set = 0, binding = 0) buffer PathBuffer { PathState paths[]; };
layout (std430, set = 0, binding = 1) buffer HitBuffer { HitRecord hits[]; };
// two queues back to back, each poolSize long
layout (std430, set = 0, binding = 2) buffer QueueBuffer { uint queues[]; };
layout (std430, set = 0, binding = 3) buffer HeaderBuffer { QueueHeader headers[]; };
// one vec4 per pool slot: rgb the path's final contribution, w = 1 for a spawned slot
layout (std430, set = 0, binding = 4) buffer RadianceBuffer { vec4 radiance[]; };
// per pixel: how many of this frame's K slots to spawn (the adaptive-sampling hook)
layout (std430, set = 0, binding = 5) readonly buffer BudgetBuffer { uint sampleBudget[]; };
// every placed mesh and shape: the unbounded shapes, then TLAS slot order
layout (std430, set = 0, binding = 6) readonly buffer InstanceBuffer { GpuInstance instances[]; };
// shapes' materials first (materials[i] belongs to shape i), then the meshes'
layout (std430, set = 0, binding = 7) readonly buffer MaterialBuffer { GpuMaterial materials[]; };
// the running mean per pixel
layout (rgba32f, set = 0, binding = 8) uniform image2D accumulation;
// samples folded into the mean per pixel
layout (r32ui, set = 0, binding = 9) uniform uimage2D sampleCount;
layout (set = 0, binding = 10) uniform sampler2D environmentMap;
// every BLAS's triangles, concatenated; an instance's triangleBase finds its own
layout (std430, set = 0, binding = 11) readonly buffer BlasTriangleBuffer { BvhTriangle blasTriangles[]; };
// every model's base-colour texture, one per layer. An array image rather than an array of
// descriptors so no descriptor indexing is needed - see src/rt_textures.h
layout (set = 0, binding = 12) uniform sampler2DArray albedoTextures;
// every BLAS's nodes, concatenated (4 words per node binary, 5 CWBVH); an instance's nodeBase
// finds its own. src/bvh_layout.h PackedBvh
layout (std430, set = 0, binding = 13) readonly buffer BlasNodeBuffer { uvec4 blasNodes[]; };
// every mesh's per-triangle shading data, concatenated; an instance's attributeBase finds its own
layout (std430, set = 0, binding = 14) readonly buffer AttributeBuffer { GpuTriangleAttributes triangleAttributes[]; };
// the top level: binary layout, leaves naming instances[] slots
layout (std430, set = 0, binding = 15) readonly buffer TlasNodeBuffer { uvec4 tlasNodes[]; };
// per mesh instance, per surface: the index into materials[]
layout (std430, set = 0, binding = 16) readonly buffer InstanceMaterialBuffer { uint instanceMaterials[]; };
// the extend stage's work, per bounce: [2 * bounce] BVH nodes visited, [2 * bounce + 1] primitives
// tested, summed over every ray of the frame. Zeroed each frame and read back like the queue
// headers: the hardware-independent measure of a BVH (src/rt_gpu.h traversalWork)
layout (std430, set = 0, binding = 17) buffer TraversalStatsBuffer { uint traversalStats[]; };

// src/rt_gpu.cpp CrtParams
layout (push_constant) uniform Params {
	vec4 origin;
	vec4 lowerLeft;
	vec4 horizontal;
	vec4 vertical;
	// n, with the lens radius in w
	vec4 lensU;
	// b
	vec4 lensV;
	// rgb what a missed ray sees when CRT_FLAG_SOLID_BACKGROUND is set
	vec4 background;
	uint width;
	uint height;
	uint samplesPerFrame;
	uint poolSize;
	uint seed;
	uint bounce;
	uint rayDepth;
	// instances reached through the TLAS, from instances[unboundedCount]; 0 when there are none
	// (or the TLAS failed to build)
	uint tlasInstanceCount;
	// the unbounded shapes at the front of instances[], which every ray tests outside the TLAS
	uint unboundedCount;
	uint flags;
	uint minBouncesBeforeRoulette;
	uint debugView;
	uint maxSamples;
	float environmentIntensity;
	// every record in instances[], unbounded and bounded alike, regardless of whether a TLAS was
	// built over them. A traversal-free variant of kernel 02 scans exactly this many
	uint instanceCount;
	float pad0;
} pc;

uint queueSlot(uint queue, uint index)
{
	return queue * pc.poolSize + index;
}

//---------------------------------------------------------------- queue allocator

shared uint s_queueCount;
shared uint s_queueBase;

// Appends pathIndex to `queue` if `survives`. One global atomic per workgroup rather than per
// invocation. MUST be called by every invocation of the workgroup, in uniform control flow: a
// dead lane passes survives = false, it never returns early before this
void queueAppend(uint queue, bool survives, uint pathIndex)
{
	if (gl_LocalInvocationID.x == 0u) {
		s_queueCount = 0u;
	}
	memoryBarrierShared();
	barrier();

	uint local = 0u;
	if (survives) {
		local = atomicAdd(s_queueCount, 1u);
	}
	memoryBarrierShared();
	barrier();

	if (gl_LocalInvocationID.x == 0u) {
		s_queueBase = atomicAdd(headers[queue].rayCount, s_queueCount);
		// the header's group count stays == ceil(rayCount / workgroup): each workgroup adds
		// the number of workgroup boundaries its contiguous range [base, base + count) crosses
		uint before = (s_queueBase + CRT_WORKGROUP - 1u) / CRT_WORKGROUP;
		uint after = (s_queueBase + s_queueCount + CRT_WORKGROUP - 1u) / CRT_WORKGROUP;
		atomicAdd(headers[queue].groupCountX, after - before);
	}
	memoryBarrierShared();
	barrier();

	if (survives) {
		queues[queueSlot(queue, s_queueBase + local)] = pathIndex;
	}
}

//---------------------------------------------------------------- geometry

// Ray-triangle by Moller-Trumbore: solves for the barycentric coordinates and t directly,
// without ever forming the triangle's plane. Two-sided - a glTF model's back faces are hit and
// shaded like its front faces, so an open mesh (a wall, a leaf) is visible from behind rather
// than invisible. Runs in the mesh's object space, where `direction` is not unit length; t is
// still the world-space parameter, because the object-space ray is the same line. Returns the
// hit in [tMin, tMax] with its barycentrics. src/bvh_layout.cpp intersectBvhTriangle
bool hitTriangle(BvhTriangle tri, vec3 origin, vec3 direction, float tMin, float tMax, out float tHit, out vec2 bary)
{
	vec3 edge1 = tri.e1.xyz;
	vec3 edge2 = tri.e2.xyz;
	vec3 pvec = cross(direction, edge2);
	float det = dot(edge1, pvec);

	// only an exactly parallel ray or a zero-area triangle: in object space under a scaled instance
	// a fixed epsilon would reject perfectly good small triangles
	if (abs(det) < 1e-30) {
		return false;
	}

	float invDet = 1.0 / det;
	vec3 tvec = origin - tri.v0.xyz;
	float u = dot(tvec, pvec) * invDet;
	if (u < 0.0 || u > 1.0) {
		return false;
	}

	vec3 qvec = cross(tvec, edge1);
	float v = dot(direction, qvec) * invDet;
	if (v < 0.0 || u + v > 1.0) {
		return false;
	}

	float t = dot(edge2, qvec) * invDet;
	if (t < tMin || t > tMax) {
		return false;
	}

	tHit = t;
	bary = vec2(u, v);
	return true;
}

// sRGB -> linear, for the base-colour textures. glTF stores them encoded, and the tracer works
// in linear light throughout: skipping this leaves every texture looking washed out and makes
// its energy wrong at every bounce
vec3 srgbToLinear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
