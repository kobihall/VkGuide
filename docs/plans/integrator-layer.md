# Integrator layer

Today the kernel registry, the scene file and `GpuPathTracer::record()` all assume one light transport algorithm: the unidirectional wavefront path tracer with slots 00 to 09. Bidirectional path tracing, photon mapping and Metropolis light transport need their own kernels, queues and schedules. This plan puts an integrator layer above the slots so that each algorithm owns its kernels and the path tracer keeps working unchanged.

*Not implemented. No prerequisites. Every other plan registers its kernels through this layer, so do it first. Part of the [roadmap](roadmap.md).*

## Decision already made

On 2026-09-26 the user chose an integrator layer over planning unidirectional methods only or forcing the other algorithms into the path tracer's slots.

## What the code assumes today

- `KernelSlot` (`src/rt_kernels.h`) is one enum, 0 to 9, whose values are filename prefixes and scene-file keys.
- `KernelSelection` is a fixed array of ten variant indices.
- `GpuPathTracer` (`src/rt_gpu.h`) owns everything: the path pool, the queues, the hit and shadow-ray buffers, the scene upload, the accumulation, the reference comparison and the readbacks. `record()` hard-codes the schedule `00 → [01 → 02 → {03, 04, 06} → 08] × depth → 09`.
- The scene file's `"kernels"` object maps slot numbers to variant ids with no notion of an algorithm.
- `RaytraceRenderer` prices a frame for the render guard from the selected kernel 02 and 06 variants.

## Design

### Integrators and their slots

An integrator is a light transport algorithm with its own set of slots, its own schedule and its own pool buffers. The first release of this plan registers one, `path`, whose slots are today's `KernelSlot` values. Later plans add:

| Id | Name | Plan |
|---|---|---|
| `path` | Wavefront path tracer | exists |
| `light` | Light tracing | [bidirectional-path-tracing.md](bidirectional-path-tracing.md) |
| `lvc` | Bidirectional path tracing with a light vertex cache | [bidirectional-path-tracing.md](bidirectional-path-tracing.md) |
| `bdpt` | Bidirectional path tracing as in pbrt 3 | [bidirectional-path-tracing.md](bidirectional-path-tracing.md) |
| `sppm` | Stochastic progressive photon mapping | [photon-mapping.md](photon-mapping.md) |
| `pssmlt` | Primary-sample-space Metropolis light transport | [metropolis-light-transport.md](metropolis-light-transport.md) |
| `mmlt` | Multiplexed Metropolis light transport | [metropolis-light-transport.md](metropolis-light-transport.md) |

The registry becomes two tables in `src/rt_kernels.cpp`:

```cpp
enum class IntegratorId : uint32_t { Path, Light, Lvc, Bdpt, Sppm, Pssmlt, Mmlt, Count };

struct IntegratorInfo {
    IntegratorId id;
    const char* key;          // saved in the scene file; never rename
    const char* name;         // what the panel shows
    std::span<const SlotInfo> slots;
};

struct KernelVariant {
    IntegratorId integrator { IntegratorId::Path };
    uint32_t slot { 0 };      // the integrator's own numbering
    // ... every existing field unchanged
};
```

`KernelSlot` stays as the path integrator's slot enum, so every existing call site compiles. Other integrators declare their own enums (`BdptSlot` and so on) and cast to `uint32_t` at the registry boundary. `kernelVariants(integrator, slot)` replaces `kernelVariants(slot)`, with the old signature kept as a path-integrator shorthand until the call sites move.

`KernelSelection` becomes one selection per integrator:

```cpp
struct KernelSelection {
    std::array<std::array<uint32_t, MAX_SLOTS>, (size_t)IntegratorId::Count> variant {};
    IntegratorId active { IntegratorId::Path };
    bool operator==(const KernelSelection&) const = default;
};
```

`MAX_SLOTS` is 16, which leaves room for integrators with more stages than the path tracer. `reconcileKernelSelection()` runs per integrator, and `followsSlot` refers to a slot of the same integrator.

### Schedules

`GpuPathTracer` splits into a shared renderer and one schedule per integrator:

- `GpuRenderer` (renamed from `GpuPathTracer`) keeps what every algorithm needs: the scene, geometry, light list, environment and texture uploads, the accumulation and sample-count images, the tonemap dispatch, the reference comparison, timestamps and the dropped-sample counter. It builds every registered variant's pipeline, as today.
- `IntegratorSchedule` is an interface with `allocate(width, height, samplesPerFrame)`, `free()`, `record(cmd, params)`, `collectReadbacks(slot)` and `estimatedWorkPerFrame(scene)`.
- `PathSchedule` implements it with today's pool buffers (paths, hits, queues, headers, radiance, shadow rays, sample budget) and today's `record()` body, moved without edits.

`GpuRenderer::record()` asks the active integrator's schedule to record the frame, then runs the shared tail: the reference pass and the tonemap. Integrators write their result into the shared accumulation through the film interface ([film-and-reconstruction.md](film-and-reconstruction.md)), or into the splat buffer that [bidirectional-path-tracing.md](bidirectional-path-tracing.md) adds.

The render guard moves into the schedule. Each integrator knows what one frame of its work costs; the path tracer's estimate stays exactly `RaytraceRenderer::estimatedWorkPerFrame()` today.

### Shader directories

Each integrator's kernels live in their own directory. The path tracer's move:

```text
shaders/rt/path/0001_generate_camera_rays.comp
shaders/rt/path/0101_generate_samples.comp
...
shaders/rt/include/        shared by every integrator, unchanged
shaders/rt/bdpt/           added by the BDPT plan
```

Scene files name variants by id, not by path, so the move changes no saved file. The CMake glob already compiles subdirectories and passes `-I shaders/`, so includes keep working. `docs/raytracing-overview.md` §2 shows the listing and needs the new path.

### Scene file

The payload version this lands in writes:

```json
"integrator": "path",
"kernels": {"path": {"00": "thin_lens", "01": "pcg_hash", ...}}
```

The reader accepts the old flat form (`"kernels": {"00": ...}`) as the path integrator's selection. An unknown integrator key falls back to `path` with a load warning, the same way an unknown variant id does today.

### Panels

- **Raytrace Render** gains an **Integrator** combo above the resolution, listing every integrator with at least one implemented variant in each of its slots.
- **Windows** → **Raytracer Shaders** lists the slots of the selected integrator only.
- Settings sections stay variant-driven (`KernelSettings`), so they follow the integrator without new code.

## Order of work

1. Add `IntegratorId`, `IntegratorInfo` and the per-integrator `KernelSelection`, with only `path` registered. Every call site passes `IntegratorId::Path`.
2. Split `GpuPathTracer` into `GpuRenderer` and `PathSchedule`, moving `record()`'s body verbatim.
3. Move the path kernels to `shaders/rt/path/` and update the registry's shader paths.
4. Scene-file format and the panel combo.

Steps 1 to 3 change no image. The first new integrator arrives with [bidirectional-path-tracing.md](bidirectional-path-tracing.md).

## Verification

- With a fixed seed, the Cornell box, the sphere scene and Sponza render bit-identical accumulation buffers before and after steps 1 to 4, compared as in codebase map §8.11.
- Every scene in `assets/scenes/` opens with the same selection as before, and saves in the new form.
- Zero validation errors with validation layers on.

## Open questions

- Whether `GpuRenderSnapshot` keeps one pool sized for the active integrator or reallocates on every integrator switch. Reallocation after a device wait is the simpler first version, since switching integrators is rare and already restarts the render.

## Sources

PBR 4ed §1.3.3 (the `Integrator` interface) and chapter 15 (the wavefront integrator as one `Integrator` among several); pbrt-v4 `src/pbrt/cpu/integrators.h`.
