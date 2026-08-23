# Feature: Raytracing in a Weekend (CPU port)

## 0. How to use this doc

Standalone implementation spec — treat it as the only context you have. For exhaustive facts about the current codebase state, read `docs/codebase-map.md` first (§6 covers the source project this feature ports from in detail). This feature depends on `docs/plans/imgui-display.md` (the `DisplayRegistry` it introduces) being implemented first — read that doc too, since this one uses its API directly. Before writing code, re-verify any line numbers cited here against current source.

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

Raytraced output uses `VK_FORMAT_R8G8B8A8_UNORM` (matching the sibling project's proven `Image` RGBA path, `docs/codebase-map.md` §6) rather than VkGuide's own HDR `m_drawImage` format — no need to match it, since this feature's output lives in its own independent window via `DisplayRegistry` (feature 1), not composited into the raster HDR pipeline. On render completion (checked once per frame, main thread only): upload the finished pixel buffer via the existing `VulkanEngine::createImage(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped)` (`vk_engine.h:229`, same staging-buffer-upload pattern already used by `load_image()` in `vk_loader.cpp:426` — pass `VK_IMAGE_USAGE_SAMPLED_BIT` as that call site already does), then register (first render) or re-register (subsequent renders — per `docs/plans/imgui-display.md` §5, unregister + destroy the old image before registering the new one, since the `VkImageView` handle changes each time) with the feature-1 `DisplayRegistry` under a fixed name like `"Raytraced Output"`.

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

Depends on `docs/plans/imgui-display.md` being implemented first (`DisplayRegistry` is used directly in §2.6/§3).

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
- **Re-render cleanup order**: `DisplayRegistry::unregisterImage()` must happen *before* `VulkanEngine::destroyImage()` on the previous raytraced output (per `docs/plans/imgui-display.md` §2.4/§5) — reversing that order leaves ImGui holding a descriptor pointing at a destroyed image.
- **Object-browser selection state**: when the unified list's selection changes from a sphere to a glTF row (or vice versa), make sure whatever ImGui state drives the sphere's `params()` widgets (e.g. slider IDs) doesn't leak/persist incorrectly across the switch — straightforward to get right, easy to get subtly wrong (e.g. showing stale slider values from the previously-selected sphere for one frame).
- **`MeshAsset`'s new CPU vectors add permanent RAM cost** proportional to total loaded mesh data (§2.3) — judged negligible at this project's scale.

## 6. Code patterns from the existing codebase to follow

- **Sphere hittable, materials, render loop shape**: `docs/codebase-map.md` §6 is the direct porting source for `rt_hittable.h/.cpp` (sphere only), `rt_material.h/.cpp`, and `rt_job.h/.cpp`'s render loop and settings panel.
- **Object-list + per-object dispatch UI**: the sibling project's `hittable_list::IDs` + `ImGui::ListBox` + `object->params()` dispatch (`docs/codebase-map.md` §6, `sphere::params()`, `main.cpp:56`) is the direct precedent for `rt_scene_editor`'s unified browser — extend the same dispatch pattern to a second, non-editable "kind" of row (glTF objects) rather than inventing a new UI pattern.
- **Scene traversal for the glTF side of the browser and the triangle scaffold**: `LoadedGLTF::Draw()` (`vk_loader.cpp:483-489`) and `MeshNode::Draw()` (`vk_engine.cpp:1338`) show the existing pattern for walking `topNodes` and accumulating world transforms — reuse the same walk, just collecting node names (for the browser) or triangles (for `buildRaytraceScene()`) instead of `RenderObject`s.
- **GPU image upload**: `load_image()` (`vk_loader.cpp:403-481`) is the exact existing precedent for "CPU pixel buffer → `engine->createImage(data, ...)` → done" that §2.6's completion handler should follow, including the `VK_IMAGE_USAGE_SAMPLED_BIT` usage flag.
- **`DisplayRegistry` usage**: follow `docs/plans/imgui-display.md` exactly for how `RaytraceJob::update()` registers/re-registers its output image.

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
