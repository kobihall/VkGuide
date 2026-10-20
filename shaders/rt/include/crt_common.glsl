// Shared by every kernel of every integrator (shaders/rt/NNVV_*.comp): the buffer contracts, the
// BINDING NUMBERS every kernel agrees on, the push-constant block, the queue set and the
// workgroup-aggregated queue allocator. The C++ side is src/rt_gpu_types.h and src/rt_kernels.h,
// which mirror the structs with static_asserts - std430 pads every vec3 to 16 bytes, so each
// struct carries an explicit fourth component.
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
// docs/plans/completed/pbrt-robust-intersections.md. Off, every trace skips the first CRT_T_MIN of its
// ray and spawns from the exact hit point, as the tracer always did; with the other two off too the
// render is bit-identical to one from before the plan
// spawn points are offset past the error of the hit point (offsetRayOrigin()) and traces accept any
// t above zero
#define CRT_FLAG_ERROR_BOUNDS 16u
// triangles are tested with the watertight algorithm, and a box test widens its far distance by
// CRT_BOX_FAR_SCALE: neither can lose a hit through rounding
#define CRT_FLAG_WATERTIGHT 32u
// the value of Params.bounce for the pass of kernel 01 that runs before kernel 00, over the whole
// pool, and writes the camera's dimensions into the record
#define CRT_CAMERA_PASS 0xFFFFFFFFu

#define CRT_DEBUG_NONE 0u
#define CRT_DEBUG_PRIMARY_DIRECTION 1u
#define CRT_DEBUG_HIT_MISS 2u
#define CRT_DEBUG_NORMAL 3u
#define CRT_DEBUG_BOUNCE_HEAT 4u
#define CRT_DEBUG_SAMPLE_COUNT_HEAT 5u
#define CRT_DEBUG_TRAVERSAL_COST 6u
// Veach's figure 9.8(d): the first vertex's direct light from area lights and the environment, red
// where BSDF samples found it and green where light samples did, each weighted as the strategy
// weighs it. Delta lights, which only light samples can reach, are left out
#define CRT_DEBUG_MIS_WEIGHTS 7u
// the shading normal the materials use against the geometric normal, both turned to face the ray:
// red where they disagree by more than 90 degrees, so a material shades a side of the surface the
// ray never reached; elsewhere grey, the cosine between them
#define CRT_DEBUG_NORMAL_DISAGREEMENT 8u

#define CRT_MATERIAL_LAMBERTIAN 0u
#define CRT_MATERIAL_METAL 1u
#define CRT_MATERIAL_PHONG 2u
#define CRT_MATERIAL_DIELECTRIC 3u
#define CRT_MATERIAL_EMISSIVE 4u
#define CRT_MATERIAL_PBR 5u

// PathState.misPdf's sentinel: whatever emission this path's next ray finds counts at full weight,
// because no light was sampled towards it - a camera ray, a ray leaving a specular surface, or a
// strategy that samples no area lights
#define CRT_MIS_FULL -1.0

// 64 bytes, by path index (== the pool slot the path was spawned in)
struct PathState {
	vec3 origin;
	uint radianceSlot;
	vec3 direction;
	uint rngState;
	vec3 throughput;
	uint bounce;
	// THE MIS RECORD: how kernels 03 and 04 weigh emission the ray from `origin` finds, left by the
	// kernel 06 variant that chose `direction`. CRT_MIS_FULL: full weight. 0: light sampling at
	// `origin` already accounted for every light it could sample. > 0: the BSDF sampling's
	// solid-angle density for `direction`, weighed against the light sampling's density there by
	// the heuristic of exponent misExponent (1 balance, 2 power). Kernel 06 alone decides the
	// strategy; 03 and 04 only follow the record (pbrt-v4's wavefront keeps r_u and r_l the same way)
	float misPdf;
	float misExponent;
	uint pad0;
	uint pad1;
};

// a light list index for "not a light": a hit on geometry the light list does not hold, which any
// strategy then counts at full weight
#define CRT_NO_LIGHT 0xFFFFFFFFu

// 24 bytes, by QUEUE POSITION, not path index - extend writes hits[i] for queue entry i and
// shade reads the same. t < 0 is a miss. THE THIN HIT RECORD: which primitive was hit and where on
// it, nothing derived from them. crt_interaction.glsl rebuilds the surface (point, normals, uv,
// derivatives, material, light record) from the record and the ray that made it, in whichever
// kernel needs it, as pbrt-v4's wavefront does (PBR 4ed 15.3.6). Ordered for std430, where the vec2
// needs an 8-byte offset
struct HitRecord {
	float t;
	// into instances[]
	uint instance;
	// a triangle's (u, v): the weights of its second and third vertices. A shape's own parameters,
	// its uv (shapeUv() in crt_shape.glsl)
	vec2 barycentrics;
	// a mesh: the global blasTriangles[] index. A shape: the part hit (shapePart()): a box's face,
	// a cylinder's side or cap, 0 for a shape of one part
	uint primitive;
	// BVH nodes visited + primitives tested finding this hit (the traversal-cost debug view)
	float traversalCost;
};

// 48 bytes: one triangle of one BLAS, in its mesh's object space, as the traversal intersects it -
// its three vertices, bit-identical to the copies the neighbouring triangles hold (the watertight
// test relies on it). v0.w's bits: the triangle's index within its mesh (CRT_TRIANGLE_INDEX_MASK),
// which finds its GpuTriangleAttributes after leaves re-ordered it and spatial splits duplicated
// it, and CRT_TRIANGLE_CUTOUT for a triangle the traversal alpha-tests. v1.w's bits: the surface
// it belongs to within its mesh, which indexes the instance's material table, so kernel 02 finds a
// hit's material without fetching the attribute record. v2.w is unused.
// src/bvh_layout.h BvhTriangle
struct BvhTriangle {
	vec4 v0;
	vec4 v1;
	vec4 v2;
};

#define CRT_TRIANGLE_INDEX_MASK 0x7FFFFFFFu
#define CRT_TRIANGLE_CUTOUT 0x80000000u
// GpuTriangleAttributes.tangents.w: bits 3-5, one per vertex, set where the vertex has no tangent
#define CRT_TANGENT_MISSING 0x38u

// 80 bytes, per triangle of a mesh in its original order: what shading needs once the triangle is
// the closest hit. Object space. src/rt_accel.h GpuTriangleAttributes
struct GpuTriangleAttributes {
	// xyz normal (un-normalised), w that vertex's u
	vec4 n0;
	vec4 n1;
	vec4 n2;
	// xyz the three v's, w the surface index within the mesh (uint bits)
	vec4 vAndSurface;
	// xyz each vertex's tangent, octahedral snorm16x2 (unpackTangent()); w bit i set when vertex i's
	// bitangent is flipped, bit 3 + i when it has no tangent at all (CRT_TANGENT_MISSING)
	uvec4 tangents;
};

// 128 bytes: a placed mesh (its BLAS, its per-surface material table) or a placed shape, either
// under its transform. The unbounded shapes first, then TLAS slot order. src/rt_accel.h GpuInstance
struct GpuInstance {
	// rows of the world-to-object transform, which carries rays into the instance
	vec4 toObject[3];
	// rows of the object-to-world transform, which carries derivatives back out of it
	vec4 toWorld[3];
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
	// a shape: its first record in lights[] (a box's six faces follow in face order), or
	// CRT_NO_LIGHT. A mesh: where its triangles' entries start in triangleLights[], or CRT_NO_LIGHT
	// for a mesh with no emitting triangle
	uint lightBase;
	uint pad1;
	uint pad2;
};

#define CRT_INSTANCE_SHAPE 0xFFFFFFFFu

// 16 bytes, one per material in binding 7: what kind it is, what kernel 02 and the traversal need
// to know of it without reading its parameters, and where those are in binding 26. Each type's
// parameter record is read by crt_material.glsl. src/rt_gpu_types.h CrtMaterialHeader
struct GpuMaterial {
	// CRT_MATERIAL_*
	uint type;
	// CRT_MATERIAL_FLAG_*, decided on the CPU when the material is packed (src/rt_material.cpp)
	uint flags;
	// into materialParams[], in vec4s
	uint paramOffset;
	uint paramCount;
};

// it emits: kernel 02 queues it for 04
#define CRT_MATERIAL_FLAG_EMITS 1u
// it scatters: kernel 02 queues it for 06
#define CRT_MATERIAL_FLAG_SCATTERS 2u
// kernel 02 queues it for the complex build of 06 rather than the basic one
#define CRT_MATERIAL_FLAG_COMPLEX 4u
// the traversal alpha-tests a triangle flagged CRT_TRIANGLE_CUTOUT against it
#define CRT_MATERIAL_FLAG_CUTOUT 8u
#define CRT_MATERIAL_FLAG_NORMAL_MAP 16u

// the kinds of light (GpuLight.kind); src/light_sampling.h AreaLightKind for the first four
#define CRT_LIGHT_SPHERE 0u
#define CRT_LIGHT_RECTANGLE 1u
#define CRT_LIGHT_CYLINDER 2u
#define CRT_LIGHT_TRIANGLE 3u
#define CRT_LIGHT_POINT 4u
#define CRT_LIGHT_SPOT 5u
#define CRT_LIGHT_DIRECTIONAL 6u
#define CRT_LIGHT_ENVIRONMENT 7u

// 96 bytes: one light the next-event estimation of kernel 06 can sample, in world space.
// src/rt_lights.h GpuLight
//   sphere       a = centre, radius          b, c unused
//   rectangle    a.xyz = a corner            b.xyz, c.xyz = the edges from it
//   cylinder     a = centre, radius          b.xyz = unit x axis, b.w = height; c.xyz = unit y axis
//   triangle     a.xyz, b.xyz, c.xyz = the vertices; a.w = its global attribute record (uint bits),
//                b.w = its emissive texture layer (int bits, -1 for none), c.w = its material (uint bits)
//   point        a = position, range (0: none)
//   spot         a = position, range; b = the direction it shines, cos(outer cone); c.x = cos(inner)
//   directional  a.xyz = the direction the light travels
//   environment  nothing: the map's tables are environmentSampling
struct GpuLight {
	vec4 a;
	vec4 b;
	vec4 c;
	// area: the emitted radiance (a triangle's before its texture). Point and spot: the radiant
	// intensity. Directional: the irradiance. Environment: unused (the map times its intensity)
	vec3 radiance;
	uint kind;
	// what the light is picked in proportion to; pmf = power x the header's inverse total
	float power;
	// Vose's alias table over every light (src/light_sampling.h buildAliasTable)
	float aliasQ;
	uint aliasIndex;
	// and over the delta lights alone, which are lights[0, deltaCount): what the BSDF strategy samples
	float deltaAliasQ;
	uint deltaAliasIndex;
	uint pad0;
	uint pad1;
	uint pad2;
};

// 32 bytes, in front of lights[]
struct LightHeader {
	uint count;
	// the punctual lights, first in the list
	uint deltaCount;
	// the environment map's record, or CRT_NO_LIGHT when the background is not a map
	uint environment;
	uint pad0;
	float invTotalPower;
	float invDeltaPower;
	float pad1;
	float pad2;
};

// 48 bytes, by RAY QUEUE POSITION like hits[]: a shadow ray kernel 06 emitted, from the spawn point
// it chose. The contribution is complete - throughput, BSDF, cosine, emitted radiance, MIS weight
// and pdf all applied - so kernel 08 only decides whether to add it
struct ShadowRay {
	vec3 origin;
	// reserved for the medium the ray starts in and its light group (docs/plans/pbrt-roadmap.md); 0
	uint mediumAndGroup;
	vec3 direction;
	float tMax;
	vec3 contribution;
	uint radianceSlot;
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
// written by 02 Intersect Closest, drained by 03 / 04 / 06 respectively. The surface queue holds the
// hits on basic materials, which the basic build of 06 drains
#define CRT_QUEUE_ESCAPED 2u
#define CRT_QUEUE_EMISSIVE 3u
#define CRT_QUEUE_SURFACE 4u
// written by 06 (and later 07) with the RAY QUEUE POSITION of each path that emitted a shadow
// ray, drained by 08 Trace Shadow Rays
#define CRT_QUEUE_SHADOW 5u
// written by 02 with the hits on complex materials (CRT_MATERIAL_FLAG_COMPLEX), drained by the
// complex build of 06 (crt_material_class.glsl)
#define CRT_QUEUE_SURFACE_COMPLEX 6u
#define CRT_QUEUE_COUNT 7u

// src/rt_gpu_types.h CRT_MAX_DEPTH: traversalStats holds the extend counters for this many bounces, then
// the shadow-ray counters for as many again
#define CRT_MAX_DEPTH 16u
// the traversalStats entry after both: how many samples kernel 09 dropped as non-finite this frame
#define CRT_STAT_DROPPED_SAMPLES (4u * CRT_MAX_DEPTH)

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
// one vec4 per pool slot: rgb what the path has carried home so far, w = 1 for a spawned slot.
// Kernel 00 zeroes it; 03 and 04 ADD to it, since a path that passes through a glowing pbr surface
// collects its emission and carries on. The debug views overwrite it
layout (std430, set = 0, binding = 4) buffer RadianceBuffer { vec4 radiance[]; };
// per pixel: how many of this frame's K slots to spawn (the adaptive-sampling hook)
layout (std430, set = 0, binding = 5) readonly buffer BudgetBuffer { uint sampleBudget[]; };
// every placed mesh and shape: the unbounded shapes, then TLAS slot order
layout (std430, set = 0, binding = 6) readonly buffer InstanceBuffer { GpuInstance instances[]; };
// every material the scene uses, as headers: each material's parameter record is in binding 26.
// A material a shape uses and one a mesh uses are separate records even when they are the same
// library entry, since a shape's is packed without textures
layout (std430, set = 0, binding = 7) readonly buffer MaterialBuffer { GpuMaterial materials[]; };
// the running mean per pixel
layout (rgba32f, set = 0, binding = 8) uniform image2D accumulation;
// samples folded into the mean per pixel
layout (r32ui, set = 0, binding = 9) uniform uimage2D sampleCount;
layout (set = 0, binding = 10) uniform sampler2D environmentMap;
// every BLAS's triangles, concatenated; an instance's triangleBase finds its own
layout (std430, set = 0, binding = 11) readonly buffer BlasTriangleBuffer { BvhTriangle blasTriangles[]; };
// every model's material textures (base colour, normal, metal/rough, emissive), one per layer. An
// array image rather than an array of descriptors so no descriptor indexing is needed - see
// src/rt_textures.h
layout (set = 0, binding = 12) uniform sampler2DArray materialTextures;
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
// tested, summed over every ray of the frame; then from [2 * CRT_MAX_DEPTH] the same for the
// shadow rays. Zeroed each frame and read back like the queue headers: the hardware-independent
// measure of a BVH (src/rt_gpu_types.h TraversalWork). Last, [CRT_STAT_DROPPED_SAMPLES], kernel 09's
// count of the samples it left out of the mean
layout (std430, set = 0, binding = 17) buffer TraversalStatsBuffer { uint traversalStats[]; };
// every light kernel 06 can sample: the punctual lights, then the area lights and the environment
// map. src/rt_lights.h
layout (std430, set = 0, binding = 18) readonly buffer LightBuffer {
	LightHeader lightHeader;
	GpuLight lights[];
};
// one shadow ray per ray queue position, written by 06 and traced by 08
layout (std430, set = 0, binding = 19) buffer ShadowRayBuffer { ShadowRay shadowRays[]; };
// per emitting mesh instance, per triangle (by its index within its mesh): its record in lights[],
// or CRT_NO_LIGHT. What crt_interaction.glsl names the light a hit landed on by
layout (std430, set = 0, binding = 20) readonly buffer TriangleLightBuffer { uint triangleLights[]; };
// the environment map's two sampling tables, full and MIS-compensated, one after the other; each
// is a marginal cdf (height + 1), the row cdfs (height x (width + 1)) and the function (height x
// width). src/light_sampling.h PiecewiseConstant2D
layout (std430, set = 0, binding = 21) readonly buffer EnvironmentSamplingBuffer {
	uint envWidth;
	uint envHeight;
	float envIntegral;
	float envCompensatedIntegral;
	float envData[];
};
// 22: reserved by docs/plans/pbrt-roadmap.md for plans not built yet
// one RaySamples per pool slot: the random numbers a path's next bounce consumes, written by kernel 01
// and read by kernels 00 and 06. A 16-byte stub while the selected sampler writes no record
// (CRT_SAMPLE_RECORD off). src/sampler.h SAMPLE_RECORD_BYTES
struct RaySamples {
	// x the light choice, yz the point on the light, w spare. In the camera pass: xy the offset in the
	// pixel, zw the position on the lens
	vec4 direct;
	// x the lobe or transmission choice, yz the direction, w Russian roulette. In the camera pass: x
	// time, y wavelength
	vec4 indirect;
	// x the distance, y the interaction type, zw the phase function's direction. Not generated yet
	vec4 medium;
};
layout (std430, set = 0, binding = 23) buffer RaySampleBuffer { RaySamples raySamples[]; };
// 24 and 25: reserved by docs/plans/pbrt-roadmap.md for plans not built yet
// every material's parameter record, back to back: GpuMaterial.paramOffset finds its own, and
// crt_material.glsl reads it in the order src/rt_material.cpp packed it
layout (std430, set = 0, binding = 26) readonly buffer MaterialParamBuffer { vec4 materialParams[]; };
// pbrt's Sobol generator matrices and the other tables the samplers read, uploaded once. The header
// and layout are in crt_sampler.glsl, built by src/rt_gpu.cpp
layout (std430, set = 0, binding = 46) readonly buffer SamplerTableBuffer { uint samplerTables[]; };

// src/rt_gpu_types.h CrtParams
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
	// what a texel of environmentMap is multiplied by: the scene's intensity times the power of two
	// the map is stored divided by, so a peak brighter than half float's 65504 stays finite
	float environmentIntensity;
	// every record in instances[], unbounded and bounded alike, regardless of whether a TLAS was
	// built over them. A traversal-free variant of kernel 02 scans exactly this many
	uint instanceCount;
	// the sampler's choices, packed (src/sampler.h PackedSamplerParams; unpacked in crt_sampler.glsl)
	uint samplerOptions;
	uint samplerHalton;
	uint samplerStrata;
	// the stratified sampler's 1 / samples, 1 / strata across and 1 / strata down: multiplied by,
	// since a GPU's division is not IEEE and the CPU mirror's is
	float samplerInvSamples;
	float samplerInvStrataX;
	float samplerInvStrataY;
	uint pad0;
	uint pad1;
	uint pad2;
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

// a world-space point in an instance's object space
vec3 toObjectPoint(GpuInstance instance, vec3 p)
{
	const vec4 q = vec4(p, 1.0);
	return vec3(dot(instance.toObject[0], q), dot(instance.toObject[1], q), dot(instance.toObject[2], q));
}

// A world-space ray in an instance's object space. The direction is deliberately NOT renormalised
// there: origin + t * direction is then the same point in both spaces, so t, tMin and the running
// closest hit carry across the transform unchanged, with no rescaling anywhere
void toObjectRay(GpuInstance instance, vec3 origin, vec3 direction, out vec3 localOrigin, out vec3 localDirection)
{
	const vec4 o = vec4(origin, 1.0);
	localOrigin = vec3(dot(instance.toObject[0], o), dot(instance.toObject[1], o), dot(instance.toObject[2], o));
	localDirection = vec3(dot(instance.toObject[0].xyz, direction), dot(instance.toObject[1].xyz, direction), dot(instance.toObject[2].xyz, direction));
}

// an object-space vector (a tangent, a position derivative) in world space
vec3 toWorldVector(GpuInstance instance, vec3 v)
{
	return vec3(dot(instance.toWorld[0].xyz, v), dot(instance.toWorld[1].xyz, v), dot(instance.toWorld[2].xyz, v));
}

// an object-space normal in world space, unnormalised: by the inverse transpose of object-to-world,
// which is the transpose of the world-to-object rows. It keeps a normal perpendicular to the surface
// under a non-uniform scale, a stretched sphere's as much as a mesh's. Normal derivatives transform
// the same way
vec3 toWorldNormal(GpuInstance instance, vec3 n)
{
	return n.x * instance.toObject[0].xyz + n.y * instance.toObject[1].xyz + n.z * instance.toObject[2].xyz;
}

// the unit world-space normal of an object-space one
vec3 worldNormal(GpuInstance instance, vec3 objectNormal)
{
	return normalize(toWorldNormal(instance, objectNormal));
}

// 1 / d with each zero component replaced by a tiny signed one, so no box test multiplies 0 by
// infinity: NaN compares false both ways and would admit or reject a box arbitrarily
vec3 safeReciprocal(vec3 d)
{
	const vec3 signs = vec3(greaterThanEqual(d, vec3(0.0))) * 2.0 - 1.0;
	return 1.0 / mix(d, signs * 1e-12, lessThan(abs(d), vec3(1e-12)));
}

// a point's uv from its triangle's attribute record and barycentrics
vec2 triangleUv(GpuTriangleAttributes attributes, vec2 bary)
{
	const float w = 1.0 - bary.x - bary.y;
	return w * vec2(attributes.n0.w, attributes.vAndSurface.x) + bary.x * vec2(attributes.n1.w, attributes.vAndSurface.y) + bary.y * vec2(attributes.n2.w, attributes.vAndSurface.z);
}

//---------------------------------------------------------------- robust spawn points and triangles
//
// Error-bounded intersections (PBR 4ed 6.8), mirrored line for line by src/robust.h and the CPU
// traversal in src/bvh_layout.cpp. Every point the tracer computes in float is accompanied by an
// absolute error bound, and a ray spawned at it starts just beyond that bound: it cannot hit the
// surface it left, so no trace needs an epsilon.

// 2^-24: the largest relative error of one rounded operation
#define CRT_MACHINE_EPSILON 5.9604644775390625e-8
// the bound on the relative error of n chained operations (PBR 4ed eq. 6.4)
#define CRT_GAMMA(n) ((float(n) * CRT_MACHINE_EPSILON) / (1.0 - float(n) * CRT_MACHINE_EPSILON))
// how far a box's slab test widens its far distance: 1 + 2 gamma(3) (Ize 2013, PBR 4ed 6.8.2)
#define CRT_BOX_FAR_SCALE (1.0 + 2.0 * CRT_GAMMA(3))

// a*b - c*d with the rounding error of about one ulp of the RESULT rather than of the products
// (Kahan's algorithm on fma), and exactly zero when the true value is
float differenceOfProducts(float a, float b, float c, float d)
{
	const float cd = c * d;
	const float difference = fma(a, b, -cd);
	const float error = fma(-c, d, cd);
	return difference + error;
}

float sumOfProducts(float a, float b, float c, float d)
{
	const float cd = c * d;
	const float sum = fma(a, b, cd);
	const float error = fma(c, d, -cd);
	return sum + error;
}

// the smallest float above v, and the largest below it (pbrt's NextFloatUp and NextFloatDown). On
// the bits throughout, and with no infinity test: a position is finite, and the compiler is free to
// treat -0 as +0 and to assume nothing is infinite
float nextFloatUp(float v)
{
	uint bits = floatBitsToUint(v);
	if (bits == 0x80000000u) {
		bits = 0u;
	}
	bits = (bits >> 31) == 0u ? bits + 1u : bits - 1u;
	return uintBitsToFloat(bits);
}

float nextFloatDown(float v)
{
	uint bits = floatBitsToUint(v);
	if (bits == 0u) {
		bits = 0x80000000u;
	}
	bits = (bits >> 31) == 0u ? bits - 1u : bits + 1u;
	return uintBitsToFloat(bits);
}

// The point p, each coordinate within pError of the truth, moved along n by more than that error to
// the side w points to, then each coordinate rounded one float further from the surface (PBR 4ed
// 6.8.6, pbrt's OffsetRayOrigin). n need not be unit length
vec3 offsetRayOrigin(vec3 p, vec3 pError, vec3 n, vec3 w)
{
	const float d = dot(abs(n), pError);
	vec3 offset = d * n;
	if (dot(w, n) < 0.0) {
		offset = -offset;
	}
	vec3 po = p + offset;
	for (int i = 0; i < 3; i++) {
		if (offset[i] > 0.0) {
			po[i] = nextFloatUp(po[i]);
		} else if (offset[i] < 0.0) {
			po[i] = nextFloatDown(po[i]);
		}
	}
	return po;
}

// The nearest t a trace accepts. Without error-bounded offsets a ray spawned on a surface would hit
// it again at a t of a few ulps, so every trace skips the first CRT_T_MIN. With them the spawn point
// is already clear of the surface and any t above zero will do. Strictly above: a ray spawned
// exactly on a plane (the infinite plane at y = 0 under an identity transform has no error along its
// normal) computes t = -0 or +0 for it, and must not hit it
float traceTMin()
{
	return (pc.flags & CRT_FLAG_ERROR_BOUNDS) != 0u ? 1e-30 : CRT_T_MIN;
}

// the factor a box test multiplies its far distance by
float boxFarScale()
{
	return (pc.flags & CRT_FLAG_WATERTIGHT) != 0u ? CRT_BOX_FAR_SCALE : 1.0;
}

// A point p of an instance's object space in world space, with the error of its coordinates: pError
// carried through the transform (PBR 4ed 6.8.5, "transformations") plus the rounding of the
// transform's own arithmetic. And one more term, the one pbrt gets from transforming the ray with
// an error bound: the next trace from the spawn point carries it back into object space with the
// toObject rows, which are the inverse of the toWorld rows only to rounding, and the more so the
// more the transform stretches one axis against another (a rotated 40:1 scale puts the two a few
// thousand ulps apart). Going out and back measures that disagreement, in world units
vec3 toWorldPoint(GpuInstance instance, vec3 p, vec3 pError, out vec3 worldError)
{
	const vec4 q = vec4(p, 1.0);
	const float g = CRT_GAMMA(3);
	vec3 world;
	for (int row = 0; row < 3; row++) {
		const vec4 m = instance.toWorld[row];
		world[row] = dot(m, q);
		worldError[row] = (g + 1.0) * dot(abs(m.xyz), pError) + g * (abs(m.x * p.x) + abs(m.y * p.y) + abs(m.z * p.z) + abs(m.w));
	}
	const vec4 w = vec4(world, 1.0);
	const vec4 back = vec4(dot(instance.toObject[0], w), dot(instance.toObject[1], w), dot(instance.toObject[2], w), 1.0);
	const vec3 again = vec3(dot(instance.toWorld[0], back), dot(instance.toWorld[1], back), dot(instance.toWorld[2], back));
	worldError += 2.0 * abs(again - world);
	return world;
}

// THE WATERTIGHT TEST'S RAY (Woop, Benthin and Wald 2013; PBR 4ed 6.8.4): the permutation that makes
// the ray's largest component z, and the shear that turns the ray into +z. Computed once per
// object-space ray, by prepareWatertightRay(), and read by every triangle test under it
ivec3 g_watertightAxes;
vec3 g_watertightShear;

void prepareWatertightRay(vec3 direction)
{
	const vec3 a = abs(direction);
	const int kz = a.x > a.y ? (a.x > a.z ? 0 : 2) : (a.y > a.z ? 1 : 2);
	const int kx = kz == 2 ? 0 : kz + 1;
	const int ky = kx == 2 ? 0 : kx + 1;
	g_watertightAxes = ivec3(kx, ky, kz);
	const float dz = direction[kz];
	g_watertightShear = vec3(-direction[kx] / dz, -direction[ky] / dz, 1.0 / dz);
}

// Ray-triangle that never misses a ray through a shared edge or vertex: the triangle is translated
// to the ray origin, permuted and sheared so the ray runs down +z, and the signs of three edge
// functions decide the hit. Triangles that share an edge compute that edge's function from the same
// two translated, permuted, sheared vertices, so they cannot both reject a ray that passes through it.
// pbrt falls back to double precision where an edge function is exactly zero; this GPU has none, so
// a ray exactly through an edge or vertex is accepted by every triangle that owns it. That may count
// a hit twice and never loses one. The shear is applied with fma so the vertex arithmetic is
// the same whichever of the three slots a vertex sits in. Two-sided, like the Moller-Trumbore test.
// t must exceed the error bound of its own computation as well as tMin (PBR 4ed 6.8.4). t is in the
// parameter of `direction`, which is not unit length in object space.
// src/bvh_layout.cpp intersectBvhTriangleWatertight
bool hitTriangleWatertight(BvhTriangle tri, vec3 origin, vec3 direction, float tMin, float tMax, out float tHit, out vec2 bary)
{
	vec3 p0 = tri.v0.xyz - origin;
	vec3 p1 = tri.v1.xyz - origin;
	vec3 p2 = tri.v2.xyz - origin;
	const ivec3 axes = g_watertightAxes;
	p0 = vec3(p0[axes.x], p0[axes.y], p0[axes.z]);
	p1 = vec3(p1[axes.x], p1[axes.y], p1[axes.z]);
	p2 = vec3(p2[axes.x], p2[axes.y], p2[axes.z]);
	const vec3 shear = g_watertightShear;
	p0.x = fma(shear.x, p0.z, p0.x);
	p0.y = fma(shear.y, p0.z, p0.y);
	p1.x = fma(shear.x, p1.z, p1.x);
	p1.y = fma(shear.y, p1.z, p1.y);
	p2.x = fma(shear.x, p2.z, p2.x);
	p2.y = fma(shear.y, p2.z, p2.y);

	const float e0 = differenceOfProducts(p1.x, p2.y, p1.y, p2.x);
	const float e1 = differenceOfProducts(p2.x, p0.y, p2.y, p0.x);
	const float e2 = differenceOfProducts(p0.x, p1.y, p0.y, p1.x);
	if ((e0 < 0.0 || e1 < 0.0 || e2 < 0.0) && (e0 > 0.0 || e1 > 0.0 || e2 > 0.0)) {
		return false;
	}
	const float det = e0 + e1 + e2;
	if (det == 0.0) {
		return false;
	}

	p0.z *= shear.z;
	p1.z *= shear.z;
	p2.z *= shear.z;
	const float tScaled = e0 * p0.z + e1 * p1.z + e2 * p2.z;
	if (det < 0.0 && (tScaled >= 0.0 || tScaled < tMax * det)) {
		return false;
	}
	if (det > 0.0 && (tScaled <= 0.0 || tScaled > tMax * det)) {
		return false;
	}

	const float invDet = 1.0 / det;
	const float b0 = e0 * invDet;
	const float b1 = e1 * invDet;
	const float b2 = e2 * invDet;
	const float t = tScaled * invDet;

	// t conservatively above zero: the error of every term t is made of (PBR 4ed 6.8.4)
	const float maxZ = max(max(abs(p0.z), abs(p1.z)), abs(p2.z));
	const float deltaZ = CRT_GAMMA(3) * maxZ;
	const float maxX = max(max(abs(p0.x), abs(p1.x)), abs(p2.x));
	const float maxY = max(max(abs(p0.y), abs(p1.y)), abs(p2.y));
	const float deltaX = CRT_GAMMA(5) * (maxX + maxZ);
	const float deltaY = CRT_GAMMA(5) * (maxY + maxZ);
	const float deltaE = 2.0 * (CRT_GAMMA(2) * maxX * maxY + deltaY * maxX + deltaX * maxY);
	const float maxE = max(max(abs(e0), abs(e1)), abs(e2));
	const float deltaT = 3.0 * (CRT_GAMMA(3) * maxE * maxZ + deltaE * maxZ + deltaZ * maxE) * abs(invDet);
	if (t <= deltaT || t < tMin) {
		return false;
	}

	tHit = t;
	// the weights of the second and third vertices: the same (u, v) Moller-Trumbore reports
	bary = vec2(b1, b2);
	return true;
}

// Ray-triangle by Moller-Trumbore: solves for the barycentric coordinates and t directly,
// without ever forming the triangle's plane. Two-sided - a glTF model's back faces are hit and
// shaded like its front faces, so an open mesh (a wall, a leaf) is visible from behind rather
// than invisible. Runs in the mesh's object space, where `direction` is not unit length; t is
// still the world-space parameter, because the object-space ray is the same line. Returns the
// hit in [tMin, tMax] with its barycentrics. src/bvh_layout.cpp intersectBvhTriangle
bool hitTriangle(BvhTriangle tri, vec3 origin, vec3 direction, float tMin, float tMax, out float tHit, out vec2 bary)
{
	if ((pc.flags & CRT_FLAG_WATERTIGHT) != 0u) {
		return hitTriangleWatertight(tri, origin, direction, tMin, tMax, tHit, bary);
	}

	vec3 edge1 = tri.v1.xyz - tri.v0.xyz;
	vec3 edge2 = tri.v2.xyz - tri.v0.xyz;
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

// sRGB -> linear, for the base-colour and emissive textures. glTF stores them encoded, and the tracer works
// in linear light throughout: skipping this leaves every texture looking washed out and makes
// its energy wrong at every bounce
vec3 srgbToLinear(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
