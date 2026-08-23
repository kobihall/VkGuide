# VkGuide Feature Plans — Overview

Planning-only, as of 2026-08-23. Nothing in `src/`/`shaders/` has been implemented yet — everything under `docs/` is a specification for a future implementation session. The seven docs below were produced through iterative, back-and-forth design discussion (not a single pass), refining and correcting each other as later docs surfaced constraints earlier ones hadn't accounted for.

## Read first

`docs/codebase-map.md` — a factual snapshot of the current codebase (`src/`, `shaders/`, and the sibling `RayTracingInAWeekend` project). Every plan doc assumes this context; read it before starting any feature below.

## The seven docs, in required implementation order

1. **`docs/plans/imgui-display.md`** — Foundational. Vendors a docking-branch ImGui, adds a generic `DisplayRegistry` for showing arbitrary GPU images in floating/dockable windows, and fixes a real input-handling gap (camera mouse-look/WASD had no concept of ImGui wanting the input instead). No dependencies.
2. **`docs/plans/raytracing-in-a-weekend.md`** — Ports the sibling CPU raytracer's sphere scene, materials, and background-threaded render loop into VkGuide, with a live scene-editor panel (`RaytraceSceneEditor`) and output shown via `DisplayRegistry`. Depends on (1).
3. **`docs/plans/scene-and-asset-management.md`** — Runtime glTF scene loading, HDR image loading, sphere-scene save/load (as minimal valid `.gltf` files using the `extras` field), and a shared 3D gizmo (`TransformGizmo`, via a new ImGuizmo dependency) for direct-manipulation editing, wired into `RaytraceSceneEditor` first. Depends on (1) and (2) — needs `RaytraceSceneEditor` to already exist.
4. **`docs/plans/compute-pipeline-general.md`** — Generalizes VkGuide's existing hardcoded single-image/fixed-push-constant compute pattern into a reusable `ComputePass` abstraction (arbitrary bindings, arbitrary push-constant size). No dependencies — could in principle be built any time, but is sequenced here since nothing needs it before this point.
5. **`docs/plans/compute-pipeline-raytracing.md`** — A GPU compute-shader path tracer (ray-gen/hit-detect/hit-shade as swappable stages) that re-expresses (2)'s already-working CPU sphere/material logic on the GPU, with progressive per-frame accumulation. Depends on (2) and (4).
6. **`docs/plans/simulation-domain.md`** — A placeable `SimulationPlane` scene object; derives 2D boundary geometry from wherever the loaded scene's meshes intersect it; a COMSOL-style panel for assigning boundary-condition types independently of the scene data. PDE-agnostic — produces a boundary mask, simulates nothing itself. Depends on (2) (reuses CPU mesh data added there) and (3) (gizmo, scene-load hooks).
7. **`docs/plans/wave-simulation.md`** — A 2D wave-equation solver running on (6)'s grid, with Start/Pause/Stop controls and live mouse injection. Depends on (6), (4), and (1).

## Dependency graph

```
(1) imgui-display
 ├─→ (2) raytracing-in-a-weekend
 │    ├─→ (3) scene-and-asset-management ──┐
 │    ├─→ (5) compute-pipeline-raytracing  │
 │    └─→ (6) simulation-domain ←──────────┘
 │                                    ↑
 (4) compute-pipeline-general ────────┼──→ (5)
                                       └──→ (7) wave-simulation
 (1) ───────────────────────────────────────┘
 (6) ─────────────────────────────────────→ (7)
```

**If you were handed these docs in a different order, check it against this one before starting** — in particular, (2) must precede (3): `scene-and-asset-management.md` adds save/load to `RaytraceSceneEditor`, which `raytracing-in-a-weekend.md` defines. Building (3) first would mean extending a type that doesn't exist yet.

## How to use these docs

- Each doc's own §0 lists its hard prerequisites and is written to be handed to a **fresh Claude Code session with no other context** as the sole input for implementing that one feature — self-contained, referencing `docs/codebase-map.md` for shared background rather than repeating it.
- Fully implement and verify each doc (per its own §4 implementation order / acceptance criteria) before starting the next — later docs assume earlier ones' types and APIs already exist and work. Don't parallelize across docs that have a dependency edge between them.
- Re-verify line-number references against current source before trusting them literally — the docs were written against one snapshot of the codebase, and both the code and the docs may have drifted since.
- Each doc's §8 ("Open questions") lists things deliberately left for implementation time — these aren't blockers, just flagged decisions to make as you go, usually because they're more naturally answered once the adjacent code actually exists.

## Cross-cutting notes (from a consistency audit across all seven docs + `codebase-map.md`)

- **`src/vk_engine.h`/`.cpp` are cumulative touch points** — every one of the seven docs adds to them (a new member, a new call in `run()`/`draw()`, or both). Expect these two files to keep growing across features; no single doc "owns" them.
- **`src/camera.h`/`.cpp` are touched more subtly than a single doc's file table might suggest**: (1) gates WASD and mouse-look at the *callback* level in `vk_engine.cpp` (not inside `Camera` itself — `Camera` stays unaware of capture state), (5) is the first to actually add a field to `Camera` (`m_hasChanged`), and (3) extends (1)'s capture-mode check (again in `vk_engine.cpp`, not `Camera`) to also arbitrate against the gizmo. If you're implementing one of these and `camera.h/.cpp` looks untouched by your doc specifically, check whether the change actually belongs in `vk_engine.cpp` instead.
- **CPU material → GPU material conversion is a small, slightly under-specified step**: `docs/plans/raytracing-in-a-weekend.md`'s materials are separate C++ classes reached via virtual dispatch (`lambertian`/`metal`/`phong`/`dielectric`); `docs/plans/compute-pipeline-raytracing.md` §2.6 uploads a single flat tagged-union `GpuMaterial{type, ...}` SSBO from them but doesn't spell out the per-material-type conversion step. Not a contradiction, just something to write when you get there.
- **A few stale cross-references were found and corrected during this audit** (mismatched file attributions for camera-input gating and the simulation plane's gizmo button, and two facts in `codebase-map.md` that had since been superseded by later findings — the ImGui docking-branch question and a real bug found in `createImage()`'s upload-size calculation). If you're reading a doc and something it says about another doc seems off, the other doc is more likely to be right — but check both against current source regardless.
