# Feature: GPU Path Tracer (replacing the CPU raytracer)

## 0. How to use this doc

Standalone implementation spec — treat it as the only context you have. For exhaustive facts about the current codebase state, read `docs/codebase-map.md` first, then the §9 as-built sections of the three completed docs below, which describe the code this feature replaces and reuses.

**Rewritten 2026-09-14, twice.** The first draft was a fixed-size-buffer compute tracer that lived next to the CPU one. The second rewrite made it a wavefront tracer but kept the "live view that resets when the camera moves" model and treated the two raytracers as permanent peers. This version reflects the actual direction: **the GPU path tracer replaces the CPU raytracer** — same panel, same output window, same Render button, same settings — with a temporary backend switch while the CPU code is still around, and a plan for deleting it. Wavefront execution (§2.7) is one section of the design, not the design.

Hard prerequisites, all built and verified:

- `docs/plans/completed/raytracing-in-a-weekend.md` — the CPU raytracer this replaces. Its scene editor, resolution presets, `RenderSettings`, `TonemapPass`, `DisplayRegistry` output window and the ported RTIOW material/sampling math all carry over; its worker thread, snapshot cloning and virtual `hittable`/`material` hierarchy do not.
- `docs/plans/completed/compute-pipeline-general.md` — every GPU stage is a `ComputePass` (read its §9 for the API as built: `workgroupSize`, `dispatchComputePassOver()`, per-frame descriptor sets, `shaders/equirect.glsl`). This doc adds an indirect-dispatch entry point (§2.12).
- `docs/plans/completed/scene-and-asset-management.md` — the scene file, the environment map and the revision-counter convention.

Optional: `docs/plans/depth-of-field.md` (aperture/focus distance). §2.6 gives those two settings a home both backends read; if that to-do lands first, its fields move there.

Sources consulted, cited where they mattered: Laine, Karras, Aila, *Megakernels Considered Harmful* (HPG 2013); Jacco Bikker, *Wavefront Path Tracing* (2019); GPSnoopy, *RayTracingInVulkan*; Karim Sayed, *CUDA Ray Tracing In One Weekend* and its write-up; the 2026 arXiv comparison *Megakernel vs Wavefront GPU Path Tracing*.

## 1. Feature goal

Keep the raster viewport as the always-live preview, and make **Render** compute what that viewport sees on the GPU: a progressive path trace of the sphere scene, from the raster camera at the moment of the click, refining sample by sample in the "Raytraced Output" window until it reaches the requested sample count or is stopped. The output window stays a free-floating, dockable, resizable window that can be sized against the raster view for comparison, with a resolution mode that matches the viewport exactly. The CPU raytracer stays selectable behind a temporary backend switch until the GPU path is trusted, then is deleted. Everything the CPU tracer taught — the four materials, the thin-lens camera, the reproducible fixed seed, the shared tonemap — survives; the wavefront execution model, environment-map lighting, Russian roulette and an adaptive-sampling hook are new. Contracts are designed for what comes next: scene cameras, triangle meshes over `RaytraceMeshData`, lights, adaptive sampling.

## 2. Architecture decisions made and WHY

### 2.1 The GPU tracer takes over `RaytraceJob`'s place; a temporary backend switch, not two peers

Today `VulkanEngine` owns `RaytraceJob m_raytraceJob` (`src/rt_job.h`): the "Raytrace Render" panel, `RenderSettings`, the worker thread, the output image and its `"Raytraced Output"` registration. The user-facing shape of that — one panel, one Render button, one output window, one settings block — is right and stays. What changes is that the thing behind it becomes the GPU tracer.

Structure: a small backend-neutral **`RaytraceRenderer`** (`src/rt_renderer.h/.cpp`) owns the panel, the shared `RenderSettings` (§2.6), the output display image and its registry entry, and a `Backend` enum `{ Gpu, CpuLegacy }` shown as a combo at the top of the panel. It forwards Render/Stop/progress to whichever backend is selected: `GpuPathTracer` (`src/rt_gpu.h/.cpp`, new — this doc) or the existing `RaytraceJob`, demoted from engine member to a backend held by the renderer. The CPU job keeps its thread and snapshot machinery untouched inside; the renderer only calls `start()`/`cancel()`/`update()` on it and takes its finished linear image through the same tonemap into the same display image. When the switch goes (§2.14), `RaytraceJob` is deleted and the renderer keeps its shape.

**Why a switch and not a second panel/window**: the point of the switch is validation — render the same scene with both backends into the same window, at the same resolution, with the same seed, and compare. Two windows with two settings blocks invite the two to drift. **Why not fold the GPU tracer straight into `RaytraceJob`**: that class is built around a worker thread and a snapshot-by-value, neither of which the GPU path has; it is the thing being retired, so it should not be the thing being extended.

### 2.2 A plain-data scene model, moved first, so the CPU classes can be deleted later

`SceneSphere` today is `{ std::string name; std::shared_ptr<sphere> object; }` where `sphere` is the CPU tracer's virtual `hittable` with `hit()` and `params()`, holding a `std::shared_ptr<material>` whose subclasses carry `scatter()`, `clone()`, `params()` and the actual parameters. The editor, the gizmo adapter, `scene_io`, the preview spheres and the scene commands all reach through those classes. Retiring the CPU tracer without changing this would leave the class hierarchy alive with its methods dead.

So step one of this feature (before any shader) replaces it with plain data in `src/rt_scene_types.h` (renamed from the raytracer-specific `rt_types.h` contents that are really *scene* types):

```cpp
struct SphereMaterial {
    MaterialType type { MaterialType::Lambertian };
    glm::vec3 albedo { 0.7f };     // lambertian, metal, phong
    float fuzz { 0.3f };           // metal
    float smoothness { 0.5f };     // phong
    float ir { 1.5f };             // dielectric
};
struct SceneSphere {
    uint64_t id;                   // stable across edits and reorders; the gizmo targets it
    std::string name;
    glm::vec3 center;
    float radius;
    SphereMaterial material;
};
```

Every parameter of every type is stored, so switching a sphere's material type and back keeps its values (today only the albedo survives a type change). `MaterialType`, `MATERIAL_TYPES`, `materialTypeName()` and `materialPreviewColor()` move here unchanged. The per-type ImGui controls that were virtual `params()` methods become two free functions in the editor (`drawSphereParams`, `drawMaterialParams`) with a `switch` — the same UI, without the vtable. The gizmo adapter captures the sphere's `id` and looks it up through the editor each frame, returning `nullopt` when it is gone, which is exactly the lifetime contract the `weak_ptr<sphere>` gave it. `scene_io` reads and writes the same field names it does now, so **the file format does not change** (still version 2); it just constructs plain structs. Preview spheres read `center`/`radius`/`materialPreviewColor(material)` directly.

**Float, not double.** The CPU port kept `double` to match the sibling project's math; the GPU works in float and the editor's sliders are float widgets round-tripping through double today. The scene model goes float. While the CPU backend lives, `buildRaytraceScene()` constructs its `sphere`/`material` objects from the plain data (a 20-line conversion in `rt_scene.cpp`), so the CPU tracer's internal double math is untouched and its output unchanged. That conversion is the only place the CPU classes are still constructed, which is what makes §2.14 a deletion.

### 2.3 Render lifecycle: snapshot at the click, refine progressively, stop when done — not a live view

*(Reversed from the second draft, which reset accumulation on every camera move. The raster viewport is the live preview; the raytracer renders a chosen view.)*

**Render** captures, at that instant, the raster camera (§2.4), the sphere list (uploaded to the GPU once, §2.9), the environment map handle (§2.10) and the `RenderSettings`. From then on the GPU tracer adds `samplesPerFrame` samples per pixel to a persistent running mean every engine frame, updates the output window each frame through the tonemap, and stops by itself at `maxSamples` (or after exactly one un-jittered sample when anti-aliasing is off — the CPU's single-ray mode, and the way to get a fast preview). **Stop** ends it early and keeps the image. Moving the free camera or editing a sphere during a render does not touch it; the next Render picks up the new state. This is the CPU job's semantics exactly ("each render is a snapshot of settings at that moment"), which is also what a scene-graph camera implies later: the render is *of a camera*, not *of the viewport*.

Progress is `samplesAccumulated / maxSamples` in the existing progress bar; "Last render" reports the GPU time of the completed render (§2.12) and its sample count. "Render every frame" goes away — with a progressive renderer, a continuous render is simply a large `maxSamples`, and the checkbox's original purpose (re-render on every UI frame while editing, from the sibling project) is exactly the live-view model this doc rejects; the raster preview covers it.

One rule the snapshot model needs: **clearing or replacing the environment map during a render cancels the render**, because `destroyEnvironmentMap()` destroys the image the shade stage samples (after a device-wide wait, so the cancel is race-free — it just has to happen). Rare, and simpler than holding a second reference to a 256 MB image.

### 2.4 Camera: the raster camera at the click, with a resolution mode that matches the viewport

Render reads `m_mainCamera` directly at the click, through the same derivation `buildRaytraceScene()` uses today (position, forward, up from the rotation matrix, `CAMERA_VERTICAL_FOV_DEGREES`). **Factor that derivation out** into `RTCameraSnapshot captureCameraSnapshot(VulkanEngine*)` in `src/rt_scene.cpp` so both backends call the same function; no camera abstraction beyond that yet — when scene-graph cameras exist, that one function grows a parameter. The generate stage ports `RTCamera` from `src/rt_job.cpp` to float verbatim (origin, lower-left corner, horizontal/vertical, lens basis, lens radius), so for the same snapshot the two backends fire the same primary rays.

**Framing.** The CPU doc recorded that the raytraced image does not track the viewport's aspect ratio (its §9.4). Fix it here in the shared resolution combo: keep `RESOLUTION_PRESETS` and add a **"Match viewport"** entry that renders at exactly `m_drawExtent` (the viewport's current draw resolution, which already includes the render-scale slider). With that selected, the output is pixel-for-pixel what the raster pass rendered, framed identically, and the output window sized to the viewport is a direct A/B. A fixed preset still works as before (a different aspect crops or letterboxes the framing relative to the viewport — same vertical FOV, different horizontal extent), which is what "render at 1920×1080 regardless of window size" wants.

### 2.5 Output window and display

Unchanged in intent from the CPU raytracer, restated because it was the part the second rewrite let slip:

- The output is a fixed-resolution image in its own `DisplayRegistry` window — dockable, movable, resizable, letterboxed to fit (`draw_image_content()` scales to the content region preserving aspect). Sizing the window like the viewport gives a like-for-like comparison; "Match viewport" (§2.4) makes it exact. The window is **not** the viewport and never replaces it.
- Accumulation is `rgba32f` — a running mean needs the range and precision; `rgba8` would quantise every update. The display image is `rgba8`, produced every frame of a running render by the shared `TonemapPass` (scale 1, since the mean is already normalised — §2.7), inside `draw()`'s command buffer, then transitioned to `SHADER_READ_ONLY_OPTIMAL` before `drawImgui()` (the registry's obligation). No readback, no staging upload, no per-frame `createImage()` — the CPU backend's `publishOutput()` upload path is what it does *because* its pixels are on the CPU, and it keeps doing that behind the switch.
- One registry entry, `"Raytraced Output"`, owned by `RaytraceRenderer` and fed by whichever backend rendered last. Both backends go through the same tonemap, so a GPU render and a CPU render of the same snapshot differ only in sampling (§5), which is the comparison the switch exists for.
- The display image is recreated only when the resolution changes; the CPU job's deferred-destroy dance (`m_pendingDestroys`) is needed because it replaces the image per render — the renderer keeps the display image persistent and that mechanism goes with the CPU backend.

### 2.6 One `RenderSettings`, shared by both backends, saved with the scene

`RenderSettings` (`src/rt_scene_types.h`) is the renderer's, not a backend's: `width`, `height` (plus the "Match viewport" flag), `rayDepth`, `antialiasing`, `useFixedSeed`/`seed` as today, `samplesPerPixel` renamed to **`maxSamples`** (the CPU backend reads it as its per-pixel count, the GPU as its stopping point — the same number, "how many samples per pixel this render takes"), and new: `samplesPerFrame` (GPU, 1–8, default 1), `russianRoulette` + `minBouncesBeforeRoulette` (GPU), `aperture` + `focusDistance` (both backends; where `docs/plans/depth-of-field.md`'s two fields live — that doc's steps 1–3 land here instead of on `RaytraceJob`). The panel shows GPU-only rows greyed when the CPU backend is selected.

Save them in the scene file as a `"render"` object in the extras (payload version 3; absent in older files means defaults). It closes an open question three other docs raised and costs a dozen lines in `scene_io`; a scene is "models + environment + spheres + how to render it".

### 2.7 Wavefront execution: a path pool, index queues compacted per bounce, indirect dispatch

The GPU tracer is a wavefront path tracer (Laine et al.; Bikker), chosen by direction for what it enables — stages that swap behind fixed buffer contracts, per-material or per-light queues later, adaptive budgets — rather than for peak speed on a sphere scene (§7 is honest about that). The per-render-frame sequence, recorded in `draw()`:

```
generate     direct over W*H*K (pixel, k) pairs   -> PathState per spawned path; index appended to queue[0]
for bounce in 0 .. rayDepth-1:
    extend   INDIRECT over queue[bounce%2]        -> HitRecord per queue slot        (the geometry swap point)
    shade    INDIRECT over queue[bounce%2]        -> radiance slot on termination; survivors -> queue[(bounce+1)%2]
resolve      direct over W*H                      -> folds the K radiance slots per pixel into the running mean
tonemap      TonemapPass, mean -> display image
```

**Buffers** (`shaders/crt_common.glsl`, mirrored in C++ with `static_assert`s; std430 pads `vec3` to 16 bytes), all sized to `poolSize = W*H*K`:

```glsl
#define CRT_WORKGROUP 64                              // every queue-driven stage is 1-D at this size
struct PathState  { vec3 origin; uint radianceSlot; vec3 direction; uint rngState; vec3 throughput; uint bounce; }; // 48 B, by path index
struct HitRecord  { vec3 position; float t; vec3 normal; uint materialAndFace; };  // 32 B, by QUEUE POSITION; t < 0 = miss
struct GpuSphere  { vec3 center; float radius; };                                   // 16 B
struct GpuMaterial{ vec3 albedo; float param; uint type; uint pad[3]; };            // 32 B
struct QueueHeader{ uint groupCountX; uint groupCountY; uint groupCountZ; uint rayCount; }; // = VkDispatchIndirectCommand + count
```

Plus `queue[2][poolSize]` (uint path indices), `queueHeader[2]` (`STORAGE | INDIRECT_BUFFER | TRANSFER_DST`), `radiance[poolSize]` (vec4), `sampleBudget[W*H]` (§2.11); images `accumulation` (`rgba32f` running mean), `sampleCount` (`r32ui`), `display` (`rgba8`).

**Why index queues over a fixed pool**: path state never moves; compaction writes 4 bytes per survivor instead of 48, and `HitRecord` written by queue position keeps extend's writes and shade's reads coherent (Laine et al. do this; Bikker copies whole rays, 12× the traffic). **Why a geometry-agnostic hit record** (world position + flipped normal + face bit, not primitive id + barycentrics): 16 bytes more per hit buys a `shade` that never knows what produced the hit, so a sphere extend now and a triangle-BVH extend over `RaytraceMeshData` later write the same record.

**Compaction** uses workgroup-aggregated atomics — one global atomic per workgroup, not per ray, and no subgroup ballots (a Vulkan 1.1 feature with MoltenVK caveats):

```glsl
shared uint s_count; shared uint s_base;
if (gl_LocalInvocationID.x == 0) s_count = 0;
barrier();
uint local = survives ? atomicAdd(s_count, 1u) : 0u;
barrier();
if (gl_LocalInvocationID.x == 0) {
    s_base = atomicAdd(nextHeader.rayCount, s_count);
    uint before = (s_base + CRT_WORKGROUP - 1u) / CRT_WORKGROUP;
    uint after  = (s_base + s_count + CRT_WORKGROUP - 1u) / CRT_WORKGROUP;
    atomicAdd(nextHeader.groupCountX, after - before);      // header stays == ceil(rayCount / WG)
}
barrier();
if (survives) nextQueue[s_base + local] = pathIndex;
```

The header *is* the indirect command — `vkCmdDispatchIndirect(cmd, headers, q * 16)` reads it directly, a zero `groupCountX` is a legal no-op, and there is no prepare pass and no CPU readback to drive dispatch. Headers are reset by `vkCmdUpdateBuffer(..., {0,1,1,0})` from the command buffer before each producer runs, never by a shader (an empty queue runs no invocations, so a shader-side reset silently leaves a stale count). `barrier()` must sit in uniform control flow: dead lanes carry a flag, never `return` early. The CPU records all `rayDepth` bounces; an exhausted queue costs two empty dispatches and three barriers, which is nothing.

**`K = samplesPerFrame` samples in one pool** rather than `K` loops: the tail bounces are where a wavefront tracer starves the GPU, and a `K`-times-larger pool makes those dispatches `K` times bigger for the same barrier count. Memory is 104 bytes per path (~380 MB at 1280×720 with `K = 4`), so the tracer clamps `K` to keep `poolSize <= CRT_MAX_POOL` (4 M paths to start) and prints the allocation. Each path writes its final contribution to `radiance[radianceSlot]` exactly once (miss → throughput × background; roulette death or depth cap → the 0 that `generate` pre-wrote), so **no image atomics**; `resolve` sums a pixel's `K` slots, drops non-finite values as `renderPerPixel()` does, and updates `mean = (mean * n + sum) / (n + spawned)` with `sampleCount[p] += spawned`. A running mean (not a running sum with a `1/N` tonemap scale) is what the adaptive hook needs once pixels hold different counts.

**Why not Laine's persistent pool with mid-loop regeneration**: it decouples pixels from samples in flight, needs image atomics and asynchronous per-pixel bookkeeping, and breaks the "one frame = `K` complete samples" contract; the per-frame pool buys most of its occupancy far more simply. Revisit with measurements (§8).

### 2.8 RNG: PCG-hash seed, carried per path — and the fixed seed stays reproducible on the GPU

`PathState.rngState` is seeded by `generate` as `pcgHash(pixelIndex ^ pcgHash(sampleIndex ^ pcgHash(renderSeed)))`, where `sampleIndex = sampleCount[p] + k` and `renderSeed` is the settings' `seed` when `useFixedSeed` is on, otherwise a value drawn at Render. Every draw is one PCG-RXS-M-XS step (`state = state * 747796405u + 2891336453u; word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u; return (word >> 22u) ^ word;`), pure integer — the `sin()`-hash rejection from the first draft stands (driver-dependent precision on AMD/MoltenVK). Carried state rather than a stateless per-(pixel, sample, bounce) hash because `shade` draws a variable number of randoms per bounce and both GPU references (RayTracingInVulkan: TEA seed + LCG; the CUDA port: PCG seed + LCG, 2–4 registers) do it this way.

**Fixed seed works on the GPU.** A path's stream depends only on `(pixel, sampleIndex, renderSeed)`; queue order, atomics and workgroup scheduling never enter it, and the running mean is order-independent up to float summation of `K` values per pixel per frame (fixed order in `resolve`). So the same scene, settings and seed reproduce the same image, which the CPU backend's users already rely on for comparisons. `shaders/crt_random.glsl` ports `src/rt_random.cpp` function for function (`random_unit_vector`, `random_in_ball` as `random_unit_vector() * cbrt(u)`, `random_in_disk` analytic, `random_in_hemisphere(normal, alpha)` with `getTangentSpace` and its fixed helper axis).

### 2.9 Materials and shading: the four RTIOW materials, a `switch`, Russian roulette after three bounces

At Render, the sphere list becomes `GpuSphere[]`/`GpuMaterial[]` (one material per sphere, `materialIndex == sphere index`, `param` = fuzz | smoothness | ir by type) in a `CPU_TO_GPU` buffer flushed with `vmaFlushAllocation()` — with the plain-data model (§2.2) this is a `memcpy` per field, no `dynamic_cast`. Uploaded once per render (snapshot semantics), so a single buffer suffices; a render in flight is never re-uploaded.

`crt_shade.comp` ports `src/rt_material.cpp` with a `switch` on `type`: lambertian `normal + random_unit_vector()` with the `1e-8` near-zero fallback; metal `reflect + fuzz * random_in_ball()`, absorbed when it points into the surface; phong `random_in_hemisphere(reflected, pow(1000, smoothness²))`, same rule; dielectric with `front_face ? 1/ir : ir`, Schlick, reflect-or-refract. Four materials only, by direction — emissive materials and explicit lights are a later feature. No branchless lobe blending (the CUDA port's trick): divergence is what queues solve at a higher level, and blending triples the ALU per hit for four cheap lobes.

**Russian roulette** (new): after `minBouncesBeforeRoulette` (default 3), survive with `p = clamp(max(throughput), 0.05, 1)` and divide throughput by `p` — unbiased, in every reference, and what makes late bounces cheap under compaction. Off by toggle for CPU-comparison renders (the CPU backend has none).

Float constants (kept from the first draft): `t_min = 0.001` (the CUDA port's value in float), the numerically stable ray–sphere test (`half_b`, and `c` from the closest-approach vector, not `dot(oc,oc) - r²`, which cancels for the radius-100 ground sphere), NaN samples dropped in `resolve`.

### 2.10 Miss lighting: the environment map when loaded; environment intensity is a scene property, exposure a display one

*(Reversed from the first draft, which deferred environment maps for lack of a loader.)* On a miss, `shade` adds `throughput * (hasMap ? textureLod(env, equirectUv(dir), 0).rgb * environmentIntensity : gradient(dir))`, binding `m_environmentMap` through `m_defaultSamplerLinear` (fallback `m_greyImage`, as the `environment` background effect does) and using `shaders/equirect.glsl` so the render's background is the viewport background effect's, by construction. `textureLod(..., 0)`: compute shaders have no derivatives, and the map has no mips. The gradient is the CPU tracer's (`0.5*(y+1)` lerp of white and `(0.5,0.7,1)`), so with no map the two backends match.

Two knobs, in two places, replacing the second draft's "reuse the background effect's exposure slider": **`environmentIntensity`** is a scene property (saved with the scene, next to the map path; default 1) — it scales the *light*, and both the raster background effect and the path tracer multiply the map by it; **`exposure`** is a display setting on `RenderSettings` passed to `TonemapPass` as its existing `scale` — it scales the *image*. The background effect's current "Exposure" slider becomes the scene's `environmentIntensity`, so the viewport and the render agree on how bright the sky is, and the tonemap decides how bright the picture is.

### 2.11 The adaptive-sampling hook: a per-pixel budget and per-pixel sample counts, no policy

*(Approved as "hook only"; the policy is `docs/plans/adaptive-sampling.md`.)* `sampleBudget[W*H]` (uint) says how many of a frame's `K` slots a pixel spawns; `generate` runs over all `W*H*K` pairs, spawns only `k < min(budget, K)`, pre-writes 0 to every radiance slot, and goes through the queue allocator so a sparse budget makes the first extend smaller for free. Today the buffer is filled with `K` at Render and never written again. `sampleCount` (`r32ui`) is per-pixel truth about the mean, maintained by `resolve`; the tonemap needs nothing from it, a future policy and a debug heat-map read it. With a uniform budget, `samplesAccumulated += K` per frame is exactly what every pixel holds, and that drives the progress bar and the stop test.

### 2.12 Additions to the general compute framework and to `vkutil`

- `dispatchComputePassIndirect(cmd, pass, set, pushData, VkBuffer, VkDeviceSize offset)` in `src/vk_compute.h/.cpp`: binds pipeline, set and push constants like `dispatchComputePass()`, then `vkCmdDispatchIndirect`.
- `vkutil::memory_barrier(cmd, srcStage, srcAccess, dstStage, dstAccess)` in `src/vk_images.h/.cpp`: one `VkMemoryBarrier2` via `vkCmdPipelineBarrier2KHR` (KHR forms, `CLAUDE.md`). Three uses per bounce: after the header reset (`COPY/TRANSFER_WRITE` → `COMPUTE/SHADER_READ|WRITE` + `DRAW_INDIRECT/INDIRECT_COMMAND_READ`), after `extend` (`COMPUTE` write → read), after `shade` (`COMPUTE/SHADER_WRITE` → `DRAW_INDIRECT/INDIRECT_COMMAND_READ` + `COMPUTE`). `DRAW_INDIRECT` is the stage that reads dispatch arguments too, despite the name.
- Two `VK_QUERY_TYPE_TIMESTAMP` queries around the sequence, read back `FRAME_OVERLAP` frames later: "GPU ms/frame" in the panel and the total for "Last render". Without it the only signal is whole-frame time, which the raster scene pollutes.
- A `GPU_TO_CPU` copy of the queue headers at frame end, read with the same latency, for a "paths alive per bounce" readout — the proof the compaction works (§4 step 7).

### 2.13 Raster preview spheres: persistent per-slot material buffers, `updateScene()` moved back (unchanged from the first draft)

`drawRaytraceSpheres()` still allocates a uniform buffer and a descriptor set per sphere every frame, which is why `updateScene()` had to move after the fence wait (`raytracing-in-a-weekend.md` §9.5) and stopped overlapping CPU scene work with the GPU. Fix it here: `FRAME_OVERLAP` slots, each with a persistent buffer, its descriptor sets from a long-lived allocator, its own `std::vector<MaterialInstance>`, and the editor revision it was written at; rewrite a slot only when the revision differs; `vmaFlushAllocation()` after every write; `updateScene()` back to the top of `draw()`; free in `cleanup()`. With the plain-data model the rewrite reads `materialPreviewColor(sphere.material)` straight off the struct.

### 2.14 Retiring the CPU backend

When the GPU backend has been compared against the CPU one on the default scene and a few saved ones (§4 step 9) and found trustworthy, the switch goes in one commit:

- **Delete**: `src/rt_job.h/.cpp` (worker, snapshot, `RTCamera`, `publishOutput`), `src/rt_random.h/.cpp`, `src/rt_hittable.h/.cpp`, `src/rt_material.h/.cpp`'s `material` classes and `scatter()`, `RaytraceScene::hit()` and the sphere conversion in `rt_scene.cpp`, the `Backend` combo. `ray`, `hit_record`, `RT_INFINITY` and the double-precision rationale go with them.
- **Keep**: `rt_scene_types.h` (the plain scene model, `RenderSettings`, `RESOLUTION_PRESETS`), `rt_scene_editor`, `scene_io`, `RaytraceMeshData` and `forEachMeshNode()` (the future mesh path's input), `captureCameraSnapshot()`, `TonemapPass`, `RaytraceRenderer`.
- **Record**: a short "retired" section appended to `docs/plans/completed/raytracing-in-a-weekend.md` naming the commit, so the git history is the archive and the doc says where to look.

Nothing in this feature should be written in a way that makes that commit harder than deleting files and one enum value.

## 3. Exact files to create/modify

| File | Role |
|---|---|
| `src/rt_scene_types.h` (new; absorbs the scene half of `rt_types.h`) | `MaterialType`, `SphereMaterial`, `SceneSphere` (§2.2), `RenderSettings` (§2.6), `RESOLUTION_PRESETS` + "Match viewport", `RaytraceMeshData`/`RTMeshInstance`, `RTCameraSnapshot`. `rt_types.h` keeps only what the CPU backend still needs (`ray`, `hit_record`) until §2.14. |
| `src/rt_scene_editor.h/.cpp` | Plain-data spheres; `drawSphereParams`/`drawMaterialParams` free functions; gizmo adapter by `id`; `findSphere(id)`. `revision()` unchanged. |
| `src/scene_io.h/.cpp` | Construct/serialise plain structs (format unchanged); payload version 3 adds `"render"` (§2.6) and `"environmentIntensity"` (§2.10), both optional on read. |
| `src/rt_scene.h/.cpp` | `captureCameraSnapshot()` factored out (§2.4); `buildRaytraceScene()` builds CPU `sphere`/`material` objects from the plain data (§2.2) — the one remaining constructor of those classes. |
| `src/rt_renderer.h/.cpp` (new) | `RaytraceRenderer` (§2.1): panel, `RenderSettings`, `Backend` combo, display image + `"Raytraced Output"` registration, Render/Stop/progress forwarding, `update()` before the imgui frame, `record(cmd)` in `draw()`, `shutdown()`. |
| `src/rt_gpu.h/.cpp` (new) | `GpuPathTracer`: the four `ComputePass`es, every §2.7 buffer/image, the sphere/material upload, seed, sample counter, timestamps, header readback; `start(snapshot)`, `stop()`, `isRunning()`, `record(cmd)`; C++ mirrors of the GLSL structs with `static_assert`s. |
| `src/rt_job.h/.cpp` | Demoted to a backend: no panel, no registry entry; hands its finished linear image to the renderer. Otherwise untouched, pending deletion. |
| `shaders/crt_common.glsl`, `crt_random.glsl` (new) | §2.7 structs, `CRT_WORKGROUP`, the allocator, the stable sphere test; §2.8 RNG and sampling ports. |
| `shaders/crt_generate.comp`, `crt_extend_sphere.comp`, `crt_shade.comp`, `crt_resolve.comp` (new) | The stages (§2.7–§2.11). `extend_sphere` is the designated swap point. Debug views (`debugView` push constant: primary direction, hit/miss, normal, bounce heat, sample-count heat) are written through the radiance slots and `resolve`. |
| `src/vk_compute.h/.cpp`, `src/vk_images.h/.cpp` | `dispatchComputePassIndirect()`, `vkutil::memory_barrier()` (§2.12). |
| `src/vk_engine.h/.cpp` | `RaytraceRenderer m_raytracer` replaces `m_raytraceJob`; `float m_environmentIntensity` (scene property, saved); the `environment` background effect multiplies by it; `m_raytracer.update(this)` where `m_raytraceJob.update()` was, its panel and menu entry likewise; `m_raytracer.record(cmd, this)` in `draw()` as its own step (before `drawBackground()`; only its display image's layout before `drawImgui()` matters); preview spheres per §2.13; `destroyEnvironmentMap()` cancels a running render (§2.3); `shutdown()` from `cleanup()`. |
| `src/CMakeLists.txt` | Add `rt_scene_types.h`, `rt_renderer.*`, `rt_gpu.*`. |
| `docs/plans/completed/raytracing-in-a-weekend.md` | At retirement, the "retired" note (§2.14). |

## 4. Implementation order and dependencies

Validation layers on for every step; the self-driving hook and draw-image readback recipe in `docs/plans/completed/compute-pipeline-general.md` §9.3 apply to the display image unchanged.

1. **Plain-data scene model** (§2.2): `rt_scene_types.h`, editor, gizmo adapter, `scene_io`, preview spheres, the CPU conversion in `buildRaytraceScene()`. Verify: every saved scene in `assets/scenes` round-trips byte-identically; with **Fixed seed**, the CPU backend renders the default scene identically before and after (this is the regression test for the whole refactor); type-switching a material and back keeps its values.
2. **`RaytraceRenderer` with the CPU backend only** (§2.1, §2.5, §2.6): move panel, settings, display image and registration out of `RaytraceJob`; add the `Backend` combo (GPU entry disabled), "Match viewport", `maxSamples`, `exposure`, saved render settings. Verify the CPU render behaves exactly as before through the new shell, and "Match viewport" frames identically to the raster view.
3. **Framework additions** (§2.12) and `captureCameraSnapshot()`. Verify with a throwaway indirect dispatch under validation.
4. **`GpuPathTracer` skeleton**: buffers, images, `start()`/`stop()`, upload, tonemap of the empty mean into the shared display image, timestamps. Verify: Render with the GPU backend shows black at the chosen resolution and stops at `maxSamples`; resolution and `K` changes reallocate cleanly; no VUIDs across renders and shutdown.
5. **`generate` + `resolve`, `debugView = direction`**. Verify against the viewport's `environment` background effect (same camera): frustum edges agree at "Match viewport"; header readback shows `rayCount == W*H*K`.
6. **`extend_sphere`, `debugView = hit/miss` then `normal`**: spheres where the preview spheres are; a clean ground horizon.
7. **`shade`, `rayDepth = 1`, no roulette**, then the full loop; wire the **paths-alive-per-bounce readout** — the counts must fall monotonically and hit 0 before `rayDepth` on the default scene. Do not skip this; it is the only direct evidence the compaction is right.
8. **Accumulation**: converges over frames, stops at `maxSamples`, Stop keeps the image, a free-cam move mid-render changes nothing. `K = 1` and `K = 4` converge to the same image. Fixed seed reproduces an image exactly across two renders.
9. **Backend comparison**: same scene, same snapshot, same seed, CPU vs GPU, roulette off, no environment map — recognisably the same image (§5). Then roulette on: converged image unchanged, GPU ms/frame down. Then the environment map: the render's background matches the viewport's.
10. **Preview spheres per §2.13**, `updateScene()` moved back. Verify no scene-update-time regression and clean validation across edits.
11. **Performance table** for §9 (§8 item 1). Then, when trusted, **§2.14**.

## 5. Edge cases / traps identified during planning

- **The plain-data refactor is the risky step, not the shaders.** It touches the editor, the file format's reader, the gizmo and the preview path at once; the Fixed-seed CPU regression render (§4 step 1) is the safety net — do it before and after.
- **std430 `vec3` padding**: every struct in §2.7 is 16-byte padded and `static_assert`ed; a mismatch is plausible garbage, not a crash.
- **`barrier()` in non-uniform control flow** is undefined; dead lanes carry a flag past the allocator's barriers.
- **Header reset must not depend on a stage running** — `vkCmdUpdateBuffer` from the command buffer, always.
- **Indirect arguments need the `DRAW_INDIRECT` stage in the barrier**; a `COMPUTE`-only barrier lets a bounce read last frame's count, intermittently.
- **`groupCountX` is maintained by the allocator**; nothing derives it from `rayCount` later.
- **Snapshot semantics have two holes to close**: the environment map (cancel on clear/replace, §2.3) and the sphere buffer (uploaded once per render, so an edit mid-render is simply not seen — correct, but say so in the panel: "rendering a snapshot").
- **`textureLod(..., 0)`** in the shade stage, not `texture()`.
- **Float constants** (§2.9); symptoms are acne and a noisy ground horizon.
- **Roulette needs the throughput divide and the probability clamp**, or the image darkens / fireflies.
- **A NaN poisons a running mean for good** — `resolve` drops non-finite samples and does not count them.
- **"Match viewport" resolution changes with the render-scale slider and window size** — it is captured at Render like everything else; the next Render picks up the new extent.
- **Do not read GPU counters or timestamps synchronously**; `FRAME_OVERLAP` frames of latency is fine for a readout.
- **This does not need to numerically match the CPU backend** (double vs float, a different RNG, a different summation order): "recognisably the same scene" is the bar; roulette off and no environment map for that comparison.
- **`updateScene()`'s position in `draw()` is load-bearing until §2.13 lands.**

## 6. Code patterns from the existing codebase to follow

- **`RaytraceJob`** (`src/rt_job.h/.cpp`) is the template for `RaytraceRenderer`'s panel and lifecycle (`update()` before the imgui frame, `RESOLUTION_PRESETS` combo, `visibilityFlag()`, `shutdown()` from `cleanup()`), even as its internals are retired.
- **Every GPU pass** through `ComputePassBuilder` with the per-frame descriptor-set idiom (`docs/plans/completed/compute-pipeline-general.md` §9.2; `TonemapPass::dispatch()` is the 12-line reference); the `environment` background effect's `record` closure for the sampler binding.
- **Ported math**: `src/rt_job.cpp` (`RTCamera`, `ray_color`'s gradient, `renderPerPixel`'s NaN drop), `src/rt_material.cpp`, `src/rt_random.cpp`, `src/rt_hittable.cpp` (`sphere::hit`, `set_face_normal`) — function for function, same names in GLSL.
- **Revision counters** (`RaytraceSceneEditor::revision()`, `m_sceneRevision`) for the preview-sphere slots; snapshot-at-click for the render itself.
- **Scene file extras** (`scene_io.cpp`: `materialJson`, `readMaterial`, the version branch) for the `"render"` and `"environmentIntensity"` additions.
- **Buffers and mapped memory**: `createBuffer()`, `vmaFlushAllocation()` after every CPU write, `vmaInvalidateAllocation()` before every CPU read (`CLAUDE.md`).
- **Display**: `DisplayRegistry::registerImage()` and its layout obligation; the "Scene Mirror" image is the precedent for an image rewritten every frame.

## 7. What NOT to do (alternatives rejected and why)

- **Do not** make the raytracer a live view of the viewport or let it replace the raster image (§2.3): the raster window is the preview, the raytracer renders a camera. Accumulation resets happen at Render, never on camera motion.
- **Do not** keep the virtual `hittable`/`material` classes as the scene model and merely flatten them at upload (§2.2) — that leaves the CPU tracer's hierarchy alive after its methods die.
- **Do not** extend `RaytraceJob` into the GPU tracer or give the GPU tracer its own panel/window (§2.1).
- **Do not** build a megakernel. Honest note: the CUDA reference reaches 9 ms at 720p / 30 spp as one, and the 2026 comparison finds megakernels competitive when materials are simple. Wavefront is chosen for what it enables, not for speed on this scene.
- **Do not** use subgroup ballots, per-ray global atomics, a prepare-indirect pass or counter readback to drive dispatch (§2.7); **do not** build Laine's persistent regenerating pool in this pass.
- **Do not** blend material lobes branchlessly; **do not** add emissive materials, explicit lights, next-event estimation, importance-sampled environment lighting, triangle intersection or any acceleration structure here — each is a later doc against the contracts designed in (§2.7's hit record and queues, `RaytraceMeshData`).
- **Do not** implement the adaptive policy (§2.11); hook only.
- **Do not** expect bit-exact CPU/GPU agreement; **do not** port `std::mt19937` or a `sin()` hash.

## 8. Open questions / things to verify before starting

1. **Performance on the Radeon Pro 560X via MoltenVK** — unknown. Measure with §2.12's timestamps at 640×360 and 1280×720, `K = 1`/`4`, roulette on/off; record a table in §9. It decides the default `K`, `CRT_MAX_POOL`, and whether the persistent pool (§7) is ever worth revisiting.
2. **Timestamp queries under MoltenVK** — supported, but check `timestampComputeAndGraphics`/`timestampPeriod` at init and make the readout optional.
3. **Scene cameras** — when cameras enter the scene graph, `captureCameraSnapshot()` gains a "which camera" argument and the panel a camera combo (free cam = the raster view's); the raster view "taking over" a scene camera is the raster side's concern. Nothing in this doc should assume the free cam is the only source — it is only the only source *today*.
4. **Should `"render"` settings in the scene file include `maxSamples` and `samplesPerFrame`?** They are more "this machine" than "this scene"; saving them is harmless (defaults on read), but a scene shared between machines might not want them. Decide when writing `scene_io` v3.
5. **Tonemap operator** — clamp + gamma 2 is a placeholder; a real operator (Reinhard, ACES) is a change to `shaders/tonemap.comp` alone, and `exposure` (§2.10) is already its input.
6. **Mipmapped environment maps** — not needed here (progressive averaging over jittered rays hides aliasing, an advantage over the raster background), but a future importance-sampled environment light will want a luminance mip chain.
7. **Per-pixel budget semantics when `K` changes** (§2.11): absolute counts clamped to `K` in `generate` is the assumption; the adaptive doc should confirm.
8. **Multi-threading the CPU backend** (its doc's §9.4) — moot once §2.14 happens; do not spend time on it.

## 9. As-built notes (implementation, 2026-09-15)

Everything below is what actually got built and verified, written after the fact. Where it disagrees with §§1–8, this section is right. The CPU backend is **not yet retired** (§2.14): the user asked for the backend switch to stay until they have compared the two interactively.

### 9.1 Deviations from this spec, and why

Three were put to the user before implementation and approved; the rest are judgment calls filling gaps the spec left.

1. **Pixel-centre mapping on both backends** (approved). The CPU camera mapped pixel `x` to `u = x / (W-1)`, putting pixel 0's sample on the frustum's edge; the raster pass uses `(x + 0.5) / W`. Both backends now use `1/W` pixels with the sample at the pixel centre (or jittered within it), so "Match viewport" is framed exactly as the raster view. Done in step 3, after the step-1 regression check (§9.3) had passed with the old mapping.
2. **The step-1 regression check is not bit-exact and cannot be**: the scene model is float, so `0.7` becomes `0.7f`, and the CPU backend's double math sees `0.699999988`. Measured: at most 6e-8 per channel; no pixel differs by even 1/255 (§9.3).
3. **Resolution changes and pool reallocation retire the old resources with `vkDeviceWaitIdle()`** at the Render click, the precedent scene loading and environment-map replacement already set, rather than a third deferred-destroy list. A Render is a menu-like action; a one-frame stall there is invisible.
4. **One descriptor set serves all four stages.** Every stage's `ComputePass` declares the same eleven bindings (`shaders/crt_common.glsl`: paths, hits, queues, headers, radiance, budget, spheres, materials, accumulation, sample count, environment map), a shader simply does not declare the ones it does not use, and identically-defined layouts are compatible, so one set per frame written once is bound to all four pipelines. One push-constant block (`CrtParams`, 160 bytes) likewise serves all stages; `bounce` is the only field that changes between dispatches.
5. **The scene file's `"render"` block saves every setting** including `maxSamples` and `samplesPerFrame` (§8 q4): defaults on read, so a scene shared between machines loses nothing. Format version 3; version 1 and 2 files still load. `newScene()` resets the render settings and `environmentIntensity` too — they are scene properties now.
6. **The tonemap runs every frame the GPU backend owns the display image**, running or not, so `exposure` is live for a finished GPU render as well. A CPU render is tonemapped once at publish (its exposure captured at Render) and left alone. `RaytraceRenderer` tracks which backend rendered last for this.
7. **`RaytraceJob` keeps a `pixels()` accessor rather than handing the buffer over**: the renderer uploads and tonemaps in `update()` the frame the worker is joined, exactly as `publishOutput()` did. Nothing else about the worker changed.
8. **The `GpuRenderSnapshot` carries `useEnvironmentMap` and `environmentIntensity` by value**, and `record()` re-checks that the map still exists each frame. Clearing or replacing the map cancels the render (§2.3) through `destroyEnvironmentMap() → RaytraceRenderer::cancelRender()`, verified.
9. **Debug views terminate the path at the shade stage** and write through the radiance slots, so they accumulate (and anti-alias) like radiance: primary direction, hit/miss, normal, and bounce heat (bounce count at termination / rayDepth). Sample-count heat is written by `resolve` directly as the mean. Switchable mid-render; the mean then mixes views until the next Render.
10. **`RaytraceSceneEditor::findSphere(id)`** and per-sphere ids assigned by the editor (`m_nextId`, never saved). The gizmo's opaque `const void*` target id is the sphere id cast, never dereferenced.
11. **`TRANSFER_SRC` usage on the accumulation and display images**, so a readback (a future "save render", the smoke test's dumps) needs no image recreation.
12. **The header-readback and timestamp collection happen inside `draw()`** (`GpuPathTracer::beginFrame()` from `RaytraceRenderer::record()`), right after the frame's fence wait, so the results are two frames old rather than three — the fence for slot `N % 2` is only known signalled from that point on.
13. **The CPU regression harness and the smoke script were temporary hooks, all removed** (`grep SMOKETEST` is empty). Two GLSL traps they found: `sample` and `active` are reserved words in GLSL 4.60.

### 9.2 The API as built

```cpp
// rt_scene_types.h - the plain scene model, float
enum class MaterialType : uint8_t { Lambertian, Metal, Phong, Dielectric };
struct SphereMaterial { MaterialType type; glm::vec3 albedo; float fuzz, smoothness, ir; };   // every type's fields kept
struct SceneSphere { uint64_t id; std::string name; glm::vec3 center; float radius; SphereMaterial material; };
glm::vec3 materialPreviewColor(const SphereMaterial&);  SphereMaterial makeSphereMaterial(MaterialType, glm::vec3 albedo = 0.7);
struct RTCameraSnapshot { glm::vec3 lookFrom, lookAt, vUp; float vfovDegrees, aperture, focusDistance; };
struct RenderSettings { int width, height; bool matchViewport, antialiasing; int maxSamples, rayDepth; bool useFixedSeed; uint32_t seed;
                        int samplesPerFrame; bool russianRoulette; int minBouncesBeforeRoulette; float aperture, focusDistance, exposure; };
// RTMeshInstance, RaytraceMeshData, RESOLUTION_PRESETS moved here unchanged. rt_types.h keeps ray, hit_record, RT_INFINITY, RT_PI.

// rt_scene_editor.h
bool drawSphereParams(SceneSphere&);  bool drawMaterialParams(SphereMaterial&);   // the former virtual params()
SceneSphere* RaytraceSceneEditor::findSphere(uint64_t id);   // null once gone; valid until the next mutation

// rt_scene.h
RTCameraSnapshot captureCameraSnapshot(VulkanEngine*, const RenderSettings&);   // both backends; aperture/focus from the settings
// rt_scene.cpp: makeCpuMaterial(const SphereMaterial&) - the only constructor of the CPU material classes

// rt_renderer.h
enum class RaytraceBackend { Gpu, CpuLegacy };
class RaytraceRenderer { void init(VulkanEngine*); void update(VulkanEngine*); void drawPanel(VulkanEngine*, const RaytraceSceneEditor&);
    void record(VkCommandBuffer, VulkanEngine*); void cancelRender(); void shutdown(VulkanEngine*);
    RenderSettings& settings(); void setSettings(const RenderSettings&); bool* visibilityFlag(); };

// rt_gpu.h
struct GpuRenderSnapshot { uint32_t width, height; RTCameraSnapshot camera; std::vector<SceneSphere> spheres; RenderSettings settings;
                           uint32_t seed; bool useEnvironmentMap; float environmentIntensity; };
class GpuPathTracer { void init(VulkanEngine*); void destroy(VulkanEngine*); void start(VulkanEngine*, GpuRenderSnapshot); void stop();
    void beginFrame(VulkanEngine*); void record(VkCommandBuffer, VulkanEngine*, const AllocatedImage& display, float exposure);
    bool isRunning(), hasImage(), hasTimestamps(); uint32_t samplesAccumulated(), maxSamples(), samplesPerFrame(), width(), height();
    float lastFrameGpuMs(), totalGpuMs(); const std::vector<uint32_t>& pathsAlivePerBounce(); CrtDebugView debugView; };
// CrtSphere (16 B), CrtMaterial (32 B), CrtQueueHeader (16 B) static_asserted; CRT_MAX_POOL = 4M paths, CRT_MAX_DEPTH = 16, CRT_MAX_SAMPLES_PER_FRAME = 8

// rt_job.h - demoted: start(RaytraceScene&&), cancel(), bool update() (true once when joined), shutdown(), isRunning(), progress(),
//            completed(), renderMs(), pixels(), width(), height(). No panel, no Vulkan.

// vk_compute.h
void dispatchComputePassIndirect(VkCommandBuffer, const ComputePass&, VkDescriptorSet, const void* push, VkBuffer args, VkDeviceSize offset);
// vk_images.h
void vkutil::memory_barrier(VkCommandBuffer, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess);

// vk_engine.h: RaytraceRenderer m_raytracer (was RaytraceJob m_raytraceJob); float m_environmentIntensity (was m_environmentBackgroundExposure,
//   now saved with the scene, the "background" window's slider is "Environment intensity"); PreviewSphereSlot m_previewSphereSlots[FRAME_OVERLAP];
//   updateScene() is cpu-only again and runs before the fence wait; drawRaytraceSpheres() runs after it.
// scene_io.h: SceneDescription gains RenderSettings render; float environmentIntensity. Payload version 3.
```

**Per-frame sequence** (`GpuPathTracer::record()`), all in `draw()`'s command buffer before the raster passes: `[clear accumulation, counts, fill budget with K]` → reset header 0 (`vkCmdUpdateBuffer`) → generate (direct, `poolSize / 64` groups) → copy header 0 to the readback → for each bounce: extend (indirect, header `b & 1`) → reset header `(b+1) & 1` → shade (indirect) → copy the next header to the readback → resolve (direct over `W*H`) → tonemap. Memory barriers between every pair use `COMPUTE | DRAW_INDIRECT` and `COPY | CLEAR` stage masks with the full storage/indirect/transfer access sets — generous by design.

### 9.3 Verification actually performed

**Step 1 regression (CPU backend, default scene, 320×180, 8 spp, depth 8, fixed seed 7)** via a temporary dump hook, before and after the plain-data refactor: 765 of 230,400 floats differ, max difference **5.96e-8**, zero pixels differ by 1/255 or more. Scene file: `assets/scenes/sphere_scene.gltf` (version 1) opened, saved, reopened, saved again — the two saves are byte-identical and their sphere data equals the original's.

**Everything below under validation layers** (`b_UseValidationLayers = true` temporarily; **0 validation messages** across the whole script and shutdown), driven by a scripted hook in `RaytraceRenderer::update()` that set the settings, called `startRender()`, waited for completion and read the display image (rgba8 → PPM) and the accumulation (rgba32f → raw) back:

| Phase | Result |
|---|---|
| Debug views: primary direction, hit/miss, normal (320×180, 1 sample) | correct: a smooth direction gradient; the three spheres and the ground as a mask; ground normal reads +Y green |
| Bounce heat, depth 1 | plausible heat; depth 1 shows sky only with black spheres and ground, matching `ray_color()`'s semantics |
| **Paths alive per bounce**, default scene, K=1 | **57600 → 26370 → 914 → 420 → 160 → 85 → 59 → 42 → 0** (monotone, empty at depth 8); K=4: 230400 → 105482 → … → 0 (4× K=1 as it should be) |
| 64 spp, K=1, roulette off, seed 7 — twice | **bit-identical** accumulation buffers (57600/57600 pixels) |
| K=1 vs K=4, same seed | same image: mean absolute difference 0.0000, mean brightness 0.8007 both (the sample streams depend only on pixel, sample index and seed, not K) |
| Roulette on vs off (K=4) | mean difference 0.0001, 97% of pixels identical (roulette only touches bounces ≥ 4); alive counts fall faster from bounce 4 |
| **GPU (K=1) vs CPU, 64 spp each** | recognisably the same image (glass / red / gold left to right, ground, sky); mean absolute difference 0.0031, max 0.138 (different RNG); mean brightness **0.8007 vs 0.8007** |
| Match viewport (1700×900, K=1) | 1,530,000-path pool (187 MB) allocated, rendered, 7.6 ms/frame |
| 8k environment map, 16 spp | the render's background is the map, spheres lit by it |
| Clear the map at 3 samples of a 4096-sample render | render stopped the same call (`cancelRender()`), image kept |
| Raster preview after recolouring a sphere and adding a fifth (slot rewrite + capacity grow) | correct: green centre, blue metal fifth sphere, glass tint, gold; scene update time **0.52 ms** |
| Shutdown after all of the above | exit 0 |

**Performance** (§8 q1; Radeon Pro 560X via MoltenVK, default scene, depth 8, 64 spp, GPU timestamps, `ms/frame` = whole generate→resolve→tonemap sequence):

| Resolution | K | Roulette | GPU ms/frame | ms per sample/pixel |
|---|---|---|---|---|
| 640×360 | 1 | off | 3.40 | 3.40 |
| 640×360 | 1 | on | 3.38 | 3.38 |
| 640×360 | 4 | off | 7.80 | 1.95 |
| 640×360 | 4 | on | 7.78 | 1.95 |
| 1280×720 | 1 | off | 8.23 | 8.23 |
| 1280×720 | 1 | on | 8.20 | 8.20 |
| 1280×720 | 4 | off | 19.3 | 4.83 |
| 1280×720 | 4 | on | 17.5 | 4.38 |
| 1700×900 | 1 | off | 7.6 | 7.6 |

Roulette buys nothing on this open scene — fewer than 1% of paths survive past bounce 3 anyway; it will matter in enclosed scenes. K=4 is ~1.7× cheaper per sample than K=1 (bigger tail dispatches, same barrier count), which argues for a default of 4 at 720p and below; left at 1 so the panel's first render is the cheapest. The 1280×720 K=4 pool is 387 MB.

### 9.4 Not verified — needs a human at the keyboard

Interactive checks, handed to the user as the test list: the Backend combo and the greyed GPU-only rows; the progress bar, "GPU ms/frame" and "Paths alive per bounce" readouts updating live; Stop keeping the image; moving the free camera or editing a sphere mid-render changing nothing; the "Raytraced Output" window sized against the viewport at "Match viewport" for an A/B; exposure and debug view changing a finished GPU render live; aperture/focus distance (depth of field) on both backends; the scene file round-trip of the render settings and environment intensity through the File menu; the "background" window's renamed slider agreeing with the render's sky brightness; loading a version-1/2 scene; the sphere material combo keeping the other types' values when switched away and back; gizmo editing on plain-data spheres; frame-time smoothness with a 720p K=4 render running alongside the raster view.

### 9.5 Answers to §8's open questions

1. Performance: the table above. Default K stays 1; `CRT_MAX_POOL` at 4M paths holds 1280×720×4 and 1920×1080×2. The persistent regenerating pool is not worth revisiting until a scene keeps paths alive past bounce 3.
2. Timestamps: supported (`timestampComputeAndGraphics`, period ~1.0 ns on this device); the readout is skipped when the limit is absent.
3. Scene cameras: `captureCameraSnapshot(engine, settings)` is the one function to grow.
4. Yes, all settings are saved (§9.1 item 5).
5–7: unchanged; not needed yet.
8. Moot.

### 9.6 Known limitations left in place

- The CPU backend and `RaytraceJob`, `rt_random`, `rt_hittable`, `rt_material` are still present pending §2.14.
- `sampleBudget` is filled with K at Render and never written again (the hook, as specified); the sample-count heat view is therefore flat.
- A debug view switched mid-render mixes into the running mean until the next Render.
- With the environment map, the dusk map's sun makes the ground noisy at low sample counts — no importance sampling, by design (§7).
- The environment map still has no mipmaps; the miss branch uses `textureLod(…, 0)`.
- The raytracer records into the same command buffer as the raster frame, so a 720p K=4 render (~19 ms of GPU work) will cost the viewport its frame rate; time-budgeting K is the adaptive-sampling doc's concern.
