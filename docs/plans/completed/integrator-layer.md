# Integrator layer

The renderer has one light transport algorithm, the wavefront path tracer, and its kernel registry, its scene file, and `GpuPathTracer::record()` all assume it. Light tracing, two bidirectional path tracers, photon mapping, and Metropolis light transport each need their own schedule and buffers, but most of what they trace, sample and accumulate is code the path tracer already has. This plan puts integrators above a shared library of wavefront stages, so that the traversal, the sampler, the light sampling, and the film are written once and every integrator runs them.

*Implemented 2026-09-29; the as-built record is the last section. Part of the [roadmap](../pbrt-roadmap.md).*

## Decisions already made

- On 2026-09-26 the user chose an integrator layer over planning unidirectional methods only or forcing the other algorithms into the path tracer's slots.
- On 2026-09-29 the user asked for as much code as possible to apply to every integrator, with the BVH code written once and usable everywhere. That replaced this plan's first draft, which gave each integrator its own slot table, its own selection and its own copy of every kernel it shared.

## What the code assumes today

- `KernelSlot` (`src/rt_kernels.h`) is one enum, 0 to 9, whose values are filename prefixes and scene-file keys. `KernelSelection` is a fixed array of ten variant indices.
- A traversal strategy is a kernel 02 variant. Each strategy is two `.comp` files and two registry rows, the kernel 02 closest-hit search and its kernel 08 shadow-ray partner, tied together by `KernelVariant::followsSlot`. The BVH node layout follows the kernel 02 variant.
- `GpuPathTracer` (`src/rt_gpu.h`) owns everything: the path pool, the queues, the hit and shadow-ray buffers, the scene upload, the accumulation, the reference comparison and the readbacks. `record()` hard-codes the schedule `00 → [01 → 02 → {03, 04, 06} → 08] × depth → 09`.
- `GpuPathTracer::init()` builds a pipeline for every registered variant at startup, and `writeSet()` maps each binding to its buffer in one `switch`.
- The scene file's `"kernels"` object maps slot numbers to variant ids, with the traversal stored as kernel 02's id.
- `RaytraceRenderer` prices a frame for the render guard from the selected kernel 02 and 06 variants.

## What pbrt offers as a model

pbrt has no GPU precedent for this. PBR 4ed §1.3.3 defines the `Integrator` interface for the CPU rendering path only, and chapter 15 states that the GPU path offers a single algorithm, volumetric path tracing; `wavefront/integrator.cpp` warns about any other integrator name and ignores it. Two parts of pbrt's design carry over:

- `Integrator::Create()` receives the camera, the sampler and the aggregate as inputs. They are scene-wide choices that the integrator uses, not parts of it.
- `WavefrontAggregate` exposes `IntersectClosest()`, `IntersectShadow()`, `IntersectShadowTr()` and `IntersectOneRandom()` as services the integrator calls.

This plan follows both. The traversal is a scene-wide component that every tracing kernel is built against, and the stages are services that integrators compose.

## Design

### The stage library

`KernelSlot` becomes the library of every wavefront stage any integrator runs. Its numbering stays append-only: 00 to 09 keep PBR 4ed figure 15.2's numbers and their scene-file keys, and the roadmap reserves the numbers after them for the plans that add stages. One `KernelSelection` covers every stage, and the scene file saves it as today.

One rule keeps a stage shareable: **a variant of a stage must work in every integrator that runs the stage.** A kernel that makes sense inside one integrator only is a new stage, not a variant. Metropolis light transport's chain sampler would break the path tracer if it were a kernel 01 variant, so it is a stage of its own, which `pssmlt` runs where the path tracer runs 01.

Below the stage level, code is shared the way kernel 06 already shares it: a kernel's body is an include file (`crt_intersect.glsl`, `crt_surface.glsl`, `crt_shadow.glsl`), and a `.comp` file is a few lines of defines and includes around it. A new stage that does most of what an existing one does includes the same body with a different define.

### Traversals

A traversal is a scene-wide component, not a kernel variant. `src/rt_kernels.cpp` gains a second table:

```cpp
struct TraversalVariant {
    const char* id;           // saved in the scene file, and the suffix of every .spv built for it
    const char* name;
    const char* description;
    // what the search reads, added to the bindings of every kernel that traces
    std::vector<CrtBinding> bindings;
    KernelSettings settings;  // AccelerationStructure for the BVHs
    TraversalCost cost;       // what the render guard prices one ray at
    bool requiresLayout;
    BvhLayout layout;         // the BLAS node layout the search reads
    bool isDefault;
};
```

A kernel that traces sets `KernelVariant::traces` and includes `include/crt_trace.glsl` instead of a strategy file. `crt_trace.glsl` includes the strategy named by one `CRT_TRAVERSAL_<ID>` define:

```glsl
#if defined(CRT_TRAVERSAL_BVH_BINARY)
#include "crt_bvh.glsl"
#elif defined(CRT_TRAVERSAL_BVH_CWBVH)
#define CRT_BVH_CWBVH 1
#include "crt_bvh.glsl"
#elif defined(CRT_TRAVERSAL_LINEAR)
#include "crt_linear.glsl"
#endif
```

CMake reads the traversal names from those `defined(...)` lines and compiles every `.comp` that includes `crt_trace.glsl` once per traversal, to `<name>.comp.<id>.spv`. The renderer builds a tracing kernel's pipeline from the `.spv` of the selected traversal, with the kernel's own bindings plus the traversal's.

- Kernels 02 and 08 become one variant each, `0201_intersect_closest` and `0801_trace_shadow_rays`. `0202`, `0203`, `0802`, `0803` and `KernelVariant::followsSlot` go.
- Adding a traversal is one strategy file under the contract in `crt_traverse.glsl`, one branch in `crt_trace.glsl`, and one `TraversalVariant` row. Every tracing kernel of every integrator is then built for it, with no new `.comp` file.
- Adding a kernel that traces, such as the subsurface probe or BDPT's connection kernel, is one `.comp` file, built for every traversal.
- The BVH node layout follows the selected traversal, and the **Acceleration structure** section follows the traversal's `settings`.

### Integrators

An integrator is a key, a name and the list of stages it runs, in dispatch order, plus a schedule that records them:

```cpp
enum class IntegratorId : uint32_t { Path, Count };

struct IntegratorInfo {
    IntegratorId id;
    const char* key;          // saved in the scene file; never rename
    const char* name;
    const char* description;
    std::vector<KernelSlot> stages;
};
```

`path` runs 00 to 09. Each later plan appends its `IntegratorId`, its row and its stages, numbered from the ranges the roadmap reserves. The stages they share, by the plans' designs:

| Integrator | Plan | Runs from the path tracer | Runs from another integrator | Stages of its own |
|---|---|---|---|---|
| `light` | [pbrt-bidirectional-path-tracing.md](../pbrt-bidirectional-path-tracing.md) | 01, 02, 09 | | 11 emit light rays, 12 connect to the camera, 13 scatter light paths, 14 trace camera connections |
| `lvc` | [pbrt-bidirectional-path-tracing.md](../pbrt-bidirectional-path-tracing.md) | 00, 01, 02, 08, 09 | `light`'s 11 to 14 | 15 handle emitters, 16 scatter and connect |
| `bdpt` | [pbrt-bidirectional-path-tracing.md](../pbrt-bidirectional-path-tracing.md) | 00, 01, 02, 09 | `light`'s 11 | 17 store vertices and scatter, 18 connect subpaths |
| `sppm` | [pbrt-photon-mapping.md](../pbrt-photon-mapping.md) | 00 to 04, 08 | `light`'s 11 and 13 | 19 store visible points, 20 deposit photons, 21 update photon statistics |
| `pssmlt` | [pbrt-metropolis-light-transport.md](../pbrt-metropolis-light-transport.md) | 00, 02 to 08 | | 22 mutate chain samples, 23 accept and splat |
| `mmlt` | [pbrt-metropolis-light-transport.md](../pbrt-metropolis-light-transport.md) | | `bdpt`'s stages | 22, 23 |

Where an integrator weighs a contribution differently from the path tracer (`lvc`'s emitters, `sppm`'s first diffuse vertex), its stage includes the path tracer's body with a define, the way kernel 06's four variants share `crt_surface.glsl`. Every stage that traces, shared or not, is built for every traversal.

### The renderer

`GpuPathTracer` splits by owner:

- `GpuRenderer`, renamed from `GpuPathTracer`, keeps what every integrator needs: the scene, geometry, light list, environment, and texture uploads; the film (accumulation and sample-count images); the reference comparison; the tonemap; timestamps; the traversal counters; the dropped-sample count; the resource table; the pipeline cache; and the active integrator's schedule.
- `WavefrontPool` holds the per-path buffers of any schedule that traces through the queues: paths, hits, queues, headers, radiance, shadow rays, and the sample budget. It also turns a variant's `KernelDomain` into a dispatch size. It is shared, so that `lvc`, `sppm` and `pssmlt` run the path tracer's stages on it.
- `IntegratorSchedule` is the interface a schedule implements: `start()`, `destroy()`, `record()`, `collectReadbacks()`, `samplesPerFrame()` (the unit the progress bar and **Max samples per pixel** count in), `estimatedWorkPerFrame()`, `traversalWork()` and `counters()`, the named per-bounce counts the panel prints.
- `PathSchedule` implements it with today's `record()` body.

Resources and pipelines:

- **The resource table.** Each owner publishes its resources into a table indexed by `CrtBinding` when it allocates or uploads them, and `writeSet()` reads the table. A binding that no owner published stops the program with its name, where a missing `switch` case used to leave the descriptor silently unwritten.
- **The pipeline cache.** Pipelines are keyed by variant and traversal, and built for the active integrator's stages when a render starts, with the default selection's built at startup. The lane-count families of [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) and the material classes of [pbrt-material-system.md](pbrt-material-system.md) extend the key.
- **Push constants.** Every kernel takes the one `Params` block, since a shared kernel is compiled once. An integrator that needs values of its own appends fields to it.

The render guard's policy (the budget, the refusal and **Render anyway**) stays in `RaytraceRenderer`. The estimate comes from the active schedule, and the cost of one ray from the traversal.

### Scene file

The payload version this lands in writes:

```json
"integrator": "path",
"traversal": "bvh_binary",
"kernels": {"00": "thin_lens", "01": "pcg_hash", "02": "closest", ...}
```

The reader takes an older file's traversal from its kernel 02 entry and ignores its kernel 08 entry, which was never read. An unknown integrator or traversal falls back to the default with a load warning, as an unknown variant id does today.

### Panels

- **Windows** → **Raytracer Shaders** shows an **Integrator** combo and a **Traversal** combo, then the active integrator's stages. A stage that traces names the traversal it is built for.
- **Raytrace Render** shows the **Integrator** combo at the top of its settings.
- The **Direct lighting** section appears only while the active integrator runs kernel 06.
- The per-bounce readouts (paths alive, shadow rays) come from the schedule's counters.

### Shader directories

Stages stay in `shaders/rt/`, because every integrator shares them. A schedule's fixed passes, which nobody swaps (a photon grid's count and prefix sum, an MLT bootstrap), are plain `ComputePass`es with their shaders in `shaders/rt/<integrator>/`, the way `image_error.comp` sits outside the registry.

## Order of work

1. The traversal component: `TraversalVariant`, `crt_trace.glsl`, the CMake permutations, and kernels 02 and 08 as one variant each.
2. `IntegratorId`, `IntegratorInfo` and the selection's integrator and traversal, with `path` registered; the scene file's new keys.
3. The split into `GpuRenderer`, `WavefrontPool` and `PathSchedule`, with the resource table and the pipeline cache.
4. The panels.

Steps 1 to 3 change no image. The first new integrator arrives with [pbrt-bidirectional-path-tracing.md](../pbrt-bidirectional-path-tracing.md).

## Verification

- With a fixed seed, the Cornell box under all three traversals and under BSDF sampling, the sphere scene, the Veach scene, and Sponza under both BVH layouts render bit-identical accumulation buffers before and after, compared as in codebase map §8.11.
- Every scene in `assets/scenes/` opens with the same selection as before, and saves in the new form.
- Zero validation errors with validation layers on.

## Open questions

- Whether the wavefront pool should be freed when the active integrator does not run on it. With `path` the only integrator, it never happens; the first integrator without a pool decides.

## As built

Implemented 2026-09-29, as designed above. What landed:

- `src/rt_kernels.h/.cpp`: `TraversalVariant` and its table (`bvh_binary`, the default; `bvh_cwbvh`; `linear`), `IntegratorId`, `IntegratorInfo` with `path` registered, `KernelVariant::traces`, and a `KernelSelection` that carries the integrator and the traversal. `KernelVariant::followsSlot`, `reconcileKernelSelection()`, and the per-variant `cost` and `layout` fields are gone; the traversal holds the last two.
- `shaders/rt/include/crt_trace.glsl` selects the strategy. `0201_intersect_closest.comp` and `0801_trace_shadow_rays.comp` replace the six per-traversal files, and the build writes `0201_intersect_closest.comp.bvh_binary.spv` and its siblings, six outputs from two sources.
- `src/rt_gpu_types.h` holds the C++ mirrors (`CrtParams` moved here from `rt_gpu.cpp`), the limits, `CrtResourceTable`, `DispatchSize` and `ScheduleCounter`. `src/rt_gpu.h/.cpp` is `GpuRenderer`. `src/rt_wavefront.h/.cpp` is `WavefrontPool`. `src/rt_schedule.h/.cpp` holds the interface and `createSchedule()`. `src/rt_path_schedule.h/.cpp` holds `PathSchedule`, whose `record()` is the previous body with the pool's dispatch sizes in place of the tracer's members.
- `RaytraceRenderer` asks the active schedule for the frame's work and the traversal for the cost of a ray. It shows the **Integrator** combo in **Raytrace Render** and in **Raytracer Shaders**, the **Traversal** combo in **Raytracer Shaders**, and the schedule's counters in place of the two hard-coded readouts.
- Scene payload version 11 writes `"integrator"` and `"traversal"`; `readKernelSelection()` in `scene_io.cpp` maps an older file's kernel 02 entry to the traversal.
- CMake re-runs its configure step when `crt_trace.glsl` changes. Adding the `crt_trace.glsl` include to an existing `.comp` needs a manual `cmake ..`, as a new `.comp` file already does.

Verification, with a temporary hook that opened a scene, rendered with seed 12345 and dumped the rgba32f accumulation:

- Bit-identical to the commit before, byte for byte: the Cornell box at 200 × 150 × 16 under `bvh_binary`, `bvh_cwbvh` and `linear`, and under BSDF sampling; the sphere scene and the Veach scene at 200 × 150 × 16; Sponza at 160 × 90 × 4 under both BVH layouts.
- Every file in `assets/scenes/` (versions 8 to 10) opened without a warning, with its traversal read from kernel 02.
- A Cornell box saved with `linear` and BSDF sampling wrote `"version":11`, `"integrator":"path"`, `"traversal":"linear"` and `"02":"closest"`, and rendered bit-identical after reopening.
- A file naming integrator `bdpt` and traversal `kdtree` opened with one warning for each, falling back to `path` and `bvh_binary`.
- Zero validation messages with `VK_LAYER_KHRONOS_validation` inserted (confirmed with `VK_LOADER_DEBUG=layer`). `bin/light_test` passes.

The **Raytracer Shaders** window was not drawn during the automated runs; the **Raytrace Render** panel, including its **Integrator** combo and the counters, was drawn every frame.

## Sources

PBR 4ed §1.3.3 (the `Integrator` interface and its CPU-only scope), chapter 15 (the wavefront path tracer, pbrt's only GPU integrator) and §15.3.6 (`WavefrontAggregate`); PBR 3ed chapter 16 (the bidirectional integrators whose stages the table above lists); pbrt-v4 `src/pbrt/cpu/integrators.h`, `wavefront/integrator.h`, `wavefront/integrator.cpp`.
