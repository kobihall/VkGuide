# The bench and the mounts

On a real table an optic is never just somewhere. It sits in a mount, the mount sits on a post, the post slides in a post holder, and a clamping fork bolts the holder to the breadboard, whose holes are 25 mm apart. That stack decides which motions are possible: the post slides across the table and turns about itself and up and down, and everything else (tip, tilt, rotation about the optical axis) comes from the mount's adjusters. It also puts metal near the beam, which is where a misaligned beam clips. This plan builds the bench from the models of [physoptics-optomechanical-models.md](physoptics-optomechanical-models.md): a procedural breadboard, the stack under every optic, placement in the degrees of freedom the stack allows, snapping to holes, real kinematics for the fine adjusters, and every part as geometry the tracer can hit.

*Not implemented. Depends on [physoptics-optical-elements.md](physoptics-optical-elements.md) and [physoptics-optomechanical-models.md](physoptics-optomechanical-models.md). Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

On 2026-10-18 the user chose:

- Breadboard-physical placement: x and z on the table (continuous, or locked to the holes), yaw about the post, height along the post. Tip and tilt only through the adjusters. A per-optic **Free placement** toggle unlocks six degrees of freedom, for a periscope or a part off the table.
- Real kinematics for the adjusters: each turns the mount's plate about the axis through the pivot ball and the other adjuster's contact.
- A resizable table with the B3030A's hole pattern, 300 × 300 mm by default, in 25 mm steps.
- A per-scene beam height, 100 mm by default.

## What the renderer does today

- `TransformGizmo` (`src/vk_gizmo.h`) edits one target through a getter and a setter closure, with an `ImGuizmo::OPERATION` that can be any combination of single axes (`TRANSLATE_X`, `ROTATE_Y` and so on).
- [physoptics-optical-elements.md](physoptics-optical-elements.md) places an optic by a free `position` and `orientation`.
- Shapes, mesh objects and their BLASes join the TLAS in `buildSceneAccel()`.

## Design

### The bench

`BenchSettings`, saved with the scene:

| Setting | Default | Meaning |
|---|---|---|
| Table size | 300 × 300 mm | in 25 mm steps, along x and z |
| Table origin | a hole at the world origin | hole centers at `(25 i, 0, 25 j)` mm |
| Beam height | 100 mm | the optical axis height every new mount is set to |
| Snap to holes | on | the default for dragging; holding **Alt** inverts it for one drag |
| Show table | on | shown only while the scene's **Optical bench** capability is on ([physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)) |

The top face is the plane y = 0. The plate's thickness, hole diameter, thread, countersink and edge margin are read from `Breadboard-B3030A.step` by the converter's measuring step and checked against Thorlabs' B3030A page (see "Pending answers"). `buildBreadboardMesh()` (`src/optic_bench.cpp`) makes the plate procedurally: a box with each hole as a short cylinder wall and a dark floor, about 40 triangles per hole, so a 1.2 × 2.4 m table of 4600 holes is under 200 000 triangles. The same mesh is the table's BLAS, in black anodized aluminum.

### The stack under an optic

Each optic kind and size maps to its mount model (the request's tables, with the 2 in kinematic mount corrected to KM200):

| Mount kind | Ø½ in | Ø1 in | Ø2 in | Optics |
|---|---|---|---|---|
| Kinematic | KM05T/M | KM100 | KM200 | mirrors, and any round optic by choice |
| Fixed | LMR05/M | LMR1/M | LMR2/M | lenses, round filters, thin round crystals |
| Rotation | RSP05/M | RSP1/M | RSP2/M | round optics with a preferred axis: polarizers, wave plates, round beamsplitters, round wedges |
| Prism | KM200PM/M, every size | | | cube beamsplitters, every non-round prism |
| Plate holder | FP01, every size | | | square filters, plate wedges, square crystals |
| Iris | its own ID-series assembly | | | irises |

From the table up, every optic stands on the same stack, assembled from the models' mates:

1. A CF125C/M clamping fork, its bolt in a hole, its slot around the post holder.
2. A PH50E/M post holder in the fork (`holder_seat`).
3. A procedural Ø12.7 mm post in the holder (`post_entry`), as deep as its length requires.
4. The mount on the post (`post_tap`).
5. The optic in the mount (`optic_seat` or `prism_seat` or `plate_seat`), through the fine adjustment's transform.

The post is an analytic capped cylinder instance in the tracer, exact and shared by every optic, and a cylinder mesh in the raster preview. Its length is what puts the optical axis at the requested height with the post inserted to the middle of the holder's usable depth. The panel shows that length and the nearest Thorlabs TR/M standard length that still reaches the height within the holder's travel.

### The mount pose

`SceneOptic`'s free `position` and `orientation` are replaced by a `MountPose`, from which they are derived:

```cpp
struct MountPose {
    glm::dvec2 post;          // the post's axis on the table, x and z, meters
    double yaw { 0 };         // about the post's axis (+y), radians
    double height;            // the optical axis above y = 0; the bench's beam height when created
    double forkAngle { 0 };   // the clamp's direction about the post, radians
    double forkReach;         // the bolt's distance from the post along the fork's slot, meters
    FineAdjust fine;          // by mount kind: two adjuster turns, a rotor angle, or nothing
    bool freePlacement { false };
    glm::dmat4 freeTransform; // the optic's world transform while freePlacement is set
    bool operator==(const MountPose&) const = default;
};
```

`opticWorldTransform(optic)` is the one function that composes the stack into the optic's body frame in world space, in `double`. The beam solver, the preview, the tracer and the locked transforms all call it, so they cannot disagree about where an optic is.

### Placing

**Edit Transform** on an optic starts the gizmo with `TRANSLATE_X | TRANSLATE_Z | TRANSLATE_Y | ROTATE_Y` in world space, at the post's axis. The setter projects what the gizmo produced onto the pose: x and z move the post, y changes the height, and rotation about y changes the yaw. ImGuizmo shows no other handles, so nothing else can move.

- **Snap to holes** rounds the post's x and z to the nearest hole center. The fork then points along the table axis (±x or ±z) nearest its previous direction, with its bolt in the next hole, 25 mm away, which the CF125C/M's slot reaches (checked against its `slot` mate; see "Pending answers").
- **Continuous** leaves the post anywhere. The fork turns to the direction in which a hole falls inside its slot's reach, preferring its previous direction. If no hole is in reach, which can happen only at the table's edge, the fork is drawn in red and the panel says so.
- The height is clamped to what the post holder's travel allows for the chosen post length; past it, the post length changes.
- **Free placement** gives the full translate-and-rotate gizmo on the optic itself. The post, holder and fork are hidden, since a free optic has no defined stack, and the panel marks the optic as free. Turning it off again drops the optic to the nearest physical pose: the post under the optic's datum, the yaw from its axis's horizontal component, the height of its datum.

### Fine adjustment

| Mount | Controls | Kinematics |
|---|---|---|
| Kinematic (KM05T/M, KM100, KM200) | two adjusters, in revolutions | the plate rests on the pivot ball and the two adjuster tips |
| Prism (KM200PM/M) | two adjusters, in revolutions | as a kinematic mount |
| Rotation (RSP) | the rotor angle, in degrees | rotation about `rotor_axis` |
| Fixed, plate holder, iris | none | |

A turn of an adjuster moves its tip along the screw's axis by the thread pitch. The plate's new orientation is the rotation about the pivot that puts each adjuster's contact back on its tip, solved by Newton's method in `double` from the previous orientation; for small turns it is a rotation about the axis through the pivot and the other adjuster's contact, by the tip's travel over its distance from that axis. The optic rides on the plate, so its vertex moves on an arc about the pivot as it tilts, which is how a real mount walks a beam while steering it. The panel shows each adjuster's turns, the plate's tip and tilt in microradians, and the optic vertex's displacement. Thread pitches come from the adjusters' modeled threads, measured by the converter before it removes them, and are checked against Thorlabs' published angle per revolution (see "Pending answers"). The adjusters' travel limits stop the widget, as the screw would stop.

The raster preview moves the plate and turns and advances the adjuster screws with the pose, using the models' separate parts.

### The bench in the tracer

Every part of every stack is a mesh instance in the TLAS (the mount's parts, the holder, the fork), the post a cylinder instance, the table one more mesh, each with the materials the models carry. Every bench instance records which optic and which part it belongs to, so the beam solver can say "clipped by the front plate of M3's KM100" and a light path that hits a mount is counted as stray light ([physoptics-detectors.md](physoptics-detectors.md)). The BLASes are the model library's, built once; an edit rebuilds only the TLAS, as moving a mesh object does today.

### Scene file

The payload version this lands in adds the bench, and each optic's pose in place of its free transform:

```json
"bench": {"size": [0.3, 0.3], "beamHeight": 0.1, "snap": true},
"optics": [{"type": "planar_mirror", "size": "1in",
            "mount": {"kind": "kinematic", "post": [0.075, -0.05], "yaw": 0.785398, "height": 0.1,
                      "fork": {"angle": 0.0, "reach": 0.025}, "fine": {"x": 0.25, "y": -0.5}}}]
```

A file from before this plan, whose optics have free transforms, opens with each optic dropped onto the table as **Free placement** turning off does.

## Registry, bindings and data

- `BenchSettings`, `MountPose`, `FineAdjust`, `opticWorldTransform()`, `buildBreadboardMesh()`, the stack assembly and the adjuster solver (`src/optic_bench.h/.cpp`, in `rt_optics` so that `bin/optics_test` can check them).
- `RaytraceSceneEditor` gains the bench settings and the constrained gizmo setter; **Optics** → **Bench settings...** is registered under the **Optical bench** capability.
- `buildSceneAccel()` gains the bench instances, each tagged with its optic and part.
- No binding or kernel change.

## Verification

- A scene without the **Optical bench** capability shows no table and renders bit-identical.
- `bin/optics_test`:
    - every kind and size, at default pose, puts its optical axis at the bench's beam height and its datum over the post, to 1e-9 m;
    - the stack for PF05-03 in KM05T/M on a post in PH50E/M in CF125C/M reproduces `example mirror.FCStd`'s placements to 0.05 mm;
    - a snapped post lies on a hole center and its fork's bolt on another, to 1e-9 m;
    - one revolution of each kinematic mount's adjuster tilts the plate by Thorlabs' angle per revolution within 2 %, and rotates the plate about the axis through the pivot and the other contact to 1e-6 rad;
    - turning an adjuster forward and back by the same amount restores the pose exactly.
- A beam aimed at the edge of a KM100 front plate is reported clipped by that part, by the beam solver and by light tracing alike.

## Order of work

1. `BenchSettings`, the table mesh, and the mount mapping with the static stack.
2. `MountPose`, the constrained gizmo, snapping and the fork.
3. Free placement and the conversion back from it.
4. Fine adjustment and the moving parts in the preview.
5. The bench in the tracer.

## Open questions

### Pending answers

These need the user's answer, checked against Thorlabs, before the step that uses them:

- B3030A dimensions. Its thickness, hole thread (M6 is expected for a metric board), countersink, and edge margin, read from the STEP file and confirmed against the Thorlabs B3030A page.
- Adjuster specifications. For KM05T/M, KM100, KM200 and KM200PM/M: thread pitch, angle per revolution, and adjustment range, from the Thorlabs pages. Search results disagree on KM100 (80 versus 100 threads per inch, 8 mrad per revolution), so the plan measures the modeled threads and asks the user to confirm.
- The fork's reach. Whether CF125C/M's slot reaches a hole 25 mm from the post's axis when the post holder sits in the fork, so that a snapped post always has a hole for its bolt.
- Rotation mount scale. RSP's graduation step and whether the panel should snap the rotor to graduations.
- Large prisms. Whether a 60 mm right-angle prism sits on a KM200PM/M platform, or needs another mount.

### Design questions

- Collisions between stacks are not detected. Two posts in one hole is visible in the preview; a check that flags it is cheap to add later.
- Pedestal posts, stages and periscopes are not in the request. A translation stage would be one more layer in the stack, with its own fine adjustment, and fits `MountPose` without a change to the other layers.

## Sources

Thorlabs product pages and drawings: B3030A breadboard; KM05T/M, KM100 and KM200 kinematic mounts (the kinematic mount tutorial's pivot and adjuster geometry); KM200PM/M prism mount; LMR05/M, LMR1/M and LMR2/M fixed mounts; RSP05/M, RSP1/M and RSP2/M rotation mounts; FP01 plate holder; ID-series irises; PH50E/M post holder; CF125C/M clamping fork; TR-series posts. Slocum 1992, *Precision machine design*, chapter 7 (kinematic couplings); `assets/optical_models/example mirror.FCStd`.
