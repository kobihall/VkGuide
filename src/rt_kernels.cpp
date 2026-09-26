#include <rt_kernels.h>

#include <algorithm>
#include <array>

#include <fmt/format.h>

namespace {

// the queue indices of shaders/rt/include/crt_common.glsl
constexpr uint32_t QUEUE_ESCAPED = 2;
constexpr uint32_t QUEUE_EMISSIVE = 3;
constexpr uint32_t QUEUE_SURFACE = 4;
constexpr uint32_t QUEUE_SHADOW = 5;

// Bindings every per-bounce kernel touches: the paths themselves, the queue set and its headers.
// Spelled out once rather than repeated in nine places
const std::vector<CrtBinding> WAVEFRONT_CORE {
	CrtBinding::Paths,
	CrtBinding::Queues,
	CrtBinding::Headers,
};

std::vector<CrtBinding> withCore(std::initializer_list<CrtBinding> extra)
{
	std::vector<CrtBinding> bindings = WAVEFRONT_CORE;
	bindings.insert(bindings.end(), extra);
	return bindings;
}

// The bindings a traversal variant of kernel 02 shares whatever its strategy: the ray queue it
// reads, the hit records and classification queues it writes, the instances and their geometry,
// the materials it classifies emissive hits with, the textures its alpha test and normal maps
// sample, the work counters, and the table that names an emitting triangle's light record
std::vector<CrtBinding> intersectBindings(bool usesAcceleration)
{
	std::vector<CrtBinding> bindings = withCore({
		CrtBinding::Hits,
		CrtBinding::Instances,
		CrtBinding::Materials,
		CrtBinding::BlasTriangles,
		CrtBinding::MaterialTextures,
		CrtBinding::TriangleAttributes,
		CrtBinding::InstanceMaterials,
		CrtBinding::TraversalStats,
		CrtBinding::TriangleLights,
	});
	if (usesAcceleration) {
		//the only difference between a BVH strategy and a brute-force one, as far as resources go:
		//the node arrays. A linear scan never binds them
		bindings.push_back(CrtBinding::BlasNodes);
		bindings.push_back(CrtBinding::TlasNodes);
	}
	return bindings;
}

// The bindings a kernel 08 variant shares with its kernel 02 partner's search - the instances,
// their geometry, and what the alpha test reads - plus the shadow rays it drains, the hits they
// start from and the radiance they add to. No hit records written, no queues filled
std::vector<CrtBinding> shadowBindings(bool usesAcceleration)
{
	std::vector<CrtBinding> bindings = withCore({
		CrtBinding::Hits,
		CrtBinding::Radiance,
		CrtBinding::ShadowRays,
		CrtBinding::Instances,
		CrtBinding::Materials,
		CrtBinding::BlasTriangles,
		CrtBinding::MaterialTextures,
		CrtBinding::TriangleAttributes,
		CrtBinding::InstanceMaterials,
		CrtBinding::TraversalStats,
	});
	if (usesAcceleration) {
		bindings.push_back(CrtBinding::BlasNodes);
		bindings.push_back(CrtBinding::TlasNodes);
	}
	return bindings;
}

// The bindings of a kernel 06 variant: the surface queue's hits, the materials and their textures,
// and the shadow rays it may emit towards the light list. `allLights` for a variant that samples
// area lights and the environment as well as delta lights, which reads their geometry and textures
std::vector<CrtBinding> scatterBindings(bool allLights)
{
	std::vector<CrtBinding> bindings = withCore({
		CrtBinding::Hits,
		CrtBinding::Radiance,
		CrtBinding::Materials,
		CrtBinding::MaterialTextures,
		CrtBinding::Lights,
		CrtBinding::ShadowRays,
	});
	if (allLights) {
		bindings.push_back(CrtBinding::TriangleAttributes);
		bindings.push_back(CrtBinding::EnvironmentMap);
		bindings.push_back(CrtBinding::EnvironmentSampling);
	}
	return bindings;
}

// The registry. One entry per shaders/rt/*.comp; the order within a slot is the order the panel
// lists them, and the variant marked isDefault (or else entry 0) is that slot's default.
const std::vector<KernelVariant>& registry()
{
	static const std::vector<KernelVariant> variants = [] {
		std::vector<KernelVariant> v;

		//---------------------------------------------------------------- 00 generate camera rays
		v.push_back(KernelVariant {
			.slot = KernelSlot::GenerateCameraRays,
			.id = "thin_lens",
			.name = "Thin lens",
			.description = "One primary ray per pool slot, jittered in the pixel and over the aperture.",
			.shader = "rt/0001_generate_camera_rays.comp",
			.domain = KernelDomain::Pool,
			.bindings = withCore({ CrtBinding::Radiance, CrtBinding::SampleBudget, CrtBinding::SampleCount }),
		});

		//---------------------------------------------------------------- 01 generate samples
		v.push_back(KernelVariant {
			.slot = KernelSlot::GenerateSamples,
			.id = "pcg_hash",
			.name = "PCG per bounce",
			.description = "Decorrelates each live path's PCG stream with the bounce index.",
			.shader = "rt/0101_generate_samples.comp",
			.domain = KernelDomain::CurrentRayQueue,
			.bindings = WAVEFRONT_CORE,
		});

		//---------------------------------------------------------------- 02 intersect closest
		v.push_back(KernelVariant {
			.slot = KernelSlot::IntersectClosest,
			.id = "bvh_binary",
			.name = "BVH (binary)",
			.description = "Two-level BVH, Aila-Laine nodes: two boxes per 64-byte fetch, ordered traversal.",
			.shader = "rt/0201_intersect_closest_bvh.comp",
			.domain = KernelDomain::CurrentRayQueue,
			.bindings = intersectBindings(true),
			.settings = KernelSettings::AccelerationStructure,
			.cost = TraversalCost::Acceleration,
			.requiresLayout = true,
			.layout = BvhLayout::Binary,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::IntersectClosest,
			.id = "bvh_cwbvh",
			.name = "BVH (compressed 8-wide)",
			.description = "Two-level BVH, CWBVH nodes: eight quantised children per node, octant-ordered.",
			.shader = "rt/0202_intersect_closest_cwbvh.comp",
			.domain = KernelDomain::CurrentRayQueue,
			.bindings = intersectBindings(true),
			.settings = KernelSettings::AccelerationStructure,
			.cost = TraversalCost::Acceleration,
			.requiresLayout = true,
			.layout = BvhLayout::Cwbvh8,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::IntersectClosest,
			.id = "linear",
			.name = "Linear scan (brute force)",
			.description = "No acceleration structure: every ray against every instance and every triangle.",
			.shader = "rt/0203_intersect_closest_linear.comp",
			.domain = KernelDomain::CurrentRayQueue,
			.bindings = intersectBindings(false),
			.cost = TraversalCost::AllPrimitives,
		});

		//---------------------------------------------------------------- 03 handle escaped
		v.push_back(KernelVariant {
			.slot = KernelSlot::HandleEscaped,
			.id = "background",
			.name = "Background",
			.description = "Solid colour, environment map or sky gradient, weighted by the path's MIS record.",
			.shader = "rt/0301_handle_escaped.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_ESCAPED,
			.bindings = withCore({ CrtBinding::Hits, CrtBinding::Radiance, CrtBinding::EnvironmentMap, CrtBinding::Lights, CrtBinding::EnvironmentSampling }),
		});

		//---------------------------------------------------------------- 04 handle emissive
		//the id predates MIS, and stays: a scene file names it, and the kernel still adds the light a
		//path hits - weighted now by the record kernel 06 left, which is 1 under BSDF sampling
		v.push_back(KernelVariant {
			.slot = KernelSlot::HandleEmissive,
			.id = "direct",
			.name = "Emission",
			.description = "A light the path hits adds its emission, weighted by the MIS record kernel 06 left.",
			.shader = "rt/0401_handle_emissive.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_EMISSIVE,
			.bindings = withCore({ CrtBinding::Hits, CrtBinding::Radiance, CrtBinding::Materials, CrtBinding::MaterialTextures, CrtBinding::Lights }),
		});

		//---------------------------------------------------------------- 05 sample medium interaction
		v.push_back(KernelVariant {
			.slot = KernelSlot::SampleMediumInteraction,
			.id = "none",
			.name = "Not implemented",
			.description = "The renderer has no participating media. See docs/plans/participating-media.md.",
			.shader = "rt/0501_sample_medium_interaction.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SURFACE,
			.bindings = WAVEFRONT_CORE,
			.implemented = false,
		});

		//---------------------------------------------------------------- 06 surface scattering
		//the direct-lighting strategies of Veach's thesis, chapter 9, in the order of its figures:
		//each one file around the shared include/crt_surface.glsl
		v.push_back(KernelVariant {
			.slot = KernelSlot::SurfaceScattering,
			.id = "bsdf",
			.name = "BSDF sampling",
			.description = "Sample the material's own lobe; a light counts only when a sample hits it. Delta lights still get shadow rays.",
			.shader = "rt/0601_surface_scatter_bsdf.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SURFACE,
			.bindings = scatterBindings(false),
			.nextEvent = NextEvent::DeltaLights,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::SurfaceScattering,
			.id = "light",
			.name = "Light sampling",
			.description = "A shadow ray to one light per bounce, picked by power; the BSDF sample only continues the path.",
			.shader = "rt/0602_surface_scatter_light.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SURFACE,
			.bindings = scatterBindings(true),
			.nextEvent = NextEvent::AllLights,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::SurfaceScattering,
			.id = "mis_balance",
			.name = "MIS (balance heuristic)",
			.description = "A light sample and a BSDF sample, each weighted by its share of the two densities.",
			.shader = "rt/0603_surface_scatter_mis_balance.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SURFACE,
			.bindings = scatterBindings(true),
			.nextEvent = NextEvent::AllLights,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::SurfaceScattering,
			.id = "mis_power",
			.name = "MIS (power heuristic)",
			.description = "A light sample and a BSDF sample, weighted by their squared densities: pbrt-v4's choice.",
			.shader = "rt/0604_surface_scatter_mis_power.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SURFACE,
			.bindings = scatterBindings(true),
			.isDefault = true,
			.nextEvent = NextEvent::AllLights,
		});

		//---------------------------------------------------------------- 07 sample medium scattering
		v.push_back(KernelVariant {
			.slot = KernelSlot::SampleMediumScattering,
			.id = "none",
			.name = "Not implemented",
			.description = "The renderer has no participating media. See docs/plans/participating-media.md.",
			.shader = "rt/0701_sample_medium_scattering.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SURFACE,
			.bindings = WAVEFRONT_CORE,
			.implemented = false,
		});

		//---------------------------------------------------------------- 08 trace shadow rays
		//one partner per kernel 02 traversal, since a shadow ray walks the same structure: selecting
		//the traversal selects its partner here
		v.push_back(KernelVariant {
			.slot = KernelSlot::TraceShadowRays,
			.id = "bvh_binary",
			.name = "BVH (binary)",
			.description = "Any-hit walk of the two-level BVH, binary nodes: stops at the first thing in the way.",
			.shader = "rt/0801_trace_shadow_rays_bvh.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SHADOW,
			.bindings = shadowBindings(true),
			.followsSlot = KernelSlot::IntersectClosest,
			.followsVariant = "bvh_binary",
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::TraceShadowRays,
			.id = "bvh_cwbvh",
			.name = "BVH (compressed 8-wide)",
			.description = "Any-hit walk of the two-level BVH, CWBVH nodes: stops at the first thing in the way.",
			.shader = "rt/0802_trace_shadow_rays_cwbvh.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SHADOW,
			.bindings = shadowBindings(true),
			.followsSlot = KernelSlot::IntersectClosest,
			.followsVariant = "bvh_cwbvh",
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::TraceShadowRays,
			.id = "linear",
			.name = "Linear scan (brute force)",
			.description = "Every instance and triangle until one is in the way.",
			.shader = "rt/0803_trace_shadow_rays_linear.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SHADOW,
			.bindings = shadowBindings(false),
			.followsSlot = KernelSlot::IntersectClosest,
			.followsVariant = "linear",
		});

		//---------------------------------------------------------------- 09 update film
		v.push_back(KernelVariant {
			.slot = KernelSlot::UpdateFilm,
			.id = "running_mean",
			.name = "Running mean",
			.description = "Folds each pixel's samples into its mean, skipping unspawned slots and NaNs.",
			.shader = "rt/0901_update_film.comp",
			.domain = KernelDomain::Pixels,
			.bindings = { CrtBinding::Radiance, CrtBinding::Accumulation, CrtBinding::SampleCount, CrtBinding::TraversalStats },
		});

		return v;
	}();
	return variants;
}

struct SlotInfo {
	const char* number;
	const char* name;
	const char* description;
};

constexpr std::array<SlotInfo, (size_t)KernelSlot::Count> SLOT_INFO { {
	{ "00", "Generate camera rays", "One primary ray per (pixel, sample) slot of the path pool. Runs once per frame." },
	{ "01", "Generate samples", "Prepares each live path's random samples for this bounce. Where a low-discrepancy sampler goes." },
	{ "02", "Intersect closest", "Finds each ray's nearest hit and sorts the paths into the escaped, emissive and surface queues." },
	{ "03", "Handle escaped", "Paths that left the scene collect the background and end." },
	{ "04", "Handle emissive geometry", "Paths that hit a light collect its emission, weighted by the MIS record kernel 06 left." },
	{ "05", "Sample medium interaction", "Would decide whether a ray scatters in a participating medium before reaching its surface." },
	{ "06", "Sample surface scattering", "Samples a light and scatters the path off the surface it hit. The choice of direct-lighting strategy." },
	{ "07", "Sample medium scattering", "Would scatter a path off a medium's phase function." },
	{ "08", "Trace shadow rays", "Traces the shadow rays kernel 06 emitted, with kernel 02's traversal, and adds the light that gets through." },
	{ "09", "Update film", "Folds the frame's samples into the running per-pixel mean. Runs once per frame." },
} };

}

const char* kernelSlotNumber(KernelSlot slot)
{
	return slot < KernelSlot::Count ? SLOT_INFO[(size_t)slot].number : "??";
}

const char* kernelSlotName(KernelSlot slot)
{
	return slot < KernelSlot::Count ? SLOT_INFO[(size_t)slot].name : "unknown";
}

const char* kernelSlotDescription(KernelSlot slot)
{
	return slot < KernelSlot::Count ? SLOT_INFO[(size_t)slot].description : "";
}

std::span<const KernelVariant> kernelVariants(KernelSlot slot)
{
	//the registry is grouped by slot and never reordered, so a slot's variants are one contiguous
	//run and a span over them needs no copy
	const std::vector<KernelVariant>& all = registry();
	size_t first = all.size();
	size_t count = 0;
	for (size_t i = 0; i < all.size(); i++) {
		if (all[i].slot != slot) {
			continue;
		}
		first = std::min(first, i);
		count++;
	}
	if (count == 0) {
		return {};
	}
	return std::span<const KernelVariant>(all.data() + first, count);
}

const KernelVariant* kernelVariant(KernelSlot slot, uint32_t index)
{
	const std::span<const KernelVariant> variants = kernelVariants(slot);
	return index < variants.size() ? &variants[index] : nullptr;
}

int findKernelVariant(KernelSlot slot, std::string_view id)
{
	const std::span<const KernelVariant> variants = kernelVariants(slot);
	for (uint32_t i = 0; i < (uint32_t)variants.size(); i++) {
		if (id == variants[i].id) {
			return (int)i;
		}
	}
	return -1;
}

KernelSelection defaultKernelSelection()
{
	KernelSelection selection;
	for (uint32_t i = 0; i < (uint32_t)KernelSlot::Count; i++) {
		const std::span<const KernelVariant> variants = kernelVariants((KernelSlot)i);
		for (uint32_t v = 0; v < (uint32_t)variants.size(); v++) {
			if (variants[v].isDefault) {
				selection.variant[i] = v;
			}
		}
	}
	return reconcileKernelSelection(selection);
}

bool kernelSlotFollows(KernelSlot slot)
{
	const std::span<const KernelVariant> variants = kernelVariants(slot);
	return !variants.empty() && variants.front().followsSlot != KernelSlot::Count;
}

KernelSelection reconcileKernelSelection(KernelSelection selection)
{
	for (uint32_t i = 0; i < (uint32_t)KernelSlot::Count; i++) {
		const KernelSlot slot = (KernelSlot)i;
		if (!kernelSlotFollows(slot)) {
			continue;
		}
		const std::span<const KernelVariant> variants = kernelVariants(slot);
		const KernelVariant* leader = selection.selected(variants.front().followsSlot);
		for (uint32_t v = 0; v < (uint32_t)variants.size(); v++) {
			if (leader != nullptr && std::string_view(variants[v].followsVariant) == leader->id) {
				selection[slot] = v;
			}
		}
	}
	return selection;
}

std::vector<std::pair<std::string, std::string>> kernelSelectionToIds(const KernelSelection& selection)
{
	std::vector<std::pair<std::string, std::string>> out;
	for (uint32_t i = 0; i < (uint32_t)KernelSlot::Count; i++) {
		const KernelSlot slot = (KernelSlot)i;
		const KernelVariant* variant = selection.selected(slot);
		if (variant == nullptr) {
			continue;
		}
		out.emplace_back(kernelSlotNumber(slot), variant->id);
	}
	return out;
}

KernelSelection kernelSelectionFromIds(const std::vector<std::pair<std::string, std::string>>& ids, std::vector<std::string>* warnings)
{
	KernelSelection selection = defaultKernelSelection();
	for (const auto& [slotNumber, variantId] : ids) {
		//the slot is named by its two-digit number, so a slot this build does not have is simply
		//not found rather than mis-indexed
		KernelSlot slot = KernelSlot::Count;
		for (uint32_t i = 0; i < (uint32_t)KernelSlot::Count; i++) {
			if (slotNumber == kernelSlotNumber((KernelSlot)i)) {
				slot = (KernelSlot)i;
				break;
			}
		}
		if (slot == KernelSlot::Count) {
			if (warnings != nullptr) {
				warnings->push_back(fmt::format("kernel slot \"{}\" is not one this build has; ignored", slotNumber));
			}
			continue;
		}
		//a following slot's choice is derived from its leader's below, whatever the file says; a
		//file older than shadow rays says "none" here
		if (kernelSlotFollows(slot)) {
			continue;
		}
		const int index = findKernelVariant(slot, variantId);
		if (index < 0) {
			if (warnings != nullptr) {
				const KernelVariant* fallback = selection.selected(slot);
				warnings->push_back(fmt::format("kernel {} has no variant \"{}\" in this build; using \"{}\"",
					kernelSlotNumber(slot), variantId, fallback != nullptr ? fallback->id : "none"));
			}
			continue;
		}
		selection[slot] = (uint32_t)index;
	}
	return reconcileKernelSelection(selection);
}
