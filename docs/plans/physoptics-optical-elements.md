# Optical elements

The request lists thirteen kinds of optic, from planar mirrors to round wedge prisms, each in the sizes Thorlabs sells. This plan makes each kind a scene object, `SceneOptic`, described by one row of a traits table the way `SHAPE_TRAITS` describes shapes: its sizes, its parameters, its material slots, its mounts, its output ports, and a builder that turns the parameters into the surface prescription of [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md). The Scene panel gains an **Optics** folder, the scene file an `"optics"` list, and the path tracer one optical body per element.

*Not implemented. Depends on [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md), [physoptics-optical-materials.md](physoptics-optical-materials.md) and [physoptics-scene-capabilities.md](physoptics-scene-capabilities.md), whose **Optical bench** capability its UI registers under. [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md) replaces this plan's free placement with mount poses. Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

On 2026-10-18 the user chose:

- Wedge angles of 2°, 4°, 6° and 10° are beam deviations, as Thorlabs specifies them.
- Off-axis parabolas take an off-axis angle from Thorlabs' set (15°, 30°, 45°, 60°, 90°; 90° by default) and a continuous reflected focal length.
- Plate beamsplitters have a 30 arcmin wedge and an AR-coated back by default, both editable.
- A prism is as tall as its leg (right angle) or its side (equilateral), editable.
- Curved mirror substrates use Thorlabs' CM-series and MPD-series thicknesses.
- Square filters sit in an FP01 plate holder, so they have no upper size.

## What the renderer does today

- `SceneShape` and `SHAPE_TRAITS` (`src/rt_scene_types.h`) define a kind once, as data the editor, the gizmo, the scene file and the preview all read. `RaytraceSceneEditor` owns the shapes, cameras, lights and mesh objects, with stable ids and one `revision()` counter, and its Scene panel shows each shape kind in its own sub-folder.
- `drawRaytraceShapes()` (`src/vk_engine.cpp`) draws each shape's preview mesh under its `objectToWorld()`.
- `buildSceneAccel()` (`src/rt_accel.cpp`) turns shapes and mesh objects into instances and the TLAS on every edit.

## Design

### The scene object

```cpp
struct SceneOptic {
    uint64_t id { 0 };           // shares the editor's id space with shapes, cameras, lights, mesh objects
    std::string name;
    OpticKind kind;
    int sizePreset { 0 };        // index into the kind's sizes, or -1 for a custom size
    OpticParams params;          // by parameter name, in SI units (meters, radians), every value ever set
    // where the optic is: free placement in this plan; physoptics-bench-and-mounts.md derives it from the mount
    glm::dvec3 position;
    glm::dquat orientation;
    std::array<uint64_t, MAX_OPTIC_MATERIAL_SLOTS> materials {};   // owned library entries, by slot
    bool operator==(const SceneOptic&) const = default;
};
```

Positions are `double` because the beam solver, which places beams to a fraction of a micron over a meter of table, reads them directly ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)). The GPU instance rounds them to float when the scene is built.

`OpticParams` stores values by name, as [pbrt-material-system.md](completed/pbrt-material-system.md)'s `ParamSet` does for materials, so switching a lens from bi-convex to plano-convex and back keeps both radii.

### The traits table

```cpp
struct OpticTraits {
    const char* name;                         // the scene file's type name; never rename
    const char* label;
    const char* plural;                       // the Optics folder's sub-folder
    std::span<const OpticSize> sizes;         // Thorlabs presets, by diameter, side or leg
    std::span<const OpticParam> params;       // name, label, unit, default, range, and whether it restarts a render
    std::span<const MaterialSlot> slots;      // name and default material of each surface group
    std::span<const MountKind> mounts;        // allowed mounts, the first the default (bench-and-mounts)
    std::span<const OpticPort> ports;         // named output beams (physoptics-beam-axis.md)
    OpticPreview preview;                     // a STEP model per size, or the tessellated prescription
    // the prescription in the body frame, from the parameters
    void (*build)(const SceneOptic&, const OpticBuildContext&, OpticPrescription&);
    // derived values the panel shows read-only: focal lengths, edge thickness, wedge angle
    void (*describe)(const SceneOptic&, const OpticBuildContext&, OpticReadout&);
};
```

`OPTIC_TRAITS` is indexed by `OpticKind` and is the only place outside the builders that knows anything about a kind. Later plans add kinds by appending rows: the laser ([physoptics-laser-sources.md](physoptics-laser-sources.md)), detectors ([physoptics-detectors.md](physoptics-detectors.md)), wave plates, polarizers and crystals ([physoptics-polarization-optics.md](physoptics-polarization-optics.md)), gratings and toroidal mirrors ([physoptics-gratings-and-xuv.md](physoptics-gratings-and-xuv.md)).

`OpticBuildContext` carries what a builder needs from outside the element: the glass catalog and the design wavelength, which a wedge needs to turn a deviation into an angle.

### The kinds

Dimensions are Thorlabs' unless marked as the user's. "Ø½ in" is 12.7 mm, "Ø1 in" 25.4 mm, "Ø2 in" 50.8 mm. Every builder produces a closed body ([physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)).

| Kind | Sizes | Parameters | Surfaces (material slot) | Default mount |
|---|---|---|---|---|
| Planar mirror | Ø½, Ø1, Ø2 in; 6, 6, 12 mm thick (PF05-03, PF10-03, PF20-03 blanks, from their STEP files) | coating | front plane (coating), back plane (back), edge cylinder (edge) | kinematic |
| Spherical concave mirror | Ø½, Ø1, Ø2 in | radius of curvature (live), center thickness (CM catalog) | front sphere (coating), back plane, edge | kinematic |
| Off-axis parabolic mirror | Ø½, Ø1, Ø2 in | off-axis angle (15° to 90°), reflected focal length (live), thickness (MPD catalog) | front paraboloid with an offset aperture (coating), back plane, edge | kinematic |
| Circular filter | Ø½, Ø1, Ø2 in | thickness (user), glass, front and back coatings | front, back, edge | fixed |
| Square filter | side ≥ 50 mm (user), any size | thickness (user, ≤ 14.7 mm in an FP01), glass, coatings | front and back rectangles, four edge rectangles | FP01 plate holder |
| Iris | ID8, ID12, ID15, ID20, ID25, ID36, ID50 | opening (live), within 0.8 to 8, 1 to 12, 1.2 to 15, 1.2 to 20, 1.4 to 25, 1.9 to 36, 2.5 to 50 mm | one annulus, two-sided (blades) | the iris's own assembly |
| Plate beamsplitter | Ø½, Ø1, Ø2 in | thickness (user), polarizing or not, wedge (30′), back coating (AR) | front plane (splitter coating), tilted back plane (back), edge cylinder between them | rotation |
| Cube beamsplitter | 12.7, 25.4, 50.8 mm sides | polarizing or not, glass | six faces (AR), the diagonal (splitter coating, `Internal`) | prism mount |
| Spherical lens | Ø½, Ø1, Ø2 in | form (plano-convex, bi-convex, plano-concave, bi-concave), R1 and R2 (user), center thickness (user), glass, coatings | front, back, edge | fixed |
| Right-angle prism | 10, 25, 60 mm legs; height = leg | glass, hypotenuse finish (uncoated for TIR, or a mirror coating) | two legs, the hypotenuse, two triangular caps | prism mount |
| Equilateral dispersing prism | 10, 25, 50 mm sides; height = side | glass | three faces, two triangular caps | prism mount |
| Plate wedge prism | side ≥ 50 mm (user) | deviation 2°, 4°, 6°, 10°; glass; thickness | front, tilted back, four trapezoidal edges | FP01 plate holder |
| Round wedge prism | Ø½, Ø1, Ø2 in | deviation 2°, 4°, 6°, 10°; glass | front, tilted back, edge cylinder between them | rotation |

Builders worth spelling out:

- Off-axis parabola. The parent paraboloid's focal length is `PFL = RFL (1 + cos θ) / 2` and its off-axis distance `OAD = RFL sin θ`, for reflected focal length RFL and off-axis angle θ. The surface is a conic with `k = −1` and `c = 1 / (2 PFL)`, its vertex at the parent's, its circular aperture centered at OAD. The body frame follows Thorlabs' MPD drawings: +z along the parent axis, the datum at the aperture's center on the surface, the back face perpendicular to the parent axis, so a collimated beam along −z focuses at RFL from the datum at θ to the axis.
- Wedges. A deviation δ becomes the wedge angle α that deviates a ray at normal incidence on the front face by δ at the design wavelength, `δ = asin(n sin α) − α`, solved by Newton's method. The readout shows α, and the deviation at the current wavelength once the scene is spectral.
- Lenses. The form fixes the signs and the planarity of R1 and R2, so a plano-convex lens has R2 = ∞ whatever was typed. The edge thickness follows from the sags at the semi-diameter, and the panel refuses a lens whose edge thickness would be below zero. The readout shows the effective and back focal lengths at the design wavelength, from a paraxial trace in `rt_optics`.
- Iris. The blades are an annulus with the opening as inner diameter and the housing's clear aperture as outer, two-sided, in the iris's absorbing blade material. The housing itself is the iris model's mesh, which the tracer hits like any mount ([physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)).

Values marked "live" change with a slider while the beam solver re-solves, and while a render restarts on change.

### Materials

Each kind's material slots are created as owned library entries when the element is made ([physoptics-optical-materials.md](physoptics-optical-materials.md)): front, back and edge for a mirror; front, back and edge for a lens; the faces and the diagonal for a cube. Defaults follow the part Thorlabs sells without a suffix where there is one (an uncoated N-BK7 lens is Thorlabs' LA, LB, LC and LD series), and are listed under "Pending answers" where the default needs the user. The panel shows each slot with a coating combo and a glass combo, and **Edit** opens the entry in the Materials window.

### Ports

Each kind names its output beams, which the beam axis connects ([physoptics-beam-axis.md](physoptics-beam-axis.md)): a mirror's **Reflected**, a lens's or filter's **Transmitted**, a splitter's **Reflected** and **Transmitted**, a right-angle prism's **Turned** (two TIR passes) and **Retro**, a dispersing prism's **Refracted**. A port is defined by the sequence of reflections and refractions that produces it inside the body, which the traits record and the solver matches. Ghosts, the weaker sequences, are the solver's concern, not named ports.

### Editing

- While the scene's **Optical bench** capability is on ([physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)), the Scene panel's tree has an **Optics** folder with one sub-folder per kind, the arrangement the user chose for shapes, and **Add** → **Optic** lists the kinds. A scene without the capability shows neither, and adding an optic by any other route (pasting, opening a file) turns the capability on.
- An element's panel shows the size combo (presets and **Custom**), its parameters in millimeters and degrees with their units, the read-only readout, the material slots, and **Edit Transform**.
- **Edit Transform** in this plan moves the optic freely with the shared `TransformGizmo`. [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md) replaces it with the mount's breadboard-physical controls.
- Invalid parameters (an edge thickness below zero, a filter too thick for its holder, an iris opening outside its range) are clamped by the widget and explained beside it, never silently.

### The preview

Planar mirrors draw their PF blank model from the mount library, and so do concave mirrors and off-axis parabolas for now, as the request asks ([physoptics-optomechanical-models.md](completed/physoptics-optomechanical-models.md)). Every other kind draws its tessellated prescription (`src/optic_mesh.cpp`) through the raster pass's transparent pipeline, tinted by its glass. The preview geometry is rebuilt when a parameter changes, which takes well under a millisecond for one element.

### The path tracer

`buildSceneAccel()` takes the optics with the shapes: each element's prescription is built (and cached by kind and parameters, so twenty identical lenses build once), packed into binding 55, and placed as one instance under the element's transform. Its owned materials join `materials` and `instanceMaterials`. An element's mount, post and holders join the TLAS as mesh instances in [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md).

### Scene file

The payload version this lands in adds:

```json
"optics": [
  {"type": "spherical_lens", "name": "L1", "size": "1in",
   "params": {"form": "plano_convex", "r1": 0.0515, "centerThickness": 0.0036},
   "position": [0.15, 0.1, 0.0], "orientation": [1, 0, 0, 0],
   "materials": {"front": {"coating": "thorlabs_A"}}}
]
```

Parameters are saved by name in SI units, only when they differ from the kind's default; materials only as overrides of the owned defaults. A size is saved by its preset's name (`"half_inch"`, `"1in"`, `"2in"`, `"10mm"`, ...) or as `"custom"` with its dimension. An unknown type is skipped with a load warning, as an unknown kernel id is.

## Registry, bindings and data

- `OpticKind`, `OpticTraits`, `OPTIC_TRAITS`, `SceneOptic`, `OpticParams` (`src/optic_catalog.h/.cpp`); builders and readouts in `src/optic_builders.cpp`, part of `rt_optics` so that `bin/optics_test` can build every kind without Vulkan.
- `RaytraceSceneEditor` gains the optics list, `findOptic()`, `replaceOptics()` and the Optics folder.
- `buildSceneAccel()` gains the optics; `drawRaytraceShapes()` gains their preview.
- No new binding beyond binding 55; no kernel change.

## Verification

- A scene without optics renders bit-identical and saves the same file apart from the version.
- `bin/optics_test` builds every kind at every size with default parameters and checks that the body is closed (the closed-body test of [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)), that its bounds contain every surface, and:
    - an off-axis parabola focuses a collimated bundle along −z to a point at RFL from its datum, at θ to the parent axis, to 1 µm in `double`;
    - every wedge deviates a normally incident ray by its nominal deviation at the design wavelength to 1e-6 rad;
    - a lens's readout matches a real-ray trace's focal length to 1e-4 relative;
    - a right-angle prism's **Turned** port leaves at 90° and its **Retro** port at 180°, within 1e-9 rad.
- Save, reopen and compare: every kind, with custom sizes and edited materials, round-trips equal by `==`.

## Order of work

1. `SceneOptic`, `OPTIC_TRAITS` with planar mirrors and lenses, the Optics folder, the scene file.
2. The path tracer: prescriptions, the cache, instances.
3. The remaining kinds, each with its `optics_test` case.
4. The preview.

## Open questions

### Pending answers

The request asks that uncertain part parameters be checked against Thorlabs and put to the user rather than assumed. These need the user's answer before the kind they belong to is implemented:

- Planar mirror substrate and back. The PF05-03, PF10-03 and PF20-03 blanks' substrate glass and back finish (polished or fine ground), and which coating a new mirror defaults to (`-P01` protected silver, `-E02` dielectric, or another). See the Thorlabs PF10-03-P01 and BB1-E02 pages.
- Concave mirror and OAP thicknesses. CM254 is 6.0 mm at the center (Thorlabs CM254-100). The CM127 and CM508 center thicknesses, and the MPD thickness per diameter and off-axis angle, are to be read from the Thorlabs drawings and confirmed with the user.
- Equilateral prism glass. Thorlabs sells the PS85x series in more than one glass (N-F2, N-SF11, CaF₂ among them); which glass is the default.
- Round wedge sizes and thickness. Thorlabs' PS810 to PS814 are Ø1 in N-BK7 (PS814 is 11.33 mm thick at its thick edge, by 3DOptix's copy of Thorlabs' model). Whether Ø½ in and Ø2 in wedges match a Thorlabs product, and their thicknesses.
- Plate wedge thickness. The thin-edge thickness of a plate wedge, which no Thorlabs product fixes.
- Cube beamsplitter glass and cement. The default glass for non-polarizing and polarizing cubes, and whether the cement layer at the diagonal is modeled (a thin layer of index about 1.56) or folded into the coating.
- Right-angle prism glass. N-BK7 uncoated (Thorlabs PS9xx) or UV fused silica, and whether the 60 mm leg matches a Thorlabs part.

### Design questions

- Chamfers on mirror blanks (0.2 to 0.25 mm on the PF blanks) are left out of the traced body: a beam that reaches the chamfer is already clipping, which the solver reports. Modeling them is a later refinement if ghosts off a chamfer ever matter.
- Whether a lens should also offer meniscus forms and achromatic doublets as catalog kinds, or leave both to [physoptics-prescription-import.md](physoptics-prescription-import.md). The request lists neither.

## Sources

Thorlabs product pages and drawings: PF05-03, PF10-03, PF20-03 mirror blanks; CM127, CM254 and CM508 concave mirrors; MPD-series off-axis parabolic mirrors (the RFL, PFL and OAD relations in their tutorial tab); LA, LB, LC and LD lens series; BSW and BSN plate beamsplitters; BS and PBS cube beamsplitters; PS9xx right-angle prisms; PS85x equilateral prisms; PS810 to PS814 round wedge prisms; ID-series mounted irises; FP01 plate holder. Smith 2008, *Modern optical engineering*, chapters 3 and 4 (thick lenses, prisms and wedges); Hecht 2017, *Optics*, §5.5 (prisms and minimum deviation).
