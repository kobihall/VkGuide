# Physical optics, end to end

*A readable account of the laser optics simulator as it is built so far: what exists, how the parts fit, and the rules that keep them consistent. What is still to come, and in what order, is in the [laser optics plan](plans/physoptics-laser-optics.md); the file-by-file detail is in `docs/codebase-map.md` §8.27. Every physoptics plan updates this file when it lands: its row in the status table, its section, and the cross-plan list at the end.*

---

## 1. The shape of it

The application is one renderer with one scene model. A laser table is not a second application or a mode the renderer switches into. It is a set of object kinds, windows and settings that a scene turns on as the **Optical bench** capability. A scene of Sponza under an HDR sky never shows a breadboard, and a bench scene renders through the same path tracer, the same materials and the same scene file as every other scene.

The optics have two consumers, which read the same data:

- A CPU solver traces one chief ray per beam segment, in double precision, fast enough to run on every gizmo drag. It gives the beam's path and its Gaussian size everywhere.
- The GPU path tracer traces millions of light paths from the lasers. It gives what detectors and cameras record, and the stray light, ghosts and clipping a single beam misses.

Neither exists yet. Today the bench has its place in the scene, its workspace and its template, and the Thorlabs mount models it will place.

```text
scene file ──► capabilities ──► which folders, menus, windows, settings and workspace appear
    │                              (UI and defaults only)
    └──────► objects ──────────► the renderer, the solver and the file see every one of them
```

## 2. Status

| Plan | Implemented | What it gave the bench |
|---|---|---|
| [physoptics-optomechanical-models](plans/completed/physoptics-optomechanical-models.md) | 2026-10-28 | 23 converted Thorlabs models with mates and drives, placeholders for the rest, the lazily loaded mount library |
| [physoptics-scene-capabilities](plans/completed/physoptics-scene-capabilities.md) | 2026-10-29 | The **Optical bench** capability and its hooks, templates, the dock space, workspaces and the **Viewport** window |

Every other physoptics plan is unbuilt. The laser optics plan's roadmap gives their order: optical surfaces, optical materials and optical elements come next.

## 3. Scene capabilities

### What a capability is

A capability is a row of `CAPABILITIES` in `src/scene_capabilities.cpp`, the only list of them. It has a key, which the scene file saves, a name, a description and a set of optional hooks, one per place in the UI that it can add to. A scene records the capabilities it has on, and they combine: a later capability, such as the 2D wave solver's simulation plane, sits beside **Optical bench** in the same scene.

The rule that makes this safe is that a capability gates UI and defaults, never rendering. The renderer, the solver and the scene file handle every object that exists, whether or not its capability is on. The scene's data and its capabilities are kept in agreement by one invariant. Write $\mathrm{need}(o)$ for the capabilities object $o$ needs, and $E$ for the scene's enabled set:

$$\bigcup_{o \in \text{objects}} \mathrm{need}(o) \subseteq E$$

Three operations maintain it:

- Turning capability $c$ off is refused while some object needs it. Each capability's `dependentObjects` hook lists those objects, and the refusal names them with **Delete them and turn off** and **Cancel**.
- Opening a file sets $E$ to the file's keys, then adds every capability whose objects the file holds. A file from before capabilities therefore opens with what it needs, and the status line says so.
- An action that needs a capability turns it on (`enableCapability()`) rather than failing.

A key this build does not know is kept in `SceneCapabilities::unknownKeys`, reported once as a load warning and saved back unchanged. A scene saved by a newer build with another capability survives a round trip through an older one.

### Where a capability adds UI

Each hook is called only while the scene has the capability on, in registry order:

- `sceneFolders`, `addMenu` and `sceneStatus`: folders after **Models** in the Scene panel's tree, entries in **+ Add**, and lines under the status line.
- `menus` and `fileMenu`: whole menus after **Windows**, and **File** items before **Export Render...**.
- `renderSettings`: sections at the end of the **Render Settings** window.
- `materialFolders`: folders after the models' in the **Materials** window.
- `viewportOverlays`: drawing over the **Viewport**, from a `ViewportView` that projects world points into the window and clips to it.
- `workspace`: a workspace offered beside **Render**.
- Windows: an entry of the window registry whose `capability` is this one. It is drawn, listed and saved in workspaces only while the scene has the capability.

**Optical bench** fills two of these today. Its `sceneStatus` warns when optical models are placeholder boxes, which used to show in every scene, and its `workspace` is **Optics**. Code outside `scene_capabilities.cpp` asks `capabilityEnabled()` or walks the hooks, and never names a capability, except the plan that owns the part.

### Recommended settings

Turning a capability on from **Edit** → **Scene Capabilities...** queues a note at the top of the Scene panel. The note lists the settings the capability would change, each as current and recommended values, with **Apply** and **Dismiss**:

```text
on enable(c, offer):
    E ← E ∪ {c}
    if offer: pending ← pending ∪ {c}
each frame, for c in pending:
    changes ← c.recommendedSettings(scene)      // only the settings that differ
    if changes = ∅: drop c                       // the scene already agrees
    else show changes; Apply runs each change; Apply or Dismiss drops c
```

Nothing changes until **Apply**. Opening a file that already has the capability shows no note, since its settings are the file's to keep. **Optical bench** recommends a fixed seed. Later plans add the light integrator when the scene has lasers, the breadboard at its default size, and millimeters in the panels.

### Templates

**File** → **New Scene** makes the Render scene template: one camera and the default settings, as it always has. **File** → **New from Template** lists the others. The built-in templates are code, `BUILTIN_TEMPLATES`, so they always match the current defaults and payload. Each is `newScene()` followed by its `populate` function. The Optical bench template turns the capability on, applies its recommended settings, puts a camera 0.55 m above the table looking at the beam height, and sets an empty, black environment.

**File** → **Save as Template...** writes the scene to `assets/templates/<name>.gltf`, with a name and description in its payload. That folder's templates follow the built-ins in the menu. A template opens untitled, so **Save** asks for a path instead of overwriting it. The template a scene started from is a label in the window title, `<file> - <template> - VkGuide`, and gates nothing.

### Workspaces and the window registry

Every top-level window is one `AppWindow` entry in `WorkspaceManager` (`src/workspace.h`): its name, its open flag, its draw function, its owning capability and its menu group. The **Windows** menu, the workspaces and the frame loop read that one list.

The main window is a dock space. The raster preview is the **Viewport** window in its center, tabbed with **Raytraced Output**. A workspace is a named layout and set of open windows. The tabs on the right of the menu bar switch between them: **Render** always, and **Optics** while the scene has **Optical bench**. Switching between frames goes like this:

```text
switch(from, to):
    saved[from] ← (ImGui's current ini text, the open windows)
    if saved[to] has a layout: load it into ImGui, restore its open windows
    else: build to's default with DockBuilder
```

Each workspace keeps the user's rearrangement until **Reset Layout**. The layouts live in `bin/imgui.ini` as `[Workspace]` entries beside ImGui's own. An inactive workspace's layout is its ini text escaped onto one line. The active one is ImGui's ordinary entries. An `imgui.ini` without a `[Workspace][State]` entry predates the dock space, and is discarded once for the default layout. A workspace whose capability the new scene lacks gives way to **Render**.

### How this compares with Mitsuba 3

Mitsuba 3 calls itself retargetable through variants: `scalar_rgb`, `cuda_ad_spectral`, `llvm_polarized` and others, each compiled from one generic codebase and chosen before a scene loads. A variant changes how everything renders, and the same scene description loads under any of them. This renderer has the same split, with different machinery. What Mitsuba fixes per variant is here a runtime render setting: the spectral representation and lane count, the integrator and the traversal. They are saved with the scene and switchable at any time. Capabilities are the other axis, which Mitsuba has no counterpart for. They change what you can author and what the UI shows, and never what a given scene renders.

## 4. Conventions every optics plan shares

- The world unit is the meter, +y is up, and the breadboard's top face is the plane y = 0, with a hole at the origin and the others on a 25 mm grid. The beam height, 100 mm by default, is measured from that face. Optics panels show millimeters, degrees and microradians.
- An optomechanical model's canonical frame is meters, +y up, and +z along the optical axis toward the incoming light, with the origin at the model's primary mate.
- An optical body's frame has +z along the axis light is designed to travel. Its origin is the element's datum: the front vertex of a lens, the reflecting vertex of a mirror, a cube's center.
- The CPU optics code is the Vulkan-free static library `rt_optics`, templated on the scalar type. The solver runs in `double`, and the `float` instantiation is the line-for-line mirror of the GLSL.
- In exact RGB, a glass's index is read at the scene's **Design wavelength**, 587.6 nm by default.

## 5. The optomechanical models

The Thorlabs mounts, holders, irises and mirror blanks are converted offline from STEP files by `tools/optical_models/convert.py`. Each one becomes `assets/optical_models/meshes/<stem>.glb`, with a node per moving part and a texture for its lettering and scales, and a `<stem>.mates.json`. A mate is a named frame $(\mathbf{o}, \mathbf{x}, \mathbf{y}, \mathbf{z})$ where two models meet, with the motions it allows. A drive maps a parameter to the angles of moving parts. An iris's aperture, for example, maps to its lever's and blades' rotations:

$$\theta_{\text{part}}(a) = \operatorname{lerp}\big(\text{table}_{\text{part}},\; a\big), \qquad a \in [a_{\min}, a_{\max}]$$

`MountLibrary` reads and uploads a model the first time something asks for it, outside the scene's models, so a mount is never saved with a scene or listed under **Models**. Nothing places one yet: [physoptics-bench-and-mounts.md](plans/physoptics-bench-and-mounts.md) does.

## 6. Where things live

```text
src/scene_capabilities.{h,cpp}   the capability registry, the Optical bench entry, templates, the capability windows
src/workspace.{h,cpp}            the window registry, the dock space, workspaces and their ini entries
src/viewport_view.h              where the raster preview is this frame, for overlays and the gizmo
src/optic_models.{h,cpp}         rt_optic_models: every model and placeholder, read from the converter's output
src/mount_library.{h,cpp}        the lazily uploaded mount library
tools/optical_models/            the STEP converter, decals, iris drives and the review renders
assets/optical_models/meshes/    the converted models (committed)
assets/templates/                templates the user saves
```

## 7. Across plans

### Checks every physoptics plan runs before it is done

Each plan that adds bench UI or objects keeps the bench consistent with the rest of the application by checking these:

1. Its UI goes in through the hooks and the window registry, under **Optical bench**, and nothing outside `scene_capabilities.cpp` and the plan's own files names the capability.
1. With the capability off, a plain scene shows exactly the menus, folders, windows and settings it did before the plan, and renders bit-identical in exact RGB with a fixed seed.
1. Every object kind the plan adds is listed by `opticalBenchDependentObjects()` with its kind, name and an `erase` function. With one in the scene, turning the capability off is refused and names it. **Delete them and turn off** leaves a scene that saves as one that never had the capability, apart from the version.
1. A file from the plan's own build, edited to drop `"capabilities"`, opens with **Optical bench** on and says so.
1. Its recommended settings join `opticalBenchRecommendedSettings()`, listing only settings that differ. None is ever applied without **Apply**, except by the Optical bench template.
1. What the plan adds to a new bench joins `populateOpticalBench()`. The plan's table lists the target contents: the breadboard, a HeNe laser at beam height, a power meter, a beam axis from the laser, and the light integrator.
1. A new window docks in `buildOpticsLayout()`, and in `buildRenderLayout()` if a render scene wants it too. A user's saved layout has no place for a new window, so it opens as a tab in the center the first time. Check both a fresh `imgui.ini` and one saved before the plan.
1. Panels show millimeters, degrees and microradians, and the file stores SI units.
1. The scene file version goes up by one, and a key is written only when it has something to say.
1. This file gets the plan's status row, its section, and any new conventions.

### Open items

- The raster viewport ignores the scene's solid background, so the Optical bench template's black environment shows the viewport's sky gradient. The path tracer renders it black.
- The Optics workspace in the laser optics plan has a top-down viewport beside the main one. That needs a second raster view: its own camera, draw image and `ViewportView`.
- The raster preview has no hook for a capability's geometry. [physoptics-bench-and-mounts.md](plans/physoptics-bench-and-mounts.md) adds one for the breadboard and mounts, and must add what it draws to `RasterKey`, or the paused preview keeps showing the old picture.
- "Showing millimeters" needs a length display setting, which no plan owns yet. [physoptics-optical-elements.md](plans/physoptics-optical-elements.md) should decide whether it is a per-scene setting for every panel or for the optics panels only.
- Whether a capability may hide UI its scenes do not need, such as the environment map controls in a bench scene, is open. So far capabilities only add, and hiding is a workspace's job.

## 8. Sources

The [laser optics plan](plans/physoptics-laser-optics.md) and its sources; the [scene capabilities plan](plans/completed/physoptics-scene-capabilities.md) and its survey of OpticStudio, COMSOL, Unreal, Blender and FreeCAD; the Mitsuba 3 documentation, "Variants" and "Scene format"; the Dear ImGui docking branch, `DockBuilder` and the `.ini` settings handlers.
