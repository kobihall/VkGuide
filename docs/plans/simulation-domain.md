# Feature: Simulation Domain (Plane + Scene-Derived Boundary Conditions)

## 0. How to use this doc

Standalone implementation spec — treat it as the only context you have. For exhaustive facts about the current codebase state, read `docs/codebase-map.md` first. This doc is PDE-agnostic — it builds the *domain* (a placeable 2D plane, the boundary geometry derived from where it cuts through the loaded scene, and a UI for assigning boundary-condition types to that geometry) that any future PDE solver plugs into. It does not itself simulate anything. `docs/plans/wave-simulation.md` is the first (and, as of this writing, only) consumer, building the actual wave-equation solver on top of what this doc produces.

Hard prerequisites, all must be implemented first:
- `docs/plans/raytracing-in-a-weekend.md` — this doc reuses `MeshAsset::cpuVertices`/`cpuIndices` (added there specifically as forward-compatible, until-now-unused scaffolding for a future mesh-intersection feature — this is that feature) and the `m_loadedScenes`/`Node::worldTransform` scene-walking pattern already established there.
- `docs/plans/scene-and-asset-management.md` — this doc's `SimulationPlane` is gizmo-editable via that doc's `TransformGizmo` (§2.8 there), and reacts to runtime glTF scene changes via that doc's scene-loading (§2.6 there).

## 1. Feature goal

Let the user place and move a 2D plane within the 3D raster scene; automatically derive 2D boundary geometry from wherever the currently-loaded glTF scene's meshes intersect that plane; and let the user independently assign a boundary-condition type (e.g. reflective, absorbing) to each intersected region through its own settings UI — without writing anything back into the scene/glTF data itself, mirroring how a tool like COMSOL separates imported geometry from the physics/boundary settings applied to selected subsets of it.

## 2. Architecture decisions made and WHY

### 2.1 `SimulationPlane`: a placeable domain object, not scene-embedded metadata

A `SimulationPlane` is a small object VkGuide owns directly (not glTF data, not something loaded/saved through `docs/plans/scene-and-asset-management.md`'s sphere-scene mechanism, at least not in this pass — see §8): an origin, an orientation (two in-plane basis vectors, derived from a `glm::quat` so it composes cleanly with `TransformGizmo`'s `glm::mat4` target), a 2D extent (width × height in world units), and a fixed grid resolution (cell count per axis — an explicit setting, matching `docs/plans/imgui-display.md` §2.5's fixed-resolution convention, not derived from any window/panel size). It's placed and moved two ways, both editing the same underlying transform: numeric sliders (mirroring `RaytraceSceneEditor`'s existing position pattern) and the shared gizmo (`docs/plans/scene-and-asset-management.md` §2.8) via an "Edit Transform" button, converting the plane's origin/orientation/extent to/from a `glm::mat4` the same way that doc's sphere adapter does.

**Designed with room for a future 3D "simulation volume" without building it now**: the placement/transform/gizmo machinery here doesn't assume "plane" any more deeply than it has to — origin + orientation + extent generalizes naturally to a box (origin + orientation + 3D extent) later. What's genuinely 2D-specific — the grid indexing, the intersection math, the boundary-mask layout — stays concretely 2D, not artificially generalized to N dimensions now for a volume feature that doesn't exist yet and isn't being designed here. This is a deliberate, narrow interpretation of "generality": keep the parts that cost nothing to keep flexible flexible, don't speculatively build the parts that would.

### 2.2 Boundary geometry comes from actual triangle-plane intersection against the loaded scene, computed on change, not every frame

Every loaded mesh instance (walking `VulkanEngine::m_loadedScenes` → each `LoadedGLTF::topNodes` → `MeshNode`s, using each one's `worldTransform` — the exact same tree-walk pattern `docs/plans/raytracing-in-a-weekend.md`'s `buildRaytraceScene()`-equivalent already established for a different purpose) has its triangles (`MeshAsset::cpuVertices`/`cpuIndices`, transformed to world space by the node's `worldTransform`) tested against the plane: classify each triangle vertex by signed distance to the plane, and where a triangle's vertices don't all share the same sign, compute the edge-crossing segment. Collected per source `MeshNode` (matching the per-node granularity `docs/plans/raytracing-in-a-weekend.md`'s object browser already uses), these segments are projected into the plane's local 2D coordinate space (using its basis vectors) to become that mesh instance's **boundary region**.

**This recomputes only when something actually changes** — the plane's transform (moved via slider or gizmo) or the loaded scene (`docs/plans/scene-and-asset-management.md` §2.6) — not every frame. This is a real, potentially expensive CPU computation (every triangle of every loaded mesh, tested against the plane) that has no reason to re-run 60+ times a second when nothing that affects its result has changed; it runs synchronously on the main thread when triggered, the same "infrequent triggered recompute, not continuous work" shape `docs/plans/compute-pipeline-raytracing.md`'s `Camera::m_hasChanged`-driven GPU buffer re-upload already established for a similar reason.

### 2.3 Intersected regions are filled, not just outlined

A mesh crossing the plane represents a **solid** obstacle (e.g. a rock sitting in a pond, for a water-wave analogy) — the boundary should be the filled interior of each region's outline, not just the outline curve itself, or a solid object would incorrectly let the simulated field pass straight through everywhere except a thin boundary shell. Each region's segments are rasterized onto the grid and flood-filled from the outline inward, marking interior cells as part of that boundary region.

**Only works cleanly for closed loops** — a well-formed (closed/manifold) mesh crossing the plane produces a closed 2D cross-section, which flood-fill handles correctly. An open or non-manifold mesh could produce a boundary curve that doesn't close, where "interior" is ambiguous — see §5 for the fallback behavior.

### 2.4 Boundary-condition assignment is simulation-local state, never written back into scene/glTF data

After intersection recompute, each region gets a stable identity (its source `MeshNode`'s name, matching how `docs/plans/raytracing-in-a-weekend.md`'s object browser already identifies glTF objects) and a **default** boundary-condition type (e.g. reflective). A dedicated "Simulation Boundary Conditions" panel — a list of currently-intersected regions, same list-driven UI shape as `RaytraceSceneEditor`'s object browser — lets the user select a region and change its assigned type via a dropdown. This assignment map (region name → boundary-condition type + parameters) lives entirely on the `SimulationPlane` object in memory; it is never written into the glTF scene's `extras` or any other scene data, and isn't affected by `docs/plans/scene-and-asset-management.md`'s save/load mechanism (which only covers the sphere scene, not this). This directly matches the explicit direction given during planning: geometry is geometry, and which subset of it plays which role in a given simulation is the simulation's own concern, decided independently, the way COMSOL separates imported CAD geometry from the physics settings applied to selected faces/edges of it.

### 2.5 Output contract: a per-cell boundary-mask buffer, the PDE-agnostic handoff point

This doc's concrete deliverable to any PDE solver (starting with `docs/plans/wave-simulation.md`) is a CPU-side `std::vector<uint8_t> boundaryMask` sized to the plane's grid resolution, where each cell encodes an interior/boundary-type value (e.g. `0` = interior/simulate normally, `1`/`2`/... = one value per assigned boundary-condition type). `SimulationPlane` owns and rebuilds this CPU-side mask whenever §2.2's recompute runs or §2.4's assignments change; it does **not** own any GPU buffer or upload anything itself — mirroring the established pattern where a CPU-side editor/domain object owns state and whichever GPU-consuming feature reads it does its own upload on its own schedule (`RaytraceSceneEditor` → `docs/plans/compute-pipeline-raytracing.md`'s `GpuSphere` upload is the direct precedent). This keeps this doc genuinely reusable by more than one future PDE solver without committing to any particular GPU resource layout now.

## 3. Exact files to create/modify

| File | Role |
|---|---|
| `src/sim_domain.h` (new) | Declares `SimulationPlane`: transform state (gizmo/slider-editable, §2.1), the boundary-region list, the assignment map (§2.4), the CPU `boundaryMask` (§2.5), `recomputeIntersections(VulkanEngine*)` (§2.2/§2.3), a placement panel (sliders + "Edit Transform" button, wired to `m_transformGizmo.beginEditing(...)` via a plane↔matrix adapter, following the exact pattern `docs/plans/scene-and-asset-management.md` §2.8/§3 established for spheres — **this button lives on `SimulationPlane`'s own panel, not on `RaytraceSceneEditor`**, since the plane is a distinct object from the sphere scene), and the "Simulation Boundary Conditions" panel draw function. |
| `src/sim_domain.cpp` (new) | Implements the above: triangle-plane intersection, per-region segment collection, projection into plane-local 2D space, rasterization + flood-fill (§2.3), the assignment UI, the plane↔matrix gizmo adapter. |
| `src/vk_engine.h` | Add `SimulationPlane m_simulationPlane;` (or a `std::vector`/`std::optional` if "no plane placed yet" needs representing — left to implementation, see §8) member. |
| `src/vk_engine.cpp` | Wire `SimulationPlane`'s panels into `run()`; wire `recomputeIntersections()` to fire on the plane's own transform-change (mirroring `Camera::m_hasChanged`'s dirty-flag shape, `docs/plans/compute-pipeline-raytracing.md` §2.4) and on scene-load events (`docs/plans/scene-and-asset-management.md` §2.6). |
| `src/CMakeLists.txt` | Add `sim_domain.h`/`sim_domain.cpp` to the explicit source list. |

## 4. Implementation order and dependencies

1. **`SimulationPlane` transform state + slider UI**, no gizmo yet. Verify: a plane can be created, positioned via sliders, and its transform is visible/sane (e.g. print/inspect its basis vectors).
2. **Wire the gizmo** (`docs/plans/scene-and-asset-management.md` §2.8) — "Edit Transform" button, matrix adapter. Verify dragging the gizmo moves the plane the same way editing the sliders does.
3. **Triangle-plane intersection** (§2.2), unfilled (outline only) first. Verify against a single known test mesh at a known plane position — hand-compute the expected intersection for something simple (e.g. a plane through the middle of a unit cube should produce a square outline) and confirm the code matches.
4. **Flood-fill / region filling** (§2.3). Verify visually — export or otherwise inspect the resulting mask and confirm a solid mesh crossing the plane produces a filled region, not just an outline.
5. **Region identity + the boundary-condition assignment panel** (§2.4). Verify: intersecting two different meshes with the plane produces two independently-selectable, independently-assignable regions.
6. **The `boundaryMask` buffer** (§2.5) — the final CPU-side output this doc exists to produce. Verify its contents directly (dump/inspect) before `docs/plans/wave-simulation.md` ever tries to consume it — a bug here is much easier to find in isolation than after a PDE solver is reading garbage boundary data.
7. **Recompute triggers**: wire the dirty-flag-driven recompute (§2.2) to both plane-transform changes and scene-load events. Verify: moving the plane or loading a different glTF scene correctly triggers a recompute, and that idle frames (nothing changed) do not.

## 5. Edge cases / traps identified during planning

- **Non-closed intersection loops** (§2.3): an open or non-manifold mesh crossing the plane won't produce a cleanly closeable outline. Fall back to outline-only (unfilled) marking for a region whose segments don't form a closed loop, rather than guessing at a fill — an incorrect flood-fill (leaking to fill the entire grid, for instance) is a much worse failure mode than an under-filled boundary.
- **Recompute cost scales with total scene triangle count**, not just what's near the plane — a large loaded scene means testing every triangle against the plane on every recompute, even ones far away. Acceptable given recomputes are infrequent/triggered (§2.2), but worth being aware of if a future scene turns out to be large enough to make this recompute noticeably slow; a spatial pre-filter (e.g. skip a mesh instance entirely if its bounding volume doesn't cross the plane) is a cheap, purely additive optimization if that happens, not required now.
- **Plane transform changes mid-recompute**: since recompute is triggered and synchronous (§2.2), there's no concurrent-mutation hazard the way `docs/plans/compute-pipeline-raytracing.md`'s background-threaded raytrace has to guard against — this work all happens on the main thread in response to a discrete trigger, not spread across frames or threads.
- **Region identity stability across recomputes**: if a scene reload changes which mesh nodes exist, previously-assigned boundary-condition types (§2.4, keyed by region/node name) for now-nonexistent regions should be dropped, not silently retained as orphaned state; new regions should get the default type, not inherit anything from stale entries with a coincidentally-matching name.
- **No visual feedback for "where is the plane" beyond the gizmo itself**: there's no persistent wireframe/outline rendering of the plane in the 3D viewport in this doc's scope (VkGuide has no debug-line-drawing capability anywhere today — confirmed absent from `docs/codebase-map.md`, would be new rendering infrastructure, not something to fold into this feature casually). The gizmo (while actively editing) and the boundary-mask preview (indirect, via whatever displays it — see §8) are the only placement feedback for now.

## 6. Code patterns from the existing codebase to follow

- **Scene tree walk**: `LoadedGLTF::Draw()` (`vk_loader.cpp:483-489`) / `MeshNode::Draw()` (`vk_engine.cpp:1338`) — same `topNodes` walk + `worldTransform` accumulation pattern, collecting triangles for intersection testing instead of `RenderObject`s.
- **CPU mesh data source**: `MeshAsset::cpuVertices`/`cpuIndices` (`docs/plans/raytracing-in-a-weekend.md` §2.3) — this is the first real consumer of that previously-unused scaffolding.
- **Dirty-flag-triggered, infrequent recompute**: `Camera::m_hasChanged` (`docs/plans/compute-pipeline-raytracing.md` §2.4) is the direct precedent for "recompute only when something changed, not every frame."
- **List-driven object browser + per-item settings**: `RaytraceSceneEditor`'s unified sphere/glTF-node browser (`docs/plans/raytracing-in-a-weekend.md` §2.1/§2.4) is the shape the boundary-condition-assignment panel should follow.
- **Gizmo wiring**: `docs/plans/scene-and-asset-management.md` §2.8's sphere "Edit Transform" button is the exact pattern to replicate for the plane, including the matrix-conversion-adapter approach.
- **CPU-owns-state, GPU-consumer-does-its-own-upload separation**: `RaytraceSceneEditor` → `docs/plans/compute-pipeline-raytracing.md`'s `GpuSphere` buffer is the precedent for §2.5's `boundaryMask` handoff.

## 7. What NOT to do (alternatives rejected and why)

- **Do not** store boundary-condition assignments in the glTF scene's `extras` or any other scene-persisted location (§2.4) — explicit direction was that this stays simulation-local, decided independently of the geometry, the way COMSOL separates imported geometry from applied physics settings.
- **Do not** build a full abstract `SimulationDomain`/`SimulationVolume` class hierarchy now (§2.1) — "generality" here means not hardcoding plane-only assumptions into the parts that don't need them (placement/transform), not building unused abstraction for a 3D volume feature that isn't designed yet.
- **Do not** recompute intersections every frame (§2.2) — this is triggered, infrequent, synchronous work, not a per-frame cost.
- **Do not** mark intersected regions as outline-only by default (§2.3) — filled regions are the physically sensible default for solid obstacles; outline-only is strictly a fallback for non-closeable geometry, not the primary behavior.
- **Do not** have this doc own or upload any GPU resource for the boundary mask (§2.5) — that's each PDE solver's own concern, on its own schedule; this doc's output is CPU-side data only.
- **Do not** add debug wireframe/line rendering for the plane in this pass (§5) — real new rendering infrastructure that doesn't exist anywhere in the codebase yet; out of scope here.

## 8. Open questions / things to verify before starting

1. **Can more than one `SimulationPlane` exist at once**, or is this a single-instance-only feature for now? Not specified during planning — a single `SimulationPlane` member (not a collection) is the simpler default assumed in §3, but multiple simultaneous planes (e.g. for comparing different cross-sections) wasn't explicitly ruled out either.
2. **Should `SimulationPlane`'s placement and boundary-condition assignments be saveable**, extending `docs/plans/scene-and-asset-management.md`'s save/load mechanism (§2.1 there) to cover this too? Not scoped into either doc currently — worth a direct check-in, since the save/load infrastructure already exists and extending it might be cheap once both features are built, but wasn't asked for explicitly.
3. **How should the boundary mask actually be inspected/previewed** before `docs/plans/wave-simulation.md` exists to display it meaningfully (§4 step 6, §5)? A minimal debug visualization (even just dumping it to a log or a temporary file) is enough to verify correctness during this doc's own implementation; a real in-engine preview naturally falls out once a `DisplayRegistry`-shown consumer exists.
4. **Exact set of boundary-condition types to support** — this doc assumes at least "reflective" and "absorbing" exist as concepts (matching `docs/plans/wave-simulation.md`'s needs) but the full enum and any per-type parameters are more naturally finalized together with that doc, not decided unilaterally here.
