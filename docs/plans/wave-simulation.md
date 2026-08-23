# Feature: Wave Equation Simulation

## 0. How to use this doc

Standalone implementation spec — treat it as the only context you have. For exhaustive facts about the current codebase state, read `docs/codebase-map.md` first. Hard prerequisites, all must be implemented first:

- `docs/plans/simulation-domain.md` — this doc's compute shaders operate on that doc's `SimulationPlane` grid resolution and consume its `boundaryMask` output (§2.5 there) directly; it does not re-derive boundary geometry itself.
- `docs/plans/compute-pipeline-general.md` — every compute dispatch here is a `ComputePass` (§2.1 there), reusing that framework rather than hand-rolling pipeline/descriptor setup.
- `docs/plans/imgui-display.md` — the simulation's output image is shown via `DisplayRegistry`, and mouse injection (§2.6 below) uses that doc's optional click/drag callback (§2.8 there) added specifically to support this.

## 1. Feature goal

Solve the 2D scalar wave equation on `SimulationPlane`'s grid, respecting the boundary conditions assigned there, running continuously once explicitly started (auto-stepping every engine frame, not a single blocking solve) — with Start/Pause/Stop controls and live mouse injection of new wave sources while it runs — displayed as a colorized image through the feature-1 display registry.

## 2. Architecture decisions made and WHY

### 2.1 Three role-rotating grid buffers, not a copy-based ping-pong

The explicit finite-difference wave equation (`u_next[i,j] = 2·u_curr[i,j] − u_prev[i,j] + (c·dt/dx)²·(u_curr[i+1,j]+u_curr[i−1,j]+u_curr[i,j+1]+u_curr[i,j−1] − 4·u_curr[i,j])`) needs both the previous *and* current timestep to compute the next one — unlike a simpler 1st-order-in-time PDE (e.g. diffusion), a straightforward two-buffer ping-pong isn't enough. Three `AllocatedImage` grids (`rgba32f` won't be used here — a single-channel float format is sufficient and cheaper; see §8) are allocated once at `SimulationPlane`'s resolution, and which buffer plays which role (`prev`/`curr`/`next`) rotates by index each step (`role = (role + 1) % 3`) rather than copying data between fixed buffers — no data movement, just changing which buffer is bound to which descriptor slot at dispatch time.

### 2.2 Boundary conditions read `SimulationPlane`'s CPU mask, uploaded once per change — not recomputed here

The "step" `ComputePass` (§2.3) binds a `boundaryType` storage buffer alongside the three grid buffers. This is populated by uploading `SimulationPlane::boundaryMask` (`docs/plans/simulation-domain.md` §2.5) via `VulkanEngine::createBuffer()`, re-uploaded only when that CPU-side mask actually changes (the same domain doc's own recompute-trigger, not a new dirty-flag invented here) — this feature never derives boundary geometry itself, it only consumes the finished mask. In the step shader: a cell marked "reflective" copies its update from the nearest interior neighbor (zero-gradient/Neumann); a cell marked "absorbing" is forced to `0` each step (a simple, if not artifact-free, Dirichlet-style absorbing boundary — see §7 for the more accurate alternative deliberately not built now); an "interior" cell runs the normal update formula from §2.1.

### 2.3 Two compute passes per step (plus a third, conditional one for injection), all via `ComputePass`

- **`step`**: reads `prev`/`curr` + `boundaryType`, writes `next` (§2.1/§2.2). Dispatched once per engine frame while running (§2.4).
- **`visualize`**: reads `curr` (the just-completed step's result, after role rotation), maps the scalar wave height to a color (a simple diverging colormap — negative → blue, zero → black/white, positive → red — is enough to start; see §8) and writes into the image registered with `DisplayRegistry`. Dispatched once per engine frame while running, immediately after `step`.
- **`inject`**: reads a push-constant grid coordinate + radius, adds a Gaussian bump directly into `curr`. Dispatched only on frames where the mouse-injection callback fired (§2.6) — not part of the normal per-frame sequence.

All three are built via `ComputePassBuilder` (`docs/plans/compute-pipeline-general.md` §2.1) and dispatched via `dispatchComputePass()` — no bespoke pipeline/descriptor code here.

### 2.4 Explicit Start / Pause / Stop state machine — auto-steps only while Running

```cpp
enum class SimState { Stopped, Running, Paused };
```
Per explicit direction, the simulation does **not** auto-start — it begins `Stopped` (grids zeroed/uninitialized) and only steps once the user clicks **Start** in a "Wave Simulation" settings window. While `Running`, `step`+`visualize` dispatch automatically every engine frame (no manual per-step triggering, unlike the CPU/GPU raytracer features' progressive-accumulation model — a running wave sim is watched evolving live, not converging toward one static image). **Pause** freezes stepping while retaining current grid state (resumable from exactly where it left off). **Stop** clears all three grids back to zero and returns to `Stopped` (a full reset, not resumable — starting again begins a fresh simulation).

### 2.5 One step per engine frame by default, with a CFL-driven cap the UI enforces rather than silently ignores

The explicit scheme in §2.1 is only numerically stable when `dt ≤ dx / (c·√2)` (the 2D CFL condition) — violating it doesn't degrade gracefully, it diverges to `NaN`/`Inf` within a handful of steps. Rather than silently clamping `dt` or `c` behind the user's back, the wave-speed (`c`) slider's usable range is computed from the current grid spacing (`dx`, derived from `SimulationPlane`'s extent and resolution) and the frame-time-derived `dt`, so the UI can't express an unstable configuration in the first place — this is simpler and more honest than either letting the user blow up their own simulation or silently overriding a value they explicitly set. One `step` dispatch per engine frame (§2.3) is the default work unit, matching `docs/plans/compute-pipeline-raytracing.md` §2.3's "one unit of work per engine frame" precedent — if a future need for sub-stepping (multiple `step` dispatches per frame, for a smaller stable `dt` than one frame's wall-clock time would otherwise imply) arises, it's a small addition to the same dispatch-count logic, not a redesign.

### 2.6 Mouse injection via `DisplayRegistry`'s click/drag callback, only while Running

The output image is registered with `docs/plans/imgui-display.md`'s `DisplayRegistry` using the optional `onInteract(glm::vec2 uv)` callback added there (§2.8) specifically for this. The callback converts the UV coordinate into a grid cell (`gridX = uv.x * resolutionX`, etc.) and, only while `SimState::Running`, dispatches the `inject` pass (§2.3) centered there. Injection is deliberately disabled while `Paused`/`Stopped` — clicking a frozen or empty simulation shouldn't silently queue up a change that appears only once Start is pressed, since that would be confusing (a click producing a delayed, disconnected-feeling effect).

## 3. Exact files to create/modify

| File | Role |
|---|---|
| `shaders/wave_step.comp` (new) | The `step` pass (§2.1/§2.2). |
| `shaders/wave_visualize.comp` (new) | The `visualize` pass (§2.3) — scalar field → colormap. |
| `shaders/wave_inject.comp` (new) | The `inject` pass (§2.3) — Gaussian bump at a push-constant grid coordinate. |
| `src/wave_sim.h` (new) | Declares `WaveSimulation`: the three role-rotating grid buffers, the uploaded `boundaryType` buffer (§2.2), the three `ComputePass` instances, `SimState`, `update(VkCommandBuffer, VulkanEngine*, const SimulationPlane&)` (the `draw()` entry point — steps if `Running`, always re-visualizes if grids exist), and the "Wave Simulation" settings panel (Start/Pause/Stop, wave-speed slider with the CFL-derived range from §2.5). |
| `src/wave_sim.cpp` (new) | Implements the above. |
| `src/vk_engine.h` | Add `WaveSimulation m_waveSimulation;` member. |
| `src/vk_engine.cpp` | In `draw()`, call `m_waveSimulation.update(cmd, this, m_simulationPlane)` as its own step, separate from `drawGeometry()`/the compute-raytracing dispatch (`docs/plans/compute-pipeline-raytracing.md` §3) — all three write to independent resources, so relative ordering among them doesn't matter functionally, only that each finishes its own barriers before `drawImgui()` per `docs/plans/imgui-display.md` §2.6's contract. Wire the settings panel into `run()`; register the output image (with the `onInteract` callback, §2.6) with `DisplayRegistry` once grids are first allocated (on Start, not before). |
| `src/CMakeLists.txt` | Add `wave_sim.h`/`wave_sim.cpp` to the explicit source list. New `.comp` files need no build-file change (existing shader glob). |

## 4. Implementation order and dependencies

Depends on both prerequisites in §0 being complete.

1. **Grid allocation + role rotation** (§2.1), no boundary conditions or visualization yet — verify by stepping a trivial case (e.g. a single initial bump in an otherwise-zero grid, no boundaries at all) and confirming it propagates outward symmetrically, the simplest possible correctness check for the core update formula.
2. **`wave_visualize.comp`** (§2.3) + `DisplayRegistry` wiring, so step 1's propagating bump becomes visible — verify visually before adding anything else.
3. **Boundary mask upload + `wave_step.comp`'s boundary handling** (§2.2) — verify: a wave reflects visibly off a region marked reflective, and is absorbed (doesn't bounce back, though may show some artifact — see §7) off a region marked absorbing.
4. **Start/Pause/Stop state machine + settings panel** (§2.4). Verify: simulation truly does not step before Start is clicked; Pause freezes and Resume continues from the same state; Stop fully resets.
5. **CFL-derived parameter range** (§2.5) — verify the wave-speed slider's bounds actually prevent an unstable configuration; deliberately try to push past them and confirm the UI, not the simulation, is what stops you.
6. **Mouse injection** (§2.6) — verify clicking/dragging in the output window while Running visibly adds a new wave source at the correct location, and that clicking while Paused/Stopped does nothing.

## 5. Edge cases / traps identified during planning

- **Numerical instability (CFL violation) presents as `NaN`/`Inf` propagating across the whole grid within a handful of steps**, not a graceful degradation — if the visualize pass ever shows the whole image suddenly going solid black/white/garbage, suspect a stability violation first, not a logic bug elsewhere.
- **Role-rotation indexing must be consistent between `step`'s bind order and whatever the CPU side considers "current" for `visualize`/`inject`** — since there's no data copying (§2.1), a mismatched role index would have `visualize` reading last step's `prev` instead of the fresh `curr`, silently displaying stale data one step behind rather than crashing.
- **Absorbing boundary cells forced to zero (§2.2) will show a visible reflection artifact**, not a perfectly clean absorption — this is a known, accepted limitation of the simplest absorbing-boundary approach, not a bug to chase down; §7 notes the more accurate alternative deliberately not built now.
- **Injection during a frame where a boundary-mask re-upload just happened** (`SimulationPlane` changed while the sim was mid-run) — the grids themselves don't need to resize (resolution is fixed per `docs/plans/simulation-domain.md` §2.1), only the boundary buffer's contents change, so this should be a simple re-upload with no grid-reallocation implications; confirm this holds once both features are actually wired together.
- **Registering the output image only on Start, not at construction** (§3): means `DisplayRegistry` has nothing to show before the user ever starts a simulation — confirm this is the intended UX (no window until Start) rather than an empty/placeholder window existing from app launch.

## 6. Code patterns from the existing codebase to follow

- **Every pass via `ComputePassBuilder`/`dispatchComputePass()`** (`docs/plans/compute-pipeline-general.md`) — no bespoke compute pipeline code here.
- **Boundary data source**: `SimulationPlane::boundaryMask` (`docs/plans/simulation-domain.md` §2.5) — read directly, never recomputed by this feature.
- **Upload-on-change, not per-frame**: mirrors both `docs/plans/compute-pipeline-raytracing.md`'s sphere/material buffer upload (`RaytraceSceneEditor` dirty flag) and `docs/plans/simulation-domain.md`'s own recompute trigger — the boundary buffer here follows the same "only re-upload when the CPU source actually changed" shape.
- **Interactive display window**: `docs/plans/imgui-display.md` §2.8's `onInteract` callback — this is that capability's first real consumer.
- **"One unit of work per engine frame"**: `docs/plans/compute-pipeline-raytracing.md` §2.3 — same shape applied here (one `step` per frame while running) for the same reason (keeps each frame's dispatch sequence self-contained and easy to reason about).

## 7. What NOT to do (alternatives rejected and why)

- **Do not** implement a proper absorbing boundary condition (e.g. a perfectly matched layer or a first-order Mur ABC) in this pass (§2.2) — the simple zero-forcing approach has known, visible reflection artifacts but is far simpler to implement correctly; a better ABC is a well-understood, purely additive future refinement if the artifact turns out to matter visually.
- **Do not** let the simulation auto-start or auto-step before Start is explicitly clicked (§2.4) — explicit direction; `Stopped` is the real initial state, not `Running`.
- **Do not** silently clamp an unstable wave-speed/timestep combination behind the user's back (§2.5) — constrain what the UI can express instead, so the user always understands why a value isn't selectable rather than wondering why their explicit setting got silently overridden.
- **Do not** allow mouse injection while Paused or Stopped (§2.6) — a click producing a delayed, disconnected-feeling effect once Start is later pressed is worse UX than simply not responding to the click at all.
- **Do not** implement multi-substep-per-frame time integration in this pass (§2.5) — one step per frame is the default; sub-stepping is a small, clearly-scoped future addition if a specific need for it appears, not something to build speculatively now.
- **Do not** copy grid data between fixed buffer slots each step (§2.1) — role rotation by index is equivalent and free.

## 8. Open questions / things to verify before starting

1. **Exact colormap for `wave_visualize.comp`** (§2.3) — "a simple diverging colormap" is specified conceptually, not pixel-exact; reasonable to finalize during implementation.
2. **Single-channel float format choice** (`R32_SFLOAT` vs. a smaller/packed format) for the three grid buffers (§2.1) — `R32_SFLOAT` is the safe default (matches the scalar field's natural precision needs and CFL-sensitivity to numerical error) but wasn't benchmarked; worth confirming it performs acceptably at whatever grid resolution `docs/plans/simulation-domain.md` ends up defaulting to.
3. **Whether `SimulationPlane` supports only one instance** (`docs/plans/simulation-domain.md` §8, item 1) directly determines whether `WaveSimulation` needs to support more than one concurrent simulation — this doc assumes a single `m_simulationPlane`/single `m_waveSimulation` pairing; revisit together if that assumption changes.
4. **Whether wave-simulation parameters (wave speed, grid resolution) should be saveable**, extending `docs/plans/scene-and-asset-management.md`'s save/load mechanism the way `docs/plans/simulation-domain.md` §8 already flagged for boundary-condition assignments — same open question, not resolved here either.
