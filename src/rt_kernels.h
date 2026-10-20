#pragma once

// THE KERNEL REGISTRY: the library of wavefront stages, which interchangeable variants each one
// has and what each variant needs bound; the traversals every kernel that traces is built for; and
// the integrators, each a list of the stages it runs. This is the file you edit to add a raytracing
// strategy, a traversal or an integrator, and - apart from the shaders and, for an integrator, its
// schedule - it is meant to be the only one.
//
// THE STAGES are one library shared by every integrator. 00-09 follow the wavefront figure of
// Physically Based Rendering 4ed, chapter 15 (figure 15.2), plus 09 for the film update that sits
// outside it; docs/plans/pbrt-roadmap.md reserves the numbers after them for the plans that add stages.
// shaders/rt/ holds one file per variant, named NNVV_<what it does>.comp for stage NN variant VV,
// so the directory listing IS the diagram.
//
//     00 Generate camera rays      once per frame, over the whole path pool
//     01 Generate samples          per bounce, over the live ray queue (and, for a sampler that
//                                  writes the sample record, once over the pool before 00)
//     02 Intersect closest         per bounce, over the live ray queue
//     03 Handle escaped            per bounce, over the queue 02 filled with misses
//     04 Handle emissive geometry  per bounce, over the queue 02 filled with light hits
//     05 Sample medium interaction not implemented
//     06 Sample surface scattering per bounce, over the queue 02 filled with surface hits
//     07 Sample medium scattering  not implemented
//     08 Trace shadow rays         per bounce, over the queue 06 filled with shadow rays
//     09 Update film               once per frame, over the pixels
//
// THE ONE RULE that keeps a stage shareable: a variant of a stage must work in every integrator
// that runs the stage. A kernel that only makes sense inside one integrator is a new stage.
//
// ADDING A VARIANT. Write shaders/rt/NNVV_name.comp, then add one KernelVariant to the table in
// rt_kernels.cpp naming it, its shader, the bindings it reads or writes, and any capability the
// UI should react to. GpuRenderer builds its pipeline, its descriptor set layout and its dispatch
// from that entry alone; nothing in rt_gpu.cpp knows the name of any particular strategy. If the
// variant needs a resource nothing else uses, add a CrtBinding at the end of the enum and a
// matching binding number at the end of shaders/rt/include/crt_common.glsl, then list it -
// existing numbers never move.
//
// A KERNEL THAT TRACES includes include/crt_trace.glsl rather than a traversal strategy, sets
// KernelVariant::traces, and is compiled once per traversal by the build. The renderer runs the
// build of the selected traversal, with the traversal's bindings added to the kernel's own, so a
// traversal is written once and every tracing kernel of every integrator walks it.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <bvh_layout.h>
#include <sampler.h>

// The stage library, in the path tracer's dispatch order. The numeric values are the NN in the
// shader filenames and are part of the scene file format - never renumber them, only append.
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
	MaterialTextures = 12,
	BlasNodes = 13,
	TriangleAttributes = 14,
	TlasNodes = 15,
	InstanceMaterials = 16,
	TraversalStats = 17,
	Lights = 18,
	ShadowRays = 19,
	TriangleLights = 20,
	EnvironmentSampling = 21,
	// 22 is reserved by docs/plans/pbrt-roadmap.md for a plan not built yet
	// one RaySamples record per pool slot (docs/plans/completed/pbrt-sample-generation.md)
	RaySamples = 23,
	// 24 and 25 are reserved by docs/plans/pbrt-roadmap.md for plans not built yet
	MaterialParams = 26,
	// 27 to 45 are reserved by docs/plans/pbrt-roadmap.md for plans not built yet
	// pbrt's Sobol matrices and the other tables the samplers read, uploaded once
	SamplerTables = 46,
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
	FixedQueue,
	// indirect over a surface queue: the basic material class's build over CRT_QUEUE_SURFACE, the
	// complex class's over CRT_QUEUE_SURFACE_COMPLEX (a KernelVariant::perMaterialClass kernel)
	SurfaceQueues
};

// The material classes a kernel that evaluates materials is built for, in dispatch order. The ids are
// the CRT_MATERIAL_CLASS_<ID> branches of shaders/rt/include/crt_material_class.glsl, lower-cased,
// and the suffix of each build's .spv. Complex is pbrt's universal evaluator, every type
enum class MaterialClass : uint32_t {
	Basic,
	Complex,
	Count
};

const char* materialClassId(MaterialClass materialClass);

// Which extra section the Raytrace Render panel should show while this variant or traversal is
// selected. The selection owns the question "are these settings relevant?", so switching strategy
// hides the settings that no longer apply without the panel knowing why.
enum class KernelSettings : uint32_t {
	None,
	// the BVH builder, leaf size and SAH costs, plus the build statistics
	AccelerationStructure,
	// the sampler's randomization (kernel 01 variants that scramble Sobol or Halton points)
	Sampler
};

// Which lights a kernel 06 variant sends shadow rays to - the strategy, as far as the rest of the
// renderer needs to know it. The render guard prices a shadow ray like another traversal, and the
// panels show the strategy by it
enum class NextEvent : uint32_t {
	// the variant samples no light
	None,
	// point, spot and directional lights only, which nothing else can reach
	DeltaLights,
	// every light in the light list
	AllLights
};

// What one ray costs the render guard (RaytraceRenderer::sceneCostPerRay). Getting this wrong on
// the cheap side is how a scene hangs the GPU, so it is a property of the traversal rather than a
// constant.
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
	// relative to shaders/, without the .spv. A kernel that traces has one build per traversal,
	// at <shader>.<traversal id>.spv, and one built per material class adds .<class id>
	const char* shader { "" };

	KernelDomain domain { KernelDomain::Pool };
	// for KernelDomain::FixedQueue: the CRT_QUEUE_* index in crt_common.glsl
	uint32_t queue { 0 };
	// the bindings the kernel itself reads or writes. A kernel that traces also gets the selected
	// traversal's (TraversalVariant::bindings), so it never lists what the search reads
	std::vector<CrtBinding> bindings;

	// false for a slot the renderer does not implement yet: no pipeline is built, the scheduler
	// skips it, and the panel greys it out. The shader file still exists as a stub
	bool implemented { true };
	// the slot's default: what a new scene selects, and what an unknown id in a scene file falls
	// back to. Entry 0 when no variant of the slot claims it
	bool isDefault { false };

	// includes crt_trace.glsl: built once per traversal, and run as the build of the selected one
	bool traces { false };
	// includes crt_material_class.glsl: built once per MaterialClass, and dispatched once per class,
	// over that class's surface queue (KernelDomain::SurfaceQueues). Every kernel 06 variant
	bool perMaterialClass { false };

	KernelSettings settings { KernelSettings::None };

	// kernel 06: which lights it samples (see NextEvent)
	NextEvent nextEvent { NextEvent::None };

	// kernel 01: the variant writes the RaySamples record, which kernels 00 and 06 then read
	// (the CRT_SAMPLE_RECORD specialization constant), and which the renderer allocates only while one is selected
	bool writesRecord { false };
	// kernel 01: the variant also runs once over the whole pool before kernel 00, with
	// CRT_CAMERA_PASS as the bounce, to write the camera's dimensions
	bool cameraPass { false };
	// kernel 01: the variant needs the render's sample count, so an unlimited render runs another
	bool needsSampleCount { false };
	// kernel 01: the generator it runs, src/sampler.h
	SamplerKind sampler { SamplerKind::Count };

	// kernels 00 and 06: the kernel draws its random numbers through crt_samples.glsl. It is built
	// with the specialization constant CRT_SAMPLE_RECORD (constant_id 0) set to whether the selected
	// sampler writes a record, so a kernel run without one is the code it always was
	bool readsRecord { false };
};

// A traversal: how every kernel that traces searches the scene. A scene-wide choice rather than a
// kernel variant, as pbrt's aggregate is an input to its integrators rather than part of them, so
// kernel 02, kernel 08 and any later kernel that traces always walk the same structure. The id is
// the CRT_TRAVERSAL_<ID> branch in shaders/rt/include/crt_trace.glsl, lower-cased, and the suffix of
// every .spv the build makes for it.
struct TraversalVariant {
	// saved in the scene file; never rename
	const char* id { "" };
	const char* name { "" };
	const char* description { "" };
	// what the search reads, added to the bindings of every kernel that traces
	std::vector<CrtBinding> bindings;
	KernelSettings settings { KernelSettings::None };
	TraversalCost cost { TraversalCost::None };
	// the search reads BLAS nodes packed in this layout, so selecting the traversal is what decides
	// how they are packed - the layout is not separately choosable
	bool requiresLayout { false };
	BvhLayout layout { BvhLayout::Binary };
	bool isDefault { false };
};

// A light transport algorithm: the stages it runs from the library, and (in rt_schedule.cpp) the
// schedule that records them. Append only; the value is not saved, the key is.
enum class IntegratorId : uint32_t {
	Path,
	Count
};

struct IntegratorInfo {
	IntegratorId id { IntegratorId::Count };
	// saved in the scene file; never rename
	const char* key { "" };
	const char* name { "" };
	const char* description { "" };
	// the stages it runs, in dispatch order: what the Raytracer Shaders window lists for it
	std::vector<KernelSlot> stages;
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

// every registered traversal, in the order the panel lists them
std::span<const TraversalVariant> traversalVariants();
// the traversal at `index`, or nullptr
const TraversalVariant* traversalVariant(uint32_t index);
// the index of the traversal with this id, or -1
int findTraversal(std::string_view id);

// every registered integrator, in IntegratorId order
std::span<const IntegratorInfo> integrators();
const IntegratorInfo& integratorInfo(IntegratorId id);
// the integrator with this key, or IntegratorId::Count
IntegratorId findIntegrator(std::string_view key);
// the integrator runs this stage
bool integratorRunsStage(IntegratorId id, KernelSlot slot);

// Which integrator runs, which traversal every kernel that traces walks, and which variant of each
// stage is selected. Plain data, compared by value so the renderer can restart a render when it
// changes, and saved with the scene by id rather than by index. One selection for every
// integrator: a stage two integrators run runs the same variant in both.
struct KernelSelection {
	uint32_t variant[(size_t)KernelSlot::Count] {};
	uint32_t traversal { 0 };
	IntegratorId integrator { IntegratorId::Path };

	uint32_t& operator[](KernelSlot slot) { return variant[(size_t)slot]; }
	uint32_t operator[](KernelSlot slot) const { return variant[(size_t)slot]; }

	const KernelVariant* selected(KernelSlot slot) const { return kernelVariant(slot, variant[(size_t)slot]); }
	const TraversalVariant* selectedTraversal() const { return traversalVariant(traversal); }
	const IntegratorInfo& selectedIntegrator() const { return integratorInfo(integrator); }

	bool operator==(const KernelSelection&) const = default;
};

// the path tracer, every stage's default variant (MIS with the power heuristic, no media) and the
// binary BVH
KernelSelection defaultKernelSelection();

// the selection by id, as the scene file stores it: the integrator's key, the traversal's id, and
// "<slot number>:<variant id>" pairs
struct KernelSelectionIds {
	std::string integrator;
	std::string traversal;
	std::vector<std::pair<std::string, std::string>> stages;
};
KernelSelectionIds kernelSelectionToIds(const KernelSelection& selection);
// the inverse. An empty integrator or traversal keeps the default silently; an unknown one, and
// unknown slots and variant ids, are left at their default and reported in `warnings` rather than
// failing the load - a scene saved by a build that had a strategy this one does not must still open
KernelSelection kernelSelectionFromIds(const KernelSelectionIds& ids, std::vector<std::string>* warnings);
