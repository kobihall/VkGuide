# Feature: Raytracing in a Weekend (CPU port)

## 0. How to use this doc

Standalone implementation spec — treat it as the only context you have. For exhaustive facts about the current codebase state, read `docs/codebase-map.md` first (§6 covers the source project this feature ports from in detail). This feature depends on `docs/plans/completed/imgui-display.md` (the `DisplayRegistry` it introduces) being implemented first — read that doc too, since this one uses its API directly. Before writing code, re-verify any line numbers cited here against current source.

**Status: implemented and verified on 2026-08-23.** The spec below is preserved as written; §9 records the as-built picture — deviations, verification evidence, and answers to every §8 open question. **Read §9 before relying on any API shape described in §2/§3** — and §9.6 in particular, which records review fixes made on 2026-09-13 that change several of §9's own API shapes.

**Scope note**: this is a CPU port of "Ray Tracing in a Weekend" — the first book only. The user has not yet implemented "Ray Tracing: The Next Week" (BVH, mesh/triangle intersection, volumes) or "The Rest of Your Life" (Monte Carlo importance sampling). Triangle-mesh ray intersection and any acceleration structure are explicitly **out of scope** for this feature and are planned as a separate, later feature. Do not build BVH/triangle-intersection code here — see §2.1/§2.2 for exactly what that means in practice.

## 1. Feature goal

Let the user interactively arrange a scene of raytraced spheres — added/edited through a dedicated panel that also browses the objects present in the currently-loaded raster scene — position the camera in the raster viewport, and click "Render" to produce a CPU-raytraced image of the sphere scene from that viewpoint, shown in its own window via the feature-1 display registry, without freezing the rest of the app while it renders.

## 2. Architecture decisions made and WHY

### 2.1 Scope: sphere-only tracing; glTF objects are browsable but not (yet) traceable

glTF is a triangle-mesh format — it has no native analytic sphere primitive (confirmed: a "sphere" would only ever appear in a `.gltf` file as an authored icosphere/UV-sphere *mesh*, indistinguishable from any other mesh). The existing CPU raytracer (`docs/codebase-map.md` §6) only supports one `hittable` subtype, `sphere`. Since this feature does not add triangle-mesh intersection (see §2.2), the raytraced scene's geometry can only ever be spheres — and since glTF can't supply those, they come from a dedicated, manually-editable sphere list, ported directly from the sibling project's "Scene" panel (`sphere::params()` pattern, `docs/codebase-map.md` §6).

Per explicit direction, that panel's object list is **unified**: it shows both the manually-added spheres and every mesh-bearing node in the currently-loaded glTF scene(s) (`VulkanEngine::m_loadedScenes`, `vk_engine.h:190`), in one combined browser. Selecting a sphere shows its existing editable sliders (position/radius, ported as-is). Selecting a glTF object shows nothing — no parameters, no editing, no error — since mesh objects aren't traceable in this pass. This is a deliberate, inert browsing seam for the future triangle-tracing feature: the list already knows those objects exist, it just can't do anything with them yet.

### 2.2 No BVH, no acceleration structure, no triangle intersection

Directly follows from §2.1 and the scope note in §0. With only a handful of user-placed spheres (matching the original project's own scale — 4 spheres, `docs/codebase-map.md` §6), a flat linear scan over the sphere list (mirroring the sibling project's `hittable_list::hit()`, ported as-is) is exactly as fast as it needs to be — there is no performance problem here to solve. No `aabb`/bounding-box type, no BVH build or traversal, and no `triangle` hittable/intersection routine (Möller–Trumbore or otherwise) are part of this feature. If a future feature adds mesh tracing, it will need those — this feature deliberately does not anticipate or half-build them.

### 2.3 CPU-side vertex/index and material-factor retention — kept, but explicitly inert this pass

Verified directly in `src/vk_loader.cpp` (`loadPrimitiveGeometry`, `loadGltf`, `loadGltfMeshes`): CPU-side `std::vector<uint32_t> indices` / `std::vector<Vertex> vertices` are already built for every mesh during loading — they're the exact data handed to `engine->uploadMesh(indices, vertices)` (`vk_loader.cpp:169`, `:345`) for the GPU upload, then cleared and reused for the next mesh (`vk_loader.cpp:153-154`, `:317-318`) rather than retained. Similarly, `GLTFMetallic_Roughness::MaterialConstants` (`colorFactors`, `metalRoughFactors`) are computed per glTF material (`vk_loader.cpp:265-272`) and written straight into a GPU uniform buffer, not kept anywhere else on the CPU side.

**Decision**: retain both, since it's a small, low-risk, well-localized change at a point in the code that's already touching this exact data — add `std::vector<Vertex> cpuVertices; std::vector<uint32_t> cpuIndices;` to `MeshAsset` (`src/vk_loader.h:29-34`), and `glm::vec4 colorFactors; glm::vec2 metalRoughFactors;` to `GLTFMaterial` (`src/vk_loader.h:12-14`), populated alongside the existing GPU-upload/material-constant code. **This data has no consumer in this feature** — nothing in this pass reads `cpuVertices`/`cpuIndices`/the retained material factors for tracing. It exists purely so a future triangle-tracing feature doesn't have to re-derive "how do I get mesh data onto the CPU" from scratch; don't treat its presence as evidence that mesh raytracing works, because it doesn't yet.

### 2.4 Two data lifecycles: the live scene editor vs. the one-shot render snapshot

This feature has two distinct, differently-scoped pieces of "scene" state, and keeping them separate matters:

- **`RaytraceSceneEditor`** — live, persistent, mutated every frame the panel is open. Owns the actual sphere list (`std::vector<std::shared_ptr<sphere>>`, starting from the original project's 4-sphere default) and draws the unified object-browser panel (§2.1) — including, read-only, the current glTF scene's mesh-node names pulled fresh from `m_loadedScenes` each time the panel draws.
- **`RaytraceScene`** (built by `buildRaytraceScene()`) — a one-shot, immutable snapshot captured only at the moment "Render" is clicked: copies the *current* sphere list out of the editor, walks `m_loadedScenes` once to populate the inert triangle/material scaffold from §2.3, and captures the camera (`m_mainCamera`'s position/orientation/FOV at that instant). This is the value handed to the worker thread (§2.5) — after handoff, the editor can keep changing (user adds another sphere, moves the camera, loads a different glTF file) without affecting a render already in flight.

**Consequence worth calling out explicitly**: editing a sphere, or moving the raster camera, *while a render is in progress* does not affect that render — same reasoning as the original project's own "each render is a snapshot of settings at that moment" behavior (`Renderer::render()` rebuilds its camera fresh each call, `Renderer.cpp:40`), just now applied to a background thread instead of a synchronous call.

### 2.5 Threading: one dedicated background thread, snapshot-then-detach

The user's raster viewport keeps rendering live while a raytrace runs — doing that inline on the UI thread (as the original project does) would freeze VkGuide's window, including the swapchain present loop, for the render's duration. This feature runs the raytrace on one dedicated `std::thread`, spawned when "Render" is clicked.

**Critical rule**: once spawned, the worker thread must never touch `VulkanEngine`, any live Vulkan handle, or the scene editor's live state again. It receives the fully self-contained `RaytraceScene` snapshot (§2.4) by move at spawn time and nothing else.

Only one render runs at a time — the "Render" button is disabled while one is in flight, and a "Cancel" button appears. Cancellation is `std::atomic<bool> m_cancelRequested`, checked between scanlines (not just once at loop entry, so Cancel actually feels responsive). Progress is `std::atomic<float> m_progress`, updated per scanline, driving a progress bar — new UI the original project didn't need (it was fully synchronous) but is close to free given the threading infrastructure this feature already requires.

**Explicitly not in scope**: parallelizing the raytrace itself across multiple cores. The requirement was "background thread, non-blocking" — moving the existing single-threaded loop off the UI thread — not building a parallel renderer. That's a straightforward, purely additive future speedup (§7) if single-core performance ever becomes a real problem, which is unlikely at this feature's scale (a handful of spheres, no BVH needed per §2.2).

### 2.6 Output format and display integration

Raytraced output uses `VK_FORMAT_R8G8B8A8_UNORM` (matching the sibling project's proven `Image` RGBA path, `docs/codebase-map.md` §6) rather than VkGuide's own HDR `m_drawImage` format — no need to match it, since this feature's output lives in its own independent window via `DisplayRegistry` (feature 1), not composited into the raster HDR pipeline. On render completion (checked once per frame, main thread only): upload the finished pixel buffer via the existing `VulkanEngine::createImage(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped)` (`vk_engine.h:229`, same staging-buffer-upload pattern already used by `load_image()` in `vk_loader.cpp:426` — pass `VK_IMAGE_USAGE_SAMPLED_BIT` as that call site already does), then register (first render) or re-register (subsequent renders — per `docs/plans/completed/imgui-display.md` §5, unregister + destroy the old image before registering the new one, since the `VkImageView` handle changes each time) with the feature-1 `DisplayRegistry` under a fixed name like `"Raytraced Output"`.

## 3. Exact files to create/modify

| File | Role |
|---|---|
| `src/rt_types.h` (new) | `ray` (origin/direction), `hit_record`, `RTTriangle` (plain data — 3 world-space positions, **not** a `hittable`, no intersection logic), `RTMeshInstance` (a source glTF node's collected triangles + its `colorFactors`/`metalRoughFactors` + name — mirrors `GeoSurface` grouping), a camera-snapshot struct. No `aabb` — not needed without a BVH (§2.2). |
| `src/rt_hittable.h/.cpp` (new) | `hittable` base (`hit()` + `params()`, matching the sibling project's interface exactly — no `bounding_box()`, since nothing builds a BVH over these) and `sphere` (ported as-is). |
| `src/rt_material.h/.cpp` (new) | `material` base, `lambertian`/`metal`/`phong`/`dielectric` (ported as-is from the sibling project). No glTF-material-mapping function — deferred, see §7. |
| `src/rt_scene_editor.h/.cpp` (new) | `RaytraceSceneEditor` (§2.4): owns the live sphere list, draws the unified "Raytracer Scene" panel — object list combining spheres (editable) and glTF mesh-node names pulled from `m_loadedScenes` (read-only, selecting one shows nothing further). Window title deliberately avoids the word "Scene" alone to not collide with VkGuide's existing "scene" terminology (`m_loadedScenes`, `GPUSceneData`) — call it e.g. `"Raytracer Scene"`. |
| `src/rt_scene.h/.cpp` (new) | `RaytraceScene` struct (§2.4 — copied sphere list + `RTMeshInstance` scaffold + camera snapshot + render settings) and `buildRaytraceScene(VulkanEngine* engine, const RaytraceSceneEditor& editor, const RenderSettings& settings) -> RaytraceScene`. Runs on the main thread, synchronously, when Render is clicked, before the worker thread is spawned. |
| `src/rt_job.h/.cpp` (new) | `RaytraceJob` — owns the worker thread and its lifecycle, the atomics (`m_progress`, `m_cancelRequested`, completion state), the ported per-pixel render loop (`renderPerPixel`/`ray_color` from the sibling project's `Renderer.cpp`, iterating **only** `scene.spheres` — `scene.triangles`/mesh-instance data is never read by this loop), the finished pixel buffer, and: `void update(VulkanEngine* engine)` (call once per frame — checks for completion, does the main-thread upload + `DisplayRegistry` registration from §2.6) and `void drawControlPanel(VulkanEngine* engine, const RaytraceSceneEditor& editor)` (a separate "Render Settings"-style window: Render/Cancel button, progress bar, resolution/samples/depth controls, last-render-time readout — ported from the sibling project's "Settings" window, `docs/codebase-map.md` §6). |
| `src/vk_loader.h` | Add `std::vector<Vertex> cpuVertices; std::vector<uint32_t> cpuIndices;` to `MeshAsset`; add `glm::vec4 colorFactors; glm::vec2 metalRoughFactors;` to `GLTFMaterial` (§2.3). |
| `src/vk_loader.cpp` | In `loadGltfMeshes()` and `loadGltf()`, populate the new `MeshAsset` fields alongside the existing `uploadMesh()` call; in `loadGltf()`, populate the new `GLTFMaterial` fields alongside the existing `constants` computation (`vk_loader.cpp:265-272`). |
| `src/vk_engine.h` | Add `RaytraceSceneEditor m_raytraceScene;` and `RaytraceJob m_raytraceJob;` members. |
| `src/vk_engine.cpp` | In `run()` (`vk_engine.cpp:419-493`): call `m_raytraceJob.update(this)` early (before the ImGui frame content is built, so a completed render's image is registered with `DisplayRegistry` in time to be drawn this same frame); draw `m_raytraceScene`'s panel and `m_raytraceJob.drawControlPanel(this, m_raytraceScene)` alongside the existing `"background"`/`"Stats"` windows; wire the Render button to call `buildRaytraceScene()` then hand the result to `m_raytraceJob`. |
| `src/CMakeLists.txt` | Add all new `rt_*.h/.cpp` files to the explicit `add_executable(vulkan_guide ...)` source list — confirmed during planning this file is an **explicit list, not a glob** (`src/CMakeLists.txt:2-16`), so every new file must be named or it silently won't build. |

## 4. Implementation order and dependencies

Depends on `docs/plans/completed/imgui-display.md` being implemented first (`DisplayRegistry` is used directly in §2.6/§3).

1. **`rt_types.h`, `rt_hittable.h/.cpp` (sphere only), `rt_material.h/.cpp`**: straight port. Verify in isolation with a tiny hand-written scene (a couple of spheres) and a manual ray, no VkGuide integration yet.
2. **`rt_scene_editor.h/.cpp`**, sphere side only first: live sphere list seeded with the original project's 4-sphere default, "Raytracer Scene" panel showing just the sphere list with the ported `params()` editing. Verify this compiles and runs showing an editable sphere list before touching glTF integration at all.
3. **`vk_loader.h/.cpp` changes** (§2.3): add the CPU-retention fields, populate them. Small, isolated, additive — verify by loading the existing `"structure"` scene and confirming `cpuVertices`/`cpuIndices` are populated with expected counts.
4. **Extend `rt_scene_editor`** to also enumerate `m_loadedScenes`' mesh nodes as read-only rows in the same list. Verify the combined list shows both spheres and glTF object names, and that selecting a glTF row shows nothing (no crash, no stale sphere-params UI left over from a previous selection).
5. **`rt_scene.h/.cpp`**: `buildRaytraceScene()` — copy the current sphere list from the editor, walk `m_loadedScenes` into the (currently unused) `RTMeshInstance` scaffold, capture the camera. Verify by dumping counts (sphere count matches the editor, triangle/instance count matches loaded meshes) — no visual output yet.
6. **`rt_job.h/.cpp`**: worker thread + the ported render loop, iterating only `scene.spheres`. Build incrementally — first get a synchronous (same-thread, blocking) render working end-to-end and confirm a correct sphere-scene image comes out, *then* move the render call onto a `std::thread` and add progress/cancel/completion-signaling around it. Debugging a wrong image is much easier before threading is in the mix.
7. **`vk_engine.h/.cpp` wiring** (§3).
8. **End-to-end smoke test**: with the default sphere scene and the `"structure"` glTF scene both present, click Render, confirm a floating/dockable window (via feature 1) shows a correctly raytraced sphere image from the current raster camera position; confirm glTF objects appear in the "Raytracer Scene" list but selecting one shows nothing; confirm the app stays responsive during the render; confirm Cancel stops it promptly; confirm a second Render correctly replaces the displayed image with no leak (previous `AllocatedImage` destroyed, previous `DisplayRegistry` entry unregistered first, per feature 1's required order).

## 5. Edge cases / traps identified during planning

- **The triangle/material scaffold (§2.3) has no consumer in this feature.** It's easy to mistake "the data extraction code runs without crashing" for "mesh raytracing works" — it doesn't; nothing intersects that data yet. Keep this distinction clear in code review/testing for this feature.
- **Thread-safety boundary is the most important correctness rule here** (§2.5): the worker thread must receive an already-copied `RaytraceScene` and never read `VulkanEngine`/`m_loadedScenes`/the live scene editor/`m_mainCamera` again after being spawned.
- **Live-editor vs. snapshot confusion** (§2.4): editing spheres or moving the camera while a render is in flight does not retroactively affect it — this is intentional, but is a real "why didn't my change show up" trap if not understood going in.
- **Cancellation must be checked frequently** (between scanlines, not just once at loop entry) or Cancel will feel broken.
- **Re-render cleanup order**: `DisplayRegistry::unregisterImage()` must happen *before* `VulkanEngine::destroyImage()` on the previous raytraced output (per `docs/plans/completed/imgui-display.md` §2.4/§5) — reversing that order leaves ImGui holding a descriptor pointing at a destroyed image.
- **Object-browser selection state**: when the unified list's selection changes from a sphere to a glTF row (or vice versa), make sure whatever ImGui state drives the sphere's `params()` widgets (e.g. slider IDs) doesn't leak/persist incorrectly across the switch — straightforward to get right, easy to get subtly wrong (e.g. showing stale slider values from the previously-selected sphere for one frame).
- **`MeshAsset`'s new CPU vectors add permanent RAM cost** proportional to total loaded mesh data (§2.3) — judged negligible at this project's scale.

## 6. Code patterns from the existing codebase to follow

- **Sphere hittable, materials, render loop shape**: `docs/codebase-map.md` §6 is the direct porting source for `rt_hittable.h/.cpp` (sphere only), `rt_material.h/.cpp`, and `rt_job.h/.cpp`'s render loop and settings panel.
- **Object-list + per-object dispatch UI**: the sibling project's `hittable_list::IDs` + `ImGui::ListBox` + `object->params()` dispatch (`docs/codebase-map.md` §6, `sphere::params()`, `main.cpp:56`) is the direct precedent for `rt_scene_editor`'s unified browser — extend the same dispatch pattern to a second, non-editable "kind" of row (glTF objects) rather than inventing a new UI pattern.
- **Scene traversal for the glTF side of the browser and the triangle scaffold**: `LoadedGLTF::Draw()` (`vk_loader.cpp:483-489`) and `MeshNode::Draw()` (`vk_engine.cpp:1338`) show the existing pattern for walking `topNodes` and accumulating world transforms — reuse the same walk, just collecting node names (for the browser) or triangles (for `buildRaytraceScene()`) instead of `RenderObject`s.
- **GPU image upload**: `load_image()` (`vk_loader.cpp:403-481`) is the exact existing precedent for "CPU pixel buffer → `engine->createImage(data, ...)` → done" that §2.6's completion handler should follow, including the `VK_IMAGE_USAGE_SAMPLED_BIT` usage flag.
- **`DisplayRegistry` usage**: follow `docs/plans/completed/imgui-display.md` exactly for how `RaytraceJob::update()` registers/re-registers its output image.

## 7. What NOT to do (alternatives rejected and why)

- **Do not** implement triangle-mesh ray intersection, bounding boxes, or a BVH in this pass (§2.1, §2.2) — this was an earlier design mistake in planning, corrected per explicit direction: those belong to a separate, later feature that hasn't been scoped yet. Retaining CPU-side mesh data (§2.3) is fine because it's cheap and forward-compatible; *acting* on that data with intersection logic is not in scope.
- **Do not** implement the glTF-material-to-raytracer-material mapping (converting `colorFactors`/`metalRoughFactors` into a `lambertian`/`metal` choice) in this pass — there's no consumer for it yet (nothing shades triangles), so building it now means guessing at a shape a future feature might not actually want. Retain the raw factors (§2.3); defer the conversion.
- **Do not** give glTF rows in the unified object browser any editing UI, even a stub — per explicit direction, selecting one should show nothing. Don't build a placeholder "not yet supported" panel either; that's speculative UI for a feature that doesn't exist yet.
- **Do not** implement multi-core/tile-parallel rendering in this pass (§2.5) — out of the scope that was asked for ("background thread," singular).
- **Do not** let an in-flight render keep reading the live scene editor, camera, or `m_loadedScenes` (§2.4/§2.5) — the one-shot snapshot-at-click-time model keeps the thread-safety story simple and produces a well-defined, non-torn image.

## 8. Open questions / things to verify before starting

1. **Granularity of glTF rows in the unified browser**: this doc assumes one row per mesh-bearing `Node` (`MeshNode`) — matching "one object" the way the original project's one-row-per-sphere did. An alternative (one row per `GeoSurface`, i.e. per material-grouped sub-mesh) is finer-grained and might matter once triangle tracing/material editing actually arrives, but wasn't specified — confirm the per-`MeshNode` assumption is right before building the browser.
2. **Default resolution/sample-count/depth starting values** can't be predicted accurately during planning — pick something reasonable (roughly matching the original project's own defaults, since scene complexity is now comparable — a handful of spheres, no BVH needed) and adjust empirically once running.
3. **Exact ImGui window/menu placement**: the raytraced *output* is a `DisplayRegistry`-managed window (feature 1's "Windows" menu can show/hide it), but the "Raytracer Scene" editor panel and the render-settings panel are separate, always-present ImGui windows not managed by the registry — confirm this split (always-visible controls vs. registry-managed output) is what's wanted, versus also making the control panels toggleable from the "Windows" menu.
4. **Should the sphere list support deleting a sphere**, or only adding/editing (the original project's UI, per `docs/codebase-map.md` §6, doesn't show delete support) — not specified, worth a quick confirmation since it's a one-line addition to decide either way now versus after the panel is built.

## 9. As-built notes (implementation, 2026-08-23)

Everything below is what actually got built and verified, written after the fact. Where it disagrees with §§1–8, this section is right.

### 9.1 Answers to §8's open questions

1. **Browser granularity**: per mesh-bearing `Node`, as assumed. This needed one thing §3's file table didn't anticipate — `Node` had no name field, so `std::string name` was added to `Node` (`vk_types.h`) and populated in `loadGltf()`. That also surfaced a latent bug at the same site: `file.nodes[node.name.c_str()];` created a null map entry instead of assigning the node, so `LoadedGLTF::nodes` was a map of names to `nullptr`. Fixed to `= newNode`. Nothing read that map before, so the fix is inert today, but `docs/plans/scene-and-asset-management.md` and `docs/plans/simulation-domain.md` both walk scene nodes.
2. **Defaults**: 640×360, MSAA on (renamed anti-aliasing in §9.6), 8 samples, ray depth 8. That renders the default sphere scene in ~310 ms, so Cancel is barely exercised at defaults; raise samples or resolution to feel it. `RenderSettings` gained `width`/`height` fields, which the sibling did not have — it sized its render to its "Viewport" panel, and this feature has no equivalent panel to measure.
3. **Panel placement**: both control panels are toggleable from the "Windows" menu, alongside `background`/`Stats`/`ImGui Demo`, matching what `docs/plans/completed/imgui-display.md` §9.3 item 6 actually built. Each panel owns its own `bool` rather than adding two more members to `VulkanEngine`.
4. **Delete**: supported. "Add Sphere" / "Delete Sphere", the latter disabled unless a sphere row is selected.

### 9.2 Deviations from this spec, and why

1. **The snapshot deep-copies spheres.** §2.4 says `buildRaytraceScene()` "copies the *current* sphere list out of the editor". Copying the `std::vector<std::shared_ptr<sphere>>` would have copied pointers, leaving the worker thread reading the same `sphere` objects a slider drag mutates — a data race, and the exact opposite of §2.4's stated guarantee. Each sphere is copied by value (`std::make_shared<sphere>(*entry.object)`). The `material` each one points at is still shared, which is safe: no code path mutates a material after construction.
2. **The render loop writes rows top-down.** The sibling wrote row `j` from the bottom and corrected it at display time with `ImGui::Image(..., ImVec2(0,1), ImVec2(1,0))`. `DisplayRegistry` has no UV flip (per `docs/plans/completed/imgui-display.md` §6, deliberately), so the flip moved into the render loop: `v = (height - 1 - y) * pixelHeight`. Without this the image displays upside down.
3. **Replacing the output image needs deferred destruction, not just ordering.** §5 requires `unregisterImage()` before `destroyImage()`, which is necessary but not sufficient — the *view* can still be referenced by command buffers from earlier frames that have not finished. `RaytraceJob` queues the replaced `AllocatedImage` and destroys it `FRAME_OVERLAP + 1` frames later, mirroring `DisplayRegistry`'s own deferral for its descriptor sets. Verified in both directions (§9.3).
4. **`src/rt_random.h/.cpp` is a new file not in §3's table.** The ported materials and camera cannot work without the sibling's `Random`. Its `std::mt19937` is `thread_local` here rather than a single global static, since the worker thread and UI thread would otherwise share a mutable engine.
5. **`sphere::params()`'s slider ranges changed.** The sibling used a fixed 0.001–2 radius / −1–1 position range, which cannot represent its own default scene (the ground sphere is radius 100 at y = −100.5) — and an ImGui slider *clamps* on touch, so selecting the ground sphere and nudging the slider silently collapsed it. Now a logarithmic 0.001–200 radius slider and an unbounded `DragScalarN` for position.
6. **Additions for downstream features that assume they already exist**: `RaytraceSceneEditor::isDirty()`/`clearDirty()` (`scene-and-asset-management.md` §3 and `compute-pipeline-raytracing.md` §2.6 both reference "the existing dirty-flag") and `MaterialType type()` on `material` (needed to write a material to a flat tagged struct, for `compute-pipeline-raytracing.md` §2.6's `GpuMaterial` SSBO and for save/load). Neither has a consumer in this feature.
7. **`CAMERA_VERTICAL_FOV_DEGREES` (`vk_engine.h`)** replaces `updateScene()`'s hardcoded `glm::radians(70.f)`, so the raytrace frames the scene the same way the viewport it was aimed from does. The camera snapshot takes its up vector from the camera's own rotation matrix rather than assuming world up, which keeps framing correct when the camera is pitched near vertical.
8. **One sibling bug not carried over**: `Renderer::render()` passed `pixelWidth` as both the width and height of the MSAA jitter box (`Renderer.cpp:56`). Fixed here.
9. `RaytraceScene::hit()` is the ported `hittable_list::hit()` linear scan, living on `RaytraceScene` rather than on a separate `hittable_list` type — there is only ever one list, and it is not itself a `hittable`.

### 9.3 Verification actually performed

Offline harness over the ported core (`rt_hittable`/`rt_material`/`rt_random`, no Vulkan): 22 checks, all passing — head-on hit distance and hit point, normal orientation and `front_face` for rays starting outside *and* inside a sphere, `t_max` rejection, miss, nearest-of-two ordering independence, all four materials scattering, metal fuzz clamping, material type tags, RNG range and unit-ball containment, tangent-space orthonormality.

In-app, under validation layers (layer confirmed loaded via `VK_LOADER_DEBUG=layer`, then reverted to `false`):

| Test | Result |
|---|---|
| Default sphere scene, default camera, full render dumped to PPM | correct RTIAW image: sky gradient above, ground sphere below, glass/red/gold left-to-right as authored — orientation and handedness both right |
| glTF browser enumeration against `assets/structure.glb` | 476 mesh nodes, all named, `cpuVertices`/`cpuIndices` populated on every one |
| `buildRaytraceScene()` snapshot | 4 spheres, 1699 `RTMeshInstance`s, 1,063,849 triangles, camera captured at (0,0,5); snapshot sphere confirmed a distinct object from the editor's |
| Render → re-render (the unregister/re-register path) | second render registered, previous image queued not destroyed, queue drained by the retire frame |
| Cancel mid-render | stopped at 6% progress (one frame after the request); previous output left registered, as intended |
| Three renders + a cancel + clean shutdown | exit 0, **0 VUIDs** |
| Same, with deferred image destruction disabled | **2× `VUID-vkDestroyImageView-imageView-01026`** — the control proving both that the messenger reports and that §9.2 item 3 is load-bearing |

**Not verified — needs a human at the keyboard.** Everything above is programmatic. Nobody has visually confirmed that the "Raytracer Scene" and "Raytrace Render" panels look and feel right, that the "Raytraced Output" window shows the image correctly, that selecting a glTF row shows nothing without leaving stale sphere sliders behind, or that the app stays responsive during a render. Attempting to screenshot the window from a backgrounded launch does not work: GLFW reports the window not visible and the engine's existing minimize path parks the loop.

### 9.4 Known limitations left in place

- **The mesh scaffold has no consumer** (§2.3, §5) and is rebuilt from scratch on every Render — 1.06 M triangles transformed and copied per click for the `structure` scene, roughly doubling the cost of a fast render. Cheap relative to a slow one, and it is what makes `docs/plans/simulation-domain.md`'s first consumer straightforward, but it is real work done for nothing today. **Planned fix**: build it once per scene load as shared, immutable, object-space data — `docs/plans/scene-and-asset-management.md` §2.9.
- **`m_loadedNodes` is not browsable** — only `m_loadedScenes`, per §2.1. The engine's own test meshes (`Suzanne`, `Cube`) are drawn by `updateScene()` with transforms passed in at the call site rather than through `worldTransform`, so they have no world placement to report.
- **The raytraced image does not track the raster viewport's aspect ratio** — resolution is set explicitly in the panel, so a mismatched aspect crops or letterboxes the framing relative to what the viewport shows. *Small additive fix, not yet scheduled*: a "Match viewport" entry at the top of the resolution combo that uses the window's aspect ratio (`m_windowExtent`) at a chosen height. `RESOLUTION_PRESETS` and the combo in `RaytraceJob::drawControlPanel()` are the only places to touch, and `RenderSettings::width`/`height` stay the source of truth.
- **The render uses one core** (§2.5, deliberately). *Small additive speedup, not yet scheduled*: split the scanlines (or tiles) across `std::thread::hardware_concurrency()` workers inside `renderScene()`. Each worker writes disjoint rows of `m_pixels`, and the `thread_local` RNG (`src/rt_random.cpp`) already makes the sampling race-free. Progress becomes an atomic completed-row count, and the per-row cancel check is unchanged. One catch: **Fixed seed** (§9.6) seeds the render thread once, which stops being reproducible once rows land on threads in a non-deterministic order. Seed per row instead (e.g. `Random::seed(seed ^ hash(row))` at the start of each row).

### 9.5 Follow-on changes requested after the first pass

Three additions made in the same session, after the feature above was verified.

1. **"Render every frame"** (`RaytraceJob::m_renderEveryFrame`), ported from the sibling project's checkbox (`main.cpp:39-42`). There it re-ran a fully synchronous render on every UI frame; here a render is asynchronous, so the equivalent is to start the next one the moment the previous finishes, leaving an in-flight render to complete rather than restarting it. Pressing Cancel clears the flag as well as cancelling — otherwise the loop would immediately start another render and Cancel would look broken.
2. **Resolution is a preset list, not sliders** — `RESOLUTION_PRESETS` in `rt_types.h`, surfaced as an `ImGui::BeginCombo`. `RenderSettings::width`/`height` are still the source of truth (so the eventual save/load path has plain numbers to serialise); the selected index is UI state on `RaytraceJob`.
3. **Editor spheres are drawn in the raster viewport.** `makeUnitSphereMesh()` (`vk_engine.cpp`) builds one 24×32 UV sphere at the origin at init; `VulkanEngine::drawRaytraceSpheres()` emits one `RenderObject` per editor sphere each frame with `translate(center) * scale(radius)`. Three consequences worth recording:
   - **Colour comes from a per-frame material, not vertex colours.** `mesh.vert` computes `outColor = v.color.xyz * materialData.colorFactors.xyz`, so leaving the mesh's vertex colours white lets a per-sphere `colorFactors` carry the colour with one shared mesh. Each frame allocates one uniform buffer holding all spheres' `MaterialConstants` (indexed by offset, the same layout `loadGltf()` uses for a file's materials) plus one descriptor set per sphere from `getCurrentFrame().frameDescriptors`. `materialPreviewColor()` (`rt_material.cpp`) maps a raytracer material to that colour; a dielectric has no albedo of its own, so it gets a pale blue tint that reads as glass rather than as a white diffuse sphere.
   - **`updateScene()` had to move.** It was called at the top of `draw()`, *before* `getCurrentFrame().deletionQueue.flush()` and `frameDescriptors.clearPools()`. Now that it allocates from that frame slot, running it first meant the buffer and descriptor sets were reclaimed immediately after being created. It now runs straight after the pool reset. *(Corrected 2026-09-13: this originally claimed the `VK_ERROR_OUT_OF_DATE_KHR` early return skips the scene update. It doesn't — `updateScene()` runs before the swapchain acquire, so it still runs and its allocations are simply released at that slot's next flush.)* The real cost of the move is that CPU scene preparation no longer overlaps the GPU finishing the previous frame. The planned fix (persistent per-slot preview buffers rewritten on scene change, then moving `updateScene()` back) is `docs/plans/compute-pipeline-raytracing.md` §2.9.
   - **`shaders/mesh.frag` now normalizes the interpolated normal.** `mesh.vert` transforms the normal by the full model matrix, so object scale ends up in its length, and `dot(inNormal, sunlightDirection)` was scaled by it. The default scene's ground sphere has radius 100, which lit it 100× too brightly. This is a pre-existing bug in the raster path — any scaled glTF node hits it — not one this change introduced.

**Measured cost of "Render every frame"** (391 frames, `structure` scene loaded, vsync at 60 Hz):

| | median | p99 | max | frames over 25 ms |
|---|---|---|---|---|
| loop off | 16.7 ms | 17.2 ms | 17.2 ms | 0 |
| loop on (18 renders) | 16.6 ms | 59.5 ms | 64.2 ms | **18** |

Exactly one dropped-frame hitch per render start. The cause is §9.4's first bullet: `buildRaytraceScene()` spends **46 ms** rebuilding the inert 1699-instance / 1.06 M-triangle mesh scaffold on the main thread, and loop mode pays that roughly three times a second instead of once per click. The raytrace itself is not the problem — it is on a worker thread and costs nothing here. Left as-is pending a decision; the fix is to build the scaffold once and share it immutably (e.g. `std::shared_ptr<const std::vector<RTMeshInstance>>`) rather than per snapshot, invalidated when the loaded scenes change.

### 9.6 Review fixes (2026-09-13)

A side-by-side review against the sibling project turned up these, all fixed in the same session. Where earlier parts of §9 describe the old shape, this section wins.

1. **Data race on the last-render time.** The worker wrote `m_lastRenderMs` while the UI read it every frame, despite `rt_job.h` claiming the main thread only reads worker-written fields after `join()`. The worker now writes `m_workerRenderMs`, which `update()` copies into `m_lastRenderMs` after the join — so the claim is true again. "Last render" now means the last render that *completed*; a cancelled one no longer overwrites it.
2. **Materials are editable.** Before, every added sphere was a grey `lambertian` and only the four seeded spheres had other materials. The sphere panel now has a material-type combo, plus per-type controls through a new virtual `bool material::params()`: albedo, metal fuzz, phong smoothness, dielectric index of refraction. Changing type keeps the albedo where both types have one (`materialAlbedo()`, `makeMaterial()`, `materialTypeName()`, `MATERIAL_TYPES` in `rt_material.h`). Material edits mark the editor dirty. `hittable::params()` also now returns whether anything changed, replacing the editor's before/after comparison.
3. **The snapshot clones materials** (new virtual `material::clone()`). §9.2 item 1's "materials are shared, which is safe because nothing mutates them" stopped being true the moment item 2 made them editable.
4. **`hit_record::mat_ptr` is a `const material*`**, not a `shared_ptr`. The old one paid two atomic refcount updates per intersection candidate in the innermost loop. It is safe because the snapshot owns every sphere and material for the whole render.
5. **`MSAA`/`msaaSamples` renamed to `antialiasing`/`samplesPerPixel`**, with UI labels "Anti-aliasing" / "Samples per pixel". It's per-pixel supersampling, and the old name collided with real Vulkan MSAA (`MSAASamples` in `initIMGUI()`). Done before save/load (`docs/plans/scene-and-asset-management.md`) can freeze the names into a file format.
6. **Output is linear HDR, tonemapped on the GPU by a shared pass.** The worker writes linear radiance (`std::vector<glm::vec4>`, averaged, no gamma) instead of packed RGBA8. `publishOutput()` uploads it as `R32G32B32A32_SFLOAT` and runs the new **`TonemapPass`** (`src/vk_tonemap.h/.cpp`, `shaders/tonemap.comp`: scale, NaN→0, clamp, gamma 2 via `sqrt`) in one `immediateSubmit()` into the registered `R8G8B8A8_UNORM` display image, then destroys the linear image. `write_color()` is gone. `docs/plans/compute-pipeline-raytracing.md` §2.7 now uses the same pass, so the two raytracers' outputs will be directly comparable. Two supporting changes:
   - `createImage(void* data, ...)` sizes uploads per format — pulled forward from `docs/plans/scene-and-asset-management.md` §2.4.
   - The linear image needs `SAMPLED` as well as `STORAGE` usage, because `createImage()` always leaves uploads in `SHADER_READ_ONLY_OPTIMAL`.
7. **Robustness and correctness:**
   - `lambertian` scatters around `normal + Random::random_unit_vector()`, a true Lambertian distribution (the sibling's in-ball point only approximates one), and falls back to the normal when the sum is near zero. The unit-vector generator was the sibling's misnamed `random_in_unit_sphere()`, renamed now that it has a caller.
   - Non-finite samples are dropped before averaging, and the tonemap shader zeroes NaNs as a second line of defence.
   - `getTangentSpace` uses `std::abs`.
   - New **Fixed seed** option (`RenderSettings::useFixedSeed`/`seed`, `Random::seed()`), so renders can be reproduced exactly.

**Verification** — clean build, then a temporary in-app self-test hook (since removed), run under validation layers from `bin/` (the engine's root path is `../`):

| Test | Result |
|---|---|
| Two renders, 320×180, 16 spp, fixed seed 7 | identical linear buffers — 0 of 57,600 pixels differ |
| GPU `TonemapPass` output read back vs a CPU `sqrt(clamp(x))` reference | max difference 1/255 on any channel (rounding), alpha 255 everywhere |
| Tonemapped image, eyeballed | correct: sky above, ground below, glass / red / gold left to right |
| Snapshot material pointers vs the editor's | all distinct |
| Validation, first run | **4× `VUID-VkImageMemoryBarrier2-oldLayout-01211`** — the linear image lacked `SAMPLED` usage; fixed |
| Validation, after the fix; renders + tonemap + shutdown | exit 0, **0 VUIDs** |
| True-Lambertian vs in-ball sampling, same scene and seed | 1.66 s vs 1.81 s — no slowdown |

Timings are from a `CMAKE_BUILD_TYPE=Debug` (`-O0`) build and aren't comparable with §9.1's ~310 ms figure.

**Not verified — needs a human at the keyboard**: the material combo and per-type controls, the Anti-aliasing / Samples per pixel / Fixed seed controls, and that editing a material updates the raster preview sphere's colour. Renders also look slightly different from the sibling project's, because of the true-Lambertian change.

