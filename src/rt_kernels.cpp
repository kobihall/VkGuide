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

// What every traversal's search reads: the instances and their triangles, what the alpha test
// samples (the materials, their textures, the per-surface material table and the triangles' uvs),
// and the work counters every strategy maintains. A BVH adds its node arrays; a linear scan needs
// nothing more
std::vector<CrtBinding> traversalBindings(bool usesAcceleration)
{
	std::vector<CrtBinding> bindings {
		CrtBinding::Instances,
		CrtBinding::Materials,
		CrtBinding::MaterialParams,
		CrtBinding::BlasTriangles,
		CrtBinding::MaterialTextures,
		CrtBinding::TriangleAttributes,
		CrtBinding::InstanceMaterials,
		CrtBinding::TraversalStats,
	};
	if (usesAcceleration) {
		bindings.push_back(CrtBinding::BlasNodes);
		bindings.push_back(CrtBinding::TlasNodes);
	}
	return bindings;
}

// What shaders/rt/include/crt_interaction.glsl reads to rebuild a surface from a thin hit record:
// the hit itself, the instance, the triangle and its attributes, the material table and the
// triangle-light table. Every kernel that calls surfaceInteraction() declares these
std::vector<CrtBinding> withInteraction(std::vector<CrtBinding> bindings)
{
	bindings.insert(bindings.end(), {
		CrtBinding::Hits,
		CrtBinding::Instances,
		CrtBinding::BlasTriangles,
		CrtBinding::TriangleAttributes,
		CrtBinding::InstanceMaterials,
		CrtBinding::TriangleLights,
	});
	return bindings;
}

// The bindings of a kernel 06 variant: the surface its hit record names, the materials and their
// textures, and the shadow rays it may emit towards the light list. `allLights` for a variant that
// samples area lights and the environment as well as delta lights, which reads their geometry and
// textures
std::vector<CrtBinding> scatterBindings(bool allLights)
{
	std::vector<CrtBinding> bindings = withInteraction(withCore({
		CrtBinding::Radiance,
		CrtBinding::Materials,
		CrtBinding::MaterialParams,
		CrtBinding::MaterialTextures,
		CrtBinding::Lights,
		CrtBinding::ShadowRays,
		CrtBinding::RaySamples,
		CrtBinding::PixelState,
	}));
	if (allLights) {
		bindings.push_back(CrtBinding::EnvironmentMap);
		bindings.push_back(CrtBinding::EnvironmentSampling);
	}
	return bindings;
}

// The stage library. One entry per shaders/rt/*.comp; the order within a slot is the order the
// panel lists them, and the variant marked isDefault (or else entry 0) is that slot's default.
const std::vector<KernelVariant>& registry()
{
	static const std::vector<KernelVariant> variants = [] {
		std::vector<KernelVariant> v;

		//---------------------------------------------------------------- 00 generate camera rays
		v.push_back(KernelVariant {
			.slot = KernelSlot::GenerateCameraRays,
			.id = "thin_lens",
			.name = "Box pixels",
			.description = "One camera ray per pool slot, jittered uniformly in the pixel and over the aperture. What a scene saved before pixel filters opens with.",
			.shader = "rt/0001_generate_camera_rays.comp",
			.domain = KernelDomain::Pool,
			.bindings = withCore({ CrtBinding::Radiance, CrtBinding::SampleBudget, CrtBinding::SamplesTaken, CrtBinding::RaySamples, CrtBinding::PixelState, CrtBinding::ImageDistributions }),
			.readsRecord = true,
			.readsCamera = true,
			.usesOutputs = true,
			.laneDependent = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::GenerateCameraRays,
			.id = "filter_importance",
			.name = "Filtered pixels",
			.description = "One camera ray per pool slot, placed by importance sampling the pixel filter, and jittered over the aperture. pbrt-v4's way of filtering.",
			.shader = "rt/0002_generate_camera_rays_filtered.comp",
			.domain = KernelDomain::Pool,
			.bindings = withCore({ CrtBinding::Radiance, CrtBinding::SampleBudget, CrtBinding::SamplesTaken, CrtBinding::RaySamples, CrtBinding::FilterTable, CrtBinding::PixelState, CrtBinding::ImageDistributions }),
			.isDefault = true,
			.settings = KernelSettings::PixelFilter,
			.readsRecord = true,
			.readsCamera = true,
			.usesOutputs = true,
			.laneDependent = true,
		});

		//---------------------------------------------------------------- 01 generate samples
		v.push_back(KernelVariant {
			.slot = KernelSlot::GenerateSamples,
			.id = "pcg_hash",
			.name = "PCG per bounce",
			.description = "No record: kernel 06 draws from each path's PCG stream, decorrelated per bounce. Renders exactly as the tracer did before samplers; what an older scene file opens with.",
			.shader = "rt/0101_generate_samples.comp",
			.domain = KernelDomain::CurrentRayQueue,
			.bindings = WAVEFRONT_CORE,
		});
		//the samplers: each writes the RaySamples record and runs a camera pass first. They differ in
		//what they read: the Sobol ones pbrt's generator matrices, the rest nothing but the film's
		//sample count, which gives a path its sample index
		auto sampler = [&](const char* id, const char* name, const char* description, const char* shader, SamplerKind kind, bool usesTables, KernelSettings settings, bool isDefault, bool needsCount) {
			std::vector<CrtBinding> bindings = withCore({ CrtBinding::RaySamples, CrtBinding::SamplesTaken, CrtBinding::SampleBudget });
			if (usesTables) {
				bindings.push_back(CrtBinding::SamplerTables);
			}
			KernelVariant variant;
			variant.slot = KernelSlot::GenerateSamples;
			variant.id = id;
			variant.name = name;
			variant.description = description;
			variant.shader = shader;
			variant.domain = KernelDomain::CurrentRayQueue;
			variant.bindings = std::move(bindings);
			variant.isDefault = isDefault;
			variant.settings = settings;
			variant.writesRecord = true;
			variant.cameraPass = true;
			variant.needsSampleCount = needsCount;
			variant.sampler = kind;
			v.push_back(std::move(variant));
		};
		sampler("independent", "Independent (PCG, recorded)", "The path's PCG stream written into the record: pcg_hash's numbers in fixed dimensions, to measure the record's own cost.",
			"rt/0102_generate_samples_independent.comp", SamplerKind::Independent, false, KernelSettings::None, false, false);
		sampler("stratified", "Stratified", "Jittered strata, one sample per stratum of the render's Max samples, permuted per dimension. Needs a known sample count.",
			"rt/0103_generate_samples_stratified.comp", SamplerKind::Stratified, false, KernelSettings::None, false, true);
		sampler("halton", "Halton", "Halton points scaled over the image, so neighbouring pixels share one well-distributed set.",
			"rt/0104_generate_samples_halton.comp", SamplerKind::Halton, false, KernelSettings::Sampler, false, false);
		sampler("sobol", "Sobol", "One Sobol sequence over the whole image: every aligned run of samples is a (0, 2) net.",
			"rt/0105_generate_samples_sobol.comp", SamplerKind::Sobol, true, KernelSettings::Sampler, false, false);
		sampler("padded_sobol", "Padded Sobol", "A (0, 2)-sequence per pixel and request, randomly padded within blocks of 2^L samples.",
			"rt/0106_generate_samples_padded_sobol.comp", SamplerKind::PaddedSobol, true, KernelSettings::Sampler, false, false);
		sampler("zsobol", "ZSobol", "Padded Sobol indexed by the pixel's Morton code: blue-noise error between pixels. pbrt-v4's default.",
			"rt/0107_generate_samples_zsobol.comp", SamplerKind::ZSobol, true, KernelSettings::Sampler, true, false);
		sampler("pmj02bn", "PMJ02BN", "Progressive multi-jittered (0, 2) points with blue noise, from pbrt-v4's tables. Needs a known sample count.",
			"rt/0108_generate_samples_pmj02bn.comp", SamplerKind::Pmj02Bn, true, KernelSettings::None, false, true);

		//---------------------------------------------------------------- 02 intersect closest
		//the search is the selected traversal's; what is listed here is what the body around it
		//reads and writes: the ray queue, the hit records and classification queues, and the
		//materials it classifies emissive hits with, which a triangle names through its surface
		//index and the instance's material table
		v.push_back(KernelVariant {
			.slot = KernelSlot::IntersectClosest,
			.id = "closest",
			.name = "Closest hit",
			.description = "Finds each ray's nearest hit with the selected traversal and sorts it onto the escaped, emissive and surface queues.",
			.shader = "rt/0201_intersect_closest.comp",
			.domain = KernelDomain::CurrentRayQueue,
			.bindings = withCore({ CrtBinding::Hits, CrtBinding::Instances, CrtBinding::BlasTriangles, CrtBinding::Materials, CrtBinding::InstanceMaterials, CrtBinding::TraversalStats }),
			.traces = true,
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
			.bindings = withCore({ CrtBinding::Hits, CrtBinding::Radiance, CrtBinding::EnvironmentMap, CrtBinding::Lights, CrtBinding::EnvironmentSampling, CrtBinding::ChannelSums }),
			.usesOutputs = true,
			.laneDependent = true,
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
			.bindings = withInteraction(withCore({ CrtBinding::Radiance, CrtBinding::Materials, CrtBinding::MaterialParams, CrtBinding::MaterialTextures, CrtBinding::Lights, CrtBinding::PixelState, CrtBinding::ChannelSums })),
			.usesOutputs = true,
			.laneDependent = true,
		});

		//---------------------------------------------------------------- 05 sample medium interaction
		v.push_back(KernelVariant {
			.slot = KernelSlot::SampleMediumInteraction,
			.id = "none",
			.name = "Not implemented",
			.description = "The renderer has no participating media. See docs/plans/pbrt-participating-media.md.",
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
			.domain = KernelDomain::SurfaceQueues,
			.queue = QUEUE_SURFACE,
			.bindings = scatterBindings(false),
			.perMaterialClass = true,
			.nextEvent = NextEvent::DeltaLights,
			.readsRecord = true,
			.usesOutputs = true,
			.laneDependent = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::SurfaceScattering,
			.id = "light",
			.name = "Light sampling",
			.description = "A shadow ray to one light per bounce, picked by power; the BSDF sample only continues the path.",
			.shader = "rt/0602_surface_scatter_light.comp",
			.domain = KernelDomain::SurfaceQueues,
			.queue = QUEUE_SURFACE,
			.bindings = scatterBindings(true),
			.perMaterialClass = true,
			.nextEvent = NextEvent::AllLights,
			.readsRecord = true,
			.usesOutputs = true,
			.laneDependent = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::SurfaceScattering,
			.id = "mis_balance",
			.name = "MIS (balance heuristic)",
			.description = "A light sample and a BSDF sample, each weighted by its share of the two densities.",
			.shader = "rt/0603_surface_scatter_mis_balance.comp",
			.domain = KernelDomain::SurfaceQueues,
			.queue = QUEUE_SURFACE,
			.bindings = scatterBindings(true),
			.perMaterialClass = true,
			.nextEvent = NextEvent::AllLights,
			.readsRecord = true,
			.usesOutputs = true,
			.laneDependent = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::SurfaceScattering,
			.id = "mis_power",
			.name = "MIS (power heuristic)",
			.description = "A light sample and a BSDF sample, weighted by their squared densities: pbrt-v4's choice.",
			.shader = "rt/0604_surface_scatter_mis_power.comp",
			.domain = KernelDomain::SurfaceQueues,
			.queue = QUEUE_SURFACE,
			.bindings = scatterBindings(true),
			.isDefault = true,
			.perMaterialClass = true,
			.nextEvent = NextEvent::AllLights,
			.readsRecord = true,
			.usesOutputs = true,
			.laneDependent = true,
		});

		//---------------------------------------------------------------- 07 sample medium scattering
		v.push_back(KernelVariant {
			.slot = KernelSlot::SampleMediumScattering,
			.id = "none",
			.name = "Not implemented",
			.description = "The renderer has no participating media. See docs/plans/pbrt-participating-media.md.",
			.shader = "rt/0701_sample_medium_scattering.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SURFACE,
			.bindings = WAVEFRONT_CORE,
			.implemented = false,
		});

		//---------------------------------------------------------------- 08 trace shadow rays
		//the shadow rays it drains, which carry their own origins, and the radiance they add to; the
		//search is the selected traversal's, the same one kernel 02 walks
		v.push_back(KernelVariant {
			.slot = KernelSlot::TraceShadowRays,
			.id = "occlusion",
			.name = "Occlusion",
			.description = "An any-hit walk with the selected traversal, stopping at the first thing in the way.",
			.shader = "rt/0801_trace_shadow_rays.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SHADOW,
			.bindings = withCore({ CrtBinding::Radiance, CrtBinding::ShadowRays, CrtBinding::TraversalStats, CrtBinding::ChannelSums }),
			.traces = true,
			.usesOutputs = true,
			.laneDependent = true,
		});

		//---------------------------------------------------------------- 09 update film
		v.push_back(KernelVariant {
			.slot = KernelSlot::UpdateFilm,
			.id = "running_mean",
			.name = "Running mean",
			.description = "Folds each pixel's samples into its float mean, skipping unspawned slots and NaNs. A scene saved before the compensated film opens with it. Ignores the sign of a filter's negative lobes.",
			.shader = "rt/0901_update_film.comp",
			.domain = KernelDomain::Pixels,
			.bindings = { CrtBinding::Radiance, CrtBinding::Accumulation, CrtBinding::SamplesTaken, CrtBinding::TraversalStats, CrtBinding::PixelState, CrtBinding::AovImages, CrtBinding::ChannelSums },
			.usesOutputs = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::UpdateFilm,
			.id = "compensated_mean",
			.foldsSplats = true,
			.name = "Compensated mean",
			.description = "Weighted sums with Neumaier compensation, the luminance's running variance, an optional clamp, and a count of samples taken apart from the count accepted, so a dropped sample is not drawn again.",
			.shader = "rt/0902_update_film_compensated.comp",
			.domain = KernelDomain::Pixels,
			.bindings = { CrtBinding::Radiance, CrtBinding::Accumulation, CrtBinding::SamplesTaken, CrtBinding::TraversalStats, CrtBinding::AccumulationError, CrtBinding::PixelState, CrtBinding::AovImages, CrtBinding::ChannelSums, CrtBinding::SplatAccumulation },
			.isDefault = true,
			.settings = KernelSettings::Film,
			.keepsFilmRecord = true,
			.usesOutputs = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::UpdateFilm,
			.id = "outlier_reweighted",
			.foldsSplats = true,
			.name = "Outlier reweighted",
			.description = "The compensated mean, with each pixel's value resolved from a cascade of buffers by brightness: samples of a brightness too rare in the pixel and its neighbors are weighed down or dropped (Zirr et al. 2018). Removes fireflies with far less lost energy than the clamp.",
			.shader = "rt/0903_update_film_outliers.comp",
			.domain = KernelDomain::Pixels,
			.bindings = { CrtBinding::Radiance, CrtBinding::Accumulation, CrtBinding::SamplesTaken, CrtBinding::TraversalStats, CrtBinding::AccumulationError, CrtBinding::FilmCascade, CrtBinding::PixelState, CrtBinding::AovImages, CrtBinding::ChannelSums, CrtBinding::SplatAccumulation },
			.settings = KernelSettings::Film,
			.keepsFilmRecord = true,
			.keepsFilmCascade = true,
			.passes = 2,
			.usesOutputs = true,
		});

		//---------------------------------------------------------------- 11 to 14: light paths
		//docs/plans/completed/pbrt-bidirectional-path-tracing.md. Light tracing runs 11, 01, 02, 12, 13 and 14; the light
		//vertex cache's light pass runs them too. A light path's splats carry its film point in its ShadowRay
		v.push_back(KernelVariant {
			.slot = KernelSlot::EmitLightRays,
			.id = "emit",
			.name = "Emit light rays",
			.description = "Picks a light by its power, samples a ray leaving it (pbrt's SampleLe()), and connects the light's own point to the camera.",
			.shader = "rt/1101_emit_light_rays.comp",
			.domain = KernelDomain::Pool,
			.bindings = withCore({ CrtBinding::Radiance, CrtBinding::SampleBudget, CrtBinding::SamplesTaken, CrtBinding::RaySamples, CrtBinding::Lights, CrtBinding::Instances,
				CrtBinding::Materials, CrtBinding::MaterialParams, CrtBinding::MaterialTextures, CrtBinding::TriangleAttributes, CrtBinding::EnvironmentMap,
				CrtBinding::EnvironmentSampling, CrtBinding::ShadowRays, CrtBinding::ImageDistributions, CrtBinding::LightVertexCache, CrtBinding::BdptVertices }),
			.readsRecord = true,
			.servesLightVertexCache = true,
			.storesBdptVertices = true,
			.laneDependent = true,
			.readsCamera = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::ConnectToCamera,
			.id = "camera",
			.name = "Connect to the camera",
			.description = "Samples a point on the lens from each light path vertex and weighs the connection by the camera's importance and the adjoint BSDF.",
			.shader = "rt/1201_connect_to_camera.comp",
			.domain = KernelDomain::SurfaceQueues,
			.queue = QUEUE_SURFACE,
			.bindings = withInteraction(withCore({ CrtBinding::Materials, CrtBinding::MaterialParams, CrtBinding::MaterialTextures, CrtBinding::ShadowRays, CrtBinding::RaySamples,
				CrtBinding::ImageDistributions, CrtBinding::Lights, CrtBinding::LightVertexCache })),
			.perMaterialClass = true,
			.readsRecord = true,
			.servesLightVertexCache = true,
			.laneDependent = true,
			.readsCamera = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::ScatterLightPaths,
			.id = "importance",
			.name = "Scatter light paths",
			.description = "Samples each light path vertex's BSDF in importance mode, the adjoint of the materials that are not reciprocal, and continues the path.",
			.shader = "rt/1301_scatter_light_paths.comp",
			.domain = KernelDomain::SurfaceQueues,
			.queue = QUEUE_SURFACE,
			.bindings = withInteraction(withCore({ CrtBinding::Materials, CrtBinding::MaterialParams, CrtBinding::MaterialTextures, CrtBinding::RaySamples, CrtBinding::Lights,
				CrtBinding::LightVertexCache })),
			.perMaterialClass = true,
			.readsRecord = true,
			.servesLightVertexCache = true,
			.laneDependent = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::TraceCameraConnections,
			.id = "splat",
			.name = "Trace camera connections",
			.description = "Traces each connection to the camera and splats the unoccluded ones over the pixel filter's footprint.",
			.shader = "rt/1401_trace_camera_connections.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_SHADOW,
			.bindings = withCore({ CrtBinding::ShadowRays, CrtBinding::TraversalStats, CrtBinding::SplatAccumulation, CrtBinding::FilterTable }),
			.traces = true,
			.laneDependent = true,
		});

		//---------------------------------------------------------------- 15, 16: the light vertex cache's camera pass
		v.push_back(KernelVariant {
			.slot = KernelSlot::HandleEmittersBidirectional,
			.id = "recursive_mis",
			.name = "Handle emitters (recursive MIS)",
			.description = "Kernels 03 and 04 for camera paths, each light found weighed against every bidirectional strategy by Georgiev's recursive MIS. Launched over the escaped queue, then the emissive one.",
			.shader = "rt/1501_handle_emitters_lvc.comp",
			.domain = KernelDomain::FixedQueue,
			.queue = QUEUE_ESCAPED,
			.bindings = withInteraction(withCore({ CrtBinding::Radiance, CrtBinding::Materials, CrtBinding::MaterialParams, CrtBinding::MaterialTextures, CrtBinding::Lights,
				CrtBinding::EnvironmentMap, CrtBinding::EnvironmentSampling, CrtBinding::PixelState, CrtBinding::LightVertexCache, CrtBinding::ImageDistributions })),
			.usesOutputs = true,
			.laneDependent = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::ScatterAndConnect,
			.id = "light_vertex_cache",
			.name = "Scatter and connect",
			.description = "Kernel 06 for camera paths with recursive MIS: a light sample, connections to vertices picked from the light vertex cache (traced here), and the scatter.",
			.shader = "rt/1601_scatter_and_connect.comp",
			.domain = KernelDomain::SurfaceQueues,
			.queue = QUEUE_SURFACE,
			.bindings = [] {
				std::vector<CrtBinding> bindings = scatterBindings(true);
				bindings.push_back(CrtBinding::LightVertexCache);
				bindings.push_back(CrtBinding::ImageDistributions);
				return bindings;
			}(),
			.traces = true,
			.perMaterialClass = true,
			.readsRecord = true,
			.usesOutputs = true,
			.laneDependent = true,
		});

		//---------------------------------------------------------------- 17, 18: pbrt 3's bidirectional path tracer
		v.push_back(KernelVariant {
			.slot = KernelSlot::StoreVertices,
			.id = "random_walk",
			.name = "Store vertices and scatter",
			.description = "pbrt-v3's RandomWalk(): records each surface a camera or light subpath reaches as a vertex with its densities both ways, and samples its BSDF in the subpath's transport mode. Also launched over the escaped and emissive queues, where a camera subpath ends.",
			.shader = "rt/1701_store_vertices_and_scatter.comp",
			.domain = KernelDomain::SurfaceQueues,
			.queue = QUEUE_SURFACE,
			.bindings = withInteraction(withCore({ CrtBinding::Materials, CrtBinding::MaterialParams, CrtBinding::MaterialTextures, CrtBinding::RaySamples, CrtBinding::Lights,
				CrtBinding::BdptVertices, CrtBinding::SamplesTaken, CrtBinding::ImageDistributions, CrtBinding::PixelState })),
			.perMaterialClass = true,
			.readsRecord = true,
			.usesOutputs = true,
			.laneDependent = true,
			.readsCamera = true,
		});
		v.push_back(KernelVariant {
			.slot = KernelSlot::ConnectSubpaths,
			.id = "connect_bdpt",
			.name = "Connect subpaths",
			.description = "pbrt-v3's ConnectBDPT() for every strategy (s, t) of a pixel sample's subpaths, each weighed by MISWeight(); the strategies that end on the camera splat. Traces its own visibility tests.",
			.shader = "rt/1801_connect_subpaths.comp",
			.domain = KernelDomain::Pool,
			//no queue, path state or sample record: the vertices hold all it reads of the subpaths, and its spectral build would
			//pass the 31 storage buffers a stage this GPU allows with them
			.bindings = { CrtBinding::Radiance, CrtBinding::SampleBudget, CrtBinding::SamplesTaken, CrtBinding::Instances, CrtBinding::Materials,
				CrtBinding::MaterialParams, CrtBinding::MaterialTextures, CrtBinding::EnvironmentMap, CrtBinding::EnvironmentSampling, CrtBinding::Lights,
				CrtBinding::BlasTriangles, CrtBinding::TriangleAttributes, CrtBinding::InstanceMaterials, CrtBinding::TriangleLights, CrtBinding::ImageDistributions,
				CrtBinding::BdptVertices, CrtBinding::SplatAccumulation, CrtBinding::FilterTable, CrtBinding::TraversalStats, CrtBinding::ChannelSums },
			.traces = true,
			.usesOutputs = true,
			.laneDependent = true,
			.readsCamera = true,
		});

		//---------------------------------------------------------------- 26: the detectors
		//docs/plans/completed/physoptics-detectors.md. Light tracing runs it after kernel 02 while the scene has a detector
		v.push_back(KernelVariant {
			.slot = KernelSlot::DepositOnDetectors,
			.id = "deposit",
			.name = "Deposit on detectors",
			.description = "Reads each light path's segment as kernel 02 left it, and adds the path's power to every detector film it ends on or crosses: a sensor it hit from the front, a probe plane on its way.",
			.shader = "rt/2601_deposit_detector.comp",
			.domain = KernelDomain::CurrentRayQueue,
			.bindings = withCore({ CrtBinding::Hits, CrtBinding::Detectors, CrtBinding::DetectorFilm }),
			.laneDependent = true,
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
	{ "00", "Generate camera rays", "One primary ray per (pixel, sample) slot of the path pool, placed in its pixel uniformly or by the pixel filter. Runs once per frame." },
	{ "01", "Generate samples", "Writes the random numbers each live path's next bounce consumes, into the sample record kernels 00 and 06 read. The sampler: independent, stratified, Halton, Sobol, padded Sobol, ZSobol or PMJ02BN." },
	{ "02", "Intersect closest", "Finds each ray's nearest hit with the selected traversal and sorts the paths into the escaped, emissive and surface queues." },
	{ "03", "Handle escaped", "Paths that left the scene collect the background and end." },
	{ "04", "Handle emissive geometry", "Paths that hit a light collect its emission, weighted by the MIS record kernel 06 left." },
	{ "05", "Sample medium interaction", "Would decide whether a ray scatters in a participating medium before reaching its surface." },
	{ "06", "Sample surface scattering", "Samples a light and scatters the path off the surface it hit. The choice of direct-lighting strategy." },
	{ "07", "Sample medium scattering", "Would scatter a path off a medium's phase function." },
	{ "08", "Trace shadow rays", "Traces the shadow rays kernel 06 emitted, with the selected traversal, and adds the light that gets through." },
	{ "09", "Update film", "Folds the frame's samples into each pixel's value: a running mean, or compensated weighted sums with a variance estimate. Runs once per frame." },
	{ "10", "Sample subsurface", "Reserved for subsurface scattering." },
	{ "11", "Emit light rays", "Starts a light path in each slot: picks a light by its power, samples a ray leaving it, and connects the light's own point to the camera. Runs once per frame." },
	{ "12", "Connect to the camera", "At each surface a light path reaches, samples a point on the lens and evaluates the importance a connection to it carries, for kernel 14 to trace." },
	{ "13", "Scatter light paths", "Samples the adjoint BSDF at each surface a light path reaches, and continues the path." },
	{ "14", "Trace camera connections", "Traces the connections to the camera with the selected traversal, and splats the unoccluded ones onto the film." },
	{ "15", "Handle emitters (light vertex cache)", "Camera paths that left the scene or hit a light collect it, weighed against every bidirectional strategy." },
	{ "16", "Scatter and connect", "Samples a light and connects to vertices of the light vertex cache at each surface a camera path reaches, then scatters it." },
	{ "17", "Store vertices and scatter", "Records each vertex of a camera or light subpath, and scatters it in its transport mode." },
	{ "18", "Connect subpaths", "Evaluates every strategy joining a pixel sample's camera and light subpaths, with pbrt's MIS weights." },
	{ "19", "Store visible points", "Reserved for photon mapping." },
	{ "20", "Deposit photons", "Reserved for photon mapping." },
	{ "21", "Update photon statistics", "Reserved for photon mapping." },
	{ "22", "Mutate chains", "Reserved for Metropolis light transport." },
	{ "23", "Accept and splat", "Reserved for Metropolis light transport." },
	{ "24", "Regenerate paths", "Reserved for the wavefront's performance work." },
	{ "25", "Film per finished path", "Reserved for the wavefront's performance work." },
	{ "26", "Deposit on detectors", "Adds the power of each light path that reaches a detector's front, or crosses a probe plane, to the detector's film. Runs after kernel 02 while the scene has a detector." },
} };

// The traversals, in the order the panel lists them. Each id is a CRT_TRAVERSAL_<ID> branch of
// shaders/rt/include/crt_trace.glsl, lower-cased
const std::vector<TraversalVariant>& traversalRegistry()
{
	static const std::vector<TraversalVariant> traversals {
		{
			.id = "bvh_binary",
			.name = "BVH (binary)",
			.description = "Two-level BVH, Aila-Laine nodes: two boxes per 64-byte fetch, ordered traversal.",
			.bindings = traversalBindings(true),
			.settings = KernelSettings::AccelerationStructure,
			.cost = TraversalCost::Acceleration,
			.requiresLayout = true,
			.layout = BvhLayout::Binary,
			.isDefault = true,
		},
		{
			.id = "bvh_cwbvh",
			.name = "BVH (compressed 8-wide)",
			.description = "Two-level BVH, CWBVH nodes: eight quantised children per node, octant-ordered.",
			.bindings = traversalBindings(true),
			.settings = KernelSettings::AccelerationStructure,
			.cost = TraversalCost::Acceleration,
			.requiresLayout = true,
			.layout = BvhLayout::Cwbvh8,
		},
		{
			.id = "linear",
			.name = "Linear scan (brute force)",
			.description = "No acceleration structure: every ray against every instance and every triangle.",
			.bindings = traversalBindings(false),
			.cost = TraversalCost::AllPrimitives,
		},
	};
	return traversals;
}

// The integrators, in IntegratorId order
const std::vector<IntegratorInfo>& integratorRegistry()
{
	static const std::vector<IntegratorInfo> integrators {
		{
			.id = IntegratorId::Path,
			.key = "path",
			.name = "Path tracing",
			.description = "Unidirectional paths from the camera, with next-event estimation at every surface.",
			.stages = {
				KernelSlot::GenerateCameraRays,
				KernelSlot::GenerateSamples,
				KernelSlot::IntersectClosest,
				KernelSlot::HandleEscaped,
				KernelSlot::HandleEmissive,
				KernelSlot::SampleMediumInteraction,
				KernelSlot::SurfaceScattering,
				KernelSlot::SampleMediumScattering,
				KernelSlot::TraceShadowRays,
				KernelSlot::UpdateFilm,
			},
		},
		{
			.id = IntegratorId::Light,
			.key = "light",
			.name = "Light tracing",
			.description = "Paths from the lights only, each vertex connected to the camera and splatted onto the film (pbrt-v4's LightPathIntegrator). Renders caustics seen directly well and everything else poorly; lights seen directly come from their own points.",
			.stages = {
				KernelSlot::GenerateSamples,
				KernelSlot::EmitLightRays,
				KernelSlot::IntersectClosest,
				KernelSlot::DepositOnDetectors,
				KernelSlot::ConnectToCamera,
				KernelSlot::ScatterLightPaths,
				KernelSlot::TraceCameraConnections,
				KernelSlot::UpdateFilm,
			},
			.bidirectional = true,
		},
		{
			.id = IntegratorId::Lvc,
			.key = "lvc",
			.name = "Bidirectional (light vertex cache)",
			.description = "Davidovic et al. 2014: the frame's light paths fill a cache of their vertices and splat onto the film, then every camera path vertex samples a light and connects to vertices picked from the cache, all weighed by Georgiev's recursive MIS.",
			.stages = {
				KernelSlot::GenerateSamples,
				KernelSlot::EmitLightRays,
				KernelSlot::IntersectClosest,
				KernelSlot::ConnectToCamera,
				KernelSlot::ScatterLightPaths,
				KernelSlot::TraceCameraConnections,
				KernelSlot::GenerateCameraRays,
				KernelSlot::HandleEmittersBidirectional,
				KernelSlot::ScatterAndConnect,
				KernelSlot::TraceShadowRays,
				KernelSlot::UpdateFilm,
			},
			.bidirectional = true,
		},
		{
			.id = IntegratorId::Bdpt,
			.key = "bdpt",
			.name = "Bidirectional (pbrt 3)",
			.description = "pbrt-v3's BDPT: each pixel sample traces a camera subpath and a light subpath and evaluates every strategy joining them, weighed by the balance heuristic over all of them. Renders the image in tiles the vertex budget holds; a sweep of the tiles is one sample per pixel.",
			.stages = {
				KernelSlot::GenerateSamples,
				KernelSlot::EmitLightRays,
				KernelSlot::IntersectClosest,
				KernelSlot::StoreVertices,
				KernelSlot::GenerateCameraRays,
				KernelSlot::ConnectSubpaths,
				KernelSlot::UpdateFilm,
			},
			.bidirectional = true,
		},
	};
	return integrators;
}

}

const char* materialClassId(MaterialClass materialClass)
{
	return materialClass == MaterialClass::Complex ? "complex" : "basic";
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

std::span<const TraversalVariant> traversalVariants()
{
	return traversalRegistry();
}

const TraversalVariant* traversalVariant(uint32_t index)
{
	const std::vector<TraversalVariant>& all = traversalRegistry();
	return index < all.size() ? &all[index] : nullptr;
}

int findTraversal(std::string_view id)
{
	const std::vector<TraversalVariant>& all = traversalRegistry();
	for (uint32_t i = 0; i < (uint32_t)all.size(); i++) {
		if (id == all[i].id) {
			return (int)i;
		}
	}
	return -1;
}

std::span<const IntegratorInfo> integrators()
{
	return integratorRegistry();
}

const IntegratorInfo& integratorInfo(IntegratorId id)
{
	const std::vector<IntegratorInfo>& all = integratorRegistry();
	for (const IntegratorInfo& info : all) {
		if (info.id == id) {
			return info;
		}
	}
	return all.front();
}

IntegratorId findIntegrator(std::string_view key)
{
	for (const IntegratorInfo& info : integratorRegistry()) {
		if (key == info.key) {
			return info.id;
		}
	}
	return IntegratorId::Count;
}

bool integratorRunsStage(IntegratorId id, KernelSlot slot)
{
	const std::vector<KernelSlot>& stages = integratorInfo(id).stages;
	return std::find(stages.begin(), stages.end(), slot) != stages.end();
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
	const std::vector<TraversalVariant>& traversals = traversalRegistry();
	for (uint32_t t = 0; t < (uint32_t)traversals.size(); t++) {
		if (traversals[t].isDefault) {
			selection.traversal = t;
		}
	}
	selection.integrator = IntegratorId::Path;
	return selection;
}

KernelSelectionIds kernelSelectionToIds(const KernelSelection& selection)
{
	KernelSelectionIds out;
	out.integrator = selection.selectedIntegrator().key;
	const TraversalVariant* traversal = selection.selectedTraversal();
	out.traversal = traversal != nullptr ? traversal->id : "";
	for (uint32_t i = 0; i < (uint32_t)KernelSlot::Count; i++) {
		const KernelSlot slot = (KernelSlot)i;
		const KernelVariant* variant = selection.selected(slot);
		if (variant == nullptr) {
			continue;
		}
		out.stages.emplace_back(kernelSlotNumber(slot), variant->id);
	}
	return out;
}

KernelSelection kernelSelectionFromIds(const KernelSelectionIds& ids, std::vector<std::string>* warnings)
{
	KernelSelection selection = defaultKernelSelection();
	auto warn = [&](std::string message) {
		if (warnings != nullptr) {
			warnings->push_back(std::move(message));
		}
	};

	if (!ids.integrator.empty()) {
		const IntegratorId integrator = findIntegrator(ids.integrator);
		if (integrator == IntegratorId::Count) {
			warn(fmt::format("integrator \"{}\" is not one this build has; using \"{}\"", ids.integrator, selection.selectedIntegrator().key));
		} else {
			selection.integrator = integrator;
		}
	}
	if (!ids.traversal.empty()) {
		const int traversal = findTraversal(ids.traversal);
		if (traversal < 0) {
			const TraversalVariant* fallback = selection.selectedTraversal();
			warn(fmt::format("traversal \"{}\" is not one this build has; using \"{}\"", ids.traversal, fallback != nullptr ? fallback->id : "none"));
		} else {
			selection.traversal = (uint32_t)traversal;
		}
	}

	for (const auto& [slotNumber, variantId] : ids.stages) {
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
			warn(fmt::format("kernel slot \"{}\" is not one this build has; ignored", slotNumber));
			continue;
		}
		const int index = findKernelVariant(slot, variantId);
		if (index < 0) {
			const KernelVariant* fallback = selection.selected(slot);
			warn(fmt::format("kernel {} has no variant \"{}\" in this build; using \"{}\"",
				kernelSlotNumber(slot), variantId, fallback != nullptr ? fallback->id : "none"));
			continue;
		}
		selection[slot] = (uint32_t)index;
	}
	return selection;
}
