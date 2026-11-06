# The bench and the mounts

On a real table an optic is never just somewhere. It sits in a mount, the mount sits on a post, the post slides in a post holder, and a clamping fork bolts the holder to the breadboard, whose holes are 25 mm apart. That stack decides which motions are possible: the post slides across the table and turns about itself and up and down, and everything else (tip, tilt, rotation about the optical axis) comes from the mount's adjusters. It also puts metal near the beam, which is where a misaligned beam clips. This plan builds the bench from the models of [physoptics-optomechanical-models.md](physoptics-optomechanical-models.md): a procedural breadboard, the stack under every optic, placement in the degrees of freedom the stack allows, snapping to holes, real kinematics for the fine adjusters, and every part as geometry the tracer can hit.

*Implemented 2026-10-30. "As built" records the user's answers to the pending questions and where the build departs from the design below, and "Not built" what it leaves out. Depends on [physoptics-optical-elements.md](physoptics-optical-elements.md) and [physoptics-optomechanical-models.md](physoptics-optomechanical-models.md). Part of the [laser optics plan](../physoptics-laser-optics.md).*

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

- **Snap to holes** rounds the post's x and z to the nearest hole center. The fork then points along the table axis (±x or ±z) nearest its previous direction, with its bolt in the next hole, 25 mm away. The converter measured that the CF125C/M's slot does not reach that hole (its `slot` mate; see "Pending answers"), so this rule needs revising before it is built.
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

An iris has no control of its own: its lever and each of its blades take the pose its optic's aperture diameter sets, through the `aperture` drive in the model's mates file (`driveMotionValue()`, `src/optic_models.h`).

A turn of an adjuster moves its tip along the screw's axis by the thread pitch. The plate's new orientation is the rotation about the pivot that puts each adjuster's contact back on its tip, solved by Newton's method in `double` from the previous orientation; for small turns it is a rotation about the axis through the pivot and the other adjuster's contact, by the tip's travel over its distance from that axis. The optic rides on the plate, so its vertex moves on an arc about the pivot as it tilts, which is how a real mount walks a beam while steering it. The panel shows each adjuster's turns, the plate's tip and tilt in microradians, and the optic vertex's displacement. Thread pitches are each adjuster tip's `pitch` in the model's mates file. The STEP files model no adjuster threads, so the converter's manifest sets them from Thorlabs' specification, and they are checked against Thorlabs' published angle per revolution (see "Pending answers"). The adjusters' travel limits stop the widget, as the screw would stop.

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
- The raster preview has no capability hook for geometry yet (`docs/physical-optics-overview.md` §9). This plan adds one to `CapabilityInfo` for the table, posts and mounts, and adds what they draw to `RasterKey`, or the paused preview keeps the old picture. The **Optics** menu is the capability's `menus` hook.
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

These needed the user's answer, checked against Thorlabs, before the step that uses them. The user answered them on 2026-10-30; "As built" has the answers:

- B3030A dimensions. Its thickness, hole thread (M6 is expected for a metric board), countersink, and edge margin, read from the STEP file and confirmed against the Thorlabs B3030A page.
- Adjuster specifications. For KM05T/M, KM100, KM200 and KM200PM/M: thread pitch, angle per revolution, and adjustment range, from the Thorlabs pages. Search results disagree on KM100 (80 versus 100 threads per inch, 8 mrad per revolution). The converter assumed 1/4"-80 for KM100, KM200 and KM200PM/M, which gives 8.4 mrad per revolution at KM100's modeled lever of 37.7 mm, and left KM05T/M's unset.
- The fork's reach. Measured by the converter: CF125C/M's slot holds the bolt 26.8 to 58.3 mm from the post's axis, so it cannot reach the hole 25 mm away. The nearest it reaches are the diagonal hole, 35.4 mm away, and the hole 50 mm away along an axis. Snapping needs a rule for which.
- Rotation mount scale. RSP's graduation step and whether the panel should snap the rotor to graduations.
- Large prisms. Whether a 60 mm right-angle prism sits on a KM200PM/M platform, or needs another mount.

### Design questions

- Collisions between stacks are not detected. Two posts in one hole is visible in the preview; a check that flags it is cheap to add later.
- Pedestal posts, stages and periscopes are not in the request. A translation stage would be one more layer in the stack, with its own fine adjustment, and fits `MountPose` without a change to the other layers.

## As built

Implemented on 2026-10-30, all five steps of the order of work, in one change. The pose is `MountPose` in
`src/optic_mount.h`, part of `SceneOptic`. The bench is `src/optic_bench.{h,cpp}` in `rt_optics`: the grid, the
breadboard's mesh, the choice of mount model, the seats, the stack, the fork rule, placement and the adjusters'
solver. The engine side is `src/bench_scene.{h,cpp}`, which uploads the table, the post and the mount models a stack
uses, draws them into the preview and gathers the tracer's instances. The editor side is in
`src/rt_scene_editor_optics.cpp`. `docs/physical-optics-overview.md` §7 is the readable account.

### The user's answers

- **The breadboard.** `Breadboard-B3030A.step` is a Nexus-style board, not a solid plate: 60 mm thick, with a 5 mm top
  skin, a ribbed core and a 5 mm bottom skin. Its holes are Ø5.0 mm (the M6 tap drill, unthreaded) and 5 mm deep, with
  no countersink, on the 25 mm pitch with a 12.5 mm border, and its corners are rounded to 12.5 mm about the corner
  holes. Its side walls are set back 3 mm, with corner bumpers set back 2 mm that run 39.5 mm along each side from the
  corner hole and wrap the corner at 10.5 mm. The user asked for the holes, the skins and the side walls, and not the
  core, which no view reaches. The cups under the holes and the logo on the side panels are left out too. The skins,
  with the holes' walls, are brushed stainless steel (the user's review, 2026-10-30), and only the side walls between
  them are black anodized: pbrt's conductor from a reflectance of 0.62, its alpha 0.05 along the brushing (x) and 0.30
  across it. The mesh's uvs are x and z in centimeters, which orient the brushing. The preview, with no anisotropic
  metal, shows a gray metal at roughness 0.35.
- **The table's origin.** The hole just below and to the left of the plate's center is at the origin, so the default
  plate spans -137.5 to 162.5 mm on each axis. An odd count puts the center hole at the origin.
- **The fork.** For any post position, the candidates are every hole the slot reaches, 26.797 to 58.293 mm from the
  post. Axis-aligned directions come first, then the nearest hole, then the smallest angle from an axis, then the
  direction nearest the fork's previous one. The fork's outline must clear every other stack's fork and post holder.
  If none does, the fork takes the best blocked hole and is drawn in red, and the panel says why. A snapped post's
  fork bolts into the hole 50 mm away along an axis.
- **The adjusters.** The pitch is tuned to Thorlabs' angle per revolution: 13 mrad for KM05T/M, 8 mrad for KM100 and 5
  mrad for KM200 and KM200PM/M (third-party listings of Thorlabs' figures, since Thorlabs' own pages did not load).
  Each mates file's adjuster tips carry `angle_per_rev` beside the real thread as `pitch` (3/16-100 for KM05T/M, 1/4-80
  for the others). The bench derives the effective pitch from the angle and the modeled lever: 0.2576, 0.3016, 0.3111
  and 0.3111 mm. The real 1/4-80 thread on KM100's 37.7 mm lever would give 8.4 mrad, 5 % above Thorlabs' rounded
  figure. Each adjuster stops where the plate reaches the mount's ±4° (the `plate` motion's range in the mates file):
  5.36 revolutions for KM05T/M, 8.72 for KM100 and 13.94 for KM200 and KM200PM/M.
- **The rotation mounts.** RSP mounts are engraved every 2°, labeled every 20°, with no vernier. The rotor is
  continuous, and holding Ctrl while you drag it stops on the graduations. Its zero is where the model has it.
- **Large prisms.** Every prism stands on KM200PM/M, whatever its size. The platform is 75 × 32 mm, and the panel notes
  a prism that overhangs it.

The user also decided these, asked as the design questions came up:

- **The post is continuous.** Its length is always from the holder's mid-depth up to the mount, and the TR/M
  standard lengths play no part. The lowest an optic stands is with the post's top at the holder's top face, 54.699 mm:
  80.1 mm for a KM100 and 103.1 mm for a 50.8 mm cube on KM200PM/M. A stack whose lowest is above the beam height
  stands at its lowest.
- **The table is a scene object.** The plan's **Show table** setting and the table shown only while **Optical bench** is
  on would have let a capability change what renders, which [physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)
  forbids. Instead the breadboard exists or not (**+ Add** → **Breadboard**, a row in the **Optics** folder, a checkbox in
  **Bench Settings**). `opticalBenchDependentObjects()` lists it, and its `erase` resets the bench's settings too.
- **Unmounted optics.** The mount combo has **None (unmounted)** for every kind: the optic alone in six degrees of
  freedom. An optic in a file from before this change opens unmounted where the file put it, not dropped onto the
  table as the plan said, so the five scenes `tools/verify/optics_check.py` measures render bit-identically.
- **Plain finishes in the tracer.** Mounts trace in their finishes, and the decals stay in the preview.
- **Tags only.** Every bench instance in the TLAS carries a `BenchTag` (the optic's id and the part, such as
  `KM100 plate`), in `RaytraceSceneAccel::instanceBenchTag`. Nothing reads the tags yet: the plan's clipping check needs
  the beam solver and the detectors, and moves to their plans.

The user's review of the first build, the same day, changed three things:

- **The skins are brushed stainless steel** (above).
- **An optic is placed by its origin, not its post.** The gizmo, the snapping and the pose all work at the optic's
  origin (`opticOrigin()`): the center of a mirror's front face, which decides the reflection, and the center of
  anything light goes through, at the beam height. That is a lens's, a filter's, a plate's or a wedge's middle on its
  axis, a cube's or a right-angle prism's box center, an equilateral prism's centroid, and an iris's blades. The pose
  stores the origin, so the optical surface stays where it was placed, and the post stands wherever that puts it, off
  the holes when the origin snaps to one. A mirror made thicker or put in another mount keeps its face, and its post
  moves behind it.
- **Z-fighting is fixed in the preview only.** An optic's faces lie exactly in the faces of the mount they rest on,
  such as a cube's bottom on KM200PM/M's platform. The optic previews and the mirror blanks draw with a depth bias of
  1 step and a slope factor of 0.25 toward the camera (`GLTFMetallic_Roughness::opaqueBiasedPipeline` and
  `transparentBiasedPipeline`), so they draw over the faces they touch. The geometry stays exactly in contact, in the
  preview and in the tracer. The first setting, 16 steps and a slope factor of 2, showed glass through the lip of its
  mount from a meter away, with the whole table in view: a depth step there was 0.012 mm with the 5 mm near plane,
  and the slope term is largest for glass seen nearly edge on.
- **The raster pass is reverse-Z** (the user's review), so that the Viewport's near plane can be 1 mm or less for
  working against an optic. Forward-Z, which `CLAUDE.md` required because vkguide's reversed `glm::perspective()` drew
  nothing on MoltenVK, would have had a depth step of 0.12 mm at 1 m with a 1 mm near plane. That failure was the
  matrix: `glm::perspective()` maps depth to OpenGL's -1 to 1, and swapped, Vulkan clips everything past twice the near
  plane. `rasterDepthProjection()` builds reverse-Z for Vulkan's 0 to 1 by hand, and it draws on MoltenVK: Sponza, and
  the bench from 1.4 m and from 3 cm with a 0.1 mm near plane, with no validation message. A depth step is now about
  1e-7 of the distance. Compared against no bias, the bias changes only the contacts (prism and cube bases, plates in
  their holders) from across the table with a 1 mm near plane, and the cube still draws cleanly over its platform. The
  Optical bench template sets a 1 mm near plane, Optical bench recommends it, and the optics scenes carry a
  `"viewport"` record with it.
- **The preview is color-correct.** The user's review found the decals' boxes grey around black lettering: the preview
  read the decal atlas's sRGB bytes as linear, while untextured colors are linear. Every 8-bit color texture (glTF,
  pbrt and the mounts) now has an sRGB view (`AllocatedImage::colorView`) that the raster samples, decoding it, and
  `shaders/viewport_encode.comp` sRGB-encodes the draw image after each raster pass for the UNORM swapchain. The
  images themselves stay UNORM, so the path tracer's texture array copies their bytes as before: Sponza traces
  bit-identically. Untextured colors in the preview are brighter than before, as they are in the path tracer. The
  gradient and sky backgrounds decode their display colors (`shaders/srgb.glsl`), so they look as they did.

### Departures from the design

- **The pose holds the origin.** `MountPose::origin` is the optic's origin's x and z, and the height is the origin's.
  The post's axis is derived: the origin less the yaw-turned offset from the mount's post tap to the origin.
  `refitFork()` rechooses a fork whose bolt has left its hole, as it does when the post moves under a fixed origin,
  and a fork another stack has been put on. `resolveBench()` runs it for every optic each frame, in order and against
  the stacks as each refit leaves them, so two forks never trade places from frame to frame. The file's mount record
  saves `"origin"`, not `"post"`.
- **Free placement is a position and a unit quaternion**, `freePosition` and `freeOrientation`, not a `glm::dmat4`. A
  matrix saved as a quaternion came back one bit different on every save. The file writes them as the elements plan
  wrote an optic's free transform, so a save and a reopen give the same bits. A free optic in a mount also keeps its
  stack's pose, which the file saves, so it round-trips equal by `==`.
- **The mount stays with a free optic**, hung around it by the inverse of its seat: only the post, holder and fork go.
- **`SceneOptic::world` holds the transform.** `opticWorldTransform()` composes it, and
  `RaytraceSceneEditor::resolveBench()` stores it, with every stack, after the panels each frame and after a scene
  opens. A change marks the scene changed, so the tracer and the paused preview see it.
- **The seats.** A mirror faces the mount's front with its back on the lip. A round transmissive optic, an iris and a
  plate are turned half a turn about y, since their light travels along their own +z, through the mount from its
  front. A plate stands on its bottom edge at FP01's `plate_seat`. A prism stands on its cap with its body x up, so a
  right-angle prism turns and an equilateral one disperses in the table's plane, and a cube reflects sideways. The
  optic's datum is the optical axis: a cube's center, half its side above the platform.
- **The holder faces away from the fork**, its thumbscrew opposite the slot, as `example mirror.FCStd` has it.
- **Retaining rings and the jaw close on the optic.** A ring slides to the optic's front at its rim, and a ring that
  would stand proud of the mount comes out, as KM05T/M's front ring does with PF05-03 (6 mm against 3.81 mm). FP01's
  jaw closes on the plate.
- **The kinematic model.** The plate turns about the pivot by R = Rot(a_X, α) Rot(a_Y, β), where a_X runs from the
  pivot to Y's contact and a_Y to X's, with α and β solved by Newton's method in double so that each contact sits on
  its advanced tip. One adjuster's turn is then a rotation about the axis through the pivot and the other contact. The
  pose is a function of the turns alone, so turning an adjuster forward and back restores it bit for bit.
- **A mounted iris's blades are its housing model's** in the preview, turned to the opening by the `aperture` drive.
  The tracer keeps the optic's annulus and leaves the model's blades out, so it does not see two sets.
- **The preview's hook** is `CapabilityInfo::rasterGeometry`, called after `drawRaytraceShapes()`, and
  `RasterKey::benchAssets` moves with every upload. Every other bench change moves the editor's revision, which the key
  already holds.
- **The tracer's meshes.** The breadboard's mesh and every uploaded mount model's parts join `RaytraceMeshData`
  (`benchMeshes`), after the models', so each part has one BLAS however many stacks use it. A new model or a resized
  table rebuilds the mesh data and builds only the new BLASes. The posts are capped cylinder shape instances.
- **The breadboard's mesh** has 12 segments a hole and about 62 triangles a hole, not 40: 9,752 triangles at 300 mm
  and 289,400 at 1.2 × 2.4 m. Neighboring cells compute their shared points from the same edges, so the top face has no
  cracks.
- **Snapping** puts the origin on the nearest hole during a gizmo drag, with Alt inverting it for the drag. A position
  typed in the panel is not snapped; **Snap to nearest hole** snaps it. A free optic's gizmo and fields work at its
  origin too, and a turn keeps the origin where it is.
- **Bench Settings** is a window under **Optical bench**, opened from **Optics** → **Bench Settings...** and docked in
  the Optics workspace, and also the breadboard's panel in the Scene tree.
- **The scene file is version 23.** `"bench"` holds `"table"`, `"size"`, `"beamHeight"` and `"snap"`, written when any
  differs from a new scene's. Each optic's `"mount"` replaces its `"position"` and `"orientation"`.

### Verification

- A scene without the bench renders bit-identically: `cornell_box`, `all_materials`, `sphere_scene`, `veach_mis`,
  `prism_dispersion` and the six `optics_*` scenes, at 32 samples, seed 7, have an RMSE of 0 against a build of
  0a3a26f. So do the five checked `optics_*` scenes after `tools/verify/optics_scenes.py` rewrote them as version 23
  with unmounted optics.
- `bin/optics_test`:
    - every kind at every size in every mount it allows, 60 stacks, stands at the beam height to 1e-9 m, or at its
      lowest where that is higher (107.676 mm at most, the 60 mm right-angle prism). Its origin is on the hole it was
      placed on, to 1e-9 m, and is the optic's own `opticOrigin()` in world space. Its post's top is on the mount's
      tap, the holder's bore on the post's axis, the holder in the fork on the table, the bolt in a hole;
    - the stack for PF05-03 in KM05T/M on a post in PH50E/M in CF125C/M, with the post on the fork's seat at the
      example's origin, places the three models exactly where
      `example mirror.FCStd` does (0.000000 mm and 0.000000 mrad), shows 23.984 mm of post, and puts the blank on the
      example's axis 0.095 mm further out, where the example sinks it into the rear retaining ring;
    - a snapped origin lies on a hole center and its fork's bolt in a hole in reach; a continuous origin keeps its place
      and its fork still bolts into a hole; a second optic 50 mm away gets a fork that clears the first, and the first's
      fork moves when the second's holder lands on it; a fork laid over another holder is marked blocked;
    - a mirror made thicker keeps its face to 1e-12 m, its post moves, and its fork is refit to a hole;
    - one revolution of each kinematic mount's adjuster tilts the plate by Thorlabs' angle within 0.013 %, about the
      axis through the pivot and the other contact within 2.1e-8 rad, and turning the adjusters and back restores the
      pose exactly;
    - turning free placement off keeps an upright optic where it was, to 1e-9 m;
    - the breadboard's extent, hole count and triangle counts.
- A scripted run of the app, with validation layers, opened `optics_bench.gltf` and drew it in the preview. It turned
  adjusters, a rotor and an iris, laid a fork over a neighbor's holder (both forks turned red), freed a prism and
  resized the table. It saved, reopened and compared: all 13 mounts and the bench were equal, and a second save
  differed from the first only in the viewport camera's floats. The Optical bench template opened with its breadboard.
  No validation message.
- Converting KM05T/M again with `tools/optical_models/convert.py --out` reproduced the edited mates file and the `.glb`
  byte for byte.
- Close-ups of the preview with the depth bias off showed the platform through the lower half of the cube on
  KM200PM/M as dark streaks; with it on, the cube's bottom face draws cleanly.
- `tools/verify/run_all.py --quick --skip-pbrt` passes all 19 of its checks, `bin/optics_test`, the GPU self-tests and
  `optics_check.py` among them.

## Not built

- Collisions between stacks other than the fork rule: two post holders in neighboring holes overlap, as the design
  question expected.
- The beam solver's and light tracing's clipping check, which need [physoptics-gaussian-beam-solver.md](../physoptics-gaussian-beam-solver.md)
  and [physoptics-detectors.md](physoptics-detectors.md).
- The mounts' decals in the path tracer, the breadboard's side lettering and its bottom skin's mounting holes.
- KM100 and KM200 turned 90° on their second tapped hole.
- Snapping the rotor to graduations without Ctrl.

## Sources

Thorlabs product pages and drawings: B3030A breadboard; KM05T/M, KM100 and KM200 kinematic mounts (the kinematic mount tutorial's pivot and adjuster geometry); KM200PM/M prism mount; LMR05/M, LMR1/M and LMR2/M fixed mounts; RSP05/M, RSP1/M and RSP2/M rotation mounts; FP01 plate holder; ID-series irises; PH50E/M post holder; CF125C/M clamping fork; TR-series posts. Slocum 1992, *Precision machine design*, chapter 7 (kinematic couplings); `assets/optical_models/example mirror.FCStd`.
