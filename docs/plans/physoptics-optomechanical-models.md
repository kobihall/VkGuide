# Optomechanical models

The raster viewport should show a table that looks like the lab: a KM100 holding a mirror on a post in a post holder clamped to the breadboard, not a disk floating in space. The user downloaded the Thorlabs STEP files for every mount, holder, iris and mirror blank into `assets/optical_models/`. STEP is a CAD exchange format of exact surfaces, which neither the raster pass nor the tracer can draw, and these files carry detail nobody sees: modeled screw threads, engraved part numbers, springs and bores inside closed bodies. This plan converts each file once, offline, into a cleaned triangle mesh with its parts separated, and records on each model the connection nodes where it mates with others. At run time a fixed model library serves the meshes to the raster preview and their BLASes to the tracer.

*Not implemented. No plan dependencies: the converter is offline, and the library uses the existing glTF loader. [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md) assembles the models into mounts. Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

- The models are fixed resources, generated from the optic that uses them and never saved in the scene, like the preview meshes of the analytic shapes (the request).
- The breadboard is not converted. Its STEP file sets the dimensions of a procedurally generated plate ([physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)).
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
3. Split into parts. A kinematic mount's front plate moves with fine adjustment, its adjusters turn, a rotation mount's rotor turns, an iris's lever turns, and the FP01's jaw slides with the plate thickness. Each such part is its own mesh in the `.glb`, a node with its own transform, so the preview can move it ([physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)).
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

`MOUNT_MODELS` names each model once: its id (`"KM100"`, `"LMR1/M"`, `"ID25/M"`, ...), its file stem, its parts and its mates. [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md) maps each optic kind and size to model ids.

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

## Sources

The Open CASCADE Technology documentation, `BRepAlgoAPI_Defeaturing` and `BRepMesh_IncrementalMesh`; the FreeCAD 1.0 Python API for `Part`, `MeshPart` and the Assembly workbench; Garland and Heckbert 1997, *Surface simplification using quadric error metrics*; the Khronos glTF 2.0 specification; Thorlabs drawings for every part in the tables above.
