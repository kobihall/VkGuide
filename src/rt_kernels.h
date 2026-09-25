#pragma once

// THE KERNEL REGISTRY: which wavefront kernels exist, which interchangeable variants each one
// has, and what each variant needs bound. This is the file you edit to add a new raytracing
// strategy, and - apart from the shader itself - it is meant to be the only one.
//
// The kernels follow the wavefront figure of Physically Based Rendering 4ed, chapter 15
// (figure 15.2), numbered 00-08 as that figure orders them, plus 09 for the film update that
// sits outside it. shaders/rt/ holds one file per variant, named NNVV_<what it does>.comp for
// kernel NN variant VV, so the directory listing IS the diagram.
//
//     00 Generate camera rays      once per frame, over the whole path pool
//     01 Generate samples          per bounce, over the live ray queue
//     02 Intersect closest         per bounce, over the live ray queue
//     03 Handle escaped            per bounce, over the queue 02 filled with misses
//     04 Handle emissive geometry  per bounce, over the queue 02 filled with light hits
//     05 Sample medium interaction not implemented
//     06 Sample surface scattering per bounce, over the queue 02 filled with surface hits
//     07 Sample medium scattering  not implemented
//     08 Trace shadow rays         not implemented
//     09 Update film               once per frame, over the pixels
//
// ADDING A VARIANT. Write shaders/rt/NNVV_name.comp, then add one KernelVariant to the table in
// rt_kernels.cpp naming it, its shader, the bindings it reads or writes, and any capability the
// UI should react to. GpuPathTracer builds its pipeline, its descriptor set layout and its
// dispatch from that entry alone; nothing in rt_gpu.cpp knows the name of any particular
// strategy. If the variant needs a resource nothing else uses, add a CrtBinding at the end of
// the enum and a matching binding number at the end of shaders/rt/include/crt_common.glsl, then
// list it - existing numbers never move.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <bvh_layout.h>

// The kernels of the wavefront, in dispatch order. The numeric values are the NN in the shader
// filenames and are part of the scene file format - never renumber them.
enum class KernelSlot : uint32_t {
	GenerateCameraRays = 0,
	GenerateSamples = 1,
	IntersectClosest = 2,
	HandleEscaped = 3,
	HandleEmissive = 4,
	SampleMediumInteraction = 5,
	SurfaceScattering = 6,
	SampleMediumScattering = 7,
	TraceShadowRays = 8,
	UpdateFilm = 9,
	Count
};

// Every resource a kernel can bind. THE VALUE IS THE BINDING NUMBER, and it must match
// shaders/rt/include/crt_common.glsl exactly. A kernel lists the subset it uses and gets a
// descriptor set layout with only those numbers in it - a layout with gaps is legal Vulkan, and
// it means a kernel is never made to carry a binding it has no use for. Append only.
enum class CrtBinding : uint32_t {
	Paths = 0,
	Hits = 1,
	Queues = 2,
	Headers = 3,
	Radiance = 4,
	SampleBudget = 5,
	Instances = 6,
	Materials = 7,
	Accumulation = 8,
	SampleCount = 9,
	EnvironmentMap = 10,
	BlasTriangles = 11,
	AlbedoTextures = 12,
	BlasNodes = 13,
	TriangleAttributes = 14,
	TlasNodes = 15,
	InstanceMaterials = 16,
	TraversalStats = 17,
	Count
};

// What a kernel is dispatched over. Everything per-bounce is indirect, with the queue's header
// doubling as the VkDispatchIndirectCommand, so the GPU sizes its own launch and the count is
// never read back to the CPU (PBR 4ed 15.2.4 takes the other option and over-launches; an
// indirect dispatch is strictly better where it is available).
enum class KernelDomain : uint32_t {
	// direct, one invocation per path-pool slot
	Pool,
	// direct, one invocation per pixel
	Pixels,
	// indirect over whichever ray queue this bounce is reading
	CurrentRayQueue,
	// indirect over the fixed queue named by KernelVariant::queue
	FixedQueue
};

// Which extra section the Raytrace Render panel should show while this variant is selected. The
// variant owns the question "are these settings relevant?", so switching strategy hides the
// settings that no longer apply without the panel knowing why.
enum class KernelSettings : uint32_t {
	None,
	// the BVH builder, leaf size and SAH costs, plus the build statistics
	AccelerationStructure
};

// How a frame's cost is estimated for the render guard (RaytraceRenderer::estimatedWorkPerFrame).
// Getting this wrong on the cheap side is how a scene hangs the GPU, so it is a property of the
// traversal strategy rather than a constant.
enum class TraversalCost : uint32_t {
	// the scene's expected cost per ray from its acceleration structure's SAH
	Acceleration,
	// every primitive in the scene, every ray
	AllPrimitives,
	// the kernel does no tracing
	None
};

struct KernelVariant {
	KernelSlot slot { KernelSlot::Count };
	// stable across builds and saved in the scene file. Renaming one silently resets that slot
	// on every scene already saved, so treat it as a format constant
	const char* id { "" };
	// what the Raytracer Shaders panel shows
	const char* name { "" };
	// one line under it
	const char* description { "" };
	// relative to shaders/, without the .spv
	const char* shader { "" };

	KernelDomain domain { KernelDomain::Pool };
	// for KernelDomain::FixedQueue: the CRT_QUEUE_* index in crt_common.glsl
	uint32_t queue { 0 };
	std::vector<CrtBinding> bindings;

	// false for a slot the renderer does not implement yet: no pipeline is built, the scheduler
	// skips it, and the panel greys it out. The shader file still exists as a stub
	bool implemented { true };

	KernelSettings settings { KernelSettings::None };
	TraversalCost cost { TraversalCost::None };
	// a traversal variant only reads BLAS nodes packed in this layout, so selecting the variant
	// is what decides how they are packed - the layout is not separately choosable
	bool requiresLayout { false };
	BvhLayout layout { BvhLayout::Binary };
};

// 00, 01, ... as a two-character string, and the human name of the slot
const char* kernelSlotNumber(KernelSlot slot);
const char* kernelSlotName(KernelSlot slot);
// what the slot is for, for the panel's tooltip
const char* kernelSlotDescription(KernelSlot slot);

// every registered variant of a slot, in the order the panel lists them. Empty for a slot with
// no shader at all
std::span<const KernelVariant> kernelVariants(KernelSlot slot);
// the variant at `index` of `slot`, or nullptr
const KernelVariant* kernelVariant(KernelSlot slot, uint32_t index);
// the index of the variant with this id, or -1
int findKernelVariant(KernelSlot slot, std::string_view id);

// Which variant of each slot is selected. Plain data, compared by value so the renderer can
// restart a render when it changes, and saved with the scene by id rather than by index.
struct KernelSelection {
	uint32_t variant[(size_t)KernelSlot::Count] {};

	uint32_t& operator[](KernelSlot slot) { return variant[(size_t)slot]; }
	uint32_t operator[](KernelSlot slot) const { return variant[(size_t)slot]; }

	const KernelVariant* selected(KernelSlot slot) const { return kernelVariant(slot, variant[(size_t)slot]); }

	bool operator==(const KernelSelection&) const = default;
};

// the first variant of every slot: the BVH traversal, BSDF scattering, no media
KernelSelection defaultKernelSelection();

// the selection as "<slot number>:<variant id>" pairs, for the scene file
std::vector<std::pair<std::string, std::string>> kernelSelectionToIds(const KernelSelection& selection);
// the inverse. Unknown slots and unknown ids are left at their default and reported in
// `warnings` rather than failing the load - a scene saved by a build that had a strategy this
// one does not must still open
KernelSelection kernelSelectionFromIds(const std::vector<std::pair<std::string, std::string>>& ids, std::vector<std::string>* warnings);
