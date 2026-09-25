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
// sample, and the work counters
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
	});
	if (usesAcceleration) {
		//the only difference between a BVH strategy and a brute-force one, as far as resources go:
		//the node arrays. A linear scan never binds them
		bindings.push_back(CrtBinding::BlasNodes);
		bindings.push_back(CrtBinding::TlasNodes);
	}
	return bindings;
}

// The registry. One entry per shaders/rt/*.comp; the order within a slot is the order the panel
// lists them, and entry 0 is that slot's default.
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
			.description = "Solid colour, environment map or sky gradient, through the path's throughput.",
			.shader = "rt/0301_handle_escaped.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_ESCAPED,
			.bindings = withCore({ CrtBinding::Hits, CrtBinding::Radiance, CrtBinding::EnvironmentMap }),
		});

		//---------------------------------------------------------------- 04 handle emissive
		v.push_back(KernelVariant {
			.slot = KernelSlot::HandleEmissive,
			.id = "direct",
			.name = "Direct emission",
			.description = "A light found by the BSDF is counted at full weight; correct without light sampling.",
			.shader = "rt/0401_handle_emissive.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_EMISSIVE,
			.bindings = withCore({ CrtBinding::Hits, CrtBinding::Radiance, CrtBinding::Materials, CrtBinding::MaterialTextures }),
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
		v.push_back(KernelVariant {
			.slot = KernelSlot::SurfaceScattering,
			.id = "bsdf",
			.name = "BSDF sampling",
			.description = "Sample the material's own lobe. No light sampling, so lights are found by chance.",
			.shader = "rt/0601_surface_scatter_bsdf.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SURFACE,
			.bindings = withCore({ CrtBinding::Hits, CrtBinding::Radiance, CrtBinding::Materials, CrtBinding::MaterialTextures }),
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
		v.push_back(KernelVariant {
			.slot = KernelSlot::TraceShadowRays,
			.id = "none",
			.name = "Not implemented",
			.description = "No kernel 06 variant emits shadow rays yet. See docs/plans/shadow-rays-nee.md.",
			.shader = "rt/0801_trace_shadow_rays.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SHADOW,
			.bindings = WAVEFRONT_CORE,
			.implemented = false,
		});

		//---------------------------------------------------------------- 09 update film
		v.push_back(KernelVariant {
			.slot = KernelSlot::UpdateFilm,
			.id = "running_mean",
			.name = "Running mean",
			.description = "Folds each pixel's samples into its mean, skipping unspawned slots and NaNs.",
			.shader = "rt/0901_update_film.comp",
			.domain = KernelDomain::Pixels,
			.bindings = { CrtBinding::Radiance, CrtBinding::Accumulation, CrtBinding::SampleCount },
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
	{ "04", "Handle emissive geometry", "Paths that hit a light collect its emission and end." },
	{ "05", "Sample medium interaction", "Would decide whether a ray scatters in a participating medium before reaching its surface." },
	{ "06", "Sample surface scattering", "Scatters a path off the surface it hit, or ends it. Where light sampling and MIS go." },
	{ "07", "Sample medium scattering", "Would scatter a path off a medium's phase function." },
	{ "08", "Trace shadow rays", "Would trace the occlusion rays that a light-sampling kernel 06 emits." },
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
	//every slot's first entry, which is how the registry orders its defaults
	return KernelSelection {};
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
		const int index = findKernelVariant(slot, variantId);
		if (index < 0) {
			if (warnings != nullptr) {
				warnings->push_back(fmt::format("kernel {} has no variant \"{}\" in this build; using \"{}\"",
					kernelSlotNumber(slot), variantId, kernelVariant(slot, 0) != nullptr ? kernelVariant(slot, 0)->id : "none"));
			}
			continue;
		}
		selection[slot] = (uint32_t)index;
	}
	return selection;
}
