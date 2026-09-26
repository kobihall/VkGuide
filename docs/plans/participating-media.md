# Kernels 05 and 07: participating media

*Not implemented. `shaders/rt/0501_sample_medium_interaction.comp` and `0701_sample_medium_scattering.comp` are stubs registered with `implemented = false`.*

## 0. Scope

Everything the tracer renders today is a surface: a ray travels through vacuum until it hits something. Participating media make the space between surfaces take part — fog, smoke, clouds, the inside of a candle or a glass of milk. A ray passing through one may be absorbed, or scattered into a new direction, before it ever reaches the surface kernel 02 found.

These are the two kernels of the PBR wavefront figure that this renderer has no equivalent of at all, and they are the reason the figure's numbering has gaps here. They are documented together because kernel 07 is meaningless without kernel 05.

## 1. Where they sit

Kernel 05 runs **between** 02 and the surface kernels, and it can overrule 02's answer:

```
02 Intersect Closest
      |  writes hits[], and pushes onto escaped / emissive / surface
      v
05 Sample Medium Interaction          <-- only for rays inside a medium
      |
      +-- scattered before the surface --> 07 Sample Medium Scattering --> next ray queue
      |                                          (and a shadow ray, with NEE)
      `-- reached the surface ----------> 03 / 04 / 06 as usual, with the
                                          throughput scaled by transmittance
```

`GpuPathTracer::record()` already dispatches slot 05 after 02 and before 06, with a barrier, and slot 07 after 06 — so the schedule needs no change. What it does need is for 05 to be able to **remove** a path from the surface queue after 02 put it there.

## 2. The hard part: 05 rewrites 02's classification

Every other kernel in this renderer only ever consumes a queue and appends to another. Kernel 05 is the first that has to *undo* an earlier kernel's decision, and it is the main design question this document exists to record.

Three options, in preference order:

**(a) 05 owns the classification, 02 stops doing it.** Kernel 02 writes `hits[]` and nothing else; a new `CRT_QUEUE_INTERSECTED` holds everything it traced. 05 then drains that queue and pushes to escaped / emissive / surface / medium-scatter. Without media, a trivial pass-through variant of 05 does the classification exactly as `crt_intersect.glsl` does now.

- Clean: one kernel owns the decision, and nothing has to be undone.
- Costs a dispatch and a queue round trip per bounce even when there is no medium, which is the common case.
- Needs a second variant of 02 (one that classifies, one that does not) or a `#define` — either way the traversal strategies stay untouched, since classification lives in `crt_intersect.glsl`, not in the strategy files.

**(b) 05 marks paths dead and the surface kernels skip them.** A flag in `PathState`; kernel 06 checks it and returns early. Cheapest to build, but it puts divergent dead lanes back into 06, which is precisely what the wavefront split exists to prevent (PBR 4ed §15.1.2). It also makes 06's queue count wrong for the readback stats.

**(c) 05 rebuilds the surface queue.** 05 drains `CRT_QUEUE_SURFACE` and re-appends the survivors to a second surface queue that 06 reads. Keeps 02 as it is, costs one more queue, and means 06's source queue depends on whether 05 ran — a conditional the scheduler would have to express.

**Recommendation: (a)**, taken when media work actually starts. It is the most invasive but it is the only one that leaves the dataflow honest, and the pass-through variant keeps the no-media path a single extra dispatch that does almost nothing. (b) is the tempting shortcut and should be resisted for the same reason the 03/04/06 split was made in the first place.

## 3. The rest of the work

### 3.1 Scene model

`rt_scene_types.h` has no medium concept. Add:

```cpp
struct SceneMedium {
    glm::vec3 sigmaA { 0.f };   // absorption
    glm::vec3 sigmaS { 0.f };   // scattering
    float     g { 0.f };        // Henyey-Greenstein asymmetry
    float     scale { 1.f };
};
```

Homogeneous only to begin with — a constant-density medium bounded by a shape instance. Heterogeneous (a voxel grid, delta tracking) is a much larger piece and should not be attempted first.

A `SceneShape` gains an optional medium index, which is what makes "this box is fog" expressible. `scene_io.cpp` gains a `"media"` array and a per-shape `"medium"` field, at payload version 11 (10 added the punctual lights); older files simply have none.

### 3.2 Bindings

Appended (never renumbered) to **both** `CrtBinding` in `src/rt_kernels.h` and `crt_common.glsl`:

- `CrtBinding::Media = 22`. Light sampling took 18 to 21 (`Lights`, `ShadowRays`, `TriangleLights`, `EnvironmentSampling`).
- `CrtBinding::MediumScatterQueue` — or reuse the generic queue set by adding `CRT_QUEUE_MEDIUM_SCATTER = 6` and raising `CRT_QUEUE_COUNT`, which is the cheaper option and consistent with how every other queue is allocated.

`PathState` needs a current medium index: which medium the ray is travelling inside. It is 64 bytes since light sampling added its MIS record, and two of its words (`pad0`, `pad1`) are free for this.

### 3.3 Kernel 05

Homogeneous majorant sampling: draw `t = -log(1 - ξ) / σ_t` along the ray. If `t < hit.t`, the path scatters in the volume — push to the medium-scatter queue. Otherwise it reaches the surface, and the throughput is scaled by the transmittance `exp(-σ_t · hit.t)`.

### 3.4 Kernel 07

Sample the Henyey-Greenstein phase function for a new direction, scale the throughput by the single-scattering albedo `σ_s / σ_t`, append the path to the next ray queue. Structurally it is kernel 06 with a phase function in place of a BSDF, and like 06 it should emit a shadow ray onto `CRT_QUEUE_SHADOW` and leave an MIS record in the path. `nextEvent()` in `shaders/rt/include/crt_surface.glsl` is the model to follow, with the phase function's value and density in place of `evalSurface()` and `pdfSurface()`.

### 3.5 Kernel 02 and transmittance through shadow rays

A shadow ray crossing a medium is attenuated rather than blocked. PBR handles this with a separate `IntersectShadowTr()` on its aggregate. Kernel 08 now has one variant per traversal, each following its kernel 02 partner (`docs/plans/completed/shadow-rays-nee.md` §9), so transmittance is a second body beside `crt_shadow.glsl` that accumulates transmittance instead of returning on the first hit, built once per traversal.

### 3.6 Cross-slot constraints

Selecting a 07 variant without a 05 variant renders nothing different, silently. The light sampling work did not build the `requires` mechanism its plan proposed. It built `KernelVariant::followsSlot` instead: a slot whose variant is derived from another slot's selection, never chosen on its own. Kernel 08 follows kernel 02 that way, and kernel 07 can follow kernel 05 the same way.

### 3.7 The render guard

A medium adds distance sampling per bounce but no traversal, so `sceneCostPerRay()` is roughly unchanged. Heterogeneous media with delta tracking would change that materially and should revisit it.

## 4. Order of work

1. Decide (a)/(b)/(c) above and restructure the 02 → 05 hand-off. Verify with the pass-through variant that images are **bit-identical** to today's before any medium exists — the same check that validated the BVH-versus-linear swap.
2. `SceneMedium`, the scene-file format, and the editor UI for attaching one to a shape.
3. Kernel 05 homogeneous, absorption only (no 07): a fog box should darken what is behind it.
4. Kernel 07 with Henyey-Greenstein.
5. Shadow-ray transmittance through kernel 08.

## 5. Sources

PBR 4ed chapter 11 (volume scattering), §14.2 (volumetric path tracing) and §15.3.7 (the wavefront's media kernels); Shirley, *Ray Tracing: The Next Week* §9 (constant-density media, which is the level step 3 should aim at).
