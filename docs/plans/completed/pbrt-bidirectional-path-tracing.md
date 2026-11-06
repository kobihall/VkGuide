# Bidirectional path tracing

A path tracer builds every path from the camera. Light that reaches the camera only through a small or specular route, such as a caustic under a glass sphere or a room lit through a narrow gap, is found by chance and converges slowly. Bidirectional methods also trace paths from the lights and connect the two. PBR 3rd edition chapter 16, whose material the 4th edition moved online, gives the path-space measurement equation, camera importance, light tracing and bidirectional path tracing (BDPT) with multiple importance sampling. On 2026-09-26 the user chose two BDPT integrators: pbrt 3's, for parity with pbrt, and a light-vertex-cache BDPT built for GPUs. This plan adds light tracing, which both share, and then both.

*Implemented 2026-11-01, steps 1 to 4 and the strategy visualization of step 5. Media and vertex merging, the rest of step 5, wait for [pbrt-participating-media.md](../pbrt-participating-media.md) and [pbrt-photon-mapping.md](../pbrt-photon-mapping.md). "As built" records what differs from the design below, and "Not built" what is left. Depends on [pbrt-light-types.md](pbrt-light-types.md) (emitted-ray sampling), [pbrt-bsdf-library.md](pbrt-bsdf-library.md) (importance transport), [pbrt-cameras.md](pbrt-cameras.md) and [pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md) (the splat hook, in `crt_film_update.glsl`). Part of the [roadmap](../pbrt-roadmap.md).*

## Decisions already made

On 2026-09-26 the user chose:

- Both BDPT designs, as two integrators: `lvc`, the light-vertex-cache BDPT of Davidovič et al. 2014, and `bdpt`, pbrt 3's.
- Light tracing first, shared by both.
- For Metropolis light transport, multiplexed MLT on top of `bdpt` ([pbrt-metropolis-light-transport.md](../pbrt-metropolis-light-transport.md)).

## What the renderer does today

- One integrator, the path tracer, with next-event estimation and MIS.
- Nothing samples a ray leaving a light, nothing evaluates a camera's response to a ray arriving at it, and every contribution lands in the pixel its path started from.
- Every BxDF assumes radiance transport; the legacy `dielectric` scales nothing on refraction.
- Three of the six original materials are not reciprocal ([pbrt-verification-harness.md](pbrt-verification-harness.md) measured it): phong, the fuzzy metal and the Lambertian lobe of `pbr` give a relative asymmetry of 0.5 to 0.95 between `f(wo, wi)` and `f(wi, wo)`. A unidirectional path tracer never evaluates the reverse direction, but a light path and a camera path joined at a vertex do, so a bidirectional estimate through those materials is wrong by that factor. Decide before the first connection test whether bidirectional integrators refuse them, treat them as non-symmetric with an explicit adjoint, or wait for [pbrt-bsdf-library.md](pbrt-bsdf-library.md)'s types. Lambertian and the GGX lobe of `pbr` are reciprocal to 1e-6.

## Design

### What every bidirectional method needs

- For camera importance, pbrt 3's `PerspectiveCamera` provides `We()` (the importance a ray carries), `Pdf_We()` and `Sample_Wi()` (a point on the lens and the raster position a scene point projects to), PBR 3ed §16.1.1. `crt_camera.glsl` gains them for the thin-lens perspective camera. pbrt supports only that camera for these methods, and so does this plan: the **Integrator** combo disables the bidirectional integrators for the other camera kinds.
- Emitted rays come from `sampleLe()` and `pdfLe()` per light kind ([pbrt-light-types.md](pbrt-light-types.md)).
- Light subpaths scatter with the BxDFs' importance mode ([pbrt-bsdf-library.md](pbrt-bsdf-library.md)): no 1/η² on refraction, and pbrt's correction factor for shading normals (PBR 3ed §16.1.3). Legacy materials have symmetric BSDFs except the shading-normal factor, which applies to them too.
- A connection to the camera lands at an arbitrary raster position, so its contribution is a splat. Splats go into binding 42, `SplatAccumulation`, a float buffer of three channels per pixel, with float atomics (`shaderBufferFloat32AtomicAdd`; image atomics are unsupported on this GPU). With a pixel filter, each splat is spread over the pixels within the filter's radius, weighted by the filter, as pbrt-v4's `RGBFilm::AddSplat()` does. The film's resolve adds `splatSum / lightPathsPerPixel` to the pixel ([pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md)).

### Light tracing, `light`

pbrt-v4's `LightPathIntegrator`: trace paths from the lights only, and at every vertex connect to the camera. It runs the path tracer's 01, 02 and 09 unchanged ([integrator-layer.md](integrator-layer.md)) and adds four stages to the library, in the roadmap's range 11 to 18:

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

On an optical bench this integrator is the renderer: a laser's light reaches a card, a screen or a camera only through specular chains that camera paths cannot connect across, so the [laser optics plan](../physoptics-laser-optics.md) needs the `light` integrator first (its roadmap's item 23, steps 1 and 2 of this plan). Its light paths also feed the detectors of [physoptics-detectors.md](physoptics-detectors.md), through stage 26 after kernel 02, and follow beam axes through [physoptics-guided-tracing.md](../physoptics-guided-tracing.md)'s modes. Camera connections use the thin-lens camera only, as pbrt's do; a realistic camera on the bench is reached by its own camera paths, not by connections.

Escaped light paths are dropped. Light tracing alone renders caustics seen directly well and everything else poorly, which makes it a reference for the caustic paths the path tracer struggles with, and the base of both BDPT integrators.

### Light-vertex-cache BDPT, `lvc`

Davidovič et al. 2014, after van Antwerpen 2011 and the vertex-connection half of VCM (Georgiev et al. 2012):

1. The light pass traces `N_L` light subpaths with the `light` kernels. Every non-specular vertex is stored in the light vertex cache: position, the thin hit record, the incoming direction, the throughput, the path length and two MIS partial sums. Connections to the camera splat as in light tracing.
2. In the camera pass, camera paths run through the path tracer's 00, 01, 02, 08 and 09. Two stages of `lvc`'s own replace 03, 04 and 06, because every contribution is weighed against more strategies than the path tracer's two:
    - Stage 15, handle emitters, drains the escaped and emissive queues with the bodies of kernels 03 and 04, built with the recursive weights below.
    - Stage 16, scatter and connect, drains the surface queue with `crt_surface.glsl` built with the same weights: the light sample goes to kernel 08 as today, and K light vertices picked uniformly from the cache (the **Connections** setting, 1 to 4) are connected inline with `occluded()`, each weighed by `cache size / (K N_L)`. It traces, so the build compiles it for every traversal.
3. The MIS weights use Georgiev's recursive form (*Implementing vertex connection and merging*, as in SmallVCM): each subpath carries partial sums (`dVCM`, `dVC`) updated per bounce, so a connection's weight comes from the two vertices' partial sums, and no path is stored.

`N_L` is sized so the cache fits a memory budget (256 MB by default): with 64-byte vertices and depth 8, about 500,000 light paths per frame. The cache goes into binding 52, `LightVertexCache` (new; the roadmap's binding table gains it). The camera paths' extra MIS state lives in the `lvc` integrator's own pool buffers, so the path tracer's `PathState` does not change.

Vertex merging, the other half of VCM, adds photon-mapping-style density estimation at camera vertices using the same cache in a hash grid. [pbrt-photon-mapping.md](../pbrt-photon-mapping.md) builds that grid, and VCM is a later step here.

### pbrt 3's BDPT, `bdpt`

PBR 3ed §16.3, faithfully:

- Each path slot traces a camera subpath and a light subpath, storing pbrt's `Vertex` records (type, throughput, the interaction, `delta`, `pdfFwd`, `pdfRev`) for both, up to the maximum depth, in binding 53, `BdptVertices` (new). At pbrt's default depth of 5 that is about 1.3 KB per slot, so the pool holds 256K slots (344 MB).
- Both subpaths trace through the path tracer's 01 and 02 and the same queues. The camera subpath starts at kernel 00 and the light subpath at `light`'s stage 11. Stage 17, store vertices and scatter, records each hit as a `Vertex` and samples the BSDF, in radiance or importance mode by a flag in the path state.
- Stage 18, connect subpaths, then evaluates every strategy (s, t): the unshadowed contribution, pbrt's MIS weight (`MISWeight()`, which walks both subpaths with the reverse densities, PBR 3ed §16.3.4), and the visibility. Visibility is tested inline with `occluded()`, one call per strategy, because queueing up to 20 shadow rays per slot would cost more memory than the vertices. It traces, so the build compiles it for every traversal. The strategies with `t = 1` splat; the rest add to the slot's pixel.
- Infinite lights follow PBR 3ed §16.3.5.
- pbrt's debugging outputs `visualizestrategies` and `visualizeweights` become an option that writes each (s, t) strategy's weighted or unweighted contribution to its own auxiliary image, through the `ChannelSums` buffer of [pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md).

### Media

Both BDPT integrators start with surfaces only. pbrt 3's BDPT handles media, with medium vertices and transmittance in the connections; that is a later step, after [pbrt-participating-media.md](../pbrt-participating-media.md).

## Registry, bindings and data

- Integrators `light`, `lvc` and `bdpt` ([integrator-layer.md](integrator-layer.md)).
- Stages 11 to 18 in the stage library, their shaders in `shaders/rt/` beside the path tracer's, since `lvc`, `bdpt` and `sppm` share them. Plumbing that nobody swaps, such as `lvc`'s cache compaction, goes in `shaders/rt/lvc/` as plain compute passes.
- Bindings 42 `SplatAccumulation`, 52 `LightVertexCache` and 53 `BdptVertices`.
- `We()`, `Pdf_We()` and `Sample_Wi()` in `crt_camera.glsl`, mirrored in C++.
- Settings: **Connections** (`lvc`), **Light vertex cache budget**, **Visualize strategies** (`bdpt`).

## Importing pbrt scenes

[completed/pbrt-scene-import.md](pbrt-scene-import.md) imports pbrt-v4 scenes today and substitutes for this plan's features. Each substitution is a line in the import report, so `./bin/pbrt_test --report <file.pbrt>` lists the scenes that need this plan. When this plan lands, change the importer as follows, then run `tools/verify/pbrt_compare.py` on the scenes named below.

- **`bdpt` and `lightpath` integrators** render with the path tracer, reported (`settings()` in `src/pbrt_import.cpp`; `pavilion-night` uses `bdpt`). Their parameters (`maxdepth`, `regularize`, `visualizestrategies`, `visualizeweights`) are read and ignored. Select this plan's integrators from them, and map `visualizestrategies` to **Visualize strategies**.

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

## As built

On 2026-11-01 the user answered four questions this plan left open: the materials that are not reciprocal get an explicit adjoint; every spectral representation and lane family works, with `lvc` sharing one hero wavelength sample per frame; [pbrt-cameras.md](pbrt-cameras.md) is built first; and the bidirectional integrators keep the auxiliary outputs and debug views from their camera paths, without light groups or spectral images. These differ from the design above, or add to it:

- **The stages.** 11 to 14 are light tracing's (`rt_light_schedule.cpp`), 15 and 16 the light vertex cache's camera pass (`rt_lvc_schedule.cpp`), and 17 and 18 pbrt 3's BDPT (`rt_bdpt_schedule.cpp`). `light` runs 01, 11, 02, 12, 13, 14 and 09; `lvc` runs those and then 00, 01, 02, 15, 16, 08 and 09 for its camera paths; `bdpt` runs 01, 11, 02 and 17 for the light subpaths, 00, 01, 02 and 17 for the camera subpaths, then 18 and 09. `IntegratorInfo::bidirectional` marks the three.
- **Camera importance** is `cameraWe()`, `cameraPdfWe()` and `cameraSampleWi()` in `crt_camera.glsl` and `camera_model.cpp`, with `We = p_lens / (A_film cos⁴θ)`. Only a perspective camera without a tilt connects (`cameraConnects()`), any aperture shape: the **Integrator** combo greys the bidirectional integrators for any other camera, and a render with one is refused with the reason.
- **Emitted rays** are `sampleLe()` and `lightPdfLe()` in `crt_light_emission.glsl` for every light kind, mirrored in `light_sampling.cpp`. A projection or goniometric light's image table joins binding 65, `ImageDistributions`, after the aperture's.
- **Importance transport** is chosen by macros the path tracer never defines (`CRT_SURFACE_TRANSPORT`, `CRT_OPTIC_RADIANCE_SCALE`), so its SPIR-V is unchanged. The six original materials get the adjoint by swapping the directions, `f*(wo, wi) = f(wi, wo)` with the cosine traded (`transportEval()` in `crt_transport.glsl`); pbrt's BxDFs take the mode themselves. pbrt's shading-normal correction applies in importance mode.
- **Splats** go into binding 42 by float atomics, over the filter table's signed values, or into one pixel for the box. Kernel 09 adds `splatSum × splatNormalization / samplesTaken` in its resolve, under `CRT_SPLATS` (specialization constant 10), so a bidirectional integrator runs a film that keeps a record: the running mean is replaced by the compensated mean, with a note.
- **Light paths draw from their own sample dimensions**, from 198 on (`SAMPLE_LIGHT_PATH_BASE`, `CrtParams::sampleDimensionBase`). The Halton tables now hold 400 primes; the first 200 are unchanged, so the path tracer renders as before.
- **`lvc`** follows SmallVCM's vertex-connection recursion (`dVCM`, `dVC`, the balance heuristic) over the cache of binding 52, with `r = N_L / N_C`. A light path's first `dVCM` is made at its first hit, because next-event estimation samples lights by solid angle. The frame traces `min(pool, capacity / (rayDepth - 2))` light paths, the capacity set by **Light vertex cache** (256 MB). Stage 15 runs twice a bounce, over the escaped queue and then the emissive one, and stage 16 evaluates every material as the complex class does, since a cache vertex can be of either. The cache is filled by an atomic append, so there is no compaction pass and no `shaders/rt/lvc/`.
- **`bdpt`'s vertices** are binding 53: per pixel sample the two counts, then up to rayDepth + 1 camera vertices and rayDepth light vertices, each 6 to 13 `uvec4`s (`crt_bdpt.glsl`). The frame takes the tile of pixel samples whose vertices fit **Vertex memory** (the same 256 MB setting), and the tiles sweep the image: a sweep is one sample per pixel, and only the frame that finishes one counts a sample. Samples per frame do not apply. The plan's 256K slots at depth 5 is about what 256 MB holds.
- **Stage 17** stores a vertex and samples the BSDF in the subpath's mode, and also runs over the escaped and emissive queues, where a camera subpath ends. pbrt's BDPT has no Russian roulette, and neither has this. Both subpaths of a pixel sample take its one hero wavelength sample (`bdptLambdaU()`).
- **Stage 18** takes an invocation per camera vertex of each pixel sample, which adds its strategies to the pixel by atomics; one per pixel sample was five times slower. `MISWeight()`'s walk along each subpath is a running sum made once a slot (`prepareMisSums()`), so a weight takes O(1) and not O(s + t). It binds no queue, path state or sample record, which keeps its spectral build under the 31 storage buffers a stage this GPU allows.
- **The outputs.** `bdpt` records the auxiliary outputs in stage 17's camera pass, at the surface the path tracer's camera path would, and they equal the path tracer's. Stage 18 draws the debug views from the camera subpath's first surface, casting the ambient occlusion ray itself; the MIS weights view, which weighs the path tracer's two strategies, stays black under `lvc` and `bdpt`. Light tracing keeps only the sample count.
- **Visualize strategies** (Render Settings → **Bidirectional**) keeps each strategy (s, t) in an image of its own, in the light groups' place in binding 45, at pbrt-v3's `BufferIndex()`. **Weighted** chooses pbrt's `visualizeweights` over `visualizestrategies`. An EXR export writes them as `bdpt_dDD_sSS_tTT.R`, pbrt's file names; a strategy with t = 1 is its splats over the pixel's samples. Depths past the outputs' 1 GB limit are left out, with a note.
- **The importer** maps pbrt's `lightpath` to `light` and `bdpt` to `bdpt`, with `visualizestrategies` and `visualizeweights`.
- **The scene file** is payload version 27: `"integrator"` may be `"light"`, `"lvc"` or `"bdpt"`, `"kernels"` names stages 11 to 18, and `"render"` gains `"bidirectional"` (the connections, the budget and the two visualize flags) when it differs from the defaults.

Verification:

- The path tracer renders bit-identical to the build before on 16 scenes, among them two with a lens, three samplers that keep a record, two optics scenes and a projection light, and on four more in hero wavelengths and two with every output and light groups.
- `bin/camera_test` checks `Sample_Wi()` against `Pdf_We()` and `We()`'s integral over the film; `bin/light_test` checks each light kind's emitted rays against `lightPdfLe()`. **Windows** → **GPU self-tests** → **Camera parity** covers the importance functions.
- `tools/verify/bidirectional_scenes.py` writes `bd_cornell.gltf`, `bd_veach.gltf`, `bd_caustic.gltf` and `bd_strategies.gltf` to `assets/scenes/`, and `tools/verify/bidirectional_check.py` checks them. On the Cornell box `light`, `lvc` and `bdpt` reach 1.0000, 0.9996 and 0.9992 of a 4096-sample path tracer's mean at 1024, 1024 and 256 samples, which is the plan's 0.1 %. On the Veach scene its tiny lights, seen in the glossy plates, are fireflies to the connections to the camera, and they reach 1.0025, 1.0058 and 1.0052.
- The weighted strategy images sum to the picture to 1e-6, and each depth's weighted strategies to 0.998 to 1.001 of its light-sampling strategy alone. `bdpt`'s normals, albedo, depth and ids equal the path tracer's exactly.
- In hero wavelengths at 4 lanes `bdpt` reaches 1.0005 of exact RGB and `lvc` 1.012 at 1024 samples. `lvc`'s frames share one wavelength sample, so its mean moves about 30 % from frame to frame and settles as one over the root of the frames. Every bidirectional kernel builds and renders at 32 lanes; Metal takes 114 s for stage 18 and 134 s for each class of stage 16, the first time.
- On the caustic floor at equal GPU time (4 s), `lvc`'s relative MSE is 19 times lower than the path tracer's (2.1e-4 against 4.1e-3), and light tracing's 2.8 times higher (it cannot show the glass sphere at all). `bdpt`'s is 2.2 times higher: each of its samples costs 15 times the path tracer's there, and its camera subpaths meet the same rare fireflies the path tracer does. Those fireflies are not this plan's. On a pbrt-v4 file of the same scene our path tracer's noise is 460 times pbrt's, with the same mean, in the build from before this plan too. A camera path through the glass sphere reaches the small lamp in pixels where pbrt's never do. Without its 16 worst pixels, `bdpt`'s noise there equals pbrt's own BDPT's (2.87e-3 against 2.90e-3), and its weighted strategy images match pbrt's `visualizeweights` images, so its weights are pbrt's.
- `pbrt_compare.py --cornell --integrator bdpt --max-depth 5` puts `bdpt` at pbrt-v4's own BDPT's noise floor: relative MSE 3.160e-4 against 3.158e-4, means within 0.16 %. Its strategy images match pbrt's within their noise, which the strategies that join two surfaces have heavy-tailed.
- The bidirectional renders run clean under the validation layers. That found an AOV view image cleared without transfer usage, which `light` and `bdpt` reached, a barrier in the `lvc` and `bdpt` schedules that named transfer access on compute stages only, and the light vertex cache copied for readback without transfer usage.

## Not built

- **Media**: pbrt 3's BDPT handles them, after [pbrt-participating-media.md](../pbrt-participating-media.md). Both bidirectional integrators are surfaces only.
- **Vertex merging** (VCM), after [pbrt-photon-mapping.md](../pbrt-photon-mapping.md)'s hash grid.
- Connections to an orthographic, spherical or tilted camera, which pbrt has none of either.
- Strategy images for `lvc`, and light groups and spectral images under any bidirectional integrator (the user's choice).
- A faster spectral `bdpt`: stage 18 rebuilds a light vertex's material for every strategy, O(depth²) a pixel sample, and a spectral material evaluates its spectra each time, so `bdpt` in hero wavelengths takes 6.6 times its RGB time against the path tracer's 1.45. Keeping each vertex's resolved material in binding 53 would remove most of it.
- pbrt's `regularize`, which [pbrt-path-regularization.md](../pbrt-path-regularization.md) adds for every integrator.
- **Found, not fixed:** camera paths through a glass sphere hit a small sphere light in pixels where pbrt's never do, which makes the path tracer and `bdpt` far noisier than pbrt on the caustic scene, with the same means. It predates this plan.

## Sources

PBR 3ed §16.1 (the path-space measurement equation, sampling cameras and light rays, non-symmetric scattering), §16.3 (bidirectional path tracing: vertices, subpaths, connections, MIS, infinite lights); PBR 4ed chapter 13 "Further Reading" (BDPT, VCM); Veach and Guibas 1994, *Bidirectional estimators for light transport*; Lafortune and Willems 1993, *Bi-directional path tracing*; van Antwerpen 2011, *Improving SIMD efficiency for parallel Monte Carlo light transport on the GPU*; Davidovič et al. 2014, *Progressive light transport simulation on the GPU: survey and improvements*; Georgiev et al. 2012, *Light transport simulation with vertex connection and merging*, and Georgiev 2012, *Implementing vertex connection and merging* (technical report, and the SmallVCM code); pbrt-v4 `src/pbrt/cpu/integrators.cpp` (`LightPathIntegrator`, `BDPTIntegrator`), `film.cpp` (`AddSplat`).
