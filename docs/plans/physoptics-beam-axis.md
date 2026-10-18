# The beam axis

On a lab notebook sketch, an experiment is a line: laser, turning mirror, lens, iris, beamsplitter, then two arms. That line is the design intent, and the alignment is the work of making the real beam follow it. The beam axis is that sketch as a scene object: a tree whose nodes are visits to optical elements and whose edges leave each element by a named port. You build it by hand, or from the beam the solver traces, and the solver then reports every place where the real beam leaves it. The tree also tells the locked transforms what moves with what ([physoptics-locked-transforms.md](physoptics-locked-transforms.md)), and the path tracer which surfaces a light path is meant to reach next ([physoptics-guided-tracing.md](physoptics-guided-tracing.md)).

*Not implemented. Depends on [physoptics-optical-elements.md](physoptics-optical-elements.md) and [physoptics-laser-sources.md](physoptics-laser-sources.md). The solver that compares it with the real beam is [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md). Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

- The graph does not place anything. You place the optics with the gizmo; the graph records the order the beam should visit them in (the request).
- Elements that split the beam (beamsplitters, acousto-optic modulators, nonlinear crystals) have several output ports, each with its own child; everything else is a chain (the request).
- The trace modes the graph enables in the path tracer are all four (the user, 2026-10-18).

## What femtoPro does

femtoPro's graph is derived, not authored: its solver discovers which element each beam segment reaches and builds the graph from that, every frame, with each round trip in a cavity a new set of edges (Müller et al. 2025). There is no notion of where the beam ought to go. This plan keeps femtoPro's derived graph as the traced graph, which the solver builds, and adds the authored one beside it, so the difference between the two is what you see.

## Design

### Nodes are visits

```cpp
struct BeamAxisNode {
    uint32_t id;                  // stable within its axis
    uint64_t optic;               // the SceneOptic this node visits
    uint32_t parent;              // the node whose port leads here; none for the root
    OpticPortId port;             // which of the parent's ports
    bool operator==(const BeamAxisNode&) const = default;
};

struct BeamAxis {
    uint64_t id;
    std::string name;
    BeamAxisKind kind;            // Beam (rooted at a laser) or Assembly (a rigid group, below)
    std::vector<BeamAxisNode> nodes;   // nodes[0] is the root
    TraceMode mode;               // physoptics-guided-tracing.md: full, seeded, guided, sequential
    float guideStrength;          // the guided mode's bias toward the graph
    float ghostThreshold;         // the solver's power floor for ghost branches, relative to the root
    bool operator==(const BeamAxis&) const = default;
};
```

A node is a visit, not an optic, so the same optic can appear more than once: the double pass through a lens in front of a retroreflector, the beamsplitter of a Michelson interferometer, which the beam crosses going out and coming back. Every visit has exactly one parent, so the authored graph is a tree. Two branches that end on the same detector, as in a Mach–Zehnder interferometer, are two nodes visiting one detector; their powers add, and the phases that would make them interfere are the wave backend's.

Each element kind names its ports ([physoptics-optical-elements.md](physoptics-optical-elements.md)): **Reflected**, **Transmitted**, **Turned**, **Retro**, **Refracted**, and later **Order m** for gratings, **Ordinary** and **Extraordinary** for birefringent crystals. A port is defined by the sequence of reflections and refractions at the element's surfaces that produces it, so the solver can match a traced branch to a port exactly. A port has at most one child node: a beam leaving a port goes to one element next.

### Assemblies

An axis of kind **Assembly** is a rigid group: a lens made of several elements, an objective, a camera lens, a beam expander built as a unit. Its members are placed relative to the assembly's datum, which is itself mounted like one optic ([physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)): on a post, or free. Moving the assembly moves every member, and **Edit Transform** on a member moves it within the assembly. An assembly has no laser; its root is an input reference (a ray and a beam, or a field of view and an aperture for a lens), and it is what [physoptics-prescription-import.md](physoptics-prescription-import.md) creates from a `.zmx` file and what [pbrt-cameras.md](pbrt-cameras.md)'s realistic camera traces through. A beam axis can visit an assembly as one node, whose ports are the assembly's input and output.

### The authored graph and the traced graph

The solver traces the real beam from each beam axis's laser and builds the traced graph: segments, each the beam between two surface interactions, with the optic and surface it ended on ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)). It then walks the authored tree beside it and gives every node a status:

| Status | Meaning |
|---|---|
| Hit | the parent's port beam reaches this optic first, inside its clear aperture |
| Clipped | it reaches it, but loses a fraction of its power on the aperture or on something on the way; the fraction and the part are shown |
| Missed | it passes the optic; the distance from the aperture's edge and the direction to steer are shown |
| Blocked | it hits another object first, which is named |
| Below threshold | the parent's port carries less power than the axis's ghost threshold |
| Unreached | an ancestor was missed or blocked |

The Scene panel colors each node by its status, and the viewport draws the authored order as labeled arrows between optics, the way it draws camera frustums today (an ImGui overlay through the foreground draw list), in green where the beam follows and red where it does not.

### Building and editing

Under the **Optical bench** capability ([physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)), the Scene panel has a **Beam axes** folder and **Add** → **Beam axis**. Each axis is a tree of its nodes, labeled by optic name and port.

- **New beam axis** asks for a laser and creates the root.
- **Follow beam** extends a node's port with whatever the traced beam actually hits next, repeatedly, until the beam leaves the table, hits a detector or a beam dump, or falls below the ghost threshold. On a setup already placed by hand, one click builds the whole axis. At a splitter it follows every port.
- **Append**, **Insert after** and **Insert before** add a visit of an optic chosen from a list, or picked in the viewport with a click, which casts one CPU ray through `bvh_scene`.
- **Remove** deletes a visit; its children move up to its parent's port and are marked for review, since the beam that reaches them has changed.
- Deleting an optic removes every visit of it, the same way.
- Each node's context menu sets its port, shows its status in detail, and starts a locked transform on its optic.

A future node editor (imnodes, vendored like ImGuizmo) could draw the tree as boxes and wires. The tree view comes first, since it needs no new dependency and shows a chain of thirty elements more compactly.

### Change tracking

The axes live in `RaytraceSceneEditor` with the optics, under the same ids and the same `revision()` counter. Each axis also keeps a revision of its own structure, and each node a dirty flag the solver sets when anything upstream of it changes, so the solver re-solves only what is downstream of an edit ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)).

### Scene file

The payload version this lands in adds:

```json
"beamAxes": [
  {"name": "Pump", "kind": "beam", "mode": "seeded", "guideStrength": 0.8, "ghostThreshold": 1e-4,
   "nodes": [{"optic": 0},
             {"optic": 3, "parent": 0, "port": "output"},
             {"optic": 5, "parent": 1, "port": "reflected"},
             {"optic": 7, "parent": 2, "port": "transmitted"},
             {"optic": 8, "parent": 2, "port": "reflected"}]}
]
```

Optics are named by their index in `"optics"`, ports by name. A node whose optic or port no longer exists is dropped with a load warning and its children move up, as **Remove** does. An assembly's record adds its datum's mount and lists its members' poses relative to it.

## Registry, bindings and data

- `BeamAxis`, `BeamAxisNode`, `BeamAxisKind`, `OpticPortId`, the node statuses (`src/beam_axis.h/.cpp`).
- `RaytraceSceneEditor` gains the axes, the **Beam axes** folder and the overlay.
- No binding here: [physoptics-guided-tracing.md](physoptics-guided-tracing.md) flattens the graph into binding 58.

## Verification

- A scene without beam axes renders bit-identical and saves the same apart from the version.
- On a test setup of a laser, two turning mirrors, a lens, a plate beamsplitter and two detectors, placed by hand, **Follow beam** builds the axis with every node **Hit**.
- Turning the second mirror's adjuster by one revolution marks the lens **Missed**, with a distance from its aperture's edge that matches the geometry (twice the tilt times the distance, minus the aperture's radius) to 1 µm.
- Sliding an iris into the beam marks it **Clipped** with the fraction the solver computes, and the nodes after it stay **Hit**.
- A retroreflected double pass through a lens records two visits of the lens and solves both.
- Save, reopen and compare: equal by `==`.

## Order of work

1. `BeamAxis` and its nodes, the folder and the tree view, the scene file.
2. Statuses from the solver, and the overlay.
3. **Follow beam** and viewport picking.
4. Assemblies.

## Open questions

- Whether a port should be allowed several children for a beam that the designer knows lands half on one element and half on another. Physically that is clipping at the first, so this plan says no; the solver reports it.
- Whether an axis should carry measurement targets (a required power at a detector, a required spot size at a focus) that a status could compare against. They would make the panel a checklist for a setup; the request does not ask for it.

## Sources

Müller, Mueller, Brixner and von Mammen 2025, *A graph-based laser path solver algorithm for virtual reality laboratory simulations* (the dynamic graph of segments and its per-round-trip edges); the Zemax OpticStudio manual, sequential versus non-sequential modes (a surface sequence as the design intent); the imnodes library (github.com/Nelarius/imnodes) for a later node editor.
