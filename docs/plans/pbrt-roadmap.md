# Physically based rendering roadmap

This roadmap covers every feature that *Physically Based Rendering* (3rd and 4th editions), the pbrt-v4 source, and *Ray Tracing: The Next Week* and *The Rest of Your Life* describe and this renderer lacks or implements weakly. Each feature has a plan in `docs/plans/`, named `pbrt-*.md` since 2026-10-18. This file records the decisions that shape all of them and reserves the resources they share, so that the plans can be implemented in any order without colliding.

*Written 2026-09-26. The integrator layer was implemented on 2026-09-29, the thin hit record on 2026-10-18, the material system, robust intersections and sample generation on 2026-10-19, the verification harness on 2026-10-20, and the film and reconstruction and the auxiliary outputs on 2026-10-21; nothing else here is. Read `docs/raytracing-overview.md` and `docs/codebase-map.md` §8 first; every plan assumes them.*

*Revised 2026-10-18 for the [laser optics plan](physoptics-laser-optics.md), whose `physoptics-*.md` plans build on these. That plan's roadmap gives one linear order for both sets of plans; it pulls several of these forward (the foundations, the BSDF library, light types, image output, the display transform, the light integrator of bidirectional path tracing, cameras and polarization). The reservations below gain the laser optics plan's rows, and every rule here applies to its plans too.*

## Decisions that shape every plan

The user settled these on 2026-09-26. Treat them as fixed.

| Topic | Decision |
|---|---|
| Spectral transport | A runtime mode. Kernels are compiled in families by lane count (`CRT_LANES` 4, 8, 16, 32). Exact RGB stays available and bit-identical. New scenes default to exact RGB. |
| Spectral data | Materials and lights are authored once as `SpectrumRef`s, and each spectral representation evaluates or projects them. |
| Bidirectional methods | An integrator layer over one shared stage library (revised and implemented 2026-09-29, when the user asked for as much code as possible to serve every integrator). The path tracer's stages keep 00 to 09. BDPT, SPPM and MLT run those stages where they can and add their own to the library. The traversal is a scene-wide choice that every tracing kernel is built for. |
| BDPT and MLT | Two BDPT integrators: a light-vertex-cache BDPT and pbrt 3's. MLT is primary-sample-space MLT over the path tracer first, then multiplexed MLT over pbrt 3's BDPT. |
| Hit record | Thin: kernel 02 writes instance, primitive, barycentrics and t. `crt_interaction.glsl` rebuilds the surface interaction where it is needed. |
| Material dispatch | Two surface queues, basic and complex, with kernel 06 compiled once per class. |
| Material assignment | A named material library. glTF materials are editable in place, and the edits are saved as overrides. |
| Sample generation | Kernel 01 writes a per-bounce sample record. New scenes default to ZSobol; older files keep PCG. |
| Light selection | A setting, a uniform switch in `crt_light.glsl`, like the BVH builder. Not a kernel variant. |
| Textures | A descriptor array of mipmapped images at native size, after a MoltenVK spike. The 2D array with mips is the fallback. |
| Procedural textures | Undecided. The plan is marked incomplete. |
| Cameras | The projection is `SceneCamera` data. Kernel 00 variants are pixel-sampling strategies. The realistic camera traces through a lens assembly from [physoptics-prescription-import.md](physoptics-prescription-import.md) (2026-10-18). |
| Participating media | pbrt-v4's split: kernel 02 sends rays inside a medium to a medium queue, and kernel 05 appends the survivors to the normal queues. |
| Subsurface scattering | Both a volumetric random walk and a BSSRDF, chosen per material. |
| Motion blur | Two poses per object and camera, shutter open and close. No animation timeline. |
| Denoising | Not planned. |
| Research extensions | Planned: path guiding, RIS and ReSTIR, appearance extras, polarization groundwork. Polarization is no longer only groundwork: the laser optics plan's polarization goals depend on it (2026-10-18). |

Technical defaults the user did not object to:

- Watertight triangle intersection behind a runtime flag, with Möller–Trumbore kept.
- Error-bounded ray offsets replace `CRT_T_MIN`, with the old epsilon behind a flag.
- Compensated accumulation, since the Radeon Pro 560X has no `shaderFloat64`.
- Filter importance sampling in kernel 00.
- A hot and cold split of the path state.
- Binding numbers reserved below.
- A line-for-line C++ mirror and a `bin/*_test` for every new GLSL math module.

## The plans

Each plan lists its own prerequisites. The groups follow the order the work should roughly go in.

### Foundations

| Plan | What it adds |
|---|---|
| [integrator-layer.md](completed/integrator-layer.md) | Implemented: the stage library every integrator shares, traversals built into every tracing kernel, per-integrator schedules |
| [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) | Implemented: 24-byte hit record, `SurfaceInteraction` reconstruction, geometric normal, shading frame, shape uvs |
| [pbrt-robust-intersections.md](completed/pbrt-robust-intersections.md) | Implemented: error-bounded spawn points, watertight triangles, conservative box tests |
| [pbrt-verification-harness.md](completed/pbrt-verification-harness.md) | Implemented: `rt_bsdf` and `bin/bsdf_test`, GPU self-tests, furnace and Cornell checks, reference images, convergence curves |
| [pbrt-material-system.md](completed/pbrt-material-system.md) | Implemented: named material library, parameter records, material traits table, basic and complex surface queues |
| [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) | Lane-count families, spectral representations, `SpectrumRef`, uplifting, sensors, blackbody, dispersion |
| [pbrt-sample-generation.md](completed/pbrt-sample-generation.md) | Implemented: per-bounce sample record and kernel 01 variants: independent, stratified, Halton, Sobol, padded Sobol, ZSobol, PMJ02BN |
| [pbrt-film-and-reconstruction.md](completed/pbrt-film-and-reconstruction.md) | Implemented: pixel filters by importance sampling, compensated accumulation, per-pixel variance, clamp and outlier reweighting |

### Appearance

| Plan | What it adds |
|---|---|
| [pbrt-bsdf-library.md](pbrt-bsdf-library.md) | pbrt-v4's BxDFs beside the existing materials: diffuse, conductor, dielectric, thin dielectric, diffuse transmission, mix |
| [pbrt-layered-materials.md](pbrt-layered-materials.md) | Coated diffuse and coated conductor (stochastic layered BSDF), Fresnel blend |
| [pbrt-measured-bsdfs.md](pbrt-measured-bsdfs.md) | RGL measured BRDFs, with the 3rd edition's Fourier BSDF as an option |
| [pbrt-appearance-extras.md](pbrt-appearance-extras.md) | Microfacet energy compensation, sheen, thin-film iridescence, glints (research) |
| [pbrt-textures.md](pbrt-textures.md) | Native-size mipmapped textures, ray-cone footprints, texture references, uv mappings, textures on shapes |
| [pbrt-procedural-textures.md](pbrt-procedural-textures.md) | Checker, noise, fBm, marble and the rest. Incomplete: representation undecided |
| [pbrt-bump-mapping.md](pbrt-bump-mapping.md) | Displacement-texture bump mapping, normal maps in the material kernel, shadow-terminator fixes |
| [pbrt-cameras.md](pbrt-cameras.md) | Orthographic, spherical, realistic lens systems, aperture shapes, tilt-shift, click-to-focus |
| [pbrt-display-and-color.md](pbrt-display-and-color.md) | sRGB display curve, tone mapping operators, photographic exposure, white balance, output gamut |
| [pbrt-aovs-and-image-output.md](completed/pbrt-aovs-and-image-output.md) | Implemented: auxiliary outputs, light groups and Relight, EXR, PFM, HDR and PNG export, the ambient occlusion view and the batch mode. Spectral band images wait for spectral rendering |

### Lights and direct lighting

| Plan | What it adds |
|---|---|
| [pbrt-light-types.md](pbrt-light-types.md) | Projection and goniometric (IES) lights, textured and one-sided area lights, photometric units, emitted-ray sampling |
| [pbrt-environment-lighting.md](pbrt-environment-lighting.md) | Portal environment lights, procedural sky and sun, equal-area maps, map rotation |
| [pbrt-light-shape-sampling.md](pbrt-light-shape-sampling.md) | Cosine-warped triangle and rectangle sampling, projected-solid-angle spheres, better disk and cylinder sampling |
| [pbrt-bvh-light-sampler.md](pbrt-bvh-light-sampler.md) | Light selection setting: uniform, power, exhaustive and BVH (Conty and Kulla) |
| [pbrt-direct-lighting-strategies.md](pbrt-direct-lighting-strategies.md) | One-sample MIS with a mixture density, light-sample splitting, sampling every light |
| [pbrt-resampled-direct-lighting.md](pbrt-resampled-direct-lighting.md) | RIS and spatial ReSTIR as kernel 06 variants (research) |

### Path construction

| Plan | What it adds |
|---|---|
| [pbrt-russian-roulette.md](pbrt-russian-roulette.md) | pbrt's roulette with `etaScale`, shadow-ray roulette, efficiency-optimized thresholds |
| [pbrt-path-regularization.md](pbrt-path-regularization.md) | Roughening after the first non-specular bounce |
| [pbrt-path-guiding.md](pbrt-path-guiding.md) | Learned incident radiance (SD-tree) as a kernel 06 variant (research) |
| [pbrt-polarization.md](pbrt-polarization.md) | Stokes and Mueller transport on top of the spectral lanes (research groundwork) |

### Volumes

| Plan | What it adds |
|---|---|
| [pbrt-participating-media.md](pbrt-participating-media.md) | Kernels 05 and 07: homogeneous and grid media, null scattering, chromatic media, volumetric emission (revised) |
| [pbrt-subsurface-scattering.md](pbrt-subsurface-scattering.md) | Random-walk subsurface and a tabulated BSSRDF with probe rays |

### Geometry and acceleration

| Plan | What it adds |
|---|---|
| [pbrt-camera-relative-space.md](pbrt-camera-relative-space.md) | Double-precision scene positions and a render origin, for scenes far from the origin. Not planned to start until a scene needs it |
| [pbrt-analytic-shapes.md](pbrt-analytic-shapes.md) | Disks, annuli, partial quadrics, cones, paraboloids, hyperboloids, bilinear patches |
| [pbrt-motion-blur.md](pbrt-motion-blur.md) | Shutter times, two-pose instances and cameras, motion bounds |
| [pbrt-curves-and-hair.md](pbrt-curves-and-hair.md) | Bézier curve primitive and the hair BSDF |
| [pbrt-subdivision-and-displacement.md](pbrt-subdivision-and-displacement.md) | Loop subdivision and displacement by tessellation |
| [pbrt-csg.md](pbrt-csg.md) | Constructive solid geometry over analytic shapes |
| [pbrt-alternative-accelerators.md](pbrt-alternative-accelerators.md) | Kd-tree and grid traversals, LBVH builder |
| [pbrt-gpu-bvh-construction.md](pbrt-gpu-bvh-construction.md) | LBVH and PLOC built on the GPU, TLAS refit |

### Other integrators

| Plan | What it adds |
|---|---|
| [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md) | Light tracing, then two BDPT integrators (a light vertex cache for the GPU, and pbrt 3's), with film splatting |
| [pbrt-photon-mapping.md](pbrt-photon-mapping.md) | Stochastic progressive photon mapping |
| [pbrt-metropolis-light-transport.md](pbrt-metropolis-light-transport.md) | Primary-sample-space MLT over the path tracer, then multiplexed MLT over pbrt 3's BDPT |

### Performance and tools

| Plan | What it adds |
|---|---|
| [pbrt-wavefront-performance.md](pbrt-wavefront-performance.md) | Path regeneration, subgroup queue allocation, shared-memory traversal stacks, ray sorting |
| [pbrt-adaptive-sampling.md](pbrt-adaptive-sampling.md) | Variance-driven sample budgets (revised) |
| [pbrt-scene-import.md](pbrt-scene-import.md) | A subset of pbrt-v4's scene format, for comparing against pbrt |

### Considered and not planned

- Denoising, at the user's request. The auxiliary outputs in [pbrt-aovs-and-image-output.md](completed/pbrt-aovs-and-image-output.md) are what a denoiser would need later.
- Neural representations of materials, lights or radiance (PBR 4ed §16.3.2), light-field cameras and focal stacks (4th edition, chapter 5 exercises), point-sampled geometry, sensor noise simulation and a shading language. None of these is a feature pbrt implements, and none fits the tracer without a large detour.
- The stochastic alpha test (PBR 4ed §7.1.1). The user chose the deterministic alpha test on 2026-09-25.
- Deformation motion blur (PBR 4ed exercise 6.15). The user chose rigid two-pose motion; [pbrt-motion-blur.md](pbrt-motion-blur.md) notes what deformation would need.
- A hardware ray-tracing backend (PBR 4ed exercise 15.8). MoltenVK exposes no ray query or ray-tracing pipeline on the Radeon Pro 560X.

[simulation-domain.md](simulation-domain.md) and [wave-simulation.md](wave-simulation.md) predate this roadmap and plan a separate 2D wave-equation solver, not the path tracer. They follow their own template and are not bound by the rules below. [pbrt-polarization.md](pbrt-polarization.md) points at them as a second route to wave effects. The coherent wave backend the user plans to build last, after every plan here and in the [laser optics plan](physoptics-laser-optics.md), is not planned in any file.

## Dependencies between the plans

```text
integrator-layer (implemented) ─> bidirectional-path-tracing ─┬─> photon-mapping
                                                              └─> metropolis-light-transport

thin-hit-record ─┬─> robust-intersections ─> camera-relative-space
                 ├─> material-system ─┬─> bsdf-library ─┬─> layered-materials
                 │                    │                 ├─> measured-bsdfs
                 │                    │                 ├─> appearance-extras
                 │                    │                 ├─> curves-and-hair
                 │                    │                 ├─> polarization
                 │                    │                 └─> subsurface-scattering
                 │                    ├─> textures ─┬─> bump-mapping
                 │                    │             ├─> procedural-textures
                 │                    │             └─> subdivision-and-displacement
                 │                    └─> light-types ─┬─> environment-lighting
                 │                                     └─> bidirectional-path-tracing
                 ├─> analytic-shapes ─> csg
                 ├─> participating-media ─> subsurface-scattering
                 └─> motion-blur

spectral-rendering ─> (bsdf-library dispersion, light-types blackbody, polarization)
sample-generation ─> motion-blur, metropolis-light-transport (and every kernel 06 and 07 variant reads its record)
film-and-reconstruction ─> display-and-color, aovs-and-image-output, adaptive-sampling, path-guiding
aovs-and-image-output ─> pbrt-scene-import (the harness already reads and writes EXR through tinyexr)
light-shape-sampling ─ prevNormal ─ bvh-light-sampler ─┬─> direct-lighting-strategies
                                                        └─> resampled-direct-lighting
path-guiding ─> russian-roulette (adjoint-driven rule)
alternative-accelerators ─> gpu-bvh-construction ─> wavefront-performance (radix sort)
verification-harness ─> (every plan with a new BSDF, sampler or light)
```

The foundations change the shape of every kernel, so do them first and in this order (the integrator layer, the thin hit record, the material system, robust intersections, sample generation, the verification harness and the film are done; the material system and sample generation came ahead of plans they do not need): spectral rendering. After that the groups are largely independent.

## Rules every plan follows

### Exact RGB stays bit-identical

Every plan must leave a scene that does not use its feature rendering bit-identical in exact RGB mode with a fixed seed, and must say so in its verification section. That comparison (codebase map §8.11) is the regression test that caught every earlier mistake. Where a plan changes default behavior on purpose (ZSobol for new scenes, error-bounded offsets), the old behavior stays reachable through a flag or a variant, so the comparison can still be made.

### Binding numbers

Binding numbers are global and append-only (`CrtBinding`, `crt_common.glsl`). Numbers 0 to 21, 23, 26, 29 to 32, 45, 46 and 62 exist. These are reserved so that plans landing in any order never collide. A plan that is never built leaves a gap, which is legal.

| Binding | Resource | Plan |
|---|---|---|
| 22 | `Media` | participating-media |
| 23 | `RaySamples` | sample-generation |
| 24 | `SpectralData` | spectral-rendering |
| 25 | `SpectralUplift` | spectral-rendering |
| 26 | `MaterialParams` | material-system |
| 27 | `Textures` (descriptor array) | textures |
| 28 | `LightBvh` | bvh-light-sampler |
| 29 | `PixelState` | aovs-and-image-output (implemented): 72 bytes a pool slot while an output is on. A sample's filter weight rides in its radiance slot's `w` ([completed/pbrt-film-and-reconstruction.md](completed/pbrt-film-and-reconstruction.md)) |
| 30 | `FilterTable` | film-and-reconstruction (implemented) |
| 31 | `AccumulationError` | film-and-reconstruction (implemented) |
| 32 | `AovImages` | aovs-and-image-output (implemented) |
| 33 | `MediumGrids` | participating-media |
| 34 | `Curves` | curves-and-hair |
| 35 | `TextureNodes` | procedural-textures |
| 36 | `EnvironmentExtras` (portals, sky) | environment-lighting |
| 37 | `GuidingTree` | path-guiding |
| 38 | `Reservoirs` | resampled-direct-lighting |
| 39 | `MotionTransforms` | motion-blur |
| 40 | `BssrdfTables` | subsurface-scattering |
| 41 | `MeasuredBsdfs` | measured-bsdfs |
| 42 | `SplatAccumulation` | bidirectional-path-tracing |
| 43 | `PhotonGrid` | photon-mapping |
| 44 | `MarkovChains` | metropolis-light-transport |
| 45 | `ChannelSums` | aovs-and-image-output (implemented): the light groups, then `sum(w)`; spectral bands will follow |
| 46 | `SamplerTables` | sample-generation |
| 47 | `BsdfTables` | appearance-extras |
| 48 | `GuidingVertices` | path-guiding |
| 49 | `MediumPathState` | participating-media |
| 50 | `ShapeParams` | analytic-shapes |
| 51 | `CsgNodes` | csg |
| 52 | `LightVertexCache` | bidirectional-path-tracing |
| 53 | `BdptVertices` | bidirectional-path-tracing |
| 54 | `VisiblePoints` | photon-mapping |

Bindings 55 to 61 are reserved by the [laser optics plan](physoptics-laser-optics.md): 55 `OpticSurfaces`, 56 `OpticCoatings`, 57 `OpticPathState`, 58 `BeamGraph`, 59 `Detectors`, 60 `DetectorFilm`, 61 `RayRecords`. Binding 62, `FilmCascade`, is the outlier-reweighted film's cascade of buffers by brightness, which the film plan added.

The Radeon Pro 560X allows 31 storage buffers per stage (`maxPerStageDescriptorStorageBuffers`). No kernel may declare more; the per-variant subsets keep every kernel well under it today (kernel 02 declares 14).

### Queue indices

The queue set (`crt_common.glsl`) grows by appending. Each queue costs `poolSize` × 4 bytes, 16 MB at the 4M-path cap.

| Queue | Name | Plan |
|---|---|---|
| 0 to 5 | existing: ray A, ray B, escaped, emissive, surface, shadow | |
| 4 | `CRT_QUEUE_SURFACE` becomes the basic-material queue | material-system |
| 6 | `CRT_QUEUE_SURFACE_COMPLEX` | material-system |
| 7 | `CRT_QUEUE_MEDIUM_SAMPLE` | participating-media |
| 8 | `CRT_QUEUE_MEDIUM_SCATTER` | participating-media |
| 9 | `CRT_QUEUE_SUBSURFACE` | subsurface-scattering |
| 10 | `CRT_QUEUE_DETECTOR` | physoptics-detectors |

### Path state

`PathState` is read by every per-bounce kernel, so it stays small ("hot"). Data used once per path moves to a new per-slot `PixelState` buffer ("cold", binding 29), which pbrt-v4's wavefront calls `PixelSampleState`. Fields in `PathState` by owner:

| Field | Type | Owner |
|---|---|---|
| `origin`, `radianceSlot`, `direction`, `rngState`, `bounce`, `misPdf`, `misExponent` | existing | |
| `prevNormal` | `uint`: the octahedral-encoded shading normal kernel 06 sampled lights with | light-shape-sampling |
| `throughput` | `Spectrum` (`CRT_LANES` floats) | spectral-rendering (was `vec3`) |
| `medium` | `uint`, reuses `pad0` | participating-media |
| `flags` | `uint`, reuses `pad1`: specular bounce, any non-specular bounce, inside-dielectric | path-regularization, russian-roulette |
| `etaScale` | `float` | russian-roulette |
| `lambdaU` | `float`: the one sample hero wavelengths are derived from | spectral-rendering |
| `laneMask` | `uint`: which lanes are still alive after dispersion | spectral-rendering |
| `time` | `float` | motion-blur |
| `coneWidth`, `coneSpread` | `float` × 2 | textures |

With `CRT_LANES = 4` that is 96 bytes: the `Spectrum` aligns to 16 bytes, so the fields end at byte 96 with no padding. A further field grows the struct to 112. Fields in `PixelState`: the first-hit position, normals, depth, uv, albedo and ids (aovs-and-image-output). The film plan did not allocate it: a sample's filter weight (+1 or -1) is the `w` of its radiance slot, and the camera's ray weight can start as the path's throughput, since pbrt multiplies it into the radiance and not into the film weight. Light groups need no path state: each contribution is added to its group's sums as it happens.

The radiance slot (`radiance[]`, binding 4) stays a `vec4` in film space in every mode. Kernels that add radiance convert their lanes to film space at the moment they add it (spectral-rendering), so kernel 09 and the accumulation image keep their size whatever the lane count.

### Shadow rays

`ShadowRay` is one record per path slot, so its size multiplies by the pool: every 16 bytes cost 64 MB at the 4M-path cap. Its layout with `CRT_LANES = 4`, 64 bytes:

| Field | Type | Owner |
|---|---|---|
| `origin` | `vec3` | thin-hit-record (implemented) |
| `mediumAndGroup` | `uint`: the medium the ray starts in (low 24 bits) and the light group (high 8 bits) | participating-media, aovs-and-image-output (the group is implemented) |
| `direction`, `tMax` | `vec3`, `float` | existing |
| `contribution` | `Spectrum` (was `vec3`) | spectral-rendering |
| `radianceSlot` | `uint` | existing |
| `lambdaU`, `laneMask` | `float`, `uint`: what kernel 08 needs to convert lanes to film space | spectral-rendering |
| `time` | `float` | motion-blur |

### Instances

`GpuInstance` grew to 128 bytes in [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md). Its two padding words are taken: the first by the medium interface (participating-media), the second by the index into `MotionTransforms` (motion-blur). For a shape, `attributeBase`, which only meshes use, indexes `ShapeParams` (analytic-shapes); for an optical body (`CRT_SHAPE_OPTIC`, physoptics-optical-surfaces) it indexes `OpticSurfaces` instead, and `triangleCount` holds the body's surface count. A plan that needs more per-instance data adds a buffer indexed by instance rather than growing the struct.

### New stages, traversals and directories

The stage library ([integrator-layer.md](completed/integrator-layer.md)) is shared by every integrator, and its numbers are scene-file keys, so they are append-only. 00 to 09 are the path tracer's. A variant of a stage must work in every integrator that runs the stage; a kernel that only makes sense inside one integrator is a new stage. These numbers are reserved so that plans landing in any order never collide:

| Stages | Plan |
|---|---|
| 10 | subsurface-scattering: sample subsurface |
| 11 to 18 | bidirectional-path-tracing: 11 emit light rays, 12 connect to the camera, 13 scatter light paths, 14 trace camera connections, 15 handle emitters (`lvc`), 16 scatter and connect (`lvc`), 17 store vertices and scatter (`bdpt`), 18 connect subpaths (`bdpt`) |
| 19 to 21 | photon-mapping: 19 store visible points, 20 deposit photons, 21 update photon statistics |
| 22, 23 | metropolis-light-transport: 22 mutate chain samples, 23 accept and splat |
| 24, 25 | wavefront-performance: 24 regenerate paths, 25 film per finished path |
| 26 | physoptics-detectors: deposit on detectors |

New variants of a stage take the next free `VV` in it; the plans name the ones they add, and a clash between two plans is settled by whichever lands second taking the next number.

A kernel that traces includes `crt_trace.glsl` and is compiled once per traversal, so a new traversal is a strategy file, a `crt_trace.glsl` branch, and a `TraversalVariant` row, never a `.comp` file. Stage shaders stay in `shaders/rt/`, since integrators share them. A schedule's fixed passes, which nobody swaps (grid builds, prefix sums, bootstraps), are plain compute passes in `shaders/rt/<integrator>/`.

### Scene files

Every plan that saves something bumps `SCENE_FILE_FORMAT_VERSION` by one when it lands (15 today), and every older version keeps loading. Plans do not pre-assign version numbers, since the order they land in is unknown.

### CPU mirrors and tests

Every GLSL module that holds sampling or scattering mathematics gets a line-for-line C++ mirror in a Vulkan-free static library, and a command-line test in `tests/`, the way `crt_light.glsl` has `light_sampling.cpp` and `bin/light_test`. The libraries these plans create:

| Library | GLSL | Test | Plan |
|---|---|---|---|
| `rt_spectrum` | `crt_spectrum.glsl` | `bin/spectrum_test` | spectral-rendering |
| `rt_bsdf` | `crt_scatter.glsl` now, then `crt_bsdf_*.glsl`, `crt_microfacet.glsl` | `bin/bsdf_test`, and the GPU self-test | verification-harness (implemented), bsdf-library |
| `rt_sampler` | `crt_sampler.glsl` | `bin/sampler_test` | sample-generation (implemented) |
| `rt_film` | `crt_filter.glsl`, `crt_film.glsl` | `bin/film_test`, and the GPU self-tests | film-and-reconstruction (implemented) |
| `rt_media` | `crt_medium.glsl` | `bin/media_test` | participating-media |
| `rt_optics` | `crt_optic.glsl`, `crt_coating.glsl`, `crt_tmm.glsl`, `crt_birefringence.glsl` | `bin/optics_test`, `bin/beam_test` | the physoptics plans |

## Sources

*Physically Based Rendering*, 3rd edition (2016, online 2018) and 4th edition (2023, online), with the pbrt-v4 source in `../pbr-book-website/pbrt-v4`; Peter Shirley, Trevor David Black and Steve Hollasch, *Ray Tracing: The Next Week* and *Ray Tracing: The Rest of Your Life* (version 4.0.2, 2025). Each plan cites the sections it draws on.
