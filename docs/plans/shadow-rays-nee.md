# Kernel 08: shadow rays and next-event estimation

*Not implemented. `shaders/rt/0801_trace_shadow_rays.comp` is a stub registered with `implemented = false`.*

## 0. Scope

Today the tracer finds lights only by chance: kernel 06 samples the material's own lobe, and a path contributes light only if that lobe happens to land on emissive geometry (kernel 04). That is unbiased but converges slowly, and for a small bright light it is close to hopeless — the Cornell box's ceiling panel is a few percent of the hemisphere.

Next-event estimation adds the other half: at every scattering event, also sample a point on a light directly and trace a *shadow ray* to it. Multiple importance sampling then combines the two strategies with weights that keep the result unbiased while taking the lower-variance one wherever it wins (Veach 1997).

This is the feature the kernel-variant machinery was built around, so most of this document is about **what already exists to plug into** rather than about the rendering maths, which is standard.

Since 2026-09-25 there are two more kinds of light than this plan first assumed: glTF surfaces that glow and also scatter (the `pbr` material with emission), and `KHR_lights_punctual` point, spot and directional lights, which the loader imports but nothing renders. Section 2.8 lists what each needs.

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

built by walking `accel->instances` and keeping those whose material type is `CRT_MATERIAL_EMISSIVE`, or `CRT_MATERIAL_PBR` with non-zero emission. Section 2.8 covers the mesh triangles and punctual lights that this per-instance record cannot describe. Uploaded into the existing per-edit `m_sceneBuffer` alongside the instances, since lights change with every scene edit exactly as instances do.

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

### 2.8 Emissive meshes and punctual lights

The loader now brings in two kinds of light that the light list in section 2.2 does not cover.

**Emissive glTF triangles.** A glTF material with an `emissiveFactor` becomes a `pbr` material with `emission` set, and optionally an `emissiveLayer` in the texture array. Kernel 02 queues a hit on one for both kernel 04 (which adds `throughput x emission x texel`) and kernel 06 (which scatters). What light sampling needs:

- **Per-triangle light records.** A mesh instance is one `GpuInstance` covering thousands of triangles, and usually only a few surfaces glow. Enumerate the emissive surfaces' triangles at `buildSceneAccel()` time into a separate emissive-triangle list: instance slot, triangle index, world-space area and power.
- **Power that accounts for the texture.** Sampling triangles by `area x luminance(emission)` ignores the texture, which is often mostly black (a lamp's bulb on a dark fixture). Use the texture's mean luminance over the triangle's uv footprint, or fall back to the mean over the whole layer.
- **Two-sided emission.** Kernel 04 counts emission from both sides, as the `emissive` type does. Light sampling must then sample and weight both sides, or the two strategies disagree and MIS goes wrong.
- **Kernel 04 MIS applies to `pbr` hits too.** The variant `0402 handle_emissive_mis` must weight a `pbr` surface's emission exactly like an `emissive` one's. With `0602 light`, it must drop the emission of a non-primary `pbr` hit and still let 06 scatter the path.

**Punctual lights.** `LoadedGLTF::lights` holds each `KHR_lights_punctual` light with its type, linear colour, intensity, range, cone angles and the owning node's world transform. Rendering them needs:

- **A scene representation.** The records live on the model today. Decide whether a light follows the model's placement (the `SceneMeshObject` transforms apply to mesh nodes, not to light nodes) or becomes its own editable `SceneLight` saved in the scene file, with a new payload version.
- **Units.** glTF gives point and spot intensity in candela and directional intensity in lux. The tracer's radiance is unitless. Pick one scale factor, for example radiant intensity = candela / 683 with the environment map taken as W/(sr·m²), and document it, or a model's lights will be orders of magnitude off against the sky.
- **Delta distributions.** A point, spot or directional light has zero area, so a BSDF sample never hits it. It contributes only through kernel 06's light sample, with MIS weight 1. The traversal needs no change, and kernel 04 never sees one.
- **Spot falloff and range.** Use glTF's smooth cone falloff between `innerConeAngle` and `outerConeAngle`, and its recommended windowed inverse-square falloff when `range` is set.
- **Light selection.** Put punctual lights in the same sampling distribution as area lights, weighted by power (`4π x intensity` for a point light), so a scene of many lights stays one sampling step per scatter.

**Alpha cutouts on shadow rays.** `occluded()` must reject cut-out candidates exactly as `traceScene()` does, or foliage casts solid shadows. If it calls the shared `testTriangles()` path in `crt_traverse.glsl`, it gets `cutAway()` automatically. Kernel 08 then has to declare `MaterialTextures`, `TriangleAttributes`, `InstanceMaterials` and `Materials`, which the alpha test reads.

## 3. Order of work

1. `occluded()` on both strategy files, checked by extending `tests/bvh_bench.cpp` with an occlusion comparison against brute force.
2. The light list and `sampleShape()`, mirrored CPU-side.
3. `0602 light` + `0402 handle_emissive_mis` + `0801` — the first end-to-end result, and already a large variance win on the Cornell box.
4. `0603 mis` and the `bsdfPdf` in `PathState`.
5. The `requires` mechanism and the guard's `raysPerScatter`.
6. Emissive glTF triangles in the light list (section 2.8).
7. Punctual lights: the scene representation, units and falloff (section 2.8).

Each step is independently checkable: with a fixed seed and enough samples, every combination must converge to the **same image** as `0601 bsdf`. That equality is the test — the same one that showed the BVH and linear traversals agreeing bit for bit.

## 4. Sources

Veach & Guibas 1995 (multiple importance sampling); PBR 4ed §13.4 and §15.3.9–15.3.10 (`SampleLd()` and the wavefront's shadow-ray kernel); Shirley, *Ray Tracing: The Rest of Your Life* (light sampling and pdf mixtures, which is the level the rest of this tracer's materials are written at).
