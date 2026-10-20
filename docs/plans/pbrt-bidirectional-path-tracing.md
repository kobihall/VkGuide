# Bidirectional path tracing

A path tracer builds every path from the camera. Light that reaches the camera only through a small or specular route, such as a caustic under a glass sphere or a room lit through a narrow gap, is found by chance and converges slowly. Bidirectional methods also trace paths from the lights and connect the two. PBR 3rd edition chapter 16, whose material the 4th edition moved online, gives the path-space measurement equation, camera importance, light tracing and bidirectional path tracing (BDPT) with multiple importance sampling. On 2026-09-26 the user chose two BDPT integrators: pbrt 3's, for parity with pbrt, and a light-vertex-cache BDPT built for GPUs. This plan adds light tracing, which both share, and then both.

*Not implemented. Depends on [pbrt-light-types.md](pbrt-light-types.md) (emitted-ray sampling), [pbrt-bsdf-library.md](pbrt-bsdf-library.md) (importance transport), [pbrt-cameras.md](pbrt-cameras.md) and [pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md) (the splat hook). Part of the [roadmap](pbrt-roadmap.md).*

## Decisions already made

On 2026-09-26 the user chose:

- Both BDPT designs, as two integrators: `lvc`, the light-vertex-cache BDPT of Davidovič et al. 2014, and `bdpt`, pbrt 3's.
- Light tracing first, shared by both.
- For Metropolis light transport, multiplexed MLT on top of `bdpt` ([pbrt-metropolis-light-transport.md](pbrt-metropolis-light-transport.md)).

## What the renderer does today

- One integrator, the path tracer, with next-event estimation and MIS.
- Nothing samples a ray leaving a light, nothing evaluates a camera's response to a ray arriving at it, and every contribution lands in the pixel its path started from.
- Every BxDF assumes radiance transport; the legacy `dielectric` scales nothing on refraction.
- Three of the six original materials are not reciprocal ([pbrt-verification-harness.md](completed/pbrt-verification-harness.md) measured it): phong, the fuzzy metal and the Lambertian lobe of `pbr` give a relative asymmetry of 0.5 to 0.95 between `f(wo, wi)` and `f(wi, wo)`. A unidirectional path tracer never evaluates the reverse direction, but a light path and a camera path joined at a vertex do, so a bidirectional estimate through those materials is wrong by that factor. Decide before the first connection test whether bidirectional integrators refuse them, treat them as non-symmetric with an explicit adjoint, or wait for [pbrt-bsdf-library.md](pbrt-bsdf-library.md)'s types. Lambertian and the GGX lobe of `pbr` are reciprocal to 1e-6.

## Design

### What every bidirectional method needs

- For camera importance, pbrt 3's `PerspectiveCamera` provides `We()` (the importance a ray carries), `Pdf_We()` and `Sample_Wi()` (a point on the lens and the raster position a scene point projects to), PBR 3ed §16.1.1. `crt_camera.glsl` gains them for the thin-lens perspective camera. pbrt supports only that camera for these methods, and so does this plan: the **Integrator** combo disables the bidirectional integrators for the other camera kinds.
- Emitted rays come from `sampleLe()` and `pdfLe()` per light kind ([pbrt-light-types.md](pbrt-light-types.md)).
- Light subpaths scatter with the BxDFs' importance mode ([pbrt-bsdf-library.md](pbrt-bsdf-library.md)): no 1/η² on refraction, and pbrt's correction factor for shading normals (PBR 3ed §16.1.3). Legacy materials have symmetric BSDFs except the shading-normal factor, which applies to them too.
- A connection to the camera lands at an arbitrary raster position, so its contribution is a splat. Splats go into binding 42, `SplatAccumulation`, a float buffer of three channels per pixel, with float atomics (`shaderBufferFloat32AtomicAdd`; image atomics are unsupported on this GPU). With a pixel filter, each splat is spread over the pixels within the filter's radius, weighted by the filter, as pbrt-v4's `RGBFilm::AddSplat()` does. The film's resolve adds `splatSum / lightPathsPerPixel` to the pixel ([pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md)).

### Light tracing, `light`

pbrt-v4's `LightPathIntegrator`: trace paths from the lights only, and at every vertex connect to the camera. It runs the path tracer's 01, 02 and 09 unchanged ([integrator-layer.md](completed/integrator-layer.md)) and adds four stages to the library, in the roadmap's range 11 to 18:

| Stage | What it does |
|---|---|
| 01 | the sampler, the path tracer's |
| 11 | emit light rays: pick a light by the light selection, sample an emitted ray, and connect the light's own point to the camera |
| 02 | intersect closest, the path tracer's, with the selected traversal |
| 12 | connect to the camera: at each surface vertex, a connection ray toward a lens point, carrying the splat |
| 13 | scatter light paths: sample the adjoint BSDF and continue |
| 14 | trace camera connections and splat the unoccluded ones; it traces, so the build compiles it for every traversal |
| 09 | the film, the path tracer's, with the splat hook of [pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md) |

Kernel 02 still sorts light-path hits onto the emissive queue, which `light` does not drain; a light path ignores the emitters it hits.

On an optical bench this integrator is the renderer: a laser's light reaches a card, a screen or a camera only through specular chains that camera paths cannot connect across, so the [laser optics plan](physoptics-laser-optics.md) needs the `light` integrator first (its roadmap's item 23, steps 1 and 2 of this plan). Its light paths also feed the detectors of [physoptics-detectors.md](physoptics-detectors.md), through stage 26 after kernel 02, and follow beam axes through [physoptics-guided-tracing.md](physoptics-guided-tracing.md)'s modes. Camera connections use the thin-lens camera only, as pbrt's do; a realistic camera on the bench is reached by its own camera paths, not by connections.

Escaped light paths are dropped. Light tracing alone renders caustics seen directly well and everything else poorly, which makes it a reference for the caustic paths the path tracer struggles with, and the base of both BDPT integrators.

### Light-vertex-cache BDPT, `lvc`

Davidovič et al. 2014, after van Antwerpen 2011 and the vertex-connection half of VCM (Georgiev et al. 2012):

1. The light pass traces `N_L` light subpaths with the `light` kernels. Every non-specular vertex is stored in the light vertex cache: position, the thin hit record, the incoming direction, the throughput, the path length and two MIS partial sums. Connections to the camera splat as in light tracing.
2. In the camera pass, camera paths run through the path tracer's 00, 01, 02, 08 and 09. Two stages of `lvc`'s own replace 03, 04 and 06, because every contribution is weighed against more strategies than the path tracer's two:
    - Stage 15, handle emitters, drains the escaped and emissive queues with the bodies of kernels 03 and 04, built with the recursive weights below.
    - Stage 16, scatter and connect, drains the surface queue with `crt_surface.glsl` built with the same weights: the light sample goes to kernel 08 as today, and K light vertices picked uniformly from the cache (the **Connections** setting, 1 to 4) are connected inline with `occluded()`, each weighed by `cache size / (K N_L)`. It traces, so the build compiles it for every traversal.
3. The MIS weights use Georgiev's recursive form (*Implementing vertex connection and merging*, as in SmallVCM): each subpath carries partial sums (`dVCM`, `dVC`) updated per bounce, so a connection's weight comes from the two vertices' partial sums, and no path is stored.

`N_L` is sized so the cache fits a memory budget (256 MB by default): with 64-byte vertices and depth 8, about 500,000 light paths per frame. The cache goes into binding 52, `LightVertexCache` (new; the roadmap's binding table gains it). The camera paths' extra MIS state lives in the `lvc` integrator's own pool buffers, so the path tracer's `PathState` does not change.

Vertex merging, the other half of VCM, adds photon-mapping-style density estimation at camera vertices using the same cache in a hash grid. [pbrt-photon-mapping.md](pbrt-photon-mapping.md) builds that grid, and VCM is a later step here.

### pbrt 3's BDPT, `bdpt`

PBR 3ed §16.3, faithfully:

- Each path slot traces a camera subpath and a light subpath, storing pbrt's `Vertex` records (type, throughput, the interaction, `delta`, `pdfFwd`, `pdfRev`) for both, up to the maximum depth, in binding 53, `BdptVertices` (new). At pbrt's default depth of 5 that is about 1.3 KB per slot, so the pool holds 256K slots (344 MB).
- Both subpaths trace through the path tracer's 01 and 02 and the same queues. The camera subpath starts at kernel 00 and the light subpath at `light`'s stage 11. Stage 17, store vertices and scatter, records each hit as a `Vertex` and samples the BSDF, in radiance or importance mode by a flag in the path state.
- Stage 18, connect subpaths, then evaluates every strategy (s, t): the unshadowed contribution, pbrt's MIS weight (`MISWeight()`, which walks both subpaths with the reverse densities, PBR 3ed §16.3.4), and the visibility. Visibility is tested inline with `occluded()`, one call per strategy, because queueing up to 20 shadow rays per slot would cost more memory than the vertices. It traces, so the build compiles it for every traversal. The strategies with `t = 1` splat; the rest add to the slot's pixel.
- Infinite lights follow PBR 3ed §16.3.5.
- pbrt's debugging outputs `visualizestrategies` and `visualizeweights` become an option that writes each (s, t) strategy's weighted or unweighted contribution to its own auxiliary image, through the `ChannelSums` buffer of [pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md).

### Media

Both BDPT integrators start with surfaces only. pbrt 3's BDPT handles media, with medium vertices and transmittance in the connections; that is a later step, after [pbrt-participating-media.md](pbrt-participating-media.md).

## Registry, bindings and data

- Integrators `light`, `lvc` and `bdpt` ([integrator-layer.md](completed/integrator-layer.md)).
- Stages 11 to 18 in the stage library, their shaders in `shaders/rt/` beside the path tracer's, since `lvc`, `bdpt` and `sppm` share them. Plumbing that nobody swaps, such as `lvc`'s cache compaction, goes in `shaders/rt/lvc/` as plain compute passes.
- Bindings 42 `SplatAccumulation`, 52 `LightVertexCache` and 53 `BdptVertices`.
- `We()`, `Pdf_We()` and `Sample_Wi()` in `crt_camera.glsl`, mirrored in C++.
- Settings: **Connections** (`lvc`), **Light vertex cache budget**, **Visualize strategies** (`bdpt`).

## Verification

- The path tracer renders bit-identical; nothing here touches its kernels.
- A C++ test for the camera functions: `Sample_Wi()`'s raster position projects back to the same point, and `We()` integrates to 1 over the film for a pinhole.
- Every integrator converges to the path tracer's reference on the Cornell box and the Veach scene (**Compare to reference**, means within 0.1 %).
- A caustic scene (a glass sphere on a diffuse floor under a small light): equal-time relative MSE for `path`, `light`, `lvc` and `bdpt`.
- `bdpt` against pbrt-v4 itself on the Cornell box through [pbrt-scene-import.md](pbrt-scene-import.md): the images converge together, and **Visualize strategies** matches pbrt's per-strategy images.

## Order of work

1. Camera importance, emitted-ray sampling, importance transport, and the splat buffer with the film's resolve.
2. The `light` integrator.
3. `lvc`: the cache, the camera-pass connections, and the recursive MIS.
4. `bdpt`: vertex storage, the connection kernel, and pbrt's MIS.
5. The strategy visualization, then media, then vertex merging (VCM) after photon mapping.

## Sources

PBR 3ed §16.1 (the path-space measurement equation, sampling cameras and light rays, non-symmetric scattering), §16.3 (bidirectional path tracing: vertices, subpaths, connections, MIS, infinite lights); PBR 4ed chapter 13 "Further Reading" (BDPT, VCM); Veach and Guibas 1994, *Bidirectional estimators for light transport*; Lafortune and Willems 1993, *Bi-directional path tracing*; van Antwerpen 2011, *Improving SIMD efficiency for parallel Monte Carlo light transport on the GPU*; Davidovič et al. 2014, *Progressive light transport simulation on the GPU: survey and improvements*; Georgiev et al. 2012, *Light transport simulation with vertex connection and merging*, and Georgiev 2012, *Implementing vertex connection and merging* (technical report, and the SmallVCM code); pbrt-v4 `src/pbrt/cpu/integrators.cpp` (`LightPathIntegrator`, `BDPTIntegrator`), `film.cpp` (`AddSplat`).
