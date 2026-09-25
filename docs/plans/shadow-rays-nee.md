# Kernel 08: shadow rays and next-event estimation

*Not implemented. `shaders/rt/0801_trace_shadow_rays.comp` is a stub registered with `implemented = false`.*

## 0. Scope

Today the tracer finds lights only by chance: kernel 06 samples the material's own lobe, and a path contributes light only if that lobe happens to land on emissive geometry (kernel 04). That is unbiased but converges slowly, and for a small bright light it is close to hopeless — the Cornell box's ceiling panel is a few percent of the hemisphere.

Next-event estimation adds the other half: at every scattering event, also sample a point on a light directly and trace a *shadow ray* to it. Multiple importance sampling then combines the two strategies with weights that keep the result unbiased while taking the lower-variance one wherever it wins (Veach 1997).

This is the feature the kernel-variant machinery was built around, so most of this document is about **what already exists to plug into** rather than about the rendering maths, which is standard.

## 1. What the infrastructure already provides

Everything below is in place and needs no change:

| Piece | Where | State |
|---|---|---|
| `CRT_QUEUE_SHADOW` | `shaders/rt/include/crt_common.glsl` | Allocated and reset every bounce; nothing writes it yet |
| Kernel slot 08 | `src/rt_kernels.h` `KernelSlot::TraceShadowRays` | Registered, `implemented = false` |
| The stub shader | `shaders/rt/0801_trace_shadow_rays.comp` | Compiles, empty `main()` |
| Scheduling | `GpuPathTracer::record()` | Already calls slot 08 after 06/07, with a barrier, whenever a variant is implemented |
| Panel row | `RaytraceRenderer::drawKernelPanel()` | Already lists slot 08, greyed out |
| Scene persistence | `scene_io.cpp` v8 `"kernels"` | Already saves slot 08's id |

So turning it on is: write the shader, flip `implemented` to `true`, and register the 06 variants that feed it.

## 2. The work

### 2.1 An occlusion entry point on the traversal contract

`shaders/rt/include/crt_traverse.glsl` currently defines one contract function:

```glsl
bool traceScene(vec3 origin, vec3 direction, float tMin, float tMax, out TraceHit hit);
```

A shadow ray does not want the closest hit — it wants "is there *any* hit inside `[tMin, tMax]`", which can stop at the first one found and never fetches attributes. Add a second contract function that every strategy file supplies alongside the first:

```glsl
bool occluded(vec3 origin, vec3 direction, float tMin, float tMax);
```

- `crt_bvh.glsl`: the same TLAS/BLAS walk, returning `true` on the first triangle or shape hit instead of lowering `tMax`.
- `crt_linear.glsl`: the same scan, with the same early out.

Doing it this way is the point of having a contract at all: shadow rays keep working when the traversal strategy is swapped, with no knowledge of which one is selected. **Both strategy files must gain it together**, or selecting one of them silently loses shadows.

### 2.2 A light list

There is no light list today — emissive materials are found through geometry. Kernel 06 needs to *sample* a light, which needs them enumerated. Add to `src/rt_accel.cpp`'s `buildSceneAccel()`:

```cpp
struct GpuLight {
    // the instance this light is, so its transform and shape/BLAS are reachable
    uint32_t instance;
    uint32_t material;
    // for uniform-by-power sampling
    float    power;
    float    area;       // world space, after the instance's scale
};
```

built by walking `accel->instances` and keeping those whose material type is `CRT_MATERIAL_EMISSIVE`. Uploaded into the existing per-edit `m_sceneBuffer` alongside the instances, since lights change with every scene edit exactly as instances do.

New bindings, appended (never renumbered) to **both** `CrtBinding` in `src/rt_kernels.h` and the binding list in `crt_common.glsl`:

- `CrtBinding::Lights = 18`
- `CrtBinding::ShadowRays = 19` — a `ShadowRayItem` buffer, poolSize entries

Sampling a point on a light needs per-shape area sampling (a quad and a sphere cover almost every practical case) — a new `sampleShape()` in `crt_shape.glsl`, mirrored in `src/shape.cpp` as everything there is.

### 2.3 `ShadowRayItem`

The shadow queue holds ray-queue positions like the others, but a shadow ray carries state that outlives kernel 06 — where it is going and what it is worth. A parallel buffer, indexed by the same position:

```glsl
struct ShadowRayItem {
    vec3  origin;    float tMax;
    vec3  direction; uint  radianceSlot;
    // throughput * f(wo,wi) * cos / pdf, already MIS-weighted: kernel 08 only has to decide
    // whether to add it
    vec3  contribution; float pad;
};
```

Keeping the weight here rather than recomputing it in 08 is what lets 08 stay a pure occlusion kernel, shared unchanged by every 06 variant.

### 2.4 Kernel 06 variants

Two new registry entries in `src/rt_kernels.cpp`, both at `KernelSlot::SurfaceScattering`:

| id | Shader | What |
|---|---|---|
| `light` | `0602_surface_scatter_light.comp` | Sample a light, emit a shadow ray, and continue the path along a BSDF sample **without** taking emission on hit |
| `mis` | `0603_surface_scatter_mis.comp` | Both, each weighted by the balance heuristic |

Both list `CrtBinding::Lights` and `CrtBinding::ShadowRays` in addition to what `0601` declares. That is the entire resource story — `writeSet()` picks them up from the declaration, and the pipeline layout follows.

### 2.5 Kernel 04 must learn about MIS

This is the one change to an existing kernel, and the easy thing to get wrong. Under `light` or `mis`, a light reached by a BSDF bounce must **not** be counted at full weight, or every light is counted twice.

- With `0602 light`: kernel 04 should contribute **nothing** for a non-primary ray (the light was already accounted for by the previous bounce's shadow ray). Primary rays still see lights directly, or lights turn black on camera.
- With `0603 mis`: kernel 04 weights by `w = p_bsdf / (p_bsdf + p_light)`, which needs the solid-angle pdf of the BSDF sample that got there. Kernel 06 has it and kernel 04 does not, so it must be carried: add a `float bsdfPdf` to `PathState` (there is room — it is 48 bytes and would go to 64, or the `bounce` field's spare bits can hold it).

Handle this with a **variant of 04**, not a flag inside it:

| id | Shader | Pairs with |
|---|---|---|
| `direct` | `0401_handle_emissive.comp` | `0601 bsdf` (exists) |
| `mis_weighted` | `0402_handle_emissive_mis.comp` | `0602`, `0603` |

### 2.6 Making incompatible pairs unselectable

Selecting `0603 mis` with `0401 direct` renders a too-bright image with no error. The registry has no cross-slot constraint mechanism today; add one:

```cpp
// in KernelVariant
// slots this variant needs a particular variant of, by id. Empty means no constraint
std::vector<std::pair<KernelSlot, const char*>> requires;
```

`drawKernelPanel()` then, on a change, applies any `requires` of the newly selected variant and marks the forced rows so it is visible rather than surprising. This is the natural place for it — the same mechanism will be wanted by participating media (`docs/plans/participating-media.md`), where kernel 07 is meaningless without 05.

### 2.7 The render guard

A shadow ray is a second traversal per scattering event, so a frame's work roughly doubles. `TraversalCost` is currently a property of kernel 02 only; `RaytraceRenderer::sceneCostPerRay()` should multiply by the number of traversals per bounce that the selected 06 variant implies (1 for `bsdf`, 2 for `light`/`mis`). Add a `raysPerScatter` field to `KernelVariant` and read it there.

## 3. Order of work

1. `occluded()` on both strategy files, checked by extending `tests/bvh_bench.cpp` with an occlusion comparison against brute force.
2. The light list and `sampleShape()`, mirrored CPU-side.
3. `0602 light` + `0402 handle_emissive_mis` + `0801` — the first end-to-end result, and already a large variance win on the Cornell box.
4. `0603 mis` and the `bsdfPdf` in `PathState`.
5. The `requires` mechanism and the guard's `raysPerScatter`.

Each step is independently checkable: with a fixed seed and enough samples, every combination must converge to the **same image** as `0601 bsdf`. That equality is the test — the same one that showed the BVH and linear traversals agreeing bit for bit.

## 4. Sources

Veach & Guibas 1995 (multiple importance sampling); PBR 4ed §13.4 and §15.3.9–15.3.10 (`SampleLd()` and the wavefront's shadow-ray kernel); Shirley, *Ray Tracing: The Rest of Your Life* (light sampling and pdf mixtures, which is the level the rest of this tracer's materials are written at).
