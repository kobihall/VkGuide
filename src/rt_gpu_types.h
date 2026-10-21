#pragma once

// What the GPU renderer (rt_gpu.h), the wavefront pool (rt_wavefront.h) and every integrator's
// schedule (rt_schedule.h) share: the C++ mirrors of the std430 structs and the push-constant
// block in shaders/rt/include/crt_common.glsl, the limits every buffer is sized by, and the
// resource table that maps each CrtBinding to the resource behind it.
//
// vec3 pads to 16 bytes in std430, so every struct is laid out with an explicit fourth component
// and checked with a static_assert. The acceleration structure's own mirrors (GpuInstance,
// GpuTriangleAttributes, BvhTriangle) are in rt_accel.h and bvh_layout.h.

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <aov.h>
#include <rt_kernels.h>
#include <rt_material_record.h>
#include <vk_types.h>

struct DescriptorWriter;

// binding 31, one per pixel while a film variant that keeps sums is selected (crt_common.glsl
// FilmPixel; the arithmetic is src/film.h FilmPixel)
struct CrtFilmPixel {
	// rgb: sum(w L), w: sum(w), each with its Neumaier compensation term beside it
	glm::vec4 sum;
	glm::vec4 compensation;
	// the luminance's Welford mean and M2, and the samples accepted
	float welfordMean;
	float welfordM2;
	uint32_t accepted;
	uint32_t pad;
};
static_assert(sizeof(CrtFilmPixel) == 48);

// binding 29, one per pool slot while an auxiliary output is on: the first visible surface the camera path in the slot
// met (crt_common.glsl PixelState; src/aov.h AovSample has the same fields). 18 four-byte words, 72 bytes
struct CrtPixelState {
	glm::vec3 position;
	// along the camera's forward axis
	float depth;
	glm::vec3 normal;
	glm::vec3 shadingNormal;
	glm::vec2 uv;
	glm::vec3 albedo;
	uint32_t instance;
	uint32_t material;
	// AOV_STATE_HIT once a surface is recorded
	uint32_t state;
};
static_assert(sizeof(CrtPixelState) == 72 && alignof(CrtPixelState) == 4, "PixelState is scalars only, so std430 packs it to 72 bytes");

struct CrtQueueHeader {
	// == VkDispatchIndirectCommand, maintained by the allocator as ceil(rayCount / workgroup)
	uint32_t groupCountX;
	uint32_t groupCountY;
	uint32_t groupCountZ;
	uint32_t rayCount;
};
static_assert(sizeof(CrtQueueHeader) == 16);

// The push-constant block every kernel takes (crt_common.glsl `Params`). One block for every
// kernel of every integrator, because a shared stage is compiled once: an integrator that needs
// values of its own appends fields here and in the GLSL together. `bounce` is the only field that
// changes between the dispatches of one frame
struct CrtParams {
	glm::vec4 origin;
	glm::vec4 lowerLeft;
	glm::vec4 horizontal;
	glm::vec4 vertical;
	// n, with the lens radius in w
	glm::vec4 lensU;
	// b
	glm::vec4 lensV;
	// rgb what a missed ray sees when CRT_FLAG_SOLID_BACKGROUND is set
	glm::vec4 background;
	uint32_t width;
	uint32_t height;
	uint32_t samplesPerFrame;
	uint32_t poolSize;
	uint32_t seed;
	uint32_t bounce;
	uint32_t rayDepth;
	// instances reached through the TLAS, after the unbounded ones; 0 when there is no TLAS to walk
	uint32_t tlasInstanceCount;
	// the unbounded shapes at the front of the instance buffer, tested by every ray
	uint32_t unboundedCount;
	uint32_t flags;
	uint32_t minBouncesBeforeRoulette;
	uint32_t debugView;
	uint32_t maxSamples;
	float environmentIntensity;
	// every record in instances[], whether or not a TLAS was built over them (shaders/rt/include/
	// crt_linear.glsl scans exactly this many)
	uint32_t instanceCount;
	// the sampler's choices, packed (src/sampler.h PackedSamplerParams; unpacked by
	// shaders/rt/include/crt_sampler.glsl). Not the field name `sampler`, a reserved word in GLSL
	uint32_t samplerOptions;
	uint32_t samplerHalton;
	uint32_t samplerStrata;
	float samplerInvSamples;
	float samplerInvStrataX;
	float samplerInvStrataY;
	// the film's clamp (RenderSettings::clampSamples): a sample whose largest component exceeds it is
	// scaled down to it. 0 clamps nothing
	float filmClamp;
	// RenderSettings::outlierKappa, for the outlier-reweighted film
	float outlierKappa;
	// the auxiliary outputs (src/aov.h): one bit per AovOutput, and AOV_PARAM_CAPTURE_THROUGH_SPECULAR
	uint32_t aovOutputs;
	// where each AovLayer is in the AovImages array, a nibble each (AovLayout::pack()), 0xF for none
	uint32_t aovLayers;
	// the light groups a render keeps, 0 for none
	uint32_t lightGroupCount;
	// the group the background belongs to
	uint32_t environmentGroup;
	// the ambient occlusion view's ray length
	float aoDistance;
};
static_assert(sizeof(CrtParams) == 224);

inline constexpr uint32_t CRT_FLAG_JITTER = 1u << 0;
inline constexpr uint32_t CRT_FLAG_ROULETTE = 1u << 1;
inline constexpr uint32_t CRT_FLAG_ENVIRONMENT_MAP = 1u << 2;
inline constexpr uint32_t CRT_FLAG_SOLID_BACKGROUND = 1u << 3;
// RenderSettings::errorBoundedOffsets and watertightTriangles (crt_common.glsl)
inline constexpr uint32_t CRT_FLAG_ERROR_BOUNDS = 1u << 4;
inline constexpr uint32_t CRT_FLAG_WATERTIGHT = 1u << 5;

// Params.bounce for kernel 01's pass over the whole pool before kernel 00 (crt_common.glsl)
inline constexpr uint32_t CRT_CAMERA_PASS = 0xFFFFFFFFu;

// crt_common.glsl CRT_WORKGROUP; every kernel is 1-D at this size
inline constexpr uint32_t CRT_WORKGROUP = 64;

// the largest path pool the tracer allocates: width x height x samplesPerFrame is clamped to it
// by lowering samplesPerFrame. 180 bytes per path (path state 64, hit 24, shadow ray 48, radiance
// 16, seven queue entries 28), so ~720 MB at the cap, and 48 more for the sample record while a
// sampler that writes one is selected (SAMPLE_RECORD_BYTES), ~912 MB
inline constexpr uint32_t CRT_MAX_POOL = 4u * 1024u * 1024u;
inline constexpr int CRT_MAX_DEPTH = 16;
inline constexpr int CRT_MAX_SAMPLES_PER_FRAME = 8;

// crt_common.glsl traversalStats: nodes visited and primitives tested, per bounce, for the closest
// hits and then for the shadow rays; then CRT_STAT_DROPPED_SAMPLES, kernel 09's count of the
// samples it left out of the mean
inline constexpr uint32_t CRT_SHADOW_STATS_BASE = 2 * CRT_MAX_DEPTH;
inline constexpr uint32_t CRT_DROPPED_SAMPLES_STAT = 4 * CRT_MAX_DEPTH;
inline constexpr uint32_t CRT_TRAVERSAL_STATS_WORDS = CRT_DROPPED_SAMPLES_STAT + 1;

// one frame's traversal work, summed over the closest-hit dispatches of every bounce, and apart
// from it the shadow rays kernel 08 traced
struct TraversalWork {
	uint64_t rays { 0 };
	uint64_t nodes { 0 };
	uint64_t primitives { 0 };
	// bounce 0 alone: camera rays, coherent and cheaper than the bounces after
	uint64_t primaryRays { 0 };
	uint64_t primaryNodes { 0 };
	uint64_t primaryPrimitives { 0 };
	// every bounce's shadow rays: an any-hit walk, which stops at the first thing in the way
	uint64_t shadowRays { 0 };
	uint64_t shadowNodes { 0 };
	uint64_t shadowPrimitives { 0 };

	double nodesPerRay() const { return rays > 0 ? (double)nodes / (double)rays : 0.0; }
	double primitivesPerRay() const { return rays > 0 ? (double)primitives / (double)rays : 0.0; }
	double primaryNodesPerRay() const { return primaryRays > 0 ? (double)primaryNodes / (double)primaryRays : 0.0; }
	double primaryPrimitivesPerRay() const { return primaryRays > 0 ? (double)primaryPrimitives / (double)primaryRays : 0.0; }
	double shadowNodesPerRay() const { return shadowRays > 0 ? (double)shadowNodes / (double)shadowRays : 0.0; }
	double shadowPrimitivesPerRay() const { return shadowRays > 0 ? (double)shadowPrimitives / (double)shadowRays : 0.0; }
};

// a named list of per-bounce counts the render panel prints, such as the paths alive entering each
// bounce: what a schedule wants the user to see of its queues, without the panel knowing them
struct ScheduleCounter {
	const char* label { "" };
	std::vector<uint32_t> values;
};

// the descriptor type behind a binding number
VkDescriptorType crtBindingType(CrtBinding binding);

// THE RESOURCE TABLE: which buffer or image each binding number names right now. Each owner - the
// renderer for the scene, the film and the counters, the wavefront pool for the per-path buffers,
// a schedule for its own - publishes its resources when it allocates or uploads them, and a
// kernel's descriptor set is written from the table for exactly the bindings the kernel declared.
// Nothing here knows what any binding is for, so adding a resource is an enumerator, a GLSL
// declaration and one publish call by its owner.
class CrtResourceTable {
public:
	void setBuffer(CrtBinding binding, VkBuffer buffer, VkDeviceSize range, VkDeviceSize offset = 0);
	// a storage image (no sampler) or a combined image sampler
	void setImage(CrtBinding binding, VkImageView view, VkImageLayout layout, VkSampler sampler = VK_NULL_HANDLE);
	void clear(CrtBinding binding);
	// writes the binding's descriptor. A binding nobody published stops the program with its
	// number: a kernel that reads it would otherwise read whatever the descriptor held
	void write(DescriptorWriter& writer, CrtBinding binding) const;

private:
	struct Entry {
		bool published { false };
		VkBuffer buffer { VK_NULL_HANDLE };
		VkDeviceSize offset { 0 };
		VkDeviceSize range { 0 };
		VkImageView view { VK_NULL_HANDLE };
		VkSampler sampler { VK_NULL_HANDLE };
		VkImageLayout layout { VK_IMAGE_LAYOUT_UNDEFINED };
	};
	std::array<Entry, (size_t)CrtBinding::Count> m_entries {};
};

// how many workgroups a dispatch launches: `groups` directly, or, with an indirect buffer, the
// VkDispatchIndirectCommand at that offset (a queue header)
struct DispatchSize {
	uint32_t groups { 0 };
	VkBuffer indirectBuffer { VK_NULL_HANDLE };
	VkDeviceSize indirectOffset { 0 };
};
