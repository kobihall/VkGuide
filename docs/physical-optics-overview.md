# Physical optics, end to end

*A readable account of the laser optics simulator as it is built so far: what exists, how the parts fit, and the rules that keep them consistent. What is still to come, and in what order, is in the [laser optics plan](plans/physoptics-laser-optics.md); the file-by-file detail is in `docs/codebase-map.md` §8.27, §8.28, §8.30, §8.35 and §8.36. Every physoptics plan updates this file when it lands: its row in the status table, its section, and the cross-plan list at the end.*

---

## 1. The shape of it

The application is one renderer with one scene model. A laser table is not a second application or a mode the renderer switches into. It is a set of object kinds, windows and settings that a scene turns on as the **Optical bench** capability. A scene of Sponza under an HDR sky never shows a breadboard, and a bench scene renders through the same path tracer, the same materials and the same scene file as every other scene.

The optics have two consumers, which read the same data:

- A CPU solver traces one chief ray per beam segment, in double precision, fast enough to run on every gizmo drag. It gives the beam's path and its Gaussian size everywhere.
- The GPU path tracer traces millions of light paths from the lasers. It gives what detectors and cameras record, and the stray light, ghosts and clipping a single beam misses.

The solver does not exist yet; light tracing does, with lasers to start its paths and detectors to measure where they land. The bench has its place in the scene, its workspace and its template, and the optics themselves: thirteen kinds of element, each an exact surface prescription the path tracer traces, made of catalog glasses and coated with thin-film stacks or metals. Each optic stands in its Thorlabs mount on a post, a post holder and a clamping fork, on a breadboard with holes on a 25 mm grid, and every part of that stack is geometry the preview draws and the tracer hits.

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
| [physoptics-bench-and-mounts](plans/completed/physoptics-bench-and-mounts.md) | 2026-10-30 | The breadboard, the stack of fork, holder, post and mount under every optic, placement on the table, the fork's hole, the adjusters' kinematics, and the bench in the preview and the tracer |
| [physoptics-laser-sources](plans/completed/physoptics-laser-sources.md) | 2026-11-02 | The laser: a Gaussian beam of any M², astigmatic or round, as an element and as a light that light tracing starts paths at; the wave reference, the beam checks, and five of the geometric verification scenes |
| [physoptics-detectors](plans/completed/physoptics-detectors.md) | 2026-11-04 | Power meters, camera sensors, beam profilers, alignment cards, screens, spectrometers, beam dumps and probe planes as sensors a render targets; light tracing's stage 26, which deposits on them; camera paths from a sensor's pixels; readings with ISO 11146 widths in the Raytraced Output window; the batch mode's readings; the verification checks on detectors |

Every other physoptics plan is unbuilt. The laser optics plan's roadmap gives their order: the beam axis and the Gaussian beam solver come next.

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
- `rasterGeometry`: geometry the raster preview draws beside the scene's own objects. What it draws must move `RasterKey` when it changes, or a paused preview keeps the old picture.
- `workspace`: a workspace offered beside **Render**.
- Windows: an entry of the window registry whose `capability` is this one. It is drawn, listed and saved in workspaces only while the scene has the capability.

**Optical bench** fills most of these. Its `sceneFolders` and `addMenu` are the **Optics** folder, with the breadboard's row, and **+ Add** → **Breadboard** and **Optic**. Its `menus` is the **Optics** menu (**Bench Settings...**, **Load Glass Catalog...** and **Import Coating Curve**), its `renderSettings` the **Optics** section, and its `materialFolders` and `materialDetails` the **Glasses** and **Coatings** folders. Its `rasterGeometry` draws the breadboard, posts and mounts, and its window is **Bench Settings**. Its `sceneStatus` warns when optical models are placeholder boxes, which used to show in every scene, and its `workspace` is **Optics**. Code outside `scene_capabilities.cpp` asks `capabilityEnabled()` or walks the hooks, and never names a capability, except the plan that owns the part.

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

Nothing changes until **Apply**. Opening a file that already has the capability shows no note, since its settings are the file's to keep. **Optical bench** recommends a fixed seed, millimeters as the scene's length unit, a 1 mm near clip plane for the **Viewport**, and **Lights** wavelength sampling, so a laser's line a few picometers wide is found in the spectral modes. A scene with a laser but another integrator shows a note in **Render Settings** with a **Use Light tracing** button rather than a recommendation, since the integrator is the scene's choice. The breadboard is an object, not a setting, so the template adds it and no recommendation does.

### Templates

**File** → **New Scene** makes the Render scene template: one camera and the default settings, as it always has. **File** → **New from Template** lists the others. The built-in templates are code, `BUILTIN_TEMPLATES`, so they always match the current defaults and payload. Each is `newScene()` followed by its `populate` function. The Optical bench template turns the capability on, applies its recommended settings (a 1 mm near plane for the **Viewport** among them), adds the 300 × 300 mm breadboard, puts a camera 0.55 m above it looking at the beam height, sets an empty, black environment, and stands the HNL050LB and a power meter 200 mm apart on the beam height, with light tracing to render them. A new bench measures 5 mW the moment it renders.

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

`MountLibrary` reads and uploads a model the first time something asks for it, outside the scene's models, so a mount is never saved with a scene or listed under **Models**. A model is read when an optic first stands in it, and the bench (§7) places its parts.

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

Every index the tracer uses is absolute. A catalog's formula is relative to air, as Schott's data and the AGF format are, so a catalog glass's absolute index is its formula times Ciddor's air (`OpticGlass::relativeToAir`, `glassIndex()`); in air a lens then refracts by the catalog's own ratio, and in vacuum by the absolute index. The spectral builds read it from `absoluteIndexRef()`, a table every nanometer from 200 to 2500 nm. Until 2026-11-02 the air was divided out twice, which made every lens 0.05 % long; the laser checks' telescopes found it.

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

The panel shows the parameters in the scene's length unit, the readout, the slots' coatings, the ports' directions, and the mount (§7). In the raster preview a planar mirror at a preset size draws its PF blank model, and every other element its tessellated prescription.

`tools/verify/optics_scenes.py` writes the scenes the checks render to `assets/scenes/tests/optics/`: `optics_furnace` (a lens in a white furnace), `optics_nd_filter`, `optics_cube_split`, `optics_lens_focus`, `optics_prism` (a camera looking through an F2 prism at a slit), and `optics_bench`, one element of each kind on a bench. `tools/verify/optics_check.py` renders the first five.

## 7. The bench and the mounts

### The breadboard

The breadboard is a scene object, saved in the file's `"bench"` with its size and the bench's settings, the beam height and snapping. It is Thorlabs' metric board as `Breadboard-B3030A.step` models it: Ø5 mm M6 holes 5 mm deep on a 25 mm pitch with a 12.5 mm border, a top skin and a bottom skin 60 mm apart, in brushed stainless steel, and black side walls between them. `buildBreadboardMesh()` makes it at any size in 25 mm steps, about 62 triangles a hole. A side of $n$ holes has its hole $\lfloor (n - 1)/2 \rfloor$ at the origin, so the default 300 mm plate spans $-137.5$ to $162.5$ mm, with the holes at

$$\mathbf{h}_{ij} = (25\,i,\; 0,\; 25\,j)\ \text{mm}, \qquad i, j \in \{-5, \dots, 6\}$$

### The stack

Every mounted optic stands on the same stack, each layer placed by the mates of [§5](#5-the-optomechanical-models)'s models: a CF125C/M clamping fork on the table, a PH50E/M post holder in it, a Ø12.7 mm post, the mount on the post, and the optic in the mount. An optic is placed by its origin (`opticOrigin()`), the point its optics are defined by: the center of a mirror's front face, and the center of anything light goes through, on its axis (an equilateral prism's centroid). The optic's `MountPose` (`src/optic_mount.h`) holds the four degrees of freedom the stack allows, at that origin: its place $\mathbf{o} = (o_x, o_z)$ on the table, the yaw $\psi$ about a vertical through it, its height $h$, and the fork's angle and reach. Write $T(\mathbf{v})$ for a translation and $R_y(\psi)$ for a turn about $+y$. With the origin $\mathbf{c}$ and the mount's post tap $\mathbf{t}$ in the mount's frame, the post stands at $\mathbf{p}$ with its top at $y_{\text{top}}$:

$$\mathbf{p} = \mathbf{o} - \big[R_y(\psi)\,(\mathbf{c} - \mathbf{t})\big]_{xz}, \qquad y_{\text{top}} = h - (c_y - t_y), \qquad M_{\text{mount}} = T(p_x, y_{\text{top}}, p_z)\; R_y(\psi)\; T(-\mathbf{t})$$

The post therefore moves when the mount or the optic changes, and the optical surface does not: a thicker mirror keeps its face. The optic's body frame is $M_{\text{mount}}\, P\, S$, where $S$ seats the body in the mount and $P$ is the motion of the part the seat rides on (a kinematic plate, a rotor). `opticWorldTransform()` (`src/optic_bench.h`) is the one function that composes it. The preview, the tracer and, later, the beam solver read the transform it gives, so they cannot disagree about where an optic is. The post always runs from the holder's mid-depth up to the mount, so its length follows the height, and the height is at least the post's top at the holder's top face: 80.1 mm for a KM100.

How a body sits in its mount follows the body frames of §6. A mirror faces the mount's front, its back on the seat. A round transmissive optic, a plate and an iris are turned half a turn about $y$, since their light travels along their own $+z$, through the mount from its front. A prism stands on its cap with its body $x$ up, so that it turns and disperses in the table's plane.

**Free placement**, and an optic whose mount is **None**, have all six degrees of freedom, a position and an orientation. A free optic keeps its mount around it but has no post, holder or fork. Turning free placement off keeps the origin where it is, turned by the optic's axis's horizontal part, and stands a post behind it.

### Placing and the fork

On the bench the gizmo translates in $x$, $y$ and $z$ and turns about $y$, at the optic's origin. A drag snaps the origin to the nearest hole when **Snap to holes** is on, and the post lands wherever that puts it; holding Alt inverts it for the drag. The fork bolts into a hole within its slot's reach, $26.797 \le |\mathbf{h} - \mathbf{p}| \le 58.293$ mm:

```text
choose fork(post p, previous angle a0, other stacks):
    C <- every hole h of the table with |h - p| in the slot's reach
    sort C by: (h - p) along an axis first, then |h - p|, then the angle from the nearest axis, then the angle from a0
    for h in C: if the fork's outline toward h clears every other fork and post holder: return h
    return C[0], blocked              // drawn red, and the panel says why
```

The hole 25 mm from a post is inside the slot's near end, so a post on a hole bolts to the one 50 mm along an axis. The editor refits every fork each frame whose bolt has left its hole, as it does when a post moves under a fixed origin, and every fork another stack has been put on, in order, so two forks never trade places.

### The adjusters

A kinematic mount's plate rests on its pivot ball $\mathbf{c}_0$ and two adjuster tips. Turning adjuster $i$ by $n_i$ revolutions advances its tip along the screw's axis $\mathbf{s}_i$ by $n_i\,\ell_i$, where the effective pitch $\ell_i$ is Thorlabs' angle per revolution times the modeled lever. With $\mathbf{a}_X$ the unit vector from the pivot to Y's contact and $\mathbf{a}_Y$ to X's, the plate turns about the pivot by

$$R(\alpha, \beta) = \mathrm{Rot}(\mathbf{a}_X, \alpha)\; \mathrm{Rot}(\mathbf{a}_Y, \beta), \qquad \mathbf{s}_i \cdot \big(R\,\mathbf{r}_i - \mathbf{r}_i\big) = n_i\,\ell_i, \quad \mathbf{r}_i = \mathbf{c}_i - \mathbf{c}_0$$

which Newton's method solves in double. One adjuster alone turns the plate about the axis through the pivot and the other contact. The optic rides on the plate, so its datum moves on an arc as it tilts, which is how a real mount walks a beam while steering it. A rotation mount's rotor turns about its axis, and an iris's lever and blades follow its opening through the model's `aperture` drive.

### In the preview and the tracer

`BenchScene` (`src/bench_scene.h`) uploads the breadboard, the post and each mount model a stack uses, and draws them through the capability's `rasterGeometry` hook. In the tracer every part is a mesh instance of the part's one BLAS, in its finish without decals, and every post is an exact capped cylinder. An optic's faces lie exactly in the mount faces they rest on, so the preview draws optics with a small depth bias toward the camera (1 step and a slope factor of 0.25), and they win the tie instead of flickering with their mount. A larger bias let glass seen nearly edge on show through its mount's lip from a meter away. The raster pass is reverse-Z, so the 1 mm near plane a bench uses keeps a depth step of about 1e-7 of the distance. Each carries a `BenchTag`, the optic's id and the part, so that a later plan can name what a beam clipped.

## 8. Lasers

A laser is an element (`OpticKind::Laser`) and a light. Its body is one disk, its output aperture, with the `laser_aperture` material; its housing (a laser head, a diode module or a fiber collimator) is a parameter the **Viewport** outlines with lines, and an arrow shows the beam in the color of its wavelength, but nothing the tracer sees. It is placed freely and never mounted. While the bench snaps, its output goes on the nearest hole and its beam along the grid, and the gizmo slides it, raises it and turns it about the vertical in quarter turns.

Its beam is two independent transverse axes, each a Gaussian beam of its own waist $w_0$, waist position and $M^2$, so a diode's elliptical, astigmatic beam is one laser. Along one axis the radius is $w(z)^2 = w_0^2 + \theta^2 (z - z_0)^2$ with $\theta = M^2 \lambda / (\pi w_0)$. The panel takes the beam either way: the waist, its position and $M^2$, or a data sheet's output diameter and full divergence, which `fitLaserAxis()` turns back into them. The HNL050LB preset is the first, from the user's data sheet: 632.8 nm, 5 mW, 0.81 mm and 1.0 mrad, which make $M^2 = 1.005$ with the waist at the output. The spectrum is a Lorentzian or Gaussian line of a given width.

As rays, the beam is its Wigner function: per axis a position at the waist and a slope, each normal, the position carried along the slope to the aperture. That distribution is positive for a Gaussian beam and free space only shears it, so the rays reproduce the beam's second moments through any paraxial system, focus included. What rays cannot carry is diffraction at a hard edge.

Only light tracing starts paths at a laser: next-event estimation never samples one (its light leaves in a cone of milliradians), so lasers sit after every other light in the list, outside both next-event tables, with a table of their own (`crt_laser.glsl`). The path tracer sees a laser's aperture glow when it looks into it, with the beam's radiance, and nothing its beam lights. Exact RGB gives a laser the display color of its wavelength, scaled so its channels average to 1, so a detector's channel mean reads watts. The design wavelength follows the scene's one laser.

### Measuring a beam on a screen

Before detectors, `tools/verify/beam_probe.py` measured a beam on a white Lambertian screen seen square-on by a pinhole camera on the beam's axis. The camera has no geometry, so it can stand in the beam. The image maps to the screen linearly, so its sum is the power and its second moments are the beam's. The checks now read detectors (§9); `tools/verify/detector_check.py` keeps the screen measurement as a cross-check, with an alignment card as the screen.

### The wave reference

`tools/verify/wave_optics.py` propagates the same beam by diffraction: diffractsim's angular spectrum method on a grid, with Matsushima's band limit, a beam of $M^2 > 1$ as a Gaussian Schell-model beam (an incoherent sum of Hermite-Gaussian modes with geometric weights, whose Wigner function is the rays' Gaussian), astigmatism, circular apertures, any mask and thin lenses. In free space the rays and the waves must agree to within the rays' noise; past a mask they part, and the comparison images show where. Its self-test checks it against the closed forms and, under diffractsim's venv, against diffractsim itself.

## 9. Detectors

A detector is a sensor, as a camera is: **Add** → **Sensor** lists both, the Scene panel's **Sensors** folder holds both, and Render Settings' **Render target** picks one of them. Its body is one flat sensor at z = 0 facing the light that arrives along its +z, placed on the bench and moved as an optic is. Eight kinds, with plain default sizes:

- A **power meter** (10 mm), a **beam dump** (10 mm) and a **spectrometer** (3 mm entrance, 350 to 700 nm in 0.1 nm bins) absorb, and read power, or a spectrum, with no image.
- A **camera sensor** and a **beam profiler** (10 × 10 mm, 512 × 512 pixels) absorb, and record an image. Either can be a **physical sensor** instead: its own pixels and pitch set its size, and the film may record fewer of them.
- An **alignment card** (85.6 × 54 mm, white 0.8) and a **screen** (white 1.0) scatter what they record as pbrt's diffuse, so a camera sees the spot on them.
- A **probe plane** records the light crossing it and is not geometry: no ray stops at it, and neither camera paths nor shadow rays see it.

A **sensor stack** is one detector copied evenly from a start point to an end point, as one sensor slid along a translation stage would read a beam at each stop. Every member has the same size and film, faces along the axis unless you turn **align normal to stack axis** off, and is an ordinary detector with its own readings. The Raytraced Output window lists a stack once, with a slider that picks the member it shows, and plots a reading of every member along the axis: the beam's radii with ISO 11146-1's caustic fit (its waist, where it is and M²), its centroid, peak irradiance or power, or a spectrometer's peak wavelength. The verification scripts measure a beam through a focus on stacks of probe planes.

### Recording

Under light tracing, stage 26 runs after kernel 02 in every bounce while the scene has a detector. For each light path it takes the segment kernel 02 traced, and where the segment ends on a sensor's front or crosses a probe plane's, it adds the path's power to that pixel of the detector's film. A light path carries the power of the laser it left over its chance of being picked, so a pixel's sum over the paths emitted is watts. Each frame's sums are kept in copies and folded into Neumaier-compensated running sums, so a reading keeps improving past 10^9 paths. Exact RGB films have three channels whose mean is the power; spectral films have one, the lanes integrated over wavelength (exact for hero wavelengths and box bands; a monochromatic render reads W/nm). A spectrometer bins its lanes by wavelength.

Light tracing records the target, and every member of the target's sensor stack, and with **render all sensors** on every detector in the scene at once. A camera target records no detector unless it is on, and the batch mode's `--detectors` turns it on. With a detector as the target there need be no camera: the render emits **Light paths per sample** and connects nothing to a camera. The path tracer renders a detector target from its own pixels instead, each camera ray cosine-weighted over the sensor's front, so a pixel reads π times its mean radiance, its irradiance. That is the bare form of the realistic camera to come, a camera sensor behind a lens assembly.

### Reading

The Raytraced Output window shows the target. While it shows a detector it adds the readings below the picture: the power, its standard error from the per-frame readings, the paths that reached it; for a detector with an image, its film with a cursor readout, the centroid and D4σ widths (ISO 11146-1, with ISO 11146-3's iterative integration area and Sheppard's correction for the pixels' size), the peak irradiance, encircled power and lineouts; a spectrometer's spectrum. **Showing** picks any other detector the render recorded. `--detectors <dir>` writes the same readings and the films for scripts.

### What the checks found

On the HNL050LB, a power meter reads 5.0001 mW of 5 mW, and a probe plane matches w(z) to 0.1 %. An ideal 50:50 cube splits the beam within the noise the meters report, and over seeds that noise is what they report. A profiler at an f = 500 mm lens's focus reads ABCD's waist to 0.03 %. A diffuse wall sends each meter what a Lambertian spot would, on both sides. A camera sensor before a glowing square reads the closed form's power both from its pixels and by light tracing. The checks also found two bugs of their own: a float pixel summing a million equal terms in one frame lost 0.1 % of them, and pixel-center moments widened a beam by pitch² / 12. The frame copies and Sheppard's correction fix them.

## 10. Where things live

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
src/optic_mount.h                MountPose, FineAdjust and MountKind: how an optic stands, saved with it
src/optic_bench.{h,cpp}          rt_optics: the breadboard, the stack, the fork rule, placement and the adjusters
src/bench_scene.{h,cpp}          the breadboard, posts and mounts on the GPU, in the preview and in the tracer
src/optic_laser.{h,cpp}          rt_optics: a laser's beam from its parameters, the data sheet fit, the housings, the design wavelength
src/light_sampling.{h,cpp}       rt_light: the laser's emitted rays and densities (LaserEmitter), beside every other light's
shaders/rt/include/crt_laser.glsl           the laser's beam, its rays and radiance, light tracing's pick
shaders/rt/include/crt_optic.glsl           the intersector and the surface records, mirroring optic_surface.cpp
shaders/rt/include/crt_coating.glsl         the coating tables and the transfer matrix, mirroring optic_coating.cpp
shaders/rt/include/crt_optic_material.glsl  the optical interface, absorption and the ABg lobe
assets/glass/bundled.agf         the bundled glass catalog
tests/optics_test.cpp            bin/optics_test
tools/verify/optics_scenes.py    writes assets/scenes/tests/optics/optics_*.gltf
tools/verify/optics_check.py     renders them and checks what each must show
tools/verify/beam_probe.py       a screen and a camera to measure a beam, ABCD and the waist fit
tools/verify/wave_optics.py      the wave reference
tools/verify/laser_scenes.py     writes assets/scenes/tests/lasers/laser_*.gltf
tools/verify/laser_check.py      renders them against the closed forms and the wave reference
tools/verify/optics_verification.py  the geometric verification scenes, assets/scenes/tests/optics_verification/*.gltf
src/optic_detectors.{h,cpp}      rt_optics: the detectors' sensors and films, and the ISO 11146 measurements
src/rt_detectors.{h,cpp}         the detector films on the GPU: records, fold, readbacks, pictures, --detectors
src/detectors_ui.{h,cpp}         the readings the Raytraced Output window adds while it shows a detector
shaders/rt/2601_deposit_detector.comp  stage 26: light paths deposit on the detectors they reach
shaders/rt/include/crt_detector.glsl   the pixel a path lands in, and what it adds there
shaders/rt/light/detector_fold.comp    the per-frame fold into compensated sums, and the frame totals
shaders/rt/light/detector_view.comp    a film as a picture for the Raytraced Output window
tools/verify/detector_scenes.py  writes assets/scenes/tests/detectors/detector_*.gltf
tools/verify/detector_check.py   renders them and checks what the detectors read
```

## 11. Across plans

### Checks every physoptics plan runs before it is done

Each plan that adds bench UI or objects keeps the bench consistent with the rest of the application by checking these:

1. Its UI goes in through the hooks and the window registry, under **Optical bench**, and nothing outside `scene_capabilities.cpp` and the plan's own files names the capability.
1. With the capability off, a plain scene shows exactly the menus, folders, windows and settings it did before the plan, and renders bit-identical in exact RGB with a fixed seed.
1. Every object kind the plan adds is listed by `opticalBenchDependentObjects()` with its kind, name and an `erase` function. With one in the scene, turning the capability off is refused and names it. **Delete them and turn off** leaves a scene that saves as one that never had the capability, apart from the version.
1. A file from the plan's own build, edited to drop `"capabilities"`, opens with **Optical bench** on and says so.
1. Its recommended settings join `opticalBenchRecommendedSettings()`, listing only settings that differ. None is ever applied without **Apply**, except by the Optical bench template.
1. What the plan adds to a new bench joins `populateOpticalBench()`. The plan's table lists the target contents: the breadboard, a HeNe laser at beam height, a power meter, a beam axis from the laser, and the light integrator. All but the beam axis are there.
1. A new window docks in `buildOpticsLayout()`, and in `buildRenderLayout()` if a render scene wants it too. A user's saved layout has no place for a new window, so it opens as a tab in the center the first time. Check both a fresh `imgui.ini` and one saved before the plan.
1. Panels show lengths through `src/length_unit.h`, in the scene's unit, and angles in degrees or microradians. The file stores SI units.
1. Anything that stands on the bench is placed through `opticWorldTransform()` or a stack of its own built the same way, never by a free transform the panel edits directly.
1. The scene file version goes up by one, and a key is written only when it has something to say.
1. This file gets the plan's status row, its section, and any new conventions.

### Open items

- The raster viewport ignores the scene's solid background, so the Optical bench template's black environment shows the viewport's sky gradient. The path tracer renders it black.
- The Optics workspace in the laser optics plan has a top-down viewport beside the main one. That needs a second raster view: its own camera, draw image and `ViewportView`.
- Nothing reads the bench's tags yet. The beam solver is to name a clipping part, and guided tracing's **Separate stray light** to count a path that strikes a mount as stray light on a detector.
- Stacks other than forks do not collide: two post holders in neighboring holes overlap.
- Mounts trace in their plain finishes: the decals are the preview's alone, since the path tracer's texture array holds 512 × 512 layers.
- Detectors record under light tracing, as lasers render only there; the path tracer renders only the target, from its pixels. Their housings are outlines, so a beam that misses a sensor passes its housing.
- The RGB Gaussians mode cannot read a detector's power in watts: its three bases are no partition of unity over a narrow line, and a 632.8 nm laser reads 60 % of its power. Exact RGB, hero wavelengths and box bands read it exactly.
- A dichroic's split by wavelength, which the optical materials plan wanted on detectors, waits for a two-line source scene ([physoptics-verification-scenes.md](plans/physoptics-verification-scenes.md), `dichroic.gltf`).
- A laser's light renders only under light tracing. The light vertex cache and pbrt 3's BDPT need their MIS taught that a laser cannot be sampled by next-event estimation.
- The scene file clamps a camera's field of view to at least 1 degree, so a check that resolves a 20 µm focus puts its camera a few millimeters from the screen.
- The coating tables keep power, not phase. [pbrt-polarization.md](plans/pbrt-polarization.md) adds the phases, and the polarizing cube's s and p split, which the renderer averages today.
- In float, about 1 in 10^6 rays crossing an element's edge ends inside the body, against none in double: float's limit at a rim, measured by `bin/optics_test`.
- An off-axis parabola's thin edge is 0.75 of its diameter. At 1 in and 90 degrees that gives 30.95 mm, where MPD149's drawing gives 31.75 mm. A per-part table would match each drawing.
- Whether a capability may hide UI its scenes do not need, such as the environment map controls in a bench scene, is open. So far capabilities only add, and hiding is a workspace's job.

## 12. Sources

The [laser optics plan](plans/physoptics-laser-optics.md) and its sources; the [bench and mounts plan](plans/completed/physoptics-bench-and-mounts.md); the [scene capabilities plan](plans/completed/physoptics-scene-capabilities.md) and its survey of OpticStudio, COMSOL, Unreal, Blender and FreeCAD; the Mitsuba 3 documentation, "Variants" and "Scene format"; the Dear ImGui docking branch, `DockBuilder` and the `.ini` settings handlers. For the lasers: the sources of the [laser sources plan](plans/completed/physoptics-laser-sources.md), chiefly Siegman 1986, *Lasers*; Bastiaans 1979 on the Wigner function; Gori 1980 and Mandel and Wolf 1995 on Gaussian Schell-model beams; and diffractsim. For the optics: the sources of the [optical surfaces](plans/completed/physoptics-optical-surfaces.md), [optical materials](plans/completed/physoptics-optical-materials.md) and [optical elements](plans/completed/physoptics-optical-elements.md) plans, chiefly Macleod 2010, *Thin-film optical filters*; Ciddor 1996, *Refractive index of air*; the Zemax OpticStudio manual's "Glass catalogs"; Stover 2012, *Optical scattering*; and the Thorlabs product pages and drawings.
