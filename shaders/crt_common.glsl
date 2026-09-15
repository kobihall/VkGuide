// Shared by every stage of the GPU path tracer (crt_*.comp): the buffer contracts, the one
// descriptor set every stage binds, the push-constant block, the queue allocator and the
// sphere test. The C++ side is src/rt_gpu.h/.cpp, which mirrors the structs with
// static_asserts - std430 pads every vec3 to 16 bytes, so each struct carries an explicit
// fourth component.
//
// Requires GL_GOOGLE_include_directive to be enabled by the including shader.

#define CRT_WORKGROUP 64

#define CRT_T_MIN 0.001
#define CRT_INFINITY 1e30

#define CRT_FLAG_JITTER 1u
#define CRT_FLAG_ROULETTE 2u
#define CRT_FLAG_ENVIRONMENT_MAP 4u

#define CRT_DEBUG_NONE 0u
#define CRT_DEBUG_PRIMARY_DIRECTION 1u
#define CRT_DEBUG_HIT_MISS 2u
#define CRT_DEBUG_NORMAL 3u
#define CRT_DEBUG_BOUNCE_HEAT 4u
#define CRT_DEBUG_SAMPLE_COUNT_HEAT 5u

#define CRT_MATERIAL_LAMBERTIAN 0u
#define CRT_MATERIAL_METAL 1u
#define CRT_MATERIAL_PHONG 2u
#define CRT_MATERIAL_DIELECTRIC 3u

// 48 bytes, by path index (== the pool slot the path was spawned in)
struct PathState {
	vec3 origin;
	uint radianceSlot;
	vec3 direction;
	uint rngState;
	vec3 throughput;
	uint bounce;
};

// 32 bytes, by QUEUE POSITION, not path index - extend writes hits[i] for queue entry i and
// shade reads the same. t < 0 is a miss. Geometry-agnostic on purpose: a triangle extend writes
// the same record
struct HitRecord {
	vec3 position;
	float t;
	vec3 normal;
	// material index << 1 | front face
	uint materialAndFace;
};

struct GpuSphere {
	vec3 center;
	float radius;
};

struct GpuMaterial {
	vec3 albedo;
	// fuzz | smoothness | ir, by type
	float param;
	uint type;
	uint pad0;
	uint pad1;
	uint pad2;
};

// == VkDispatchIndirectCommand followed by the count. The allocator keeps groupCountX equal to
// ceil(rayCount / CRT_WORKGROUP); nothing derives one from the other later
struct QueueHeader {
	uint groupCountX;
	uint groupCountY;
	uint groupCountZ;
	uint rayCount;
};

layout (std430, set = 0, binding = 0) buffer PathBuffer { PathState paths[]; };
layout (std430, set = 0, binding = 1) buffer HitBuffer { HitRecord hits[]; };
// two queues back to back, each poolSize long
layout (std430, set = 0, binding = 2) buffer QueueBuffer { uint queues[]; };
layout (std430, set = 0, binding = 3) buffer HeaderBuffer { QueueHeader headers[2]; };
// one vec4 per pool slot: rgb the path's final contribution, w = 1 for a spawned slot
layout (std430, set = 0, binding = 4) buffer RadianceBuffer { vec4 radiance[]; };
// per pixel: how many of this frame's K slots to spawn (the adaptive-sampling hook)
layout (std430, set = 0, binding = 5) readonly buffer BudgetBuffer { uint sampleBudget[]; };
layout (std430, set = 0, binding = 6) readonly buffer SphereBuffer { GpuSphere spheres[]; };
layout (std430, set = 0, binding = 7) readonly buffer MaterialBuffer { GpuMaterial materials[]; };
// the running mean per pixel
layout (rgba32f, set = 0, binding = 8) uniform image2D accumulation;
// samples folded into the mean per pixel
layout (r32ui, set = 0, binding = 9) uniform uimage2D sampleCount;
layout (set = 0, binding = 10) uniform sampler2D environmentMap;

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
	uint width;
	uint height;
	uint samplesPerFrame;
	uint poolSize;
	uint seed;
	uint bounce;
	uint rayDepth;
	uint sphereCount;
	uint flags;
	uint minBouncesBeforeRoulette;
	uint debugView;
	uint maxSamples;
	float environmentIntensity;
	float pad0;
	float pad1;
	float pad2;
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

// Ray-sphere in the numerically stable form (Ray Tracing Gems ch. 7): the discriminant from
// the closest-approach vector rather than |oc|^2 - r^2, which cancels catastrophically for the
// radius-100 ground sphere seen from a unit away. `direction` must be normalised, so t is a
// world distance. Returns the nearest root in [tMin, tMax]
bool hitSphere(GpuSphere s, vec3 origin, vec3 direction, float tMin, float tMax, out float tHit)
{
	vec3 f = origin - s.center;
	float bPrime = -dot(f, direction);
	vec3 closest = f + bPrime * direction;
	float delta = s.radius * s.radius - dot(closest, closest);
	if (delta < 0.0) {
		return false;
	}

	float q = bPrime + (bPrime >= 0.0 ? 1.0 : -1.0) * sqrt(delta);
	if (abs(q) < 1e-20) {
		// a tangent graze through the origin: no meaningful hit
		return false;
	}
	float c = dot(f, f) - s.radius * s.radius;
	float t0 = c / q;
	float t1 = q;
	float tNear = min(t0, t1);
	float tFar = max(t0, t1);

	tHit = tNear;
	if (tHit < tMin || tHit > tMax) {
		tHit = tFar;
		if (tHit < tMin || tHit > tMax) {
			return false;
		}
	}
	return true;
}

// the CPU backend's sky: white at the horizon blending to light blue overhead
vec3 skyGradient(vec3 direction)
{
	float t = 0.5 * (direction.y + 1.0);
	return mix(vec3(1.0), vec3(0.5, 0.7, 1.0), t);
}
