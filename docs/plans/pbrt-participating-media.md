# Participating media

Everything the tracer renders is a surface: a ray crosses vacuum until it hits something. Participating media make the space between surfaces take part: fog, smoke, clouds, murky water, the inside of a glass of milk. A ray crossing one may be absorbed or scattered before it reaches the surface kernel 02 found. Kernels 05 and 07 of PBR 4ed Figure 15.2 exist for this and are stubs here. This plan fills them with pbrt-v4's volumetric path tracing: null-scattering delta tracking against majorants, ratio-tracked transmittance for shadow rays, chromatic media through spectral MIS, and homogeneous, grid and NanoVDB media. *Ray Tracing: The Next Week*'s constant-density medium is the homogeneous case.

*Not implemented. `shaders/rt/0501_sample_medium_interaction.comp` and `0701_sample_medium_scattering.comp` are registered with `implemented = false`. Depends on [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) and [pbrt-material-system.md](pbrt-material-system.md) (the `interface` material). Chromatic media use [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)'s lanes. This revision replaces the earlier draft of this file. Part of the [roadmap](pbrt-roadmap.md).*

## Decision already made

The earlier draft weighed three ways for kernel 05 to overrule kernel 02's classification of a hit: (a) move all classification into kernel 05, (b) mark paths dead for kernel 06 to skip, or (c) have kernel 05 rebuild the surface queue. On 2026-09-26 the user chose pbrt-v4's arrangement instead:

- Kernel 02 classifies rays that travel through vacuum exactly as today, and sends rays that travel inside a medium to a medium queue with their hit.
- Kernel 05 drains that queue, samples the medium up to the hit, and appends the paths that reach the surface to the normal escaped, emissive and surface queues itself.

Nothing is undone, scenes without media pay nothing, and kernel 05 runs only over the rays that need it.

## What the renderer does today

- No medium concept exists in `rt_scene_types.h`, the scene file or the GPU structs.
- Kernel 02 writes `hits[]` and appends each path to the escaped, emissive and surface queues (`crt_intersect.glsl`).
- `PathState.pad0` and `pad1` are free; the roadmap gives `pad0` to this plan as `medium`.
- `GpuInstance` (`src/rt_accel.h`) has two padding words.
- Kernel 08 answers visible or blocked; it has no notion of partial transmittance.

## Design

### The schedule

```text
02 intersect
 ├─ ray in vacuum ──> escaped, emissive and surface queues, as today
 └─ ray in a medium ─> queue 7 (MEDIUM_SAMPLE) ─> 05 sample the medium
                                                  ├─ reached the surface ─> escaped, emissive and surface queues
                                                  ├─ absorbed: the path ends
                                                  └─ scattered ─> queue 8 (MEDIUM_SCATTER) ─> 07 ─> next ray, shadow ray
```

The per-bounce order becomes `02 → 05 → {03, 04, 06, 07} → 08`. The classification code moves from `crt_intersect.glsl` into `crt_classify.glsl`, which kernels 02 and 05 share, so both append to the queues the same way, including the basic and complex surface queues of [pbrt-material-system.md](pbrt-material-system.md). Queues 7 and 8 are the roadmap's `CRT_QUEUE_MEDIUM_SAMPLE` and `CRT_QUEUE_MEDIUM_SCATTER`.

### Scene model

- `SceneMedium` in a named list beside the materials: a kind, σa and σs as `SpectrumRef`s with a scale, a Henyey–Greenstein asymmetry `g`, and optional emission `Le` with a scale. Grid kinds add a file and a transform.
- A `MediumInterface {inside, outside}` on every shape and mesh object, both "none" by default. `GpuInstance` packs the two indices into one of its padding words.
- `SceneCamera.medium`, the medium the camera sits in. Kernel 00 starts paths in it.
- The `interface` material type ([pbrt-material-system.md](pbrt-material-system.md)), pbrt's material-less surface: invisible, it only changes the medium a ray is in. Kernels 02 and 05 continue the ray through it into the next ray queue with the new medium, without a bounce. Such a crossing still takes one wavefront iteration, so a scene with interfaces records its bounce loop 4 iterations past the ray depth.

*Ray Tracing: The Next Week*'s `constant_medium` with density `d` and albedo `a` inside a boundary shape is a homogeneous medium with σs = d·a and σa = d·(1 − a), isotropic, inside a shape whose material is `interface`. Its "Cornell box with smoke" scene becomes a test scene.

### Media kinds

| Kind | Data | pbrt-v4 |
|---|---|---|
| Homogeneous | σa, σs, g, Le | `homogeneous` |
| Density grid | a float grid scaling σa and σs; optional temperature grid for blackbody emission ([pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)'s `Blackbody` kind) | `uniformgrid` |
| RGB grid | per-voxel RGB σa, σs and Le, uplifted like textures | `rgbgrid` |
| NanoVDB | a sparse `.nvdb` volume, read on the GPU through NanoVDB's portable `PNanoVDB.h`, which compiles as GLSL | `nanovdb` |
| Cloud | pbrt's procedural cloud density, once `crt_noise.glsl` exists ([pbrt-procedural-textures.md](pbrt-procedural-textures.md)) | `cloud` |

Medium records go into binding 22 (`Media`). Grids, NanoVDB buffers and majorant grids go into binding 33 (`MediumGrids`).

### Kernel 05: sampling the medium

pbrt's `SampleT_maj()` with null scattering (PBR 4ed §11.2.1 and §14.1.3): a majorant σmaj bounds σt along the ray. For a homogeneous medium it is σt itself; for a grid it comes from a 16³ majorant grid walked by a DDA (pbrt's `MajorantGrid` and `DDAMajorantIterator`). At each tentative collision, sampled by the majorant:

- Add emission `β σa Le / σmaj` if the medium emits, as pbrt does at every event.
- Choose absorption, real scattering or null scattering with probabilities `σa/σmaj`, `σs/σmaj` and `σn/σmaj`.
- Absorption ends the path. Scattering appends it to queue 8 with the scattering point. Null scattering continues.
- Passing the hit distance appends the path to the surface queues through `crt_classify.glsl`, or to the escaped queue if there was no hit.

The number of events is unbounded, so after the first two numbers, which come from `RaySamples.medium` ([pbrt-sample-generation.md](pbrt-sample-generation.md)), kernel 05 draws from the path's PCG stream. pbrt does the same with an RNG hashed from the ray.

### Chromatic media

When σt differs between lanes, sampling distances by one lane's majorant would starve the others. pbrt samples by the first (hero) wavelength and weighs with spectral MIS, carrying per-lane rescaled path probabilities `r_u` (unidirectional) and `r_l` (light sampling) that fold the distance-sampling densities into the MIS weights (PBR 4ed §14.2.2 and §14.2.3). This plan does the same:

- `MediumPathState {Spectrum r_u; Spectrum r_l;}` per path slot, 32 bytes at four lanes, in binding 49 (new). It is allocated only when the scene has a medium, and kernels 03, 04, 05, 06, 07 and 08 read it behind a uniform `CRT_FLAG_MEDIA` flag. Scenes without media keep the scalar MIS record.
- In exact RGB the lanes are fixed channels, not random wavelengths, so kernel 05 picks the sampling channel uniformly at random per path. That keeps pbrt's one-sample MIS over lanes valid.
- pbrt's formulation is the balance heuristic over techniques and lanes. In scenes with media, the MIS power variant therefore behaves as the balance one, and the panel says so.

### Kernel 07: scattering in the medium

The medium's counterpart of kernel 06, with a phase function in place of a BSDF:

- Henyey–Greenstein sampling and evaluation (`crt_medium.glsl`), isotropic at `g = 0`.
- Next-event estimation as in the MIS variants of kernel 06, with the phase function's value and density. A medium vertex has no normal, so it sets `CRT_PATH_PREV_IN_MEDIUM` in `PathState.flags`, and light shape sampling ([pbrt-light-shape-sampling.md](pbrt-light-shape-sampling.md)) and the BVH light sampler's receiver term use their normal-free forms.
- The continuation ray goes to the next ray queue in the same medium, and the shadow ray records the medium it starts in (`ShadowRay.mediumAndGroup`, the roadmap's shadow-ray table).

### Kernel 08: transmittance

A shadow ray through a medium is attenuated, not blocked. pbrt's `TraceTransmittance()` walks the segment: the closest hit along it is either an opaque surface (the ray is blocked) or an interface, in which case ratio tracking through the current medium up to the interface multiplies the contribution per lane by the transmittance estimate and updates `r_u` and `r_l`, and the walk continues in the next medium.

The walk needs closest hits, not the any-hit `occluded()` query. Kernel 08 therefore gains a second variant, `0802_trace_transmittance`, built from `traceScene()` through `crt_trace.glsl` and so compiled for every traversal ([integrator-layer.md](completed/integrator-layer.md)). `KernelVariant` gains `withMedia`, the id of the variant the schedule runs instead of the selected one when the scene has a medium. The walk stops after 16 interfaces.

### On an optical bench

A thin homogeneous fog (σs around 0.01 m⁻¹) makes a laser beam visible in a render the way smoke does in a lab, and is the only way the path tracer itself draws a beam in free space. The glass inside an optical body does not need this plan: [physoptics-optical-materials.md](physoptics-optical-materials.md) applies Beer–Lambert absorption locally at each exit surface. A body can instead name a medium here for scattering glass or a cell of turbid water.

### Later

- Sampling volumetric emission directly: a medium's emission is found only by paths that pass through it (PBR 4ed exercise 14.4). Adding emissive grids to the light list with a distribution over their majorant cells (Villemin and Hery 2013; Simon et al. 2017) fixes that.
- Equiangular sampling toward point lights in homogeneous media (Kulla and Fajardo 2012).
- Product sampling of distances (Wrenninge and Villemin 2020, PBR 4ed exercise 14.5) with the reservoirs of [pbrt-resampled-direct-lighting.md](pbrt-resampled-direct-lighting.md).
- Guiding in volumes ([pbrt-path-guiding.md](pbrt-path-guiding.md)).
- Residual ratio tracking for transmittance (Novák et al. 2014, PBR 4ed exercise 14.2).

## Registry, bindings and data

- `0501_null_scattering` and `0701_phase_function` replace the `none` stubs. Kernel 05's domain becomes queue 7, and kernel 07's queue 8.
- `0802_trace_transmittance`, built for every traversal; `KernelVariant::withMedia`.
- Bindings 22 `Media`, 33 `MediumGrids` and 49 `MediumPathState` (new; the roadmap's binding table gains it).
- `PathState.medium`; `CRT_PATH_PREV_IN_MEDIUM` in `PathState.flags`; `ShadowRay.mediumAndGroup`; the medium interface in `GpuInstance`'s padding.
- `crt_classify.glsl`, `crt_medium.glsl`; `rt_media` and `bin/media_test` (roadmap).
- Scene file: `"media"`, per-object `"mediumInterface"`, and the camera's `"medium"`.
- Panels: a Media folder in the Scene panel, inside and outside combos per object, and a medium combo per camera.

## Verification

- With no medium in the scene, every scene renders bit-identical. The classification moves into `crt_classify.glsl` unchanged, and queues 7 and 8 stay empty.
- `bin/media_test`: Henyey–Greenstein samples match their pdf (χ²); ratio tracking's mean transmittance equals `exp(−σt d)` for a homogeneous segment and a quadrature for a grid; the majorant grid bounds the density in every cell.
- Beer–Lambert: an emissive quad seen through an absorbing slab of thickness `d` is dimmed by `exp(−σa d)`, within noise.
- White furnace ([pbrt-verification-harness.md](pbrt-verification-harness.md)): a sphere of non-absorbing medium (albedo 1) under a uniform white environment is invisible.
- A density grid filled with one constant value renders the same image as the equivalent homogeneous medium.
- A chromatic medium in exact RGB equals three monochromatic renders, one per channel.
- *The Next Week*'s Cornell box with smoke renders with and without next-event estimation in kernel 07, and both converge to the same image.

## Order of work

1. `crt_classify.glsl`, queues 7 and 8, `PathState.medium` and the flag. Bit-identical.
2. `SceneMedium`, medium interfaces, the `interface` material, the camera's medium, the scene file and the panels.
3. Homogeneous media: kernel 05 with absorption, then kernel 07 without next-event estimation.
4. Next-event estimation in kernel 07, and the transmittance variant of kernel 08.
5. `MediumPathState` and chromatic media.
6. Density and RGB grids with majorant grids, and emission.
7. NanoVDB.

## Sources

PBR 4ed chapter 11 (§11.1 to §11.3, volume scattering processes, transmittance, null scattering and phase functions; §11.4, the medium interface, homogeneous media, the DDA majorant iterator, and uniform and RGB grid media), §14.1 and §14.2 (the equation of transfer with null scattering, sampling the majorant transmittance, `VolPathIntegrator`, chromatic media and spectral MIS, ratio tracking), §15.3 (the wavefront integrator's medium queues), chapter 14 "Further Reading" and exercises 14.2, 14.4 and 14.5; pbrt-v4 `src/pbrt/media.h` (including the cloud and NanoVDB media, which the book does not describe), `wavefront/media.cpp`, `wavefront/intersect.h`; Shirley, *Ray Tracing: The Next Week*, "Volumes"; Novák et al. 2014, *Residual ratio tracking for estimating attenuation in participating media*; Kulla and Fajardo 2012, *Importance sampling techniques for path tracing in participating media*; Villemin and Hery 2013, *Practical illumination from flames*; Simon et al. 2017, *Line integration for rendering heterogeneous emissive volumes*; Museth 2021, *NanoVDB: a GPU-friendly and portable VDB data structure for real-time rendering and simulation*.
