# Feature: Compute-Shader Raytracing Pipeline

## 0. How to use this doc

Standalone implementation spec — treat it as the only context you have. For exhaustive facts about the current codebase state, read `docs/codebase-map.md` first. This feature has two hard prerequisites, both must exist and work **before this doc's implementation begins**:

- `docs/plans/compute-pipeline-general.md` — this doc builds its three compute stages using that doc's `ComputePass`/`ComputePassBuilder`/`dispatchComputePass()`.
- `docs/plans/raytracing-in-a-weekend.md` — **this feature is substantially a GPU re-expression of that feature's already-implemented CPU logic.** It reuses that feature's sphere list/editor (`RaytraceSceneEditor`) and ports its material model (lambertian/metal/phong/dielectric) to GLSL. Do not start this doc's implementation until the CPU raytracer is built and working — this is not a from-scratch raytracer design, it's a port of working, already-verified logic onto compute shaders.

## 1. Feature goal

Raytrace the same sphere scene the CPU raytracer (`docs/plans/raytracing-in-a-weekend.md`) already renders, but on the GPU via a sequence of swappable compute stages (ray generation → hit detection → hit shading) dispatched from `VulkanEngine::draw()`, progressively accumulating one new sample per engine frame into a persistent image — matching temporal anti-aliasing rather than a single blocking/threaded render — shown via the same feature-1 display registry the CPU version uses.

## 2. Architecture decisions made and WHY

### 2.1 Three `ComputePass` stages with a fixed buffer contract between them, not a monolithic shader

Ray generation, hit detection, and hit shading are each a separate `ComputePass` (`docs/plans/compute-pipeline-general.md`), not one big shader doing everything. The contract between stages is a pair of storage buffers sized to the render resolution:

```cpp
struct GpuRay      { vec3 origin; vec3 direction; vec3 throughput; uint pixelIndex; uint alive; };
struct GpuHitRecord { vec3 position; vec3 normal; float t; uint materialIndex; uint didHit; };
```

**Why stages instead of one shader**: this is the explicit requirement driving the whole feature — "flexibility to swap these shaders... easily in code... swap between different shaders handling different acceleration structures." As long as a replacement hit-detection shader reads `GpuRay` and writes `GpuHitRecord` in this same layout, it can swap in (a future triangle-BVH shader, a voxel-grid shader, whatever) without touching ray-gen or hit-shade at all. The fixed intermediate buffer layout *is* the swap point — this is why it's designed deliberately, not left implicit.

### 2.2 Explicit CPU-orchestrated bounce loop over a fixed-size buffer, not shader-side recursion or wavefront compaction

Monte Carlo bounces (the CPU raytracer's recursive `ray_color()`, `docs/codebase-map.md` §6) don't translate to GPU shaders — there's no practical shader-side recursion for this. Two GPU-appropriate alternatives exist:

- **(Chosen) Fixed-size buffer, CPU-dispatched loop.** `GpuRay`/`GpuHitRecord` buffers are sized to the full image resolution (one entry per pixel) and stay that size for the whole sample. `draw()` dispatches `hit-detect` then `hit-shade` once per bounce depth (`rayDepth` iterations, matching the CPU raytracer's own depth setting), rebinding the *same* buffers each time — `hit-shade` updates each ray's origin/direction/throughput in place for the next bounce, or sets `alive = 0` on miss or absorption. Both stages check `alive` at the top and no-op for dead rays.
- **(Rejected) True wavefront path tracing with compaction** — shrinking the active-ray buffer as rays die and using indirect dispatch sized to the live count. This is the "correct" high-performance GPU path tracer architecture, but adds real complexity (stream compaction, indirect dispatch parameter buffers) for a benefit (avoiding wasted work on already-dead rays within a fixed-size dispatch) that doesn't matter here — this renderer is explicitly not real-time, and wasted compute on dead lanes within one non-real-time sample is an acceptable tradeoff against materially simpler code. Revisit only if profiling later shows this is an actual bottleneck.

### 2.3 One accumulated sample per engine frame, matching "temporal anti-aliasing for static images"

Each engine frame that isn't busy resetting: dispatch ray-gen once (new per-pixel jitter for this sample), then the bounce loop (§2.2), then a resolve step blending this sample's per-pixel result into a persistent `rgba32f` accumulation image via a running average, and increment a sample counter. This directly matches the stated goal — "a single frame of the full engine corresponds to a subset of the total rays being processed... a single ray intersection loop" — one full sample (all pixels, all bounces) per engine frame was chosen as that unit of work, rather than something finer-grained (e.g. spreading one sample's bounces across multiple engine frames), because it keeps each frame's dispatch sequence self-contained and easy to reason about — a frame either completes a full new sample or the accumulation isn't touched at all, never a half-finished sample.

### 2.4 Auto-reset accumulation on camera change, via a `Camera::m_hasChanged` dirty flag

Every frame, before dispatching: check a new `bool m_hasChanged` field on `VulkanEngine`'s `Camera` (`src/camera.h/.cpp`) — set to `true` by every code path that actually mutates camera state (`Camera::update()` when `velocity` is non-zero, `Camera::processMouseMotion()` when yaw/pitch change), and cleared by whichever code reads it. If set, this feature clears the accumulation image, resets the sample counter to 0, restarts from sample 1, and clears the flag. The same reset also triggers on scene changes (sphere added/edited in `RaytraceSceneEditor`, §2.6).

This was chosen over two alternatives: a manual-reset-only button (rejected — progressive accumulation is only physically meaningful for a static viewpoint, and a manual-only reset makes "forgot to click reset after moving the camera" the default failure mode, silently blending samples from different viewpoints into a smeared result), and comparing the camera's view matrix against a stored snapshot with an epsilon threshold each frame (considered, but a dirty flag set exactly where camera state actually mutates is both more efficient — no per-frame matrix diff — and more reliable — no threshold to mistune, either too tight, causing spurious resets from floating-point jitter, or too loose, missing small genuine moves). The dirty-flag approach mirrors Unity's own `Transform.hasChanged` pattern (a prior project's compute raytracer used exactly this to trigger its own accumulation reset) — `Camera` doesn't have an equivalent today, so this feature adds one.

**Known limitation, inherited from the same pattern in Unity's `Transform.hasChanged`**: this is a single boolean, so only one consumer can reliably clear-on-read it without other consumers missing the notification. That's fine today (this feature is the only reader), but if a second future system also needs to react to camera movement, `m_hasChanged` as a single clear-on-read flag won't safely serve both — that would need revisiting (e.g. a per-consumer "last-seen frame" counter instead of one shared boolean) at that point, not now.

### 2.5 GPU-side PCG-style integer hash PRNG, not `std::mt19937` and not a `sin()`-based hash

Ray-gen's per-pixel jitter and hit-shade's Monte Carlo scatter direction sampling both need random numbers. The CPU raytracer's single mutable `std::mt19937` engine (`docs/codebase-map.md` §6) is inherently serial and has no GPU equivalent — this feature needs a stateless, per-invocation PRNG seeded from `(pixel coordinates, sample index, bounce index)`, computed fresh in-shader from those inputs rather than any persistent generator state.

**Chosen**: a PCG-style integer/bitwise hash (pure integer arithmetic, no transcendental functions) — a common single-line technique (e.g. `state = state * 747796405u + 2891336453u; word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u; return (word >> 22u) ^ word;`), seeded by combining pixel coordinates, sample index, and bounce index into the initial state.

**Rejected**: the classic `frac(sin(dot(p, vec2(12.9898,78.233))) * 43758.5453)`-style hash (used by a prior Unity compute-shader raytracing project the user built, which reseeds a scalar `_Seed` per dispatch and increments it per call). That technique is simpler to port line-for-line from working prior art, but its quality depends on the GPU driver's `sin()` implementation — some GPU/driver combinations are known to show visible banding or repeating patterns from this specific hash due to reduced-precision transcendental functions, which is a real risk worth avoiding specifically because this project's target hardware is an AMD GPU (Radeon Pro 560X) via MoltenVK, where such driver-specific precision behavior is exactly the kind of thing likely to surface. A pure-integer hash has no such dependency.

This is new shader infrastructure either way (nothing like it exists in `shaders/` today), but a standard, well-understood technique, not a novel design.

### 2.6 Reuses feature 2's `RaytraceSceneEditor` directly — no duplicate scene UI

The GPU pipeline does not get its own sphere-editing UI. It reads the same `RaytraceSceneEditor` (`docs/plans/raytracing-in-a-weekend.md` §3) the CPU raytracer uses, uploading the current sphere list to a `GpuSphere{center, radius, materialIndex}` SSBO and the current material list to a `GpuMaterial{type, albedo, fuzzOrSmoothness, refractionIndex}` SSBO whenever the editor's data changes (a dirty flag, not a per-frame re-upload). Editing a sphere affects both raytracers identically, since there is only one scene. This was the obvious choice once feature 2 already owns working scene-editing UI — building a second, parallel editor would duplicate that UI for no benefit and risk the two raytracers silently diverging on what "the scene" is.

### 2.7 Output format and display integration

Accumulation is `rgba32f` (HDR, needed for a numerically stable running average over many samples) but display needs LDR. Before upload each frame the accumulated result is tonemapped (start with a simple clamp-to-`[0,1]`; a proper operator like Reinhard can replace it later without affecting anything else in this design — see §8) into an `rgba8` staging buffer, uploaded via the same `VulkanEngine::createImage()` staging path feature 1/2 already establish, and registered with the feature-1 `DisplayRegistry` under a name like `"Compute Raytraced Output"` (distinct from feature 2's `"Raytraced Output"`, so both can be open and visually compared at once — useful for validating this feature against the already-working CPU version, §4). Resolution is an explicit setting (not tied to window/panel size), consistent with feature 1's fixed-resolution/letterboxed convention and feature 2's own UI pattern.

### 2.8 Background/miss lighting: procedural gradient now, HDR environment maps deferred to a future feature

On a ray miss, `crt_shade.comp` writes a simple procedural vertical color gradient (based on ray direction) — the same formula as the CPU raytracer (`docs/plans/raytracing-in-a-weekend.md`, porting the original RTIAW book's sky gradient). This was weighed against sampling an HDR equirectangular environment texture on miss (the technique the same prior Unity project uses, sampling a loaded `.hdr` skybox asset) — visually richer, but rejected for *this* feature specifically because it requires a new asset-loading path VkGuide has no precedent for (the existing loader, `src/vk_loader.cpp`, only handles glTF meshes/textures, not standalone HDR environment maps), and because keeping the GPU and CPU raytracers' backgrounds consistent matters more than either alone looking as good as possible.

**HDR environment lighting is wanted eventually**, but per explicit direction it's being deliberately bundled into `docs/plans/scene-and-asset-management.md` alongside two other related needs that came up during this feature's planning: saving/loading the `RaytraceSceneEditor`'s sphere scene to/from disk (today it only lives in memory for the session), and broader glTF scene-loading capabilities generally. That doc makes an HDR image loadable and available as a `VulkanEngine`-owned resource (`m_environmentMap`) — it does not add the shader-side sampling logic. This doc's raytracing pipeline should be written so that swapping in HDR-sampling later (once `m_environmentMap` exists) only touches the miss-handling branch of `crt_shade.comp`, not the rest of the staged pipeline.

## 3. Exact files to create/modify

| File | Role |
|---|---|
| `shaders/crt_raygen.comp` (new) | Ray-gen stage: for each pixel, compute a jittered camera ray (using the PRNG from §2.5 and camera basis vectors passed via push constant), write into the `GpuRay` buffer with `throughput = vec3(1)`, `alive = 1`. |
| `shaders/crt_hit_sphere.comp` (new) | Hit-detect stage: for each live ray, test against the `GpuSphere` SSBO (linear scan — matching the CPU raytracer's own un-accelerated approach, §2 of `docs/plans/raytracing-in-a-weekend.md`; no BVH here either, consistent with that doc's scope), write the closest hit (or `didHit = 0`) into `GpuHitRecord`. This is the designated swap point (§2.1) for a future acceleration-structure/geometry-type variant. |
| `shaders/crt_shade.comp` (new) | Hit-shade stage: for each live ray with a hit, look up its material in `GpuMaterial`, apply the matching BSDF sample (`switch` on material type — lambertian/metal/phong/dielectric, ported from `docs/codebase-map.md` §6's CPU implementations) using the PRNG (§2.5), update the ray's origin/direction/throughput for the next bounce. For a miss, accumulate this ray's throughput-weighted background contribution into the accumulation image and set `alive = 0`. |
| `src/crt_pipeline.h` (new) | Declares `CrtPipeline`: the three `ComputePass` instances, the `GpuSphere`/`GpuMaterial` buffers and their upload/dirty-tracking, the `GpuRay`/`GpuHitRecord` buffers, the accumulation image, the sample counter, and `void dispatch(VkCommandBuffer cmd, VulkanEngine* engine, const RaytraceSceneEditor& scene)` — the `draw()` entry point. |
| `src/crt_pipeline.cpp` (new) | Implements the above: buffer creation (`VulkanEngine::createBuffer`), the three `ComputePassBuilder` calls, the per-frame dispatch sequence (§2.2/§2.3), checking+clearing `Camera::m_hasChanged` and the scene-editor dirty flag to trigger accumulation reset (§2.4/§2.6), and the tonemap-and-upload-and-register step (§2.7). |
| `src/camera.h` | Add `bool m_hasChanged = false;` to `Camera` — set by any code path that mutates camera state, cleared by whoever reads it (§2.4). |
| `src/camera.cpp` | Set `m_hasChanged = true` inside `Camera::update()` when `velocity` is non-zero this call, and inside `Camera::processMouseMotion()` when yaw/pitch actually change (§2.4). |
| `src/vk_engine.h` | Add `CrtPipeline m_crtPipeline;` member. |
| `src/vk_engine.cpp` | In `draw()` (`vk_engine.cpp:60-170`), add a call to `m_crtPipeline.dispatch(cmd, this, m_raytraceScene)` as its own step, separate from `drawBackground()`/`drawGeometry()` — per the explicit requirement that this be integrated into `draw()` but distinct from `drawGeometry()`. Exact placement (before/after the raster scene's own passes) doesn't functionally matter since this writes to its own independent accumulation image, not `m_drawImage` — see §5 for the one real ordering constraint (barriers before `drawImgui()`). |
| `src/CMakeLists.txt` | Add `crt_pipeline.h`/`crt_pipeline.cpp` to the explicit source list. New `.comp` files need no build-file change (picked up by the existing shader glob). |

## 4. Implementation order and dependencies

Depends on both prerequisites in §0 being complete. Build and verify incrementally — a wrong image is far easier to debug one stage at a time than after the full pipeline is wired together:

1. **`Camera::m_hasChanged`** (§2.4) — small, isolated addition to `src/camera.h/.cpp`, independent of everything else here. Verify by temporarily logging/breakpointing on it while moving the raster camera around, confirming it sets on genuine movement and doesn't fire spuriously when idle.
2. **GPU sphere/material upload** from `RaytraceSceneEditor` (§2.6). Verify by reading the buffers back (or via a validation-layer/debug tool) and confirming they match the CPU-side list.
3. **`crt_raygen.comp` + its `ComputePass`.** Verify by writing ray *direction* (mapped to a color) directly into the accumulation image instead of running hit-detect/shade yet — a classic, cheap raytracer sanity check that catches camera-basis-vector bugs immediately and visually.
4. **`crt_hit_sphere.comp` + its `ComputePass`.** Verify by writing hit/miss as binary black/white into the accumulation image (no shading yet), then hit normal as color — again, classic cheap-to-eyeball debug visualizations before shading complexity is involved.
5. **`crt_shade.comp`, single bounce only** (skip the loop from §2.2 for now — `rayDepth = 1`). Verify basic direct-lit shading looks plausible for each material type.
6. **Add the bounce loop** (§2.2, `rayDepth` iterations). Verify reflections (metal) and refractions (dielectric) appear, matching what the CPU raytracer already produces for the same scene.
7. **Accumulation + sample averaging + camera/scene-change reset** (§2.3/§2.4). Verify: the image visibly denoises/refines over consecutive still frames; moving the camera or editing a sphere visibly restarts it from a fresh (noisy) sample 1.
8. **Tonemap + `DisplayRegistry` wiring** (§2.7). Verify the output window appears via feature 1's mechanism and can be compared side-by-side with feature 2's CPU output window on the same scene — they should look broadly similar (same geometry, same materials), not identical (different sampling, different float precision, different PRNG).
9. **`draw()` wiring** (§3) — confirm the dispatch sequence runs every frame without interfering with the raster scene's own rendering.

## 5. Edge cases / traps identified during planning

- **std430 buffer layout alignment**: `vec3` fields in `GpuRay`/`GpuHitRecord`/`GpuSphere`/`GpuMaterial` have 16-byte alignment in std430 layout (same as `vec4`), not 12 — a classic, easy-to-get-wrong source of silent data corruption if the C++-side struct doesn't insert matching padding. Verify the C++ and GLSL struct layouts agree byte-for-byte (e.g. `static_assert(sizeof(GpuRay) == expected)` on the C++ side) before debugging anything downstream — a misaligned buffer produces plausible-looking-but-wrong garbage, not an obvious crash, and MoltenVK is not lenient about this (`CLAUDE.md`'s general MoltenVK-strictness theme applies here too, even though this specific rule is a standard std430 rule rather than a MoltenVK-specific one).
- **Dead-ray no-op guard**: `hit-detect`/`hit-shade` must check `alive == 0` and return immediately for dead rays — beyond wasting GPU work, forgetting this risks a shading pass re-processing (and corrupting) a ray whose contribution was already finalized and accumulated on a previous bounce.
- **Fixed ray-buffer size vs. resolution changes**: the `GpuRay`/`GpuHitRecord` buffers and the accumulation image are all sized to the current raytrace resolution (an explicit setting, §2.7) — changing that setting requires reallocating all of them together, and should itself trigger an accumulation reset (same as a camera/scene change).
- **`m_hasChanged` is single-consumer** (§2.4): this feature clears the flag once it reads it each frame; if a second future system also needs to know "did the camera move," it can't safely share a clear-on-read boolean with this one without missing notifications — the same limitation Unity's own `Transform.hasChanged` has. Not a problem today (one reader), but don't assume a second reader can just check the same flag later without revisiting this.
- **This does not need to numerically match the CPU raytracer.** Different float precision (GPU vs. CPU double in the sibling project's original code, though `docs/plans/raytracing-in-a-weekend.md`'s ported version may already be float-based — verify), different PRNG, and different iteration/summation order mean pixel-exact matching against feature 2's output is not a meaningful goal, only "recognizably the same scene."

## 6. Code patterns from the existing codebase to follow

- **Every stage is built via `ComputePassBuilder`/dispatched via `dispatchComputePass()`** from `docs/plans/compute-pipeline-general.md` — no bespoke pipeline/descriptor-set code should be written here that duplicates what that framework already provides.
- **Material logic**: `docs/codebase-map.md` §6 (`lambertian`/`metal`/`phong`/`dielectric` scatter functions) is the direct porting source for `crt_shade.comp`'s material `switch` — same formulas, re-expressed in GLSL.
- **Scene data source**: `docs/plans/raytracing-in-a-weekend.md`'s `RaytraceSceneEditor` (§2.6 above) — read its sphere/material lists directly, don't re-derive scene state independently.
- **GPU image upload/display**: `load_image()` (`vk_loader.cpp:403-481`) and `docs/plans/imgui-display.md`'s `DisplayRegistry` — same upload-then-register pattern feature 2 already uses for its own output.
- **Buffer creation**: `VulkanEngine::createBuffer()` (`vk_engine.h:226`) for all new SSBOs (`GpuRay`, `GpuHitRecord`, `GpuSphere`, `GpuMaterial`).
- **`Camera` changes** (§2.4): `docs/plans/imgui-display.md` §2.9 (built earlier, per `docs/plan-overview.md`) already touches `src/camera.h/.cpp`'s neighborhood for capture-mode gating, though that gating lives in `vk_engine.cpp`'s callbacks, not inside `Camera` itself — this feature is the first to actually add a new field/method to `Camera` (`m_hasChanged`). Follow the existing `velocity`/`processMouseMotion()` code shape rather than introducing a new input-handling pattern.

## 7. What NOT to do (alternatives rejected and why)

- **Do not** implement wavefront-style stream compaction / indirect dispatch (§2.2) — unnecessary complexity for a non-real-time renderer; revisit only if profiling shows dead-ray waste is an actual bottleneck.
- **Do not** implement any acceleration structure or non-sphere geometry (triangles, voxels, volumes) in this pass — `crt_hit_sphere.comp` is deliberately the *only* hit-detect variant built now; the swap point (§2.1) is designed in, but filling it with alternatives is separate future work, same scoping principle `docs/plans/raytracing-in-a-weekend.md` already applied to its own CPU version.
- **Do not** give this feature its own sphere-editing UI (§2.6) — reuse `RaytraceSceneEditor` directly; a second scene editor would duplicate UI and risk the two raytracers disagreeing about scene state.
- **Do not** try to reuse the CPU raytracer's `std::mt19937`-based RNG approach, or a `sin()`-based hash, or expect bit-exact output matching against the CPU version (§2.5, §5) — different execution model, different (acceptable) goal, and the `sin()`-hash specifically carries real portability risk on this project's AMD target hardware.
- **Do not** allow accumulation to continue across a camera or scene change without resetting (§2.4) — the manual-reset-only alternative was explicitly considered and rejected as the default failure mode being silently wrong output.
- **Do not** sample an HDR environment texture on ray miss in this pass (§2.8) — deliberately deferred to `docs/plans/scene-and-asset-management.md`, which also covers sphere-scene save/load and broader glTF capabilities; keep the miss branch to the simple procedural gradient for now.

## 8. Open questions / things to verify before starting

1. **`maxPushConstantsSize` on the target hardware** — flagged in the companion general-compute doc (§8 there); ray-gen's push constant (camera basis vectors + sample index + resolution) should be checked against it.
2. **Tonemap operator** (§2.7) — starting with a plain clamp-to-`[0,1]` is a placeholder, not a final decision; a proper operator (Reinhard or similar) can replace it later without touching anything else in this design, since it's an isolated step just before display upload.
3. **Whether `docs/plans/raytracing-in-a-weekend.md`'s ported material/vector math ends up float- or double-precision** — the original sibling project uses `glm::dvec3` throughout (`docs/codebase-map.md` §6); if the CPU port keeps double precision, this feature's GPU material formulas should be re-derived from single-precision-appropriate versions rather than assumed identical, since GPU shaders work in `float`/`vec3` by default.
4. **Default raytrace resolution / expected performance** on the Radeon Pro 560X via MoltenVK — can't be predicted during planning; a linear-scan sphere-only scene should be comfortably fast even unoptimized, but worth confirming empirically once running.
5. **`docs/plans/scene-and-asset-management.md`** (§2.8) now covers HDR environment map loading, sphere-scene save/load, and broader glTF capabilities — once it's implemented, revisit `crt_shade.comp`'s miss branch to sample `m_environmentMap` instead of the procedural gradient, if desired.
