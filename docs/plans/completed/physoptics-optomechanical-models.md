# Optomechanical models

The raster viewport should show a table that looks like the lab: a KM100 holding a mirror on a post in a post holder clamped to the breadboard, not a disk floating in space. The user downloaded the Thorlabs STEP files for every mount, holder, iris and mirror blank into `assets/optical_models/`. STEP is a CAD exchange format of exact surfaces, which neither the raster pass nor the tracer can draw, and these files carry detail nobody sees: modeled screw threads, engraved part numbers, springs and bores inside closed bodies. This plan converts each file once, offline, into a cleaned triangle mesh with its parts separated, and records on each model the connection nodes where it mates with others. At run time a fixed model library serves the meshes to the raster preview and their BLASes to the tracer.

*Implemented 2026-10-27, revised 2026-10-28 after the user's review (decals instead of lettering geometry, spring and thread proxies, moving iris blades, the RSP bearings removed). "As built" records where the build departs from the design below, and "Not built" what it leaves out. No plan dependencies: the converter is offline, and the library uses the existing importer path. [physoptics-bench-and-mounts.md](../physoptics-bench-and-mounts.md) assembles the models into mounts. Part of the [laser optics plan](../physoptics-laser-optics.md).*

## Decisions already made

- The models are fixed resources, generated from the optic that uses them and never saved in the scene, like the preview meshes of the analytic shapes (the request).
- The breadboard is not converted. Its STEP file sets the dimensions of a procedurally generated plate ([physoptics-bench-and-mounts.md](../physoptics-bench-and-mounts.md)).
- Lasers and detectors get procedural placeholders now, and this plan lists Thorlabs parts whose STEP files replace them later (the user, 2026-10-18).
- The FP01 plate holder replaces the SFH3 for square filters (the user, 2026-10-18). `Filter_mount-SFH3.step` is not converted.

## What exists today

The 24 files to convert, by the Thorlabs part each file's header names:

| File | Part | Unit |
|---|---|---|
| `Kinematic_mount-KM05T_M.step` | KM05T/M, Ø½ in kinematic mount | mm |
| `Kinematic_mount-KM100.step` | KM100, Ø1 in kinematic mount | inch |
| `Kinematic_mount-KM200.step` | KM200, Ø2 in kinematic mount | mm |
| `Mirror_blank-PF05-03.step`, `PF10-03`, `PF20-03` | mirror blanks, 6, 6 and 12 mm thick | mm |
| `Fixed_mount-LMR05_M.step`, `LMR1_M`, `LMR2_M` | fixed lens mounts | inch |
| `Rotation_mount-RSP05_M.step`, `RSP1_M`, `RSP2_M` | rotation mounts | inch |
| `Prism_mount-KM200PM_M.step` | KM200PM/M kinematic prism platform | inch |
| `Filter_mount-FP01.step` | FP01 plate holder | mm |
| `Iris-ID8_M.step` to `Iris-ID50_M.step` | seven mounted irises | inch (ID8 to ID20), mm (ID25 to ID50) |
| `Post_holder-PH50E_M.step` | PH50E/M post holder | inch |
| `Post_holder_clamp-CF125C_M.STEP` | CF125C/M clamping fork | mm |

`example mirror.FCStd` is a FreeCAD 1.0 assembly of KM05T/M, PF05-03, PH50E/M and CF125C/M, with a grounded joint and three revolute joints. Its part placements are the ground truth for the mates below.

FreeCAD 1.0.2 is installed (`/Applications/FreeCAD.app/Contents/Resources/bin/freecadcmd`), which imports STEP through Open CASCADE with each file's own units, and Blender is installed for review renders. The engine loads `.glb` through fastgltf (`loadGltf()`, `src/vk_loader.cpp`), and `RaytraceBlasCache` builds a BLAS per `MeshAsset`.

## Design

### The converter

`tools/optical_models/convert.py`, run with `freecadcmd`, reads `tools/optical_models/manifest.json` and writes, per model, `assets/optical_models/meshes/<model>.glb` and `<model>.mates.json`. Both outputs are committed, so building the application never needs FreeCAD. The manifest is the only hand-made input:

```json
{"model": "KM100", "step": "Kinematic_mount-KM100.step",
 "frame": {"origin": [...], "x": [...], "y": [...], "z": [...]},
 "parts": {"base": [...solid ids...], "plate": [...], "adjuster_x": [...], "adjuster_y": [...]},
 "remove": {"threads": "auto", "text": [...face ids...], "faces": [...]},
 "materials": {"anodized_black": [...], "stainless": [...], "brass": [...]},
 "mates": {"optic_seat": {...}, "post_tap": {...}, "pivot": {...}, "adjuster_x_tip": {...}, "adjuster_y_tip": {...}},
 "budget": 20000}
```

The steps, each a function of the script:

1. **Import** the STEP with `Part.read`, in millimeters whatever the file's unit, and keep its solids and their AP214 colors.
2. Canonical frame. Move the model into its frame from the manifest: meters, +y up, +z along the optical axis toward the incoming light, origin at the model's primary mate (the optic seat for a mount, the post axis at the base for a holder). The frame is set once per model by hand from the Thorlabs drawing, and checked against the example assembly.
3. Split into parts. A kinematic mount's front plate moves with fine adjustment, its adjusters turn, a rotation mount's rotor turns, an iris's lever turns, and the FP01's jaw slides with the plate thickness. Each such part is its own mesh in the `.glb`, a node with its own transform, so the preview can move it ([physoptics-bench-and-mounts.md](../physoptics-bench-and-mounts.md)).
4. Remove threads. Open CASCADE's defeaturing (`Shape.defeaturing(faces)`, `BRepAlgoAPI_Defeaturing`) removes a set of faces and heals the solid. Thread faces are found automatically: B-spline or swept faces whose bounding box lies inside a cylindrical hole or around a cylindrical shank, with a helical sweep. The hole or shank is healed to a plain cylinder at the thread's pitch diameter. The manifest can name extra faces where the detection misses.
5. Remove text. Engraved and embossed lettering (part numbers, scales, the Thorlabs logo) are clusters of small faces on a planar or cylindrical face. The manifest lists them, a review render shows each candidate cluster, and defeaturing removes them. The removed faces are projected onto their host face and rasterized into a decal texture, 1024² at most, white on transparent, which the mesh's material carries as a base color texture over the anodized black. Engraved scales on rotation mounts keep their graduations this way.
6. **Tessellate** each part with `MeshPart.meshFromShape` at a linear deflection of 0.02 mm and an angular deflection of 15°, with per-face normals smoothed only across edges below 30°.
7. **Remove hidden geometry** with `bin/mesh_visibility`, a small C++ tool on `rt_bvh` (no Vulkan): it casts rays at every triangle from 2562 directions on a sphere around the model, with every part at each end of its motion range, and drops triangles no ray reaches. Springs, bores and pockets inside closed bodies go; the inside of a tapped hole, which a ray can reach, stays.
8. **Decimate** to the manifest's triangle budget with quadric edge collapse, only if the budget is exceeded, preserving part boundaries and sharp edges.
9. **Write** the `.glb` (positions, normals, uvs for decals, indices, one material per finish) and the mates file.
10. Review. `tools/optical_models/review.py`, run with `blender -b`, renders each original and converted model from 26 directions to `build/optical_models/review/`, side by side.

Finishes are a short list mapped from the STEP colors: anodized black (diffuse reflectance 0.04), stainless steel and the posts (conductor, pbrt's named iron or a gray reflectance of 0.55 with roughness 0.3), brass adjusters, and the mirror blank's glass.

### Connection nodes

A mate is a named frame on a model, an origin and three axes in the model's canonical frame, plus parameters where the mate allows motion. These are the mates each model kind carries:

| Model | Mates |
|---|---|
| Kinematic mounts (KM05T/M, KM100, KM200) | `optic_seat` (the seated optic's front vertex and axis), `post_tap` (the bottom tapped hole's axis), `pivot` (the pivot ball's center), `adjuster_x_tip` and `adjuster_y_tip` (the adjusters' ball contacts) with each adjuster's thread pitch and travel |
| Fixed lens mounts (LMR) | `optic_seat`, `post_tap`, the retaining ring's seat |
| Rotation mounts (RSP) | `optic_seat`, `post_tap`, `rotor_axis` with the zero of its scale |
| Prism mount (KM200PM/M) | `prism_seat` (the platform's top face center), `post_tap`, `pivot`, the two adjuster tips |
| Plate holder (FP01) | `plate_seat` (the slot's center line and the plate's face), `post_tap`, the jaw's travel |
| Irises (ID8 to ID50) | `aperture_center` (the blade plane's center and axis), `post_tap`, `lever_axis` with the lever angles at the minimum and maximum opening |
| Post holder (PH50E/M) | `post_entry` (the bore's axis at the top face), `base_contact` (the bottom face center), the bore's usable depth, `thumbscrew` |
| Clamping fork (CF125C/M) | `holder_seat` (where the post holder's base sits), `slot` (the bolt slot's center line and length), `bolt` |
| Mirror blanks (PF05-03, PF10-03, PF20-03) | `front_center` and its axis |

The converter writes mates from the manifest, where each is given either as coordinates read off the drawing or as a reference to a face (the axis of a cylindrical face, the center of a planar one), which the script resolves on the healed solid. A mate given by face reference survives a re-export of the STEP file.

### The model library

`MountLibrary` (`src/optic_models.h/.cpp`) loads every `.glb` in `assets/optical_models/meshes/` at startup into `MeshAsset`s and their mates, outside `VulkanEngine::m_models`, so models are never saved and never appear under **Models**. Its meshes go through `RaytraceBlasCache` once, so every placed mount shares one BLAS per model part. A missing or unreadable file falls back to a placeholder box of the model's nominal size with a warning in the Scene panel, so a scene still opens on a machine without the meshes.

`MOUNT_MODELS` names each model once: its id (`"KM100"`, `"LMR1/M"`, `"ID25/M"`, ...), its file stem, its parts and its mates. [physoptics-bench-and-mounts.md](../physoptics-bench-and-mounts.md) maps each optic kind and size to model ids.

### Placeholders

Procedural meshes in `src/optic_models.cpp`, each with the same mates as a real model would have (`post_tap`, an output or input axis), so swapping in a real model changes no code:

| Placeholder | Shape | Replaced by (suggested Thorlabs part, to confirm with the user) |
|---|---|---|
| Laser head | a box, its output aperture on +z | HNL050LB, 632.8 nm polarized HeNe, with its cylindrical mount |
| Diode module | a cylinder | CPS635R, 635 nm collimated diode module |
| Fiber collimator | a cylinder in a KM100 | F220APC-633 in an AD11F adapter |
| Power meter head | a disk on a stem | S120C photodiode sensor |
| Camera | a box with a C-mount front | CS165MU Kiralux camera |
| Beam profiler | a box | BC207VIS/M camera beam profiler |
| Alignment card | an 85.6 × 54 mm card | VRC2 (visible to IR) or VRC4 viewing card |
| Beam dump | a cylinder | BT610/M beam trap, or LB1/M beam block |

The user downloads the STEP files for the parts they want, the manifest gains their entries, and the converter makes them like the rest.

## Registry, bindings and data

- `tools/optical_models/convert.py`, `review.py`, `manifest.json`; `tools/mesh_visibility.cpp` → `bin/mesh_visibility`.
- `assets/optical_models/meshes/*.glb` and `*.mates.json`, committed.
- `MountLibrary`, `MOUNT_MODELS`, the placeholders (`src/optic_models.*`).
- No binding, kernel or scene-file change.

## Verification

- Every scene renders bit-identical: nothing is drawn or traced until an optic uses a model.
- The converter is deterministic: running it twice gives byte-identical `.glb` files.
- Silhouettes: for each model, depth renders of the original tessellation and the cleaned mesh from 26 directions agree in coverage to 99.5 % of pixels, so cleanup changed nothing visible from outside. `bin/mesh_visibility --compare` reports it.
- Threads: no face of a converted model has a helical sweep, and the review renders show plain bores.
- Mates against the example assembly: placing PF05-03 in KM05T/M at `optic_seat`, KM05T/M on a post in PH50E/M at `post_tap` and `post_entry`, and PH50E/M in CF125C/M at `holder_seat`, reproduces the placements in `example mirror.FCStd` to 0.05 mm and 0.1 mrad.
- Budgets: every model is within its budget, and the full library loads in under a second.

## Order of work

1. The converter for KM100 alone, end to end: frame, parts, mates, tessellation, `.glb`. Check its mates against the example assembly using KM05T/M.
2. Thread removal and `bin/mesh_visibility`.
3. Every model in the table, with review renders.
4. `MountLibrary` and the placeholders.
5. Text decals.

## Open questions

- The triangle budgets: 20 000 per mount and 5 000 per holder are starting points for a laptop GPU drawing 50 elements. The review renders decide whether they are enough.
- Whether rotation mount graduations should be readable in the viewport. The decal makes them visible; whether the preview zooms close enough to read them is a look-and-feel question.

## As built

### The converter

- `tools/optical_models/convert.py` runs with FreeCAD's own Python, `/Applications/FreeCAD.app/Contents/Resources/bin/python`, which imports FreeCAD as a module. `freecadcmd` cannot pass arguments to a script. `--describe` lists a file's solids and faces, `--preview` writes a raw `.glb` of a file, and `--check-example` checks the mates against `example mirror.FCStd`. `decals.py` holds the lettering, chart, atlas and bake steps, and `iris.py` the iris kinematics.
- FreeCAD's command-line importer drops the STEP colors and solid names, so `step_styles.py` reads them from the file's text and pairs each FreeCAD solid with its `MANIFOLD_SOLID_BREP` by the sequence of its face types. Every solid of all 23 files pairs.
- Thorlabs colors most threads' cylinders white instead of modeling the helix. A white cylinder or cone of radius 0.8 mm or more and area over 3 mm² is such a cosmetic thread and takes its solid's finish. CF125C/M's bolt is the exception: its M6 × 1 thread is modeled, as two helical B-spline flanks between slices of the crest and root cylinders.
- Lettering and scale graduations become a decal texture (step 5), but not through defeaturing. Defeaturing one tick of RSP2/M's scale ran for more than 150 s, and rebuilding the faces the lettering is cut into from their wires failed wherever a tick crosses a face's edge or a seam. Instead, the converter works on the tessellation: it moves every vertex of the lettering onto the plane, cylinder or cone it is cut into, merges the lettering's triangles into that face's mesh, and removes the face's interior vertices while the mesh stays within the level's deflection. A vertex shared with another face never moves, so the mesh stays watertight. Lettering is the white faces, and on ID12/M, ID36/M and ID50/M the faces of the color the manifest's `"lettering_colors"` names, which those files print flush on the housing. LMR2/M and CF125C/M have no colors, so `"find_lettering"` finds theirs from geometry: a connected group of at least 3 faces, each under 3 mm across, that borders one analytic face only and lies within 0.3 mm of its surface. It finds 14 glyphs on LMR2/M (0.01 mm deep) and 13 on CF125C/M (0.13 mm deep), and none of the chamfers, counterbores and pin ends, which border several faces. Glyph walls that lean outward by a few micrometers fold when projected; the log counts them, up to 1,129 triangles on RSP05/M, and they are dropped, which leaves an overlap and no hole.
- Springs and the modeled thread become proxies: a capped cylinder in place of the faces it covers, with those faces baked onto its side. A spring's cylinder is fitted to the cylindrical faces inside its coil and replaces the faces that lie mostly inside it. KM200's and KM200PM/M's extension springs keep their hooks, which show on the mounts' backs; they are meshed at level 2 (0.08 mm, 29°). CF125C/M's bolt has a cylinder at the thread's major radius from its head to its tip, named in the manifest.
- The decals of a model share one texture, an sRGB PNG embedded in its `.glb`. Each face that carries one has a chart: a plane's own axes per rectangle, or angle and height on a cylinder or cone. Strips longer than 20 mm are cut into pieces, and the pieces are packed into a square atlas, 256 to 2,048 texels, at 0.04 mm a texel. `bin/mesh_visibility --trace` bakes it. A lettering texel is ink where a ray down onto the face hits the lettering it replaced, and the face's finish elsewhere. A spring or thread texel is the finish, darker where the replaced surface turns away and, for the thread, in its roots. The triangles of a decal face are cut along their chart's rectangles, so that the pieces inside take the texture and the rest keep the plain finish.
- The manifest names solids by index in the file, and gives frames and mates as references to faces: a sphere's center, a cylinder's axis, a plane, or an axis meeting a plane. `format_manifest.py` keeps it compact after a scripted edit.
- Normals: an analytic face takes the surface's exact normal at each vertex, and a free-form face the mean of its own triangles' normals. Projecting a point onto a B-spline costs 12 ms, and a spring has tens of thousands of vertices.
- The budget is met by meshing a solid coarser, not by quadric decimation (step 8). Level k meshes at 0.02 × 2^k mm and 15° × 1.4^k (at most 45°), and the converter coarsens, one level at a time, the solid whose next level saves the most triangles. Open CASCADE's mesher keeps every B-rep edge and the exact normals at any level.
- `bin/mesh_visibility` (`tools/mesh_visibility.cpp`) runs twice. First it decides which faces any view reaches, then which of the kept faces' triangles do. A triangle is tried from a grid of points 0.2 mm apart (4 to 64 of them) along 2,562 icosphere directions, and along the 26 cube directions also from points 0.2 mm apart just inside its edges. It is tried at rest, with each moving part at each end of its range, and with every part a drive moves at the drive's two ends and middle together (642 icosphere directions for the moved configurations). An `"OMV2"` file lists those configurations. Fewer sample points, no cube directions or edge points, or a coarse face pass each removed the floor of a 0.1 to 0.25 mm slit between a rotor and its housing.
- Two silhouette checks run on every model. The first compares the kept triangles with every face at the same levels, so it measures the removal of hidden geometry alone. The second compares the STEP file's own faces at the same levels with the result and counts only rays the result lets through or stops more than 0.5 mm farther: a hole that flattening left. It caught KM200's spring hooks when the proxies first dropped them.
- `review.py` renders with Blender 2.91's Workbench engine, showing the decals. Its sheets are in `build/optical_models/review/`, next to each model's finest tessellation (`<stem>.original.glb`) and decal atlas (`<stem>.decals.png`); `--zoom` magnifies.
- Each `.mates.json` also records `source_frame`, the canonical frame in the STEP file's millimeters, `budget`, and `decals` (the atlas's size and texel).

### Frames, parts and mates

- Every kinematic mount stands the way the user stood KM05T/M in `example mirror.FCStd`: seen from the front, the pivot is at the bottom right, `adjuster_x` at the bottom left and `adjuster_y` at the top right. KM100 and KM200 each have a tapped hole on two faces, so they can also stand turned 90°.
- `optic_seat` is the face the optic's back rests on, not its front vertex. That is a lip in KM100, KM200, the LMRs and the RSPs, and the rear retaining ring in KM05T/M. The bench adds the optic's thickness. Mirror blanks have `back_center` beside `front_center` for this.
- Retaining rings (KM05T/M, the LMRs and the RSPs) are their own part, with a `retaining_ring_seat` and a slide from flush at the front back to the seat. KM05T/M's rings leave 3.81 mm for an optic, less than PF05-03's 6 mm, so with a 6 mm optic the front ring comes out.
- A rotation mount stands on its M4 tap, with its lock screw on top. Its rotor, the back piece that carries the lip (RSP05/M) and the retaining ring turn together. The ring's motion records `"follows": "rotor"`. RSP1/M's and RSP2/M's two needle thrust bearings are removed; they showed only at grazing angles through the gap between rotor and housing (the user, 2026-10-28).
- KM200PM/M's `prism_seat` is on the platform's top face, over the post (the back plate's middle tapped hole, as KM200 stands) and halfway across the platform in front of the plate. Thorlabs gives no seat, so this is a choice.
- FP01 clamps a plate's bottom edge, and the beam passes through the plate above it, along the file's x. `plate_seat` is the fixed wall's face at the slot's floor. The jaw closes a gap of 14.605 mm (Thorlabs: plates up to 14.7 mm) to nothing.
- Each iris file includes the iris's TR75/M post and its set screw. The converter removes the post (`"remove_solids"`), since the bench's procedural post replaces it, and `post_tap` is where the post's top met the housing.
- An iris's blades are each a part, `blade_01` and on in order about the axis, turning about a `blade_NN_pivot` mate on the pin that holds it in the housing. The lever and the ring it turns are the `actuator` part. The mates file's `"drives"` has an `aperture` drive: 17 aperture diameters from Thorlabs' smallest to its largest, and at each the lever's angle and every blade's, which `driveMotionValue()` interpolates. The kinematics: a second pin on each blade rides in a radial slot of the ring, so as the lever turns by φ the blade turns by the angle that moves that pin's polar angle by φ, and the aperture is the largest circle about the axis no blade covers. ID36/M's blades have a round corner in a pocket of the ring instead of a pin, which the converter treats as a slot. The lever's range is where the aperture reaches the published diameters (3doptix.com's listing of Thorlabs' figures). Turning the lever until it struck the housing gave stops symmetric about the modeled pose on every iris, and ID50/M then opened to only 36 mm of its 50.
- CF125C/M's recess is exactly as deep as PH50E/M's base flange (4.699 mm), so the holder's base and the fork's underside share a plane, as the example has them. The fork's foot at the far end reaches 1.016 mm below that plane (`foot_drop`), which the bolt springs flat. The slot's end circles are 26.797 and 58.293 mm from the post's axis, so the bolt cannot reach a hole 25 mm away; the nearest are 35.4 mm (diagonal) and 50 mm.

### Results

- Against `example mirror.FCStd`, all three joints are coaxial to 0.0000 mm and 0.0000 mrad. The axial offsets are the example's free choices: the mirror sits 0.095 mm into KM05T/M's rear retaining ring (the converter's seat is the ring's face), the post shows 23.984 mm between mount and holder, and the holder sits in the fork at 0.0000 mm.
- Every scene renders bit-identical: `all_materials.gltf` and `cornell_box.gltf` at 64 samples, seed 7, have an RMSE of 0 with the library loaded and with its load removed.
- All 23 models pass both silhouette checks, the first at 0.9998 or better and the second at 0.9985 or better, and fit their budgets. Without lettering geometry the library is 297,959 triangles, from 370,207, and every solid of the kinematic and rotation mounts is meshed at the finest level: KM200 is 29,562 triangles, RSP2/M 15,840 from 34,834. Converting all 23 takes 45 minutes; running the converter twice gives byte-identical files.
- The irises reach their published apertures: ID8/M 0.7 to 8 mm over 79° of lever, ID25/M 1 to 25 mm over 94°, ID50/M 2 to 50 mm over 94°. ID36/M needs 148°, 107° of it opening, to reach 36 mm, which a real lever does not turn: its pocket is not the slot the model assumes.
- `bin/optic_models_test` reads the whole library and decodes its textures in 0.2 to 0.4 s. The app reads nothing at startup. Read and uploaded on first use, KM100 took 0.010 s to read, RSP2/M 0.039 s and ID25/M 0.009 s. Loading every model at startup took 1.8 s in a Debug build, because each texture and each of the irises' 88 blade meshes uploads in its own immediate submit.

### The library

- `src/optic_models.h/.cpp` are the static library `rt_optic_models`, with no Vulkan: `mountModels()` (the plan's `MOUNT_MODELS`), the readers, the placeholders and the fallback boxes. It decodes each model's decal texture with its own static copy of stb_image, at -O2 in every build. `tests/optic_models_test.cpp` (`bin/optic_models_test`) checks every model's mates, frames, normals, budget, drives and uvs, the fallback boxes' mates, and the load time.
- A `MountSurface` that carries the decals is `textured`, with a uv per vertex, and `MountModel::decalPixels` holds the texture. A `MountMotion` with a `drive` follows a `MountDrive`.
- `MountLibrary` (`src/mount_library.h/.cpp`) reads and uploads a model when it is first needed, not at startup (the user, 2026-10-28). `open()` only checks that each converted model's two files exist, so the **Scene** panel still warns about a missing one; `model(id)` reads a model on its first call and `meshes(id)` uploads it on its first call. It turns each model into an `ImportedModel`, a mesh per part with a surface per finish, and uploads it through `loadImportedModel()`. Anodized black is Lambertian 0.04, steel and brass the glTF metal at roughness 0.3, a blank pbrt's glass. A textured surface's material has a white albedo and the decal texture, which holds the finish's color around the decals. `ImportedImage::rgba8` carries an image already decoded.
- The budgets the first review settled on stay: 40,000 triangles for the kinematic, prism and rotation mounts, 20,000 for FP01 and the irises, 10,000 for the fixed mounts and PH50E/M, 5,000 for CF125C/M and 2,000 for a blank. Without lettering and springs no mount but KM200PM/M (35,398) comes near its budget; ID50/M, whose moving blades show more of it, meshes nine solids coarser, three of them at 0.16 mm or more.

## Not built

- BLASes through `RaytraceBlasCache`. That cache builds from the scene's `RaytraceMeshData`, and until an optic places a model there is nothing to build; [physoptics-bench-and-mounts.md](../physoptics-bench-and-mounts.md) adds the library's meshes when it places them.
- The path tracer's textures. `RaytraceTextureArray` holds every texture at 512 × 512 and rebuilds from `m_models` only, so when the bench places a model, its atlas, 1,024 or 2,048 texels for most mounts, reaches the tracer at a quarter or less of the preview's resolution.
- The rotation mounts' scale zero is where the model leaves it. Finding the graduation that reads zero needs the lettering read, which the converter does not do.
- Batched uploads. A model's textures and meshes each wait on their own submit when it is first uploaded.

## Pending answers

- Adjuster thread pitch. The files model no adjuster threads, so the converter cannot measure them. KM100, KM200 and KM200PM/M carry 1/4"-80 (0.3175 mm), which with KM100's modeled lever of 37.7 mm gives 8.4 mrad per revolution against Thorlabs' 8 mrad. KM05T/M has none. All four are marked `"to confirm"`.
- Adjuster travel: ±2.5 mm (±1.5 mm for KM05T/M) is a guess from Thorlabs' ±4° range, used only for the visibility poses.
- The fixed mounts' and FP01's front: the converter faces the retaining ring, and FP01's free plate face, toward the light. Both mounts are symmetric enough that either way works.
- The texture size the tracer should get, above.

## To investigate

The user, 2026-10-28, will look at these.

- A seam line on the proxy cylinders. In Blender's Workbench close-ups a thin dark line runs along each spring's cylinder, where its texture wraps from one edge of its atlas rectangle to the other. The texture itself has no line there, and the split leaves no gap in the mesh: the triangles keep their area exactly. The likely cause is a mipmap level chosen across the jump in uvs at the seam. Not yet checked in the app's raster preview or the path tracer.
- The irises' motion. The lever's range is solved from Thorlabs' published apertures, not from a stop in the files, since turning the lever until it struck the housing gave stops symmetric about the modeled pose on every iris. ID36/M needs 148° of lever, 107° of it opening, to reach 36 mm, which a real lever does not turn: its blades have a round corner in a pocket of the ring, which the converter treats as a radial slot. The drive table is 17 samples, linear between them, and the converter does not check that the blades clear one another as they turn.

## Sources

The Open CASCADE Technology documentation, `BRepAlgoAPI_Defeaturing` and `BRepMesh_IncrementalMesh`; the FreeCAD 1.0 Python API for `Part`, `MeshPart` and the Assembly workbench; Garland and Heckbert 1997, *Surface simplification using quadric error metrics*; the Khronos glTF 2.0 specification; Thorlabs drawings for every part in the tables above.
