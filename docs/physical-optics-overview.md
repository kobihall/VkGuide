# Physical optics, end to end

*A readable account of the laser optics simulator as it is built so far: what exists, how the parts fit, and the rules that keep them consistent. What is still to come, and in what order, is in the [laser optics plan](plans/physoptics-laser-optics.md); the file-by-file detail is in `docs/codebase-map.md` §8.27 and §8.28. Every physoptics plan updates this file when it lands: its row in the status table, its section, and the cross-plan list at the end.*

---

## 1. The shape of it

The application is one renderer with one scene model. A laser table is not a second application or a mode the renderer switches into. It is a set of object kinds, windows and settings that a scene turns on as the **Optical bench** capability. A scene of Sponza under an HDR sky never shows a breadboard, and a bench scene renders through the same path tracer, the same materials and the same scene file as every other scene.

The optics have two consumers, which read the same data:

- A CPU solver traces one chief ray per beam segment, in double precision, fast enough to run on every gizmo drag. It gives the beam's path and its Gaussian size everywhere.
- The GPU path tracer traces millions of light paths from the lasers. It gives what detectors and cameras record, and the stray light, ghosts and clipping a single beam misses.

Neither exists yet. Today the bench has its place in the scene, its workspace and its template, the Thorlabs mount models it will place, and the optics themselves: thirteen kinds of element, each an exact surface prescription the path tracer traces, made of catalog glasses and coated with thin-film stacks or metals.

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
| [physoptics-optical-surfaces](plans/completed/physoptics-optical-surfaces.md) | 2026-10-29 | Optical bodies as lists of exact surfaces with apertures and media, traced on the CPU in double and on the GPU in float |
| [physoptics-optical-materials](plans/completed/physoptics-optical-materials.md) | 2026-10-29 | Glass catalogs, air, coatings from ideal splitters to thin-film stacks and metals, the `optical_interface` material, surface scatter |
| [physoptics-optical-elements](plans/completed/physoptics-optical-elements.md) | 2026-10-29 | Thirteen kinds of element in Thorlabs sizes, the **Optics** folder, their panel and preview, and the scene's length unit |

Every other physoptics plan is unbuilt. The laser optics plan's roadmap gives their order: bench and mounts come next, then light types, laser sources and the beam axis.

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
- `materialFolders` and `materialDetails`: folders after the models' in the **Materials** window, and the details pane for an entry one of those folders selected.
- `viewportOverlays`: drawing over the **Viewport**, from a `ViewportView` that projects world points into the window and clips to it.
- `workspace`: a workspace offered beside **Render**.
- Windows: an entry of the window registry whose `capability` is this one. It is drawn, listed and saved in workspaces only while the scene has the capability.

**Optical bench** fills most of these. Its `sceneFolders` and `addMenu` are the **Optics** folder and **+ Add** → **Optic**, its `menus` the **Optics** menu (**Load Glass Catalog...** and **Import Coating Curve**), its `renderSettings` the **Optics** section, and its `materialFolders` and `materialDetails` the **Glasses** and **Coatings** folders. Its `sceneStatus` warns when optical models are placeholder boxes, which used to show in every scene, and its `workspace` is **Optics**. Code outside `scene_capabilities.cpp` asks `capabilityEnabled()` or walks the hooks, and never names a capability, except the plan that owns the part.

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

Nothing changes until **Apply**. Opening a file that already has the capability shows no note, since its settings are the file's to keep. **Optical bench** recommends a fixed seed and millimeters as the scene's length unit. Later plans add the light integrator when the scene has lasers and the breadboard at its default size.

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

- The world unit is the meter, +y is up, and the breadboard's top face is the plane y = 0, with a hole at the origin and the others on a 25 mm grid. The beam height, 100 mm by default, is measured from that face. Every panel shows lengths in the scene's length unit, meters or millimeters (**Edit** → **Length Unit**, `src/length_unit.h`), and the optics panels show angles in degrees. The scene file stores SI units.
- An optomechanical model's canonical frame is meters, +y up, and +z along the optical axis toward the incoming light, with the origin at the model's primary mate.
- An optical body's frame has +z along the axis light is designed to travel. Its origin is the element's datum: the front vertex of a lens, the reflecting vertex of a mirror, a cube's center.
- The CPU optics code is the Vulkan-free static library `rt_optics`, templated on the scalar type. The solver runs in `double`, and the `float` instantiation is the line-for-line mirror of the GLSL.
- In exact RGB, a glass's index is read at the scene's **Design wavelength**, 587.6 nm by default.

## 5. The optomechanical models

The Thorlabs mounts, holders, irises and mirror blanks are converted offline from STEP files by `tools/optical_models/convert.py`. Each one becomes `assets/optical_models/meshes/<stem>.glb`, with a node per moving part and a texture for its lettering and scales, and a `<stem>.mates.json`. A mate is a named frame $(\mathbf{o}, \mathbf{x}, \mathbf{y}, \mathbf{z})$ where two models meet, with the motions it allows. A drive maps a parameter to the angles of moving parts. An iris's aperture, for example, maps to its lever's and blades' rotations:

$$\theta_{\text{part}}(a) = \operatorname{lerp}\big(\text{table}_{\text{part}},\; a\big), \qquad a \in [a_{\min}, a_{\max}]$$

`MountLibrary` reads and uploads a model the first time something asks for it, outside the scene's models, so a mount is never saved with a scene or listed under **Models**. Nothing places one yet: [physoptics-bench-and-mounts.md](plans/physoptics-bench-and-mounts.md) does.

## 6. Optical bodies, glasses, coatings and elements

### Bodies

An optical body is a list of surfaces in the body's frame (`OpticPrescription`, `src/optic_surface.h`). Each surface has a shape, a placement, an aperture and the two media it separates: `mediumMinus` behind it and `mediumPlus` in front, by index into the body's media table, where medium 0 is the surrounding air or vacuum. The normal points to the plus side, and every outer face's normal points out of the body. A surface has one of three shapes:

- A conic, with sag $z(r) = \dfrac{c\,r^2}{1 + \sqrt{1 - (1 + k)\,c^2 r^2}}$. A plane is $c = 0$, a sphere $k = 0$ and a paraboloid $k = -1$.
- A cylinder of radius $R$ about the frame's z axis, between two heights: a lens's or a filter's edge.
- An even asphere: the conic's sag plus $\sum_i \alpha_{2i} r^{2i}$.

An aperture bounds a surface in its frame's xy plane: a circle, an annulus, a rectangle, a polygon, or an ellipse for a tilted back face such as a wedge's. A ray hits a body at the nearest root, over all its surfaces, whose point lies inside that surface's aperture, with a slack of 4e-6 of the size so that two faces meeting at an edge leave no gap.

The code is templated on the scalar type. The beam solver will use `double`, and the `float` instantiation is the line-for-line mirror of `shaders/rt/include/crt_optic.glsl`. In float, a conic or a cylinder is a quadratic whose discriminant cancels when the ray starts far from the surface, so the intersector first moves the ray's origin to its closest approach to the surface frame's origin, solves there with the stable form of the roots, and refines each root by one Newton step that it keeps only when the step moves t by at most 1e-4 of t. An asphere starts from its base conic's root and takes up to 8 Newton steps. A root it cannot place within 1e-5 of the point's size is a miss.

A hit point is moved back onto its surface before the next ray leaves it (`opticReproject()`), and the spawn offset of [pbrt-robust-intersections.md](plans/completed/pbrt-robust-intersections.md) uses one error bound on every axis:

$$\delta = \gamma_{12}\,\Big(\max_i |p_i| + \max_i |o_i|\Big), \qquad \gamma_n = \frac{n\,\epsilon}{1 - n\,\epsilon}$$

where $\mathbf{p}$ is the hit point and $\mathbf{o}$ the surface frame's origin, in the body frame. A bound that vanished along a flat window's normal let a ray find the face it had just left.

On the GPU an optic is a shape instance of kind 255 (`CRT_SHAPE_OPTIC`). Its surfaces are six vec4 each in binding 55 (`OpticSurfaces`), followed by their payloads (a cylinder's heights, an asphere's terms, a polygon's corners) and the media table. The TLAS takes the body's bounds as the instance's box, so an optic sits in the same two-level BVH as meshes and shapes, and `bin/bvh_bench` checks it against brute force with every builder and layout. Elements with the same body share one block.

### Glasses and air

Glasses live in an `OpticalLibrary` (`src/optic_library.h`) beside the material library: the built-in AIR and VACUUM, the bundled catalog `assets/glass/bundled.agf`, the catalogs you load from **Optics** → **Load Glass Catalog...**, and glasses you add. The bundled catalog holds Schott's N-BK7, N-SF11, N-SF10, N-F2, F2, N-SF6 and N-SF1, fused silica, CaF2, MgF2 and sapphire, the coating oxides SiO2, Ta2O5, TiO2, HfO2 and Al2O3, and ND-OD1, an absorbing glass of optical density 1 at 2 mm.

The AGF reader (`src/optic_glass.cpp`) reads Zemax's catalogs, in ASCII or UTF-16, with every dispersion formula they use: Schott, the Sellmeier forms, Herzberger, Conrady, the Handbook forms and the Extended ones. A glass's `IT` lines give its internal transmittance $T_i$ through thickness $d_i$, which becomes an absorption coefficient interpolated over wavelength, $\alpha(\lambda) = -\ln T_i / d_i$. A ray that crosses length $\ell$ of the glass keeps $e^{-\alpha \ell}$ of its throughput.

Air is Ciddor 1996 at 15 C, 101,325 Pa and 450 ppm CO2: 1.000276 at 633 nm. **Render Settings** → **Optics** → **Surrounding medium** picks air or vacuum, which every body's medium 0 is.

### Coatings

A coating is what a surface's reflectance and transmittance come from in place of bare Fresnel (`src/optic_coating.h`). It is one of five kinds:

- Uncoated: exact Fresnel between the two media.
- Ideal: fixed Rs, Rp, Ts and Tp, such as a 50:50 splitter, a polarizing splitter, a perfect mirror or an absorber.
- Tabulated: R or T against wavelength, from a CSV curve (**Optics** → **Import Coating Curve**).
- Stack: thin films over the substrate, each a glass of the library and a thickness or a number of quarter waves at the coating's design wavelength.
- Metal: an optional dielectric stack over a metal from pbrt's named spectra (Ag, Al, Au, Cu).

A stack or a metal is evaluated by the characteristic matrix of each layer (Macleod's form), with $N = n - ik$ and the layer's phase thickness $\delta_j$:

$$M_j = \begin{pmatrix} \cos\delta_j & i \sin\delta_j / \eta_j \\ i\,\eta_j \sin\delta_j & \cos\delta_j \end{pmatrix}, \qquad \delta_j = \frac{2\pi}{\lambda}\, N_j\, d_j \cos\theta_j$$

where $\eta_j$ is the layer's tilted admittance for s or p. The presets approximate Thorlabs' E02, P01, G01, F01, A, B, C, BSW and BSN, with single-layer MgF2, a 567 nm long-pass dichroic and the ideal coatings. Each preset names the curve it approximates; a measured curve you download replaces it as a tabulated coating.

On the GPU, each coating between each pair of media becomes a table in binding 56 (`OpticCoatings`): Rs, Rp, Ts and Tp in both directions, against wavelength and 91 cosines spaced as $u_j = 1 - \cos(\pi j / 180)$ in the pair's lower-index medium, which follows the steep rise at the critical angle. Past the critical angle the lookup returns total reflection. **Exact coatings** evaluates the transfer matrix at every hit instead (`coatingExact()` in `crt_coating.glsl`). Neither stores the phases, which [pbrt-polarization.md](plans/pbrt-polarization.md) needs.

### The optical interface

Every surface of an element is an `optical_interface` material, a complex-class type with no index of its own: the indices are the surface's media, and the side the ray arrives from says which is incident. It has a finish, polished or ground, and a polished one has optional surface scatter.

- Polished: a delta reflection with probability $p = R / (R + T)$, weighted $R / p$, or a refraction weighted $T / (1 - p)$, divided by $(n_t / n_i)^2$ in radiance transport. What a coating absorbs, $1 - R - T$, is lost.
- Ground: pbrt's rough dielectric at roughness 1 between the same media. A mirror's back and a lens's edge are ground.
- Surface scatter: a share TIS of what a polished face reflects or transmits goes into an ABg lobe about the specular direction, $\mathrm{BSDF} = A / (B + |\beta - \beta_0|^g)$ in projected directions. The lobe is renormalized by its share inside the unit disk, sampled by its inverse CDF with rejection, and evaluated for light samples.

Kernel 06 applies a body's absorption at the hit that ends a segment inside it. A refraction through a dispersive medium ends a hero path's secondary wavelengths, as the legacy glass does. In exact RGB, indices are read at the **Design wavelength**, absorption at 630, 532 and 465 nm, and each coating's table holds its spectral R and T projected onto the three channels.

### Elements

An element is a `SceneOptic`: a kind, a size, parameters by name in SI units, a position and orientation in double, and one material per slot. The kind's row of `OPTIC_TRAITS` (`src/optic_catalog.cpp`) is all the code outside the builders knows about it: its sizes, parameters, material slots, mounts and output ports, and a builder that makes its prescription and a readout of what follows from the parameters (a lens's focal length, a prism's minimum deviation, a wedge's thick edge).

| Kind | Sizes | Defaults |
|---|---|---|
| Planar mirror | PF05-03, PF10-03, PF20-03 | N-BK7, `-P01` front, fine ground back |
| Spherical concave mirror | CM127, CM254, CM508 | N-BK7, `-P01` front, R = 200 mm |
| Off-axis parabolic mirror | MPD 1/2, 1 and 2 in, at 15 to 90 degrees | N-BK7, `-P01` front |
| Circular filter | 1/2, 1, 2 in | N-BK7, uncoated; a filter is the glass that absorbs, such as ND-OD1 |
| Square filter | 50 mm | as the circular filter, at most 14.7 mm thick (FP01) |
| Iris | ID8 to ID25 | black blades, one sheet |
| Plate beamsplitter | 1/2, 1, 2 in; 3, 5, 8 mm thick | N-BK7, BSW front, `-A` back |
| Cube beamsplitter | 12.7, 25.4, 50.8 mm | N-BK7 (N-SF1 polarizing), `-A` faces, the cement in the diagonal's coating |
| Spherical lens | 1/2, 1, 2 in; plano-convex, bi-convex, plano-concave, bi-concave | N-BK7, uncoated |
| Right-angle prism | PS910, PS911, PS913 | N-BK7, uncoated |
| Equilateral dispersing prism | PS850, PS852, 50 mm | F2, uncoated |
| Plate wedge prism | 50 mm | N-BK7, uncoated, 3 mm thin edge |
| Round wedge prism | 1/2, 1 (PS81x), 2 in | N-BK7, uncoated, 2, 3 and 6 mm thin edges |

A slot's material is a library entry the element owns (`OpticSlotOrigin`): it is made with the element, deleted with it, hidden from every material combo, and saved in the element's record only when you change it. **Reset to default** in the slot's details remakes it. A port names the sequence of reflections and refractions that produces an output beam, such as a right-angle prism's **Turned** and **Retro**, and `traceEvents()` follows it in double, which is what the beam axis will read.

The panel shows the parameters in the scene's length unit, the readout, the slots' coatings, the ports' directions and the mounts a kind may sit in, which [physoptics-bench-and-mounts.md](plans/physoptics-bench-and-mounts.md) makes real. In the raster preview a planar mirror at a preset size draws its PF blank model, and every other element its tessellated prescription.

`tools/verify/optics_scenes.py` writes the scenes the checks render to `assets/scenes/`: `optics_furnace` (a lens in a white furnace), `optics_nd_filter`, `optics_cube_split`, `optics_lens_focus`, `optics_prism` (a camera looking through an F2 prism at a slit), and `optics_bench`, one element of each kind on a bench. `tools/verify/optics_check.py` renders the first five.

## 7. Where things live

```text
src/scene_capabilities.{h,cpp}   the capability registry, the Optical bench entry, templates, the capability windows
src/workspace.{h,cpp}            the window registry, the dock space, workspaces and their ini entries
src/viewport_view.h              where the raster preview is this frame, for overlays and the gizmo
src/optic_models.{h,cpp}         rt_optic_models: every model and placeholder, read from the converter's output
src/mount_library.{h,cpp}        the lazily uploaded mount library
tools/optical_models/            the STEP converter, decals, iris drives and the review renders
assets/optical_models/meshes/    the converted models (committed)
assets/templates/                templates the user saves
src/optic_surface.{h,cpp}        rt_optics: surfaces, apertures, the intersector, normals, curvatures, packing for binding 55
src/optic_glass.{h,cpp}          rt_optics: glasses, the AGF reader, absorption, Ciddor's air
src/optic_coating.{h,cpp}        rt_optics: coatings, the transfer matrix, the presets, the GPU tables
src/optic_library.{h,cpp}        rt_optics: the optical library of glass catalogs and coatings
src/optic_catalog.{h,cpp}        rt_optics: the kinds, their traits, builders, readouts and traceEvents()
src/optic_mesh.{h,cpp}           rt_optics: the raster preview's tessellation
src/optics_ui.{h,cpp}            the glass and coating combos, folders and details, the Optics menu and settings
src/rt_scene_editor_optics.cpp   the Optics folder, the element panel, its gizmo and slot materials
src/length_unit.{h,cpp}          the scene's length unit and the widgets that show lengths in it
shaders/rt/include/crt_optic.glsl           the intersector and the surface records, mirroring optic_surface.cpp
shaders/rt/include/crt_coating.glsl         the coating tables and the transfer matrix, mirroring optic_coating.cpp
shaders/rt/include/crt_optic_material.glsl  the optical interface, absorption and the ABg lobe
assets/glass/bundled.agf         the bundled glass catalog
tests/optics_test.cpp            bin/optics_test
tools/verify/optics_scenes.py    writes assets/scenes/optics_*.gltf
tools/verify/optics_check.py     renders them and checks what each must show
```

## 8. Across plans

### Checks every physoptics plan runs before it is done

Each plan that adds bench UI or objects keeps the bench consistent with the rest of the application by checking these:

1. Its UI goes in through the hooks and the window registry, under **Optical bench**, and nothing outside `scene_capabilities.cpp` and the plan's own files names the capability.
1. With the capability off, a plain scene shows exactly the menus, folders, windows and settings it did before the plan, and renders bit-identical in exact RGB with a fixed seed.
1. Every object kind the plan adds is listed by `opticalBenchDependentObjects()` with its kind, name and an `erase` function. With one in the scene, turning the capability off is refused and names it. **Delete them and turn off** leaves a scene that saves as one that never had the capability, apart from the version.
1. A file from the plan's own build, edited to drop `"capabilities"`, opens with **Optical bench** on and says so.
1. Its recommended settings join `opticalBenchRecommendedSettings()`, listing only settings that differ. None is ever applied without **Apply**, except by the Optical bench template.
1. What the plan adds to a new bench joins `populateOpticalBench()`. The plan's table lists the target contents: the breadboard, a HeNe laser at beam height, a power meter, a beam axis from the laser, and the light integrator.
1. A new window docks in `buildOpticsLayout()`, and in `buildRenderLayout()` if a render scene wants it too. A user's saved layout has no place for a new window, so it opens as a tab in the center the first time. Check both a fresh `imgui.ini` and one saved before the plan.
1. Panels show lengths through `src/length_unit.h`, in the scene's unit, and angles in degrees or microradians. The file stores SI units.
1. The scene file version goes up by one, and a key is written only when it has something to say.
1. This file gets the plan's status row, its section, and any new conventions.

### Open items

- The raster viewport ignores the scene's solid background, so the Optical bench template's black environment shows the viewport's sky gradient. The path tracer renders it black.
- The Optics workspace in the laser optics plan has a top-down viewport beside the main one. That needs a second raster view: its own camera, draw image and `ViewportView`.
- The raster preview has no hook for a capability's geometry. [physoptics-bench-and-mounts.md](plans/physoptics-bench-and-mounts.md) adds one for the breadboard and mounts, and must add what it draws to `RasterKey`, or the paused preview keeps showing the old picture.
- No detector exists, so the checks of the optics plans that need one (a 50:50 split measured as power on two detectors, a dichroic's split by wavelength) render images instead. [physoptics-detectors.md](plans/physoptics-detectors.md) runs them.
- The design wavelength is one scene setting. When lasers exist, a monochromatic scene may want it to follow the laser's line.
- The coating tables keep power, not phase. [pbrt-polarization.md](plans/pbrt-polarization.md) adds the phases, and the polarizing cube's s and p split, which the renderer averages today.
- In float, about 1 in 10^6 rays crossing an element's edge ends inside the body, against none in double: float's limit at a rim, measured by `bin/optics_test`.
- An off-axis parabola's thin edge is 0.75 of its diameter. At 1 in and 90 degrees that gives 30.95 mm, where MPD149's drawing gives 31.75 mm. A per-part table would match each drawing.
- Whether a capability may hide UI its scenes do not need, such as the environment map controls in a bench scene, is open. So far capabilities only add, and hiding is a workspace's job.

## 9. Sources

The [laser optics plan](plans/physoptics-laser-optics.md) and its sources; the [scene capabilities plan](plans/completed/physoptics-scene-capabilities.md) and its survey of OpticStudio, COMSOL, Unreal, Blender and FreeCAD; the Mitsuba 3 documentation, "Variants" and "Scene format"; the Dear ImGui docking branch, `DockBuilder` and the `.ini` settings handlers. For the optics: the sources of the [optical surfaces](plans/completed/physoptics-optical-surfaces.md), [optical materials](plans/completed/physoptics-optical-materials.md) and [optical elements](plans/completed/physoptics-optical-elements.md) plans, chiefly Macleod 2010, *Thin-film optical filters*; Ciddor 1996, *Refractive index of air*; the Zemax OpticStudio manual's "Glass catalogs"; Stover 2012, *Optical scattering*; and the Thorlabs product pages and drawings.
