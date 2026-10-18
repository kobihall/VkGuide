# Scene capabilities and templates

The laser optics plans add an Optics folder, beam axes, a breadboard, detectors, a Beam window, a design wavelength and a dozen settings that mean nothing in a scene of Sponza under an HDR sky. The application has to stay a plain raytracer for such scenes, and become an optics editor without becoming a second application. This plan adds the mechanism: a scene lists the capabilities it uses, each capability brings its own object kinds, folders, windows, menus and recommended settings, and **New Scene** offers templates that start with a set of them on. **Optical bench** is the first capability, and later modes are more of them.

*Not implemented. No plan dependencies. Every physoptics plan that adds UI registers it through this plan, so build it before [physoptics-optical-elements.md](physoptics-optical-elements.md). Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

On 2026-10-18 the user asked for one unified editor that does not take over the renderer, proposed fixed scene types as one solution, and asked how other software solves it. Given the survey below, they chose:

- Capabilities recorded in the scene, which combine, and templates that preset them, over exclusive scene types or UI-only workspaces. A "scene type" is the name of the template a scene started from.
- A capability cannot be turned off while objects that need it exist. The toggle names them and offers to delete them, so a scene's data and its capabilities never disagree.
- Enabling a capability offers its recommended settings once, listed in a note that can be dismissed; after that the user's settings are theirs, and the capability never resets them.
- Named workspaces (saved docking layouts per task) are planned, as a later step of this plan.

## How other software solves it

| Approach | Example | What it gates | Trade-off |
|---|---|---|---|
| An exclusive mode per file | OpticStudio's sequential and non-sequential modes, switched under **Setup** → **System**; Mitsuba 3's variants (`scalar_rgb`, `scalar_spectral`, `*_polarized`), chosen before a scene loads | the editor, the analyses, the solver | simple, but users want both, and OpticStudio grew a mixed mode (sequential with non-sequential components) to allow it |
| Add-ons recorded in the document | COMSOL's physics interfaces, chosen in the Model Wizard and addable later in the Model Builder, with couplings between them; Unreal's per-project plugin list in the `.uproject`; Speos running inside SpaceClaim on the same CAD document | object kinds, settings, solvers | composes, and a new domain is one more add-on; more combinations to test |
| UI-only workspaces | Blender's workspaces; FreeCAD's workbenches over one document | which tools and panels are in front | never locks data away; gives no place to keep domain settings or defaults |
| Settings that unlock panels | Blender's per-scene render engine (Cycles or EEVEE) and Cycles' experimental feature set | groups of settings | already how this renderer hides the BVH settings for the linear scan |
| Templates | Blender's new-file templates (General, Sculpting, VFX); Unreal's project templates | the starting state only | nothing is locked afterwards |

The plan takes the add-on model for what a scene contains, templates for how one starts, the renderer's existing "the selection declares its settings" pattern for panels ([pbrt-roadmap.md](pbrt-roadmap.md), integrator layer), and workspaces for layout later.

## What the renderer does today

- **File** → **New Scene** (`VulkanEngine::newScene()`, `src/vk_engine.cpp`) clears the models, the environment, the shapes and the lights, adds one camera, and resets the render settings, the kernel selection and the BVH settings to their defaults. The startup scene is `assets/scenes/sphere_scene.gltf`.
- The **Windows** menu is a fixed list of window toggles (`vk_engine.cpp`), plus the `DisplayRegistry`'s image windows.
- The Scene panel's **Add** menu is fixed: Geometry, Camera, Light, Mesh from glTF file (`rt_scene_editor.cpp`).
- Settings sections already follow the selection: the **Acceleration structure** section appears only for a traversal that declares `KernelSettings::AccelerationStructure`.

## Design

### Capabilities

```cpp
enum class CapabilityId : uint32_t { OpticalBench, Count };   // append only; the key is what is saved

struct CapabilityInfo {
    CapabilityId id;
    const char* key;            // saved in the scene file; never rename
    const char* name;           // "Optical bench"
    const char* description;
    // what turning it on offers once, as a list the note shows and Apply sets
    void (*recommendedSettings)(VulkanEngine*, std::vector<SettingChange>&);
    // the objects that need it, for the refusal and "Delete them"
    void (*dependentObjects)(const VulkanEngine*, std::vector<ObjectRef>&);
};

struct SceneCapabilities {
    std::bitset<(size_t)CapabilityId::Count> enabled;
    bool operator==(const SceneCapabilities&) const = default;
};
```

`CAPABILITIES` (`src/scene_capabilities.h/.cpp`) is the registry, like `OPTIC_TRAITS` and the kernel registry: the only place that lists them. The engine owns the scene's `SceneCapabilities`; `capabilityEnabled(id)` is what every gated piece of UI asks.

### What a capability gates

A capability gates UI and defaults. It never gates rendering: the renderer, the beam solver and the scene file handle every object that exists, whether or not its capability is on. Since a capability cannot be off while its objects exist, this only ever matters for a file written by a build that knew a capability this build does not.

Each plan registers its UI under a capability. For **Optical bench**:

| Where | What appears |
|---|---|
| Scene panel tree | the **Optics**, **Beam axes** and **Detectors** folders ([physoptics-optical-elements.md](physoptics-optical-elements.md), [physoptics-beam-axis.md](physoptics-beam-axis.md), [physoptics-detectors.md](physoptics-detectors.md)) |
| **Add** menu | **Optic**, **Laser**, **Detector**, **Beam axis** |
| Menu bar | an **Optics** menu: **Import prescription...**, **Load glass catalog...**, **Bench settings...** ([physoptics-prescription-import.md](physoptics-prescription-import.md), [physoptics-optical-materials.md](physoptics-optical-materials.md), [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)) |
| **File** menu | **Export Optical Data...** ([physoptics-data-export.md](physoptics-data-export.md)) |
| **Windows** menu | **Beam**, **Detectors** ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md), [physoptics-detectors.md](physoptics-detectors.md)) |
| Viewport | the breadboard, beams, beam-axis overlays ([physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md), [physoptics-beam-preview.md](physoptics-beam-preview.md)) |
| **Raytrace Render** | an **Optics** section: design wavelength, surrounding medium, exact coatings, record rays |
| **Materials** window | the **Glasses** and **Coatings** folders |

Registration is by name: the Scene panel asks the registry for the folders, add entries and windows of every enabled capability, in registry order, and draws them. Code outside `scene_capabilities.cpp` never tests for **Optical bench** by name except the plans that own its parts.

### Turning one on and off

**Edit** → **Scene capabilities...** lists every capability with a checkbox and its description.

- Turning one on adds its UI at once and shows a note above the Scene panel: "Optical bench recommends:" and the list of settings it would change, with **Apply** and **Dismiss**. **Optical bench** recommends the light integrator ([pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md)) when the scene has lasers, a fixed seed for comparisons, the breadboard at the default size, and showing millimeters. Nothing changes until **Apply**, and the note never returns for that scene.
- Turning one off is refused while its dependent objects exist. The dialog lists them (by name and kind) with **Delete them and turn off** and **Cancel**.
- An action that needs a capability turns it on, with the same note: **Import prescription...** is under the Optics menu, but opening a scene, pasting an optic, or a script adding a laser to a scene without the capability enables it rather than failing.

### Templates

A template is a scene file in `assets/templates/`, with a name and a one-line description in its payload. **File** → **New Scene** becomes a submenu of templates; **New Scene** (the existing item) stays as the default template, so nothing changes for a user who never looks at the submenu.

| Template | Capabilities | Contents |
|---|---|---|
| **Render scene** | none | today's `newScene()`: one camera, default settings |
| **Optical bench** | Optical bench | a 300 × 300 mm breadboard, a HeNe laser on the bench's beam height, a power meter, an empty beam axis from the laser, a camera looking down at the table, the light integrator, and an empty environment |

**File** → **Save as Template...** writes the current scene there with a name, so a lab's standard table becomes a template. Opening a template opens it untitled, so **Save** asks for a path instead of overwriting the template. The "scene type" a user sees is the template the scene started from, recorded in the payload and shown in the window title, and it gates nothing: it is a label.

### Scene file

The payload version this lands in adds:

```json
"capabilities": ["optical_bench"],
"template": "optical_bench"
```

A file without `"capabilities"` opens with none, unless it holds objects that need one (an older optics file), in which case the loader turns on what they need and says so. An unknown key is kept, saved back unchanged, and reported as a load warning, so a scene saved by a newer build with another capability survives a round trip through this one.

### Workspaces, a later step

A workspace is a named docking layout and a set of open windows: **Render** (the Raytrace Render panel, the output, the shader window) and **Optics** (the Scene panel, Beam, Detectors, a top-down viewport). It needs the main viewport to be a dock space (`ImGui::DockSpaceOverViewport()`), which the application does not have yet (memory note, GPU path tracer state). A capability can contribute a default workspace; switching workspaces changes only windows, never the scene. Workspaces are saved per user in `imgui.ini`'s layout data, not in the scene.

### Future capabilities

The mechanism is general, which is the point of it. Candidates the plans already contain: the 2D wave solver of [simulation-domain.md](simulation-domain.md) and [wave-simulation.md](wave-simulation.md), whose simulation plane and boundary panel are exactly a capability's folder and window; and the user's future wave backend, if it brings its own objects and settings.

## Registry, bindings and data

- `CapabilityId`, `CapabilityInfo`, `CAPABILITIES`, `SceneCapabilities` (`src/scene_capabilities.h/.cpp`).
- Registration hooks in the Scene panel's tree and **Add** menu, the menu bar, the **Windows** menu, the Raytrace Render panel and the Materials window.
- **Edit** → **Scene capabilities...**; **File** → **New Scene** submenu and **Save as Template...**; the recommendation note.
- `assets/templates/render_scene.gltf`, `assets/templates/optical_bench.gltf`.
- Scene file: `"capabilities"`, `"template"`.
- No binding or kernel change.

## Verification

- Every existing scene opens with no capability, shows exactly today's menus, panels and folders, and renders bit-identical; `newScene()` with the default template is unchanged.
- Turning **Optical bench** on and off in an empty scene shows and hides every entry in the table above, and changes no setting until **Apply**.
- Turning it off with an optic in the scene is refused, and lists that optic; **Delete them and turn off** leaves a scene that saves the same as one that never had it, apart from the version.
- An optics scene saved by an earlier physoptics build, without `"capabilities"`, opens with **Optical bench** on.
- A scene with an unknown capability key round-trips with the key intact.
- **Save as Template...** followed by **New Scene** from that template gives an untitled scene equal to the saved one.

## Order of work

1. The registry, `SceneCapabilities`, the scene-file keys and the edit dialog, with no capability yet registered except **Optical bench** as an empty entry.
2. Registration hooks in the Scene panel, the menus and the panels. Each physoptics plan then registers its own UI as it lands.
3. The refusal, dependent objects and the recommendation note.
4. Templates, the New Scene submenu and **Save as Template...**.
5. Workspaces, after the main viewport becomes a dock space.

## Open questions

- Whether a capability should also be able to hide existing UI that its scenes do not need (the environment map controls in a bench scene with an empty environment). The decision so far is that capabilities only add; hiding would be a workspace's job.
- Whether templates should also exist for scenes the user commonly builds (a Cornell box, the Veach scene), which would make the existing `assets/scenes/` files templates. That is a convenience with no design consequence.

## Sources

The Zemax OpticStudio manual, sequential, non-sequential and mixed modes and **Setup** → **System**; Mitsuba 3 documentation, variants; COMSOL Multiphysics reference manual, "The Model Wizard" and "Creating a Multiphysics Model"; Unreal Engine documentation, "Plugins" and the project descriptor; the Blender manual, workspaces, application templates and the Cycles feature set; the FreeCAD wiki, workbenches; Ansys Speos documentation (Speos in SpaceClaim).
