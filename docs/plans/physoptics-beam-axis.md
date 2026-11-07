# The beam axis

On a lab notebook sketch, an experiment is a line: laser, turning mirror, lens, iris, beamsplitter, then two arms. That line is the design intent, and the alignment is the work of making the real beam follow it. The beam axis is that sketch as a scene object: a source and a tree whose nodes are visits to optical elements and whose edges leave each element by a named port. You build it by hand, or from the beam the solver traces, and the solver then reports every place where the real beam leaves it. The tree also tells the locked transforms what moves with what ([physoptics-locked-transforms.md](physoptics-locked-transforms.md)), and the path tracer which surfaces a light path is meant to reach next ([physoptics-guided-tracing.md](physoptics-guided-tracing.md)).

*Partly implemented. Step 1 of the order of work (the axes, their tree, the Beam axes folder and panel, the scene file) and an overlay of the authored order were built on 2026-11-07; the statuses, Follow beam, viewport picking and assemblies wait for [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md). Depends on [physoptics-optical-elements.md](completed/physoptics-optical-elements.md) and [physoptics-laser-sources.md](completed/physoptics-laser-sources.md). Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

- The graph does not place anything. You place the optics with the gizmo; the graph records the order the beam should visit them in (the request).
- Elements that split the beam (beamsplitters, acousto-optic modulators, nonlinear crystals) have several output ports, each with its own child; everything else is a chain (the request).
- The trace modes the graph enables in the path tracer are all four (the user, 2026-10-18).
- A beam axis is a general object, not a laser's: it has its own folder in the Scene panel, and a source you attach separately, a laser or a camera (a realistic camera's chief ray, later an assembly's input). The source is not a node (the user, 2026-11-07).
- Removing a visit with several children keeps the child on its lowest port in its place and leaves the others dangling, with their subtrees, until you attach them elsewhere or remove them (the user, 2026-11-07).
- How renders trace along an axis is a render setting, not the axis's: one guide axis at a time, with the trace mode and guide strength, in **Render Settings** → **Optics** → **Guided tracing** ([physoptics-guided-tracing.md](physoptics-guided-tracing.md); the user, 2026-11-07).

## What femtoPro does

femtoPro's graph is derived, not authored: its solver discovers which element each beam segment reaches and builds the graph from that, every frame, with each round trip in a cavity a new set of edges (Müller et al. 2025). There is no notion of where the beam ought to go. This plan keeps femtoPro's derived graph as the traced graph, which the solver builds, and adds the authored one beside it, so the difference between the two is what you see.

## Design

### Nodes are visits

As built in `src/beam_axis.h`:

```cpp
struct BeamSource {
    BeamSourceKind kind;          // None, Laser (an optic's id) or Camera (a scene camera's id)
    uint64_t id;
};

struct BeamAxisNode {
    uint32_t id;                  // unique within its axis; a load numbers the nodes by their places in the file
    uint64_t optic;               // the SceneOptic this node visits
    uint32_t parent;              // the node whose port leads here; BEAM_SOURCE_NODE or BEAM_DANGLING_NODE
    OpticPortId port;             // which of the parent's optic's ports; NO_PORT under the source and when dangling
    bool review;                  // an edit changed the beam that reaches it
};

struct BeamAxis {
    uint64_t id;
    std::string name;
    BeamSource source;
    std::vector<BeamAxisNode> nodes;   // in the order they were added; the tree is the parent links
    float ghostThreshold;         // the solver's power floor for ghost branches, relative to the source
};
```

A node is a visit, not an optic, so the same optic can appear more than once: the double pass through a lens in front of a retroreflector, the beamsplitter of a Michelson interferometer, which the beam crosses going out and coming back. Every visit has exactly one parent, so the authored graph is a tree. Two branches that end on the same detector, as in a Mach–Zehnder interferometer, are two nodes visiting one detector; their powers add, and the phases that would make them interfere are the wave backend's.

Each element kind names its ports in `OPTIC_TRAITS` (`src/optic_catalog.cpp`, [physoptics-optical-elements.md](completed/physoptics-optical-elements.md)): **Reflected**, **Transmitted**, **Turned**, **Retro**, **Refracted** and a laser's **Output**, and later **Order m** for gratings, **Ordinary** and **Extraordinary** for birefringent crystals. A port is defined by the sequence of reflections and refractions at the element's surfaces that produces it (`PortEvent`), so the solver can match a traced branch to a port exactly. A port has at most one child node: a beam leaving a port goes to one element next. The source has one output, and its first visit hangs off it with no port.

A probe plane has a **Transmitted** port with no events, as an iris has, since the beam crosses it unchanged. The other detectors have none: a beam ends on them.

A visit names an optic of the file's own. A laser is a source and never a visit, and a sensor stack's members cannot be visited, since the file never saves them and their ids go when a stack shrinks. A probe plane stack is passed through anyway, and a single detector can still be a visit.

### Dangling visits

A visit can dangle: its parent is `BEAM_DANGLING_NODE`, it keeps the visits that follow it, and it reads **Unreached** until you attach it to a free place (`moveBeamNode()`) or remove it. **Remove** makes them (above); **Detach** cuts any visit loose by hand, and a load cuts loose a visit whose port its parent's kind lacks, that shares a place with another, or that is part of a cycle. Any number of visits can dangle.

### Assemblies

An axis whose source is an assembly is a rigid group: a lens made of several elements, an objective, a camera lens, a beam expander built as a unit. Its members are placed relative to the assembly's datum, which is itself mounted like one optic ([physoptics-bench-and-mounts.md](completed/physoptics-bench-and-mounts.md)): on a post, or free. Moving the assembly moves every member, and **Edit Transform** on a member moves it within the assembly. An assembly's source is an input reference (a ray and a beam, or a field of view and an aperture for a lens), and it is what [physoptics-prescription-import.md](physoptics-prescription-import.md) creates from a `.zmx` file and what [pbrt-cameras.md](completed/pbrt-cameras.md)'s realistic camera traces through. A beam axis can visit an assembly as one node, whose ports are the assembly's input and output. `BeamSourceKind` gains the assembly's kind when this lands.

### The authored graph and the traced graph

The solver traces the real beam from each beam axis's source and builds the traced graph: segments, each the beam between two surface interactions, with the optic and surface it ended on ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)). A laser's beam leaves its output; a camera's is its chief ray, traced outward from the sensor's center along the view axis, as a realistic camera traces. The solver then walks the authored tree beside it and gives every node a status (`BeamNodeStatus` and `BeamNodeReport`, declared in `src/beam_axis.h`):

| Status | Meaning |
|---|---|
| Hit | the parent's port beam reaches this optic first, inside its clear aperture |
| Clipped | it reaches it, but loses a fraction of its power on the aperture or on something on the way; the fraction and the part are shown |
| Missed | it passes the optic; the distance from the aperture's edge and the direction to steer are shown |
| Blocked | it hits another object first, which is named |
| Below threshold | the parent's port carries less power than the axis's ghost threshold |
| Unreached | an ancestor was missed or blocked, the axis has no source, or the node dangles |

The Scene panel colors each node by its status. The Viewport already draws the authored order as arrows from the source through the visits (`RaytraceSceneEditor::drawBeamAxisOverlays()`, through Optical bench's `viewportOverlays` hook and `ViewportView`), in one color, brighter and labeled by port for the selected axis, and grey where a visit dangles. With statuses, the arrows turn green where the beam follows and red where it does not.

### Building and editing

Under the **Optical bench** capability ([physoptics-scene-capabilities.md](completed/physoptics-scene-capabilities.md)), the Scene panel has a **Beam axes** folder and **Add** → **Beam axis** (`src/rt_scene_editor_beam_axes.cpp`). Each axis is a row that opens on its visits, each labeled by the port it leaves its parent by and the optic it visits, and indented by depth.

Built:

- **Add** → **Beam axis** makes an axis from the selected laser or camera, else the scene's one laser, else with no source. Its panel's **source** combo attaches one later.
- **Add after** a visit (or the source) lists the optics the beam can go to next. On a free port it appends the visit; on a taken one it inserts it between the port and what hung there, which then hangs off the port chosen for the new optic. **Insert before** puts a visit in a node's place, the node hanging off the new optic's chosen port; an optic with no output (a detector) cannot go in front of anything.
- **Remove** deletes a visit; its children move up as the decisions say, and are marked for review, since the beam that reaches them has changed. **Remove with what follows** deletes a subtree. **Detach** and **Attach to** move a subtree.
- Deleting an optic removes every visit of it, the same way. Deleting an axis's laser or camera detaches its source; the axis stays.
- Each visit's context menu holds these edits and **Mark reviewed**; the axis's panel has them as buttons on the selected visit, with the ghost threshold under **Beam solver**, and under **Guided tracing** whether renders trace along this axis and **Guide renders by this axis**.

Waiting for the solver:

- **Follow beam** extends a node's port with whatever the traced beam actually hits next, repeatedly, until the beam leaves the table, hits a detector or a beam dump, or falls below the ghost threshold. On a setup already placed by hand, one click builds the whole axis. At a splitter it follows every port.
- Picking the next optic in the viewport with a click, which casts one CPU ray through `bvh_scene`.
- Each node's status in detail, and a locked transform started from a node's context menu.

A future node editor (imnodes, vendored like ImGuizmo) could draw the tree as boxes and wires. The tree view comes first, since it needs no new dependency and shows a chain of thirty elements more compactly.

### Change tracking

The axes live in `RaytraceSceneEditor` with the optics, in the same id space. An axis is changed only through `editBeamAxis()`, which moves the axis's own revision (`beamAxisRevision()`) and `beamAxesRevision()` when the edit changed anything. Neither is `revision()`: no render reads an axis until guided tracing does, so an axis edit restarts nothing. The solver re-solves an axis whose revision moved, and the segments downstream of an optic or blocker that changed ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)); the per-node dirty flags of selective updating belong to the solver's own state, not to the axis.

### Scene file

Payload version 32 adds:

```json
"beamAxes": [
  {"name": "Pump", "source": {"laser": 0}, "ghostThreshold": 1e-4,
   "nodes": [{"optic": 3},
             {"optic": 5, "parent": 0, "port": "Reflected"},
             {"optic": 7, "parent": 1, "port": "Transmitted"},
             {"optic": 8, "parent": 1, "port": "Reflected", "review": true},
             {"optic": 9, "parent": "dangling"}]}
]
```

Beside them, `"guideAxis"` is the index of the axis renders trace along, and `"render"` `"optics"` holds `"traceMode"` and `"guideStrength"` ([physoptics-guided-tracing.md](physoptics-guided-tracing.md)). The source is `{"laser": i}`, an index into `"optics"`, or `{"camera": i}`, one into `"cameras"`, and absent for none. Nodes name optics by their index in `"optics"`, their parent by its index in `"nodes"`, and ports by the name the kind gives them. The load drops a node whose optic no longer exists with a warning, and its children move up, as **Remove** does; it cuts loose a node whose port or place is not valid. The engine turns the indices into ids after the optics and cameras load (`VulkanEngine::openScene()`), and back on save, which renumbers the nodes by their places (`renumberBeamNodes()`). An assembly's record will add its datum's mount and list its members' poses relative to it.

## Registry, bindings and data

- `BeamAxis`, `BeamAxisNode`, `BeamSource`, `OpticPortId`, the tree edits and the node statuses (`src/beam_axis.h/.cpp`, in `rt_optics`).
- `RaytraceSceneEditor` holds the axes; the folder, the panel and the overlay are `src/rt_scene_editor_beam_axes.cpp`.
- `bin/beam_test` (`tests/beam_test.cpp`) checks the ports and the trees; `tools/verify/beam_axis_check.py` the scene file's round trip through the batch mode's `--save-scene`.
- No binding here: [physoptics-guided-tracing.md](physoptics-guided-tracing.md) flattens the graph into binding 58.

## Verification

Done on 2026-11-07:

- `bin/beam_test`: every kind's port names back to their ids; appending, a splitter's children by port, inserting before and after, each removal (a chain closes up, a splitter's lowest port takes its place and the other arm dangles, a dangling root's children dangle), both visits of an optic visited twice, moving and its refusals, renumbering, and 20000 random edits that each leave a tree. `--selfcheck` catches 115 planted errors.
- `tools/verify/beam_axis_check.py`: the seven `optics_verification` scenes each save their axes as they were, through the editor's ids, and save the same file again on a second round trip. A file with a missing optic, a port its parent lacks, two visits on one place, a cycle and a source that is no laser opens repaired as described above, with a warning for each. A malformed optic the load skips leaves the axes naming the optics they named. A version 31 file saves with no axes.
- The setup of the verification below, `beam_axis_setup` in `assets/scenes/tests/optics_verification/`, written by `tools/verify/optics_verification.py`: its two power meters read 2.076 and 2.547 mW of the laser's 5 mW at 64 samples per pixel, 0.925 of the laser's power, so every visit is in the beam. `beam_axis_double_pass` returns 0.929 of it through the lens's second visit.

Waiting for the solver, on `beam_axis_setup` (laser, two turning mirrors, an iris, a lens, a plate beamsplitter and two power meters, placed by hand; its `expected.json` lists these):

- **Follow beam** builds the axis with every node **Hit**.
- Turning the second mirror's adjuster by one revolution marks the lens **Missed**, with a distance from its aperture's edge that matches the geometry (twice the tilt times the distance, minus the aperture's radius) to 1 µm.
- Closing the iris to 0.6 mm marks it **Clipped** with the fraction the solver computes, and the nodes after it stay **Hit**.
- `beam_axis_double_pass` records two visits of the lens and solves both.

Still to check: a scene without beam axes renders bit-identically (nothing in the renderer reads an axis, but no render compared it).

## Order of work

1. ~~`BeamAxis` and its nodes, the folder and the tree view, the scene file.~~ Done 2026-11-07, with the overlay of the authored order.
2. Statuses from the solver, and the overlay's colors.
3. **Follow beam** and viewport picking.
4. Assemblies.

## Open questions

- Whether a port should be allowed several children for a beam that the designer knows lands half on one element and half on another. Physically that is clipping at the first, so this plan says no; the solver reports it.
- Whether an axis should carry measurement targets (a required power at a detector, a required spot size at a focus) that a status could compare against. They would make the panel a checklist for a setup; the request does not ask for it.
- Whether an insert should mark the visits after it for review, as a removal does. It does not now.

## Sources

Müller, Mueller, Brixner and von Mammen 2025, *A graph-based laser path solver algorithm for virtual reality laboratory simulations* (the dynamic graph of segments and its per-round-trip edges); the Zemax OpticStudio manual, sequential versus non-sequential modes (a surface sequence as the design intent); the imnodes library (github.com/Nelarius/imnodes) for a later node editor.
