# Feature: Restructured ImGui Rendering Display

## 0. How to use this doc

This is a standalone implementation spec — treat it as the only context you have. For exhaustive facts about the current codebase state (full `draw()` walkthrough, descriptor/image management, existing compute pattern, etc.) read `docs/codebase-map.md` first; this doc only restates what's directly relevant to this feature. Before writing code, re-verify any line numbers cited here against the current source, since they may have drifted.

**Status: implemented and verified on 2026-08-23.** The spec below is preserved as written. Where implementation proved a passage wrong, that passage carries an inline **Update (as built)** note; §9 records the full as-built picture — deviations, verification evidence, and answers to every §8 open question. **Read §9 before relying on any API shape described in §2**, since the `registerImage()` signature changed.

## 1. Feature goal

Give VkGuide a reusable mechanism for showing arbitrary GPU-rendered images (compute pass outputs, a future raytracer's output, a future physics sim's output) inside floating or dockable/tabbed ImGui windows, on top of the existing full-screen 3D raster scene — with the display mode (floating vs. tabbed) freely changeable at runtime by the user dragging windows, the way any docking-enabled ImGui app works. This is purely a *display* mechanism; it does not itself produce any new rendered content. It also fixes a real, related input-handling gap: today the camera's mouse-look and WASD input have no concept of ImGui wanting the input instead, so this feature introduces an explicit capture-mode boundary between "controlling the camera" and "interacting with the UI."

## 2. Architecture decisions made and WHY

### 2.1 Switch vendored ImGui to the docking branch — prerequisite, outside this repo

The vendored ImGui at `../CPPLibraries/imgui` (sibling directory, **not a git repo, not a submodule** — confirmed via `git rev-parse --is-inside-work-tree` failing there — it's a plain copied source tree) is version `1.90.9 WIP` from upstream **master**, not the **docking** branch. Grepping `imgui.h` for `Docking`/`DockSpace`/`DockNode` turns up nothing except two incidental comment mentions — `ImGuiConfigFlags_DockingEnable`, `DockBuilder`, `DockSpace()` etc. do not exist in this vendored copy at all.

**Why this matters**: the feature's core requirement — "floating window OR a tab I switch between, changeable at runtime" — is exactly what ImGui's docking branch gives you for free on *any* two regular windows (drag one window's title bar onto another and they merge into a tabbed group; drag a tab back out and it floats again). No `DockSpace()` host is even required for this — ad hoc docking between arbitrary floating windows works as soon as `ImGuiConfigFlags_DockingEnable` is set. Building "tabs" by hand with `BeginTabBar`/`TabItem` was considered and rejected (see §7) once the decision was made to take the docking branch, since real docking is a strict superset of what a hand-rolled tab bar gives you, for comparable implementation effort once the dependency is upgraded.

**Action required before any VkGuide code changes**: replace the vendored source in `../CPPLibraries/imgui` with a docking-branch build. This must be a matched *set* — `imgui.h/.cpp`, `imgui_internal.h`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, and both used backends (`backends/imgui_impl_glfw.h/.cpp`, `backends/imgui_impl_vulkan.h/.cpp`) all need to come from the same docking-branch snapshot — mixing a master-branch backend with docking-branch core (or vice versa) will not compile or will misbehave. Pick a docking-branch version at or after `1.90.9` to stay closest to what's currently vendored. **This is a change to a shared sibling directory, not to the VkGuide repo** — confirm before doing it whether any other project on this machine also points at `../CPPLibraries/imgui` (not verified during planning; see §8).

**Update (as built)**: `../CPPLibraries/imgui` **is** shared — `PhysicsSimulator` and `VulkanMinimalTest` both `add_subdirectory` it. At the user's explicit direction it was swapped **in place** anyway (rather than vendoring VkGuide its own copy), to upstream tag **`v1.92.9b-docking`** — the most recent *released* docking tag, picked over branch HEAD (`1.93.0 WIP`) so the build stays reproducible. The pre-swap tree is backed up at `../CPPLibraries/imgui.bak-1.90.9-master`. It was diffed against upstream `v1.90.9` before overwriting and had **no local source modifications** (all differences were upstream WIP drift — Windows-only code, version strings); only the custom root `CMakeLists.txt` was local, and it was preserved. The "at or after `1.90.9` to minimise API drift" advice above turned out to be materially wrong at this distance — 1.92 rewrote the exact APIs this feature depends on. See §9.1.

### 2.2 The 3D raster scene stays a full-screen background; it does NOT become a dockable panel

Two architectures were considered:
- **(Chosen) Scene stays full-screen.** `draw()` keeps blitting `m_drawImage` straight to the swapchain exactly as it does today (`vk_engine.cpp:117`, `vkutil::copy_image_to_image`), and `drawImgui()` keeps rendering ImGui content on top of that already-blitted swapchain image (`vk_engine.cpp:123`). Every *other* output (raytrace, physics, compute) gets its own floating/dockable ImGui window layered on top via the new registry below. These extra windows can dock/tab with each other, and float over the 3D view, but the 3D view itself never becomes a window and can't be tabbed away or moved.
- **(Rejected) Scene becomes a panel too**, Walnut/RTIAW-style: swapchain becomes a pure ImGui canvas with a full-viewport `DockSpace`, and the 3D scene is shown via `ImGui::Image()` in its own dockable "Viewport" panel alongside every other output. Rejected because it requires removing the current direct-blit-to-swapchain step and making `m_drawImage`/`m_depthImage` resize to track an arbitrary panel's content region instead of the window (they are currently allocated once in `initSwapchain()`, sized to `m_windowExtent`, and never recreated — see `docs/codebase-map.md` §1) — a much larger, riskier change to a well-tested code path, for a capability (tabbing the 3D view itself) that wasn't requested.

**Consequence**: this feature's changes to `draw()`/`run()` are additive only — nothing about the existing scene-render path is touched.

### 2.3 New `DisplayRegistry` type, not methods bolted onto `VulkanEngine`

Followed the codebase's existing per-concern-file convention (`vk_descriptors.h/.cpp` → `DescriptorLayoutBuilder`/`DescriptorAllocatorGrowable`/`DescriptorWriter`; `vk_images.h/.cpp` → `vkutil::transition_image`/`copy_image_to_image`; `vk_pipelines.h/.cpp` → `PipelineBuilder`). A new `src/vk_display.h/.cpp` pair holds a `DisplayImage` struct and a `DisplayRegistry` class; `VulkanEngine` owns exactly one instance (`DisplayRegistry m_displayRegistry;`) the same way it owns one `DescriptorAllocatorGrowable m_globalDescriptorAllocator`. This was chosen over adding a pile of new methods directly on the already-1500-line `VulkanEngine` class, and over a purely free-function API (rejected because the registry needs to hold state — the vector of registered images — between calls, which a class models more directly than free functions taking an opaque state blob).

### 2.4 The registry does NOT own the displayed `VkImage`/`AllocatedImage`

`registerImage()` takes a `VkImageView` + `VkSampler` + display name + a fixed `VkExtent2D`, not an `AllocatedImage`. The owning feature (background compute, future raytracer, future physics sim) creates and destroys its own image via the existing `VulkanEngine::createImage()`/`destroyImage()` (`vk_engine.h:228-230`) exactly as it does today; the registry only tracks the *display-side* bookkeeping (cached `ImTextureID`, visibility flag, window name). **Why**: image lifetime is inherently owned by whichever pass produces it — the registry has no way to know when a producer is done writing or wants to resize/reallocate, so making it own the image would require threading that producer-specific knowledge into a generic registry. Keeping ownership with the producer and the registry purely a display-side cache mirrors how `m_drawImageDescriptors` already just points at `m_drawImage.imageView` without owning it (`vk_engine.cpp:939-942`).

**Required call order this imposes on every future producer**: call `unregisterImage()` (which internally calls `ImGui_ImplVulkan_RemoveTexture`) *before* destroying the underlying `AllocatedImage`/`VkImageView`. Destroying the image first and unregistering after leaves a dangling descriptor that ImGui may still sample from that frame.

**Update (as built)**: two corrections to this section's API.
- **The `VkSampler` parameter does not exist.** ImGui 1.92 replaced per-texture combined image samplers with a shared image view plus a per-draw sampler selection; `ImGui_ImplVulkan_AddTexture`'s sampler argument is now *ignored* by the backend. Passing `m_defaultSamplerLinear` would have been a silently discarded argument. `registerImage()` takes a `DisplayFilter { Linear, Nearest }` instead, applied around the `ImGui::Image()` call via `ImGuiPlatformIO::DrawCallback_SetSamplerNearest`/`SetSamplerLinear`. This preserves this section's and §6's intent (the producer chooses filtering) on the mechanism that actually exists.
- **`unregisterImage()` does not free the descriptor set synchronously.** `ImGui_ImplVulkan_RemoveTexture` calls `vkFreeDescriptorSets` immediately, which is invalid while command buffers referencing that set are still executing. The registry defers the free instead — see §5's updated bullet and §9.2.

### 2.5 Fixed resolution, letterboxed to fit — no auto-resize-to-panel mode

Every registered image is displayed at a fixed resolution chosen explicitly by its owning feature; the registry never reallocates or resizes an image just because its ImGui window changed size. Instead, `drawWindows()` computes an aspect-correct "fit inside the available content region" size each frame (classic letterbox math: `scale = min(availWidth / imageWidth, availHeight / imageHeight)`) and passes that to `ImGui::Image()`, so the full image is always visible, un-cropped, centered, whatever size the user drags the window to.

**Why**: this was an explicit design choice over "track panel size, reallocate on resize" (the pattern the sibling `RayTracingInAWeekend` project uses — see `docs/codebase-map.md` §6, `Image::AllocateMemory` is called whenever `ImGui::GetContentRegionAvail()` changes). That pattern actively conflicts with progressive/accumulating rendering — a future compute-shader raytracer producing an image over many frames via temporal accumulation would have its accumulation buffer silently wiped every time the user resizes or redocks its window. Fixed resolution avoids that trap entirely and was chosen as the single global behavior (not a per-image opt-in/out) to keep the registry API simple, since no current or planned use case needs auto-resize.

**Update (as built)**: letterboxing into `ImGui::GetContentRegionAvail()` carries a trap this section did not anticipate. A fresh ImGui window auto-fits to its contents; contents that size *themselves* to the window close the loop, and the window settles at nothing — measured as `avail = 16x0` for the first ~180 frames, with the image never drawn at all. `drawWindows()` therefore calls `ImGui::SetNextWindowSize(..., ImGuiCond_FirstUseEver)` with a default width and the registered image's aspect ratio before `ImGui::Begin()`, so there is a real region to fit into. Any future window showing a registry image inherits this; do not remove it.

### 2.6 Layout transitions are the producer's responsibility, not the registry's

`DisplayRegistry` has no access to a command buffer at the point `drawWindows()` runs (that call happens during ImGui UI construction in `run()`, before that frame's command buffer is even opened — command buffers are opened later, inside `draw()`). It therefore cannot insert a `vkutil::transition_image` barrier itself.

**Contract**: any feature that registers an image must transition it to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` itself, inside `draw()`, before `drawImgui(cmd, ...)` is called (`vk_engine.cpp:123`) — i.e. right after that feature's own pass (compute dispatch, render pass, etc.) finishes writing the image. This is the same barrier pattern already used for `m_drawImage`/`m_depthImage` elsewhere in `draw()` (`vkutil::transition_image`, `vk_engine.cpp:101-103`, `108`, `113-114`). Getting this wrong produces validation errors or garbage/undefined sampling — MoltenVK is strict about this (see `CLAUDE.md`'s MoltenVK section); there is no implicit "the driver will figure it out" fallback.

### 2.7 "Windows" menu for show/hide

VkGuide currently has no ImGui menu bar at all (confirmed via grep — no `BeginMenuBar`/`BeginMainMenuBar`/`MenuItem` anywhere in `vk_engine.cpp`). This feature adds one: `ImGui::BeginMainMenuBar()` in `run()`, with a "Windows" menu listing every currently-registered display image as a checkbox bound to its `visible` flag. This is in addition to, not instead of, each window's own native close (X) button — closing via the X button clears the same `visible` flag the menu checkbox controls, so either one re-shows the other correctly. Unregistering (fully removing an entry, as opposed to hiding it) stays exclusively a producer-code action, not something exposed in the menu — the menu only ever toggles visibility of images some feature has chosen to register.

**Update (as built)**: the menu also toggles the three pre-existing engine windows (`background`, `Stats`, `ImGui Demo`), above a separator, from three `bool` members on `VulkanEngine` — agreed with the user as a deliberate widening of this section's scope. `ImGui::ShowDemoWindow()` now defaults **off** rather than being drawn unconditionally, since a permanently-open demo window is noise once a real menu bar exists. `DisplayRegistry::drawWindowsMenu()` still emits only registry entries; the engine windows are emitted by `run()` around that call, so the registry stays unaware of them.

### 2.8 Optional per-image click callback, for features that need interactive display windows

Some future consumers (e.g. a physics simulation's mouse-injection interaction, `docs/plans/wave-simulation.md`) need to know *where inside* a displayed image the user clicked or is dragging, not just show it passively. `registerImage()` takes an additional optional parameter, `std::function<void(glm::vec2 uv)> onInteract = nullptr`. `drawWindows()` already computes the letterbox-fit rectangle for every image each frame (§2.5) — reusing that exact same rectangle for `ImGui::IsItemHovered()`/`ImGui::IsItemActive()` hit-testing means the click-to-UV conversion is a natural extension of code that already exists, not a parallel calculation that could silently drift out of sync with what's actually drawn.

When a callback is set, `drawWindows()` checks each frame whether the image is currently being clicked-and-held (not just the initial click — `ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)`), so a press-and-drag gesture fires the callback repeatedly as the cursor moves, not just once. On a firing frame, it converts the cursor's window-local pixel position into a `[0,1]×[0,1]` UV coordinate relative to the image content (using the same letterbox offset/scale already computed for that frame's rendering) and invokes the callback. The consumer owns converting that UV into whatever domain-specific coordinate space it actually needs (e.g. a simulation grid cell) — `DisplayRegistry` itself has no idea what the image represents.

This was scoped as narrowly as possible — "tell me the UV coordinate of an active click/drag" — rather than a general input-event system (press/release edges, drag deltas, other mouse buttons, modifiers), since no currently-planned feature needs more than that. If richer interaction is needed later, extend the callback signature then rather than building it speculatively now.

### 2.9 Explicit camera-capture mode, gating both mouse-look and WASD behind one boundary — not two independent heuristics

**Verified this is a real, currently-existing gap, not a hypothetical**: `initGLFW()` (`vk_engine.cpp:495-527`) installs raw GLFW callbacks — `glfwSetKeyCallback` feeding `Camera::processKeyEvent()` directly, `glfwSetCursorPosCallback` feeding `Camera::processMouseMotion()` directly (via `m_firstMouse`/`m_lastMouseX`/`m_lastMouseY` bookkeeping, `vk_engine.cpp:515-526`) — with **no check of `ImGuiIO::WantCaptureMouse`/`WantCaptureKeyboard` anywhere** (confirmed via grep across `vk_engine.cpp`). This produces two concrete, currently-present bugs, not just the reported "mouse hits the screen edge" symptom:
1. Moving the mouse rotates the camera even while dragging an ImGui slider, a `DisplayRegistry` image (§2.8), or the gizmo (`docs/plans/scene-and-asset-management.md` §2.8) — nothing currently arbitrates who the mouse belongs to.
2. **Typing into any ImGui text field also moves the camera** — `docs/plans/scene-and-asset-management.md`'s planned file-path text fields would have this problem immediately, since W/A/S/D are literal camera-translation keys with no keyboard-focus check.

**Fix**: a single `bool m_cameraCaptureActive` (or equivalent) mode flag on `VulkanEngine`. Camera mouse-look **and** WASD translation are both gated on this one flag — not gated independently on `WantCaptureMouse`/`WantCaptureKeyboard` directly — so there's one coherent mode boundary ("captured" = you're driving the camera, nothing else gets input; "not captured" = you're using the UI, the camera gets nothing) rather than two separately-tuned heuristics that could disagree with each other.

**While captured**: `glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED)` — GLFW's standard mechanism for unbounded relative mouse-look. This is the actual, complete fix for "can't rotate past the screen edge"; it isn't something to weigh alternatives on, every FPS-style camera implementation uses this exact technique, and it directly solves the reported symptom as a side effect of fixing the underlying arbitration gap.

**Entry trigger — hold-to-look, e.g. holding the right mouse button**: pressing RMB while the cursor is over the raster background (i.e. `!ImGui::GetIO().WantCaptureMouse`) enters capture mode; releasing RMB exits immediately. Two alternatives were weighed and are worth recording since the complexity difference between them turned out to be small: a click-once-to-enter/Esc-to-exit "sticky" toggle (closer to a game's pointer-lock convention, better for long navigation sessions since you don't have to hold a button, but needs an explicit "am I stuck in this mode" mental model) and a persistent UI toggle button (rejected as not matching "click into the viewport" at all). Hold-to-look was chosen specifically because it matches Blender/Unity/Unreal editor-viewport convention (all use hold-based look-around), which fits an editor-like tool better than a game-style pointer-lock pattern, and is momentary by construction — there's no way to get "stuck" in camera mode the way a sticky toggle could leave you if the exit key were ever missed.

**Composes with §2.8 without new arbitration code**: a click landing inside a `DisplayRegistry` image window is already inside an ImGui window (`WantCaptureMouse` true), so §2.8's click/drag-to-UV callback needs no changes to coexist with this — the `WantCaptureMouse` check this feature's entry trigger needs is the same one ImGui already maintains for exactly this purpose.

**Update (as built)**: two corrections.
- **The `WantCaptureMouse`-goes-quiet assumption is wrong** (this also settles §5's last bullet and §8 q5, in the opposite direction from what both expected). The GLFW backend explicitly *reverted* ignoring mouse data under `GLFW_CURSOR_DISABLED` — `imgui_impl_glfw.cpp` changelog, 2023-07-18: *"Revert ignoring mouse data on GLFW_CURSOR_DISABLED as it can be used differently. User may set ImGuiConfigFlags_NoMouse if desired."* So the unbounded virtual cursor keeps being fed to ImGui and drifts over real windows, hovering them mid-look. `setCameraCapture()` sets `ImGuiConfigFlags_NoMouse` on entry and clears it on exit — the remedy the backend's own changelog names. This is not optional.
- **Gating `Camera::processKeyEvent()` introduces a bug of its own**: a movement key released *after* capture ends never reaches the camera, so that axis' velocity sticks on forever and the camera drifts. `setCameraCapture(false)` zeroes `m_mainCamera.velocity`.

**Forward-reference for whenever the gizmo exists**: this doc is built before `docs/plans/scene-and-asset-management.md`'s `TransformGizmo`, so the entry trigger above only checks `WantCaptureMouse` for now — a gizmo isn't part of the UI-capture surface yet because it doesn't exist yet. `docs/plans/scene-and-asset-management.md` §2.8 extends this same entry-trigger check with `&& !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()` once it's implemented, rather than building a separate camera-suppression mechanism of its own — see that doc for the specifics.

## 3. Exact files to create/modify

| File | Role |
|---|---|
| `../CPPLibraries/imgui/**` (sibling dir, outside this repo) | Replace with a matched docking-branch snapshot: `imgui.h/.cpp`, `imgui_internal.h`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `backends/imgui_impl_glfw.h/.cpp`, `backends/imgui_impl_vulkan.h/.cpp`. Do this first — nothing else compiles meaningfully without it. |
| `src/vk_display.h` (new) | Declares `struct DisplayImage` and `class DisplayRegistry` (see §2.3–2.8 for exact shape). |
| `src/vk_display.cpp` (new) | Implements `DisplayRegistry::registerImage/unregisterImage/setVisible/drawWindows/drawWindowsMenu` — the `ImGui_ImplVulkan_AddTexture`/`RemoveTexture` calls, the letterbox-fit math, the per-window `ImGui::Begin`/`ImGui::Image`/`ImGui::End`, the optional click/drag-to-UV callback (§2.8), and the "Windows" menu contents. |
| `src/vk_engine.h` | Add `#include "vk_display.h"`; add member `DisplayRegistry m_displayRegistry;` near the other persistent-state members (alongside `m_globalDescriptorAllocator`, `vk_engine.h:173`). |
| `src/vk_engine.cpp` | In `initIMGUI()` (`vk_engine.cpp:1143-1204`): after `ImGui::CreateContext()`, set `ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;`. In `run()` (`vk_engine.cpp:419-493`), inside the ImGui-content-building section (after the existing `"background"`/`"Stats"` windows, before `ImGui::Render()` at line 482): add `ImGui::BeginMainMenuBar()` with the "Windows" menu (delegates to `m_displayRegistry.drawWindowsMenu()`), and call `m_displayRegistry.drawWindows()`. |
| `docs/codebase-map.md` | Read-only reference for this feature; not modified by it. |
| `src/vk_engine.h` | (In addition to `DisplayRegistry`, above) Add `bool m_cameraCaptureActive = false;` and whatever small state §2.9's cursor-restore edge case needs (§5). |
| `src/vk_engine.cpp` | (In addition to §2.1–§2.7's changes) Rewrite `initGLFW()`'s callbacks (`vk_engine.cpp:511-526`) to gate **both** `Camera::processMouseMotion()` and `Camera::processKeyEvent()` on `m_cameraCaptureActive` — i.e. only forward the GLFW key/cursor-pos callback's event to the camera at all when captured, rather than changing `Camera`'s own methods or signatures. Add a `glfwSetMouseButtonCallback` handling RMB press/release to toggle `m_cameraCaptureActive` (checking `WantCaptureMouse` on press per §2.9 — extended later by `docs/plans/scene-and-asset-management.md` §2.8 to also check the gizmo's flags) and call `glfwSetInputMode`. No changes to `src/camera.h/.cpp` are needed for this — `Camera` itself stays unaware of capture state, matching how mouse-look gating already works. |
| `src/CMakeLists.txt` | Confirmed during planning: this is an **explicit file list** (`add_executable(vulkan_guide main.cpp vk_engine.cpp vk_engine.h ...)`, not a glob) — add `vk_display.h`/`vk_display.cpp` to it by name or they silently won't build. |

No changes to the root `CMakeLists.txt`'s shader glob — no new shaders are introduced by this feature.

**Update (as built)**: two entries this table missed.
- `src/vk_engine.cpp`'s `initIMGUI()` needed a full port to the 1.92 ImGui API, not just the one-line `ConfigFlags` addition listed above — see §9.1.
- `DisplayRegistry` has a fifth method not implied by this table, `beginFrame(uint64_t)`, called from `run()` immediately after `ImGui::NewFrame()`. It advances the frame counter and retires deferred descriptor-set frees (§5 update). Forgetting it leaks descriptor sets on every unregister.

## 4. Implementation order and dependencies

1. **Vendor the docking-branch ImGui** into `../CPPLibraries/imgui` (§2.1, §3). Verify VkGuide still builds and runs unmodified afterward (docking-branch ImGui is API-compatible with master for existing calls) before touching any VkGuide source — this isolates "did the dependency swap break something" from "did my new code break something."
2. **Enable docking**: one-line `ConfigFlags` change in `initIMGUI()`. Build and run; confirm no crash, confirm existing `"background"`/`"Stats"` windows can now be dragged into a docked/tabbed group with each other (proves docking is actually active before writing any new display code).
3. **Add `src/vk_display.h/.cpp`** with `DisplayImage`/`DisplayRegistry` as designed in §2.3–2.7. No VulkanEngine changes yet — this can be written and compiled in isolation (it only depends on ImGui + Vulkan types).
4. **Wire `DisplayRegistry` into `VulkanEngine`**: add the member, add the `drawWindows()`/menu calls into `run()`.
5. **Smoke test using `m_drawImage` itself.** `m_drawImage` currently has usage `TRANSFER_SRC | TRANSFER_DST | STORAGE | COLOR_ATTACHMENT` (`vk_engine.cpp:819`, per `docs/codebase-map.md` §1) — **no `SAMPLED_BIT`**, which `ImGui_ImplVulkan_AddTexture` requires. Temporarily add `VK_IMAGE_USAGE_SAMPLED_BIT` to `m_drawImage`'s creation flags, transition it to `SHADER_READ_ONLY_OPTIMAL` right before `drawImgui()` in `draw()` (in addition to, not instead of, its existing transitions — it's still also being blitted to the swapchain the normal way), and register it with the display registry under a name like `"Scene Mirror"`. This validates the entire mechanism end-to-end (window shows live GPU content, can be dragged to float or dock/tab with `"Stats"`, "Windows" menu toggles it) using an image that already exists, with no dependency on any of the other three planned features. Once verified, either remove this test registration or leave it as a permanent debug view — user's call at implementation time.

Step 5 is the acceptance test for this feature: no other planned feature (raytracer, compute pipeline, physics sim) needs to exist yet for this feature to be fully verified.

6. **Camera-capture mode** (§2.9) — independent of steps 1–5, can be built and verified in parallel. Rewire `initGLFW()`'s callbacks to gate on `m_cameraCaptureActive`, add the RMB press/release handling, gate `Camera::processKeyEvent()`'s WASD the same way. Verify: holding RMB over the background looks around and enables WASD with the cursor hidden and no screen-edge limit; releasing RMB restores the normal cursor and stops the camera from responding to mouse/WASD; clicking/dragging a `DisplayRegistry` window or an active gizmo (once either exists) never triggers capture mode; typing in any ImGui text field never moves the camera.

## 5. Edge cases / traps identified during planning

- **`../CPPLibraries/imgui` is not version-controlled** (no `.git` in it, not a submodule) and may be shared by other projects on this machine beyond VkGuide — not verified during planning (see §8). Overwriting it in place is a bigger blast radius than a normal in-repo dependency bump.
- **Backend/core version mismatch**: the docking branch and master branch of Dear ImGui are not drop-in compatible at arbitrary version skew — vendor a matched snapshot (core + both backends) from one specific commit/tag, not core from one place and backends from another.
- **`m_drawImage` lacks `VK_IMAGE_USAGE_SAMPLED_BIT`** today — any image registered for display must have this flag; it's easy to forget when a future feature creates its own output image via `createImage()`.
- **Layout-transition contract is easy to get wrong silently**: forgetting to transition a registered image to `SHADER_READ_ONLY_OPTIMAL` before `drawImgui()` won't necessarily crash — it may just sample stale/garbage/black data, especially on MoltenVK. Worth a debug-mode assertion or validation-layer check when implementing, though the exact mechanism is left to implementation time.
- **`ImGui_ImplVulkan_AddTexture` requires the descriptor pool passed at `ImGui_ImplVulkan_Init` to have been created with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`** (confirmed by reading `backends/imgui_impl_vulkan.h`). VkGuide's existing `imguiPool` (`vk_engine.cpp:1148-1168`) already has this flag set, so no changes are needed there — confirmed during planning, not an open question.
- **Double-buffering (`FRAME_OVERLAP = 2`) does not require special handling here**: `ImGui_ImplVulkan_AddTexture` returns one `VkDescriptorSet` tied to a specific `VkImageView`, reused every frame as long as that view doesn't change — this mirrors how `m_drawImageDescriptors` is written once at init and reused every frame (`vk_engine.cpp:939-942`), not re-written per frame-in-flight. Only re-register (unregister + register again) when the underlying `VkImageView` handle itself changes (e.g. a future feature reallocating its output image at a different fixed resolution the user chose).
- **Duplicate registration names**: `registerImage()` on a name that's already registered should be treated as a caller bug and hard-abort, consistent with the codebase's existing error philosophy (`checkVkResult`/`vkbErr` in `vk_types.h:115-129` hard-abort on any Vulkan failure rather than recovering silently) — no silent overwrite, no auto-suffixing.
- **`m_firstMouse` must be re-armed on every capture-mode entry, not just once at startup** (§2.9): the existing `m_firstMouse`/`m_lastMouseX`/`m_lastMouseY` bookkeeping (`vk_engine.cpp:517-521`) exists specifically to avoid a jump on the very first mouse-move callback ever received — but it currently only fires once, at startup. If it isn't reset to `true` every time capture mode is re-entered, the first delta computed after re-engaging RMB will be relative to a stale cursor position from wherever the free cursor happened to be during UI mode, producing a jarring one-frame camera-rotation snap the instant capture re-engages.
- **`ImGui_ImplVulkan_RemoveTexture` frees the descriptor set immediately** (found during implementation, not anticipated by this doc): it calls `vkFreeDescriptorSets` straight away, but `VUID-vkFreeDescriptorSets-pDescriptorSets-00309` requires every submitted command referring to that set to have completed first. With `FRAME_OVERLAP = 2` an unregister mid-frame violates this. **Reproduced under validation**: 28 unregister/re-register cycles on a live-sampled image produced exactly 28 hits of that VUID. `DisplayRegistry` therefore queues removals and frees them in `beginFrame()` once `framesInFlight + 1` frames have passed (the `+1` because `beginFrame()` runs *before* `draw()`, so the most recent fence wait only proves frames up to `N - framesInFlight - 1` are complete). With deferral in place the same test produces zero VUIDs. This matters most for `docs/plans/raytracing-in-a-weekend.md`, which re-registers on every completed render.
- **ImGui 1.92 changed which descriptor types the pool needs**: textures are now `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` + `VK_DESCRIPTOR_TYPE_SAMPLER` rather than `COMBINED_IMAGE_SAMPLER`. VkGuide's existing `imguiPool` allocates 1000 of *every* type, so it already covers this — but a future attempt to trim that pool must keep both.
- **Cursor position on exit isn't automatically restored**: GLFW's disabled-cursor mode tracks an unbounded virtual position (that's the whole point — no screen-edge clamping), so simply switching back to `GLFW_CURSOR_NORMAL` doesn't guarantee the OS cursor reappears somewhere sensible. Store the cursor position at capture-entry time and explicitly `glfwSetCursorPos()` back to it on exit, so the visible cursor reappears where the user's hand actually is, not wherever the virtual position drifted to.
- **This does not need to special-case ImGui's own mouse polling while captured**: with the cursor disabled, `ImGui_ImplGlfw_NewFrame()`'s position polling should naturally not land on any real on-screen ImGui window, so `WantCaptureMouse` should read false throughout capture without extra code — worth confirming visually once built rather than assuming with certainty, since this depends on backend polling details not traced through in full during planning.

## 6. Code patterns from the existing codebase to follow

- **Display mechanism itself**: `/Users/kobihall/Documents/Code/RayTracingInAWeekend/Image.cpp:95-166` (`Image::AllocateMemory`) shows the exact `ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)` call and cast to `ImTextureID`. `main.cpp:59-67` in that project shows the `ImGui::Begin("Viewport")` / `ImGui::Image(descriptorSet, size, uv0, uv1)` / `ImGui::End()` pattern, including the `ImVec2(0,1)→ImVec2(1,0)` UV flip — **note VkGuide will not need that UV flip**, since it isn't uploading from a CPU row-major buffer; Vulkan-native render targets don't have that inversion.
- **Barrier pattern**: `vkutil::transition_image` (`src/vk_images.h/.cpp`) is the only transition helper in the codebase — use it for the producer-side `SHADER_READ_ONLY_OPTIMAL` transitions described in §2.6, the same way `draw()` already uses it at `vk_engine.cpp:101-103`, `108`, `113-114`.
- **Member-owned reusable subsystem**: `DescriptorAllocatorGrowable`/`DescriptorWriter` (`src/vk_descriptors.h/.cpp`) are the closest existing precedent for "a small reusable class with its own header/impl file, instantiated as a `VulkanEngine` member" — `DisplayRegistry` should look and feel like a sibling to these, not like a one-off.
- **Cleanup ordering**: follow the `DeletionQueue` convention (`vk_engine.h:11-27`) for anything `DisplayRegistry` itself allocates that needs teardown (if anything — most of what it holds are non-owning handles per §2.4).
- **Samplers**: ~~reuse `m_defaultSamplerLinear`/`m_defaultSamplerNearest` (`vk_engine.h:201-202`, created in `initDefaultData()`) as the default `VkSampler` passed to `registerImage()`~~ — **superseded**: ImGui 1.92 removed per-texture samplers from the Vulkan backend, so `registerImage()` takes a `DisplayFilter { Linear, Nearest }` enum instead and the engine's own samplers are not involved. See §2.4's update. The underlying intent (the producer picks filtering, and does not create its own sampler for this) is unchanged.
- **Existing input-handling shape**: `initGLFW()`'s lambda callbacks (`vk_engine.cpp:511-526`) are the exact code §2.9 modifies — keep the same "lambda closure over `glfwGetWindowUserPointer`" style rather than introducing a different callback-registration pattern.

## 7. What NOT to do (alternatives rejected and why)

- **Do not** hand-roll tabs with `BeginTabBar`/`TabItem` instead of real docking. This was the fallback plan before the docking-branch dependency swap was approved; now that real docking is in scope, it strictly subsumes what a manual tab bar offers (free-form docking/undocking, splitting, persisted layouts via ImGui's `.ini` mechanism) for comparable implementation cost.
- **Do not** make the 3D raster scene a dockable panel in this pass (§2.2) — that's a materially larger change to a working, well-tested render path, for a capability that wasn't requested. If wanted later, it's a separate, focused change, not something to fold into this feature.
- **Do not** have `DisplayRegistry` own/allocate the displayed `AllocatedImage` (§2.4) — ownership belongs with whoever produces the content; a registry that owns images would need per-producer resize/recreate logic baked into a supposedly-generic type.
- **Do not** implement auto-resize-to-panel behavior (§2.5), even as an opt-in per image, in this pass — it wasn't requested for any currently-planned use case and actively conflicts with the raytracer's future progressive-accumulation model. If a genuine future need for it appears, add it then, deliberately, rather than speculatively now.
- **Do not** try to transition image layouts from inside `DisplayRegistry` — it has no command buffer access at the point it runs (§2.6); this responsibility must stay with producers.
- **Do not** open a second Vulkan surface/swapchain or a second `ImGui_ImplVulkan` instance for any of this — the single-OS-window, single-swapchain, ImGui-docking approach covers every display requirement raised so far without new Vulkan surface/present-loop code.
- **Do not** gate mouse-look and WASD independently on separate heuristics (§2.9) — e.g. checking `WantCaptureMouse` for the mouse and something else (or nothing) for the keyboard. One shared capture-mode flag is the whole point; two independently-tuned checks could disagree with each other and reintroduce exactly the kind of gap this feature exists to close.
- **Do not** implement a sticky click-to-enter/Esc-to-exit toggle or a persistent UI toggle button for capture mode (§2.9) — both were considered; hold-to-look was chosen for closer genre-convention fit and because it can't leave the user "stuck" in camera mode.

## 8. Open questions / things to verify before starting

All six were resolved during implementation. Questions kept as written; answers appended.

1. **Is `../CPPLibraries/imgui` shared by other projects** beyond VkGuide? Not checked during planning. If yes, coordinate the docking-branch swap so it doesn't break something else pointed at the same path (e.g. vendor VkGuide's own copy instead of modifying the shared one, if sharing is confirmed).
   - **Answered: yes, shared** by `PhysicsSimulator` and `VulkanMinimalTest`. The user chose to swap it in place regardless and handle any fallout in those two projects themselves. Backup at `../CPPLibraries/imgui.bak-1.90.9-master`. See §2.1's update and §9.1 for the breaking-change list those projects would have to absorb.
2. **Exact docking-branch commit/tag to vendor.** Should be at or after the currently-vendored `1.90.9 WIP` to minimize API drift; pick a specific pinned snapshot rather than "latest" so the build is reproducible.
   - **Answered: `v1.92.9b-docking`**, the most recent released docking tag, per the user's request for the latest docking branch. Branch HEAD was `1.93.0 WIP` and was rejected as an unreleased snapshot. Note `v1.90.9-docking` does exist and would have been the zero-drift choice; the far larger jump was a deliberate user decision, and the resulting API port is §9.1.
3. **Validation-layer / debug-assert strategy for the layout-transition contract** (§5) — worth deciding at implementation time whether to add a debug-only check (e.g. tracking last-known layout per registered image and asserting before use) or to rely entirely on Vulkan validation layers catching misuse.
   - **Answered: rely on the validation layers; no bespoke tracker was built.** Verified empirically — deliberately omitting the `SHADER_READ_ONLY_OPTIMAL` transition while the image was displayed produced 402 hits of `VUID-VkDescriptorImageInfo-imageLayout-00344` and `VUID-vkCmdDrawIndexed-None-08114`. The contract is enforceable simply by flipping `b_UseValidationLayers` to `true` (`vk_engine.cpp:26`, left `false`). A per-image layout tracker would have needed every `vkutil::transition_image` call site to report in — a far larger refactor for strictly less coverage.
4. Confirm whether the temporary `m_drawImage` smoke-test registration (§4 step 5, "Scene Mirror") should be removed after verification or kept permanently as a debug view — left as a call for whoever implements this.
   - **Answered: kept permanently, registered hidden** (`visible = false`), shown from the "Windows" menu. `VK_IMAGE_USAGE_SAMPLED_BIT` is now a permanent flag on `m_drawImage` and the extra `TRANSFER_SRC_OPTIMAL → SHADER_READ_ONLY_OPTIMAL` transition runs every frame. Known cosmetic limitation: it mirrors the *whole* draw image, so at a Render Scale below 1 the unused border shows the previous frame's pixels (§2.5 fixes extents at registration time; a UV sub-rect would be needed to crop, and was not added).
5. **Whether `ImGui_ImplGlfw_NewFrame()`'s mouse polling truly never lands on a real ImGui window while the cursor is disabled** (§5, §2.9) — expected behavior based on how GLFW's disabled-cursor virtual position works, but not traced through the vendored backend's exact polling code during planning; confirm visually once built.
   - **Answered: no — the assumption was wrong.** The backend deliberately does *not* suppress mouse data under `GLFW_CURSOR_DISABLED` (reverted upstream in 2023). Fixed by setting `ImGuiConfigFlags_NoMouse` for the duration of capture. See §2.9's update.
6. **Exact mouse button for capture entry** — this doc assumes right mouse button (matching Blender/Unity/Unreal convention, §2.9), but the specific button (vs. e.g. middle mouse) wasn't independently scrutinized beyond that convention match.
   - **Answered: right mouse button, as specified.** Implemented as written; changing it is a one-line edit to the `glfwSetMouseButtonCallback` lambda in `initGLFW()`.

## 9. As-built notes (implementation, 2026-08-23)

Everything below is what actually got built and verified, written after the fact. Where it disagrees with §§1–8, this section is right.

### 9.1 The dependency swap cost an API port

`../CPPLibraries/imgui` went from `1.90.9 WIP` (master) to `v1.92.9b-docking`. That is a much bigger jump than §2.1/§8 q2 contemplated, and 1.92 rewrote the exact APIs this feature is built on. Every project sharing that directory has to absorb the same list:

| Change | Effect on VkGuide |
|---|---|
| `ImGui_ImplVulkan_InitInfo::ApiVersion` now required | Set to `VK_API_VERSION_1_2`. **Load-bearing on this project** — it is what makes the backend load the KHR dynamic-rendering entry points instead of core 1.3 ones, matching `CLAUDE.md`'s Vulkan 1.2 + KHR rule. |
| `InitInfo::QueueFamily` now required | Set to `m_graphicsQueueFamily`; the backend needs it for its own texture-upload command pool. |
| `MSAASamples` / `PipelineRenderingCreateInfo` moved into `PipelineInfoMain` | Mechanical move in `initIMGUI()`. |
| `ImGui_ImplVulkan_CreateFontsTexture()` removed | Call deleted. The atlas now uploads itself on demand via `ImGuiBackendFlags_RendererHasTextures`. |
| `ImTextureID` is now `ImU64`, drawing takes `ImTextureRef` | `VkDescriptorSet` ↔ `ImTextureID` conversion goes through `uintptr_t`; `vk_display.cpp` has `to_texture_id`/`to_descriptor_set` helpers for it. |
| `ImGui_ImplVulkan_AddTexture` dropped its sampler argument | Drove the `DisplayFilter` design (§2.4 update). |
| Texture descriptors are `SAMPLED_IMAGE` + `SAMPLER`, not `COMBINED_IMAGE_SAMPLER` | No change needed — `imguiPool` already allocates 1000 of every type. |

One thing that did **not** need changing: the 1.92 backend uploads textures on its own private command buffer with its own queue submit, so calling `ImGui_ImplVulkan_RenderDrawData` from inside `drawImgui()`'s dynamic-rendering scope stays valid.

### 9.2 The API as built

```cpp
enum class DisplayFilter { Linear, Nearest };

void init(uint32_t framesInFlightCount);   // called with FRAME_OVERLAP from initIMGUI()

void registerImage(const std::string& name, VkImageView imageView, VkExtent2D extent,
                   DisplayFilter filter = DisplayFilter::Linear,
                   std::function<void(glm::vec2 uv)> onInteract = nullptr);
void unregisterImage(const std::string& name);
bool isRegistered(const std::string& name) const;
void setVisible(const std::string& name, bool visible);

void beginFrame(uint64_t currentFrame);    // called from run() with m_frameNumber
void drawWindows();
void drawWindowsMenu();
void destroyAll();                         // device-idle only; runs before ImGui_ImplVulkan_Shutdown()
```

Error philosophy, following `checkVkResult`/`vkbErr`: `registerImage` **aborts** on a duplicate name (it cannot be honoured without silently dropping an entry) and `setVisible` **aborts** on an unknown name (it asserts something that isn't true), but `unregisterImage` is a **no-op** on an unknown name — removal is idempotent, so a producer's cleanup path can call it unconditionally. §5's "duplicate registration hard-aborts" is honoured; the asymmetry is deliberate.

`beginFrame()` is new and not in §3's file table: it advances the frame counter and retires deferred descriptor-set frees. It must be called once per frame before `drawWindows()`.

### 9.3 Deviations from this spec, and why

1. **No `VkSampler` parameter; `DisplayFilter` instead** — forced by 1.92, §2.4 update.
2. **Deferred descriptor-set removal** — the hazard was real and reproducible, §5 update. Approved by the user before implementation.
3. **`ImGui::SetNextWindowSize` on first use** — fixes a genuine display bug, §2.5 update.
4. **`ImGuiConfigFlags_NoMouse` during capture** — §2.9 update, resolves §8 q5 the other way.
5. **`m_mainCamera.velocity` zeroed on capture exit** — fixes a stuck-velocity bug the gating itself introduced, §2.9 update.
6. **"Windows" menu also toggles `background`/`Stats`/`ImGui Demo`** — beyond §2.7's letter (which scoped the menu to registry images only), agreed with the user. Three `bool` members on `VulkanEngine`; the demo window now defaults **off**, since a permanently-open demo window is noise once a real menu bar exists. `docs/plans/scene-and-asset-management.md`'s "File" menu is a sibling addition to this same bar, as it already anticipates.
7. **`"Stats"` now uses `if (ImGui::Begin(...))`** like `"background"` already did, instead of building its contents unconditionally while collapsed. Incidental consistency fix.

### 9.4 Verification actually performed

Run under validation layers (`b_UseValidationLayers = true`, layer confirmed inserted at instance *and* device level via `VK_LOADER_DEBUG=layer`), then reverted to `false`.

The validation messenger was itself verified before trusting any silence from it — an early "0 validation errors" result was **false confidence**: the window-sizing bug (§2.5 update) meant the image was never drawn, so nothing was sampling it and nothing could fail. Deliberately breaking the layout contract afterwards produced 402 VUID hits, proving the messenger reports.

| Test | Result |
|---|---|
| Build + run on docking 1.92.9b, before any VkGuide feature code | clean (isolates dependency swap from new code, per §4 step 1) |
| Production code, Scene Mirror visible and sampled, 200 frames, clean shutdown through `destroyAll()` | exit 0, **0 VUIDs** |
| 28 unregister/re-register cycles on a live-sampled descriptor (feature 2's pattern) | **0 VUIDs** |
| Same, with removal forced immediate | **28× `VUID-vkFreeDescriptorSets-pDescriptorSets-00309`** — the control proving §5's deferral is necessary |
| `SHADER_READ_ONLY_OPTIMAL` transition deliberately omitted | **402× `VUID-VkDescriptorImageInfo-imageLayout-00344` + `VUID-vkCmdDrawIndexed-None-08114`** — answers §8 q3 |

**Not verified — needs a human at the keyboard.** Everything above is programmatic. Nobody has yet visually confirmed that docking/tabbing behaves (drag "Scene Mirror" onto "Stats"), that the mirrored image *looks* correct, or that RMB hold-to-look feels right in the hand. §4 step 5's acceptance test is therefore only partly discharged.

### 9.5 Known limitations left in place

- **Scene Mirror at Render Scale < 1** shows stale pixels in the border (§8 q4 answer). Would need per-image UV sub-rects, which §2.5's fixed-extent design does not have.
- **Focus loss while RMB is held** can strand capture mode, since GLFW may never deliver the release. This dents §2.9's "momentary by construction — no way to get stuck" rationale. A GLFW focus callback calling `setCameraCapture(false)` would close it; not added, as it was outside what was asked for.
