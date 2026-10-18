# Locked transforms

Rebuilding a setup by hand after one change is most of the work of a lab. Turn a 90° turning mirror to 60° and every lens, filter and detector after it is now in the wrong place. Move the second of two steering mirrors and both mirrors need re-aiming before the beam reaches the next optic again. A locked transform does that rework in one gesture: you choose one degree of freedom of one optic to drive, each other optic on the beam axis responds by a rule, and a deterministic solver moves them so that the beam path stays as it was wherever the rules say it should. This plan defines the rules, the order they are solved in, the presets for the request's two examples and for the classic two-iris beam walk, and how it all sits behind the existing gizmo.

*Not implemented. Depends on [physoptics-beam-axis.md](physoptics-beam-axis.md), [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md) (the chief rays) and [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md) (the poses an optic can take). Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

- The request asks for a deterministic result for a given set of options, and leaves the method and the options to this plan.
- Placement is breadboard-physical ([physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)): a response may move a post, turn it, raise it, or turn an adjuster, and nothing else, unless the optic is free.

## What the renderer does today

- `TransformGizmo` edits one target through a getter and a setter. A setter can write anything, so one setter can move many objects.
- `RaytraceSceneEditor` counts changes with one `revision()`, which every consumer compares, so many objects moved in one frame cost one restart.
- There is no undo.

## Design

### The rules

Each node of a beam axis has a rule for locked edits, stored on the node with a default per kind:

| Rule | What the optic does when the beam reaching it changes | Default for |
|---|---|---|
| **Fixed** | nothing | detectors, the laser |
| **Carry** | keeps its pose relative to the beam arriving at it: the same distance along the chief ray from its parent's hit point, the same transverse offset, the same orientation in the beam's frame | lenses, filters, irises, wave plates, prisms |
| **Re-aim** | stays where it is and turns so that its port's output passes through its child's datum | mirrors, beamsplitters |
| **Re-center** | slides across the beam (post x and z, and height) so that the beam meets its datum, and turns to the same incidence it had | none by default; a choice for lenses and irises |

Beam-relative coordinates for **Carry** are taken from the solved beam when the edit begins, so the layout carried is the one on the screen.

### The driven degree of freedom

**Lock** in an optic's panel, or in its node's context menu, starts a locked edit. You pick the degree of freedom to drive from those its mount allows:

- slide the post along x, along z, or along the horizontal projection of the beam arriving at it;
- raise or lower it;
- turn it about the post, shown for a mirror as its **turning angle**, the angle between the beam arriving and the beam leaving, which changes by twice the yaw;
- turn one adjuster;
- for a free optic, translate or rotate about any one world axis.

The gizmo then shows only that axis (ImGuizmo's single-axis operations), and a numeric field beside it takes an exact value, so "90° to 60°" is typed rather than dragged.

### The solve

On every change of the driven value, the solver runs these steps in this order. Every step is closed-form or a Newton solve started from the edit's initial pose, so the same inputs always give the same poses, bit for bit.

1. Drive. Set the driven optic's pose from the driven value.
2. Re-aim the parent. If the driven optic's parent node is **Re-aim**, turn the parent so its port's output passes through the driven optic's datum. This is what keeps the beam on a mirror that has been moved.
3. Walk the tree. Re-trace the chief ray from the root, node by node in the axis's order, and at each node apply its rule against the chief ray that now reaches it: **Carry** places the optic from its beam-relative coordinates, **Re-aim** turns it toward its child, **Re-center** slides it onto the beam, **Fixed** leaves it. The driven optic applies its own rule here too, which lets a moved mirror re-aim at its child.
4. Project onto the mount. A pose that a rule asks for may be one the mount cannot take: a carried post would have to tilt when the beam it follows rises. The solver keeps the post vertical, takes the requested position and yaw from the requested pose's horizontal components and height, and puts the remaining tip and tilt into the mount's adjusters, within their travel. Whatever is left over, past an adjuster's travel or on a mount without adjusters, is reported per optic as the misalignment the rule could not remove.
5. Re-solve. The beam solver re-solves the changed part of the axis, and the beam axis statuses show the result.

Two rules would make the order ill-defined, and the panel refuses them with an explanation: a **Re-aim** node whose child is **Carry** (it would aim at a target that is itself following the beam it is aiming), and a cycle through repeated visits of the same optic, which the solver breaks by solving only the first visit.

For a mirror, **Re-aim** splits its turn into a yaw for the horizontal component and a vertical adjuster for the rest, which is how a mirror on a post is aimed on a bench. **Adjusters only** in the edit's settings keeps the yaw and uses both adjusters, failing (and saying so) when the required turn exceeds their range.

### Presets

| Preset | Driven | Rules it sets |
|---|---|---|
| **Rotate and carry** | an optic's turn about its post, or a mirror's turning angle | the optic **Re-aim** off; every descendant **Carry** |
| **Translate and re-steer** | a slide of an optic | its parent **Re-aim**, the optic **Re-aim**, its descendants **Fixed** |
| **Slide along the beam** | a slide along the arriving beam | the optic **Carry** off (it moves along its own beam, so it stays centered); its descendants **Fixed** |
| **Walk through two irises** | nothing: a one-shot solve | the two mirrors upstream of two chosen irises **Re-aim** to the line through the irises' centers |
| **Custom** | any | the rules as set per node |

The request's two examples are the first two presets. Turning a 90° turning mirror to 60° with **Rotate and carry** turns the mirror by 15° about its post and swings everything after it by 30° about the mirror's hit point, rigidly, so every downstream optic is exactly where it was relative to the beam. Moving the second of two mirrors with **Translate and re-steer** re-aims the first at it and re-aims it at the optic after it, which stays where it was.

**Walk through two irises** is the beam walk every laser lab does: two irises define the target line, the second mirror's hit point is where that line, traced backward, crosses the second mirror's surface, the first mirror re-aims at that point, and the second re-aims along the line. With both mirrors' positions fixed and their four adjuster angles free, the four conditions (two points on a line) fix the solution.

### Snapping

The driven post follows **Snap to holes** like any drag. Carried posts land wherever the beam takes them; **Snap carried posts** rounds them to holes afterwards and reports the misalignment that rounding causes, which is often what an experimenter wants to see before deciding where to bolt things down.

### Revert

There is no undo in the application. A locked edit keeps every pose it started from, and **Revert** in the edit's panel restores them all. **Done** keeps the result. The edit ends either way when another object is selected.

## Registry, bindings and data

- `LockRule` on `BeamAxisNode`, saved with the axis; the presets; `LockedEdit`, the session (`src/beam_locks.h/.cpp`), with the solve in `rt_optics` so that `bin/beam_test` runs it without Vulkan.
- The gizmo's setter for a locked edit; **Lock**, **Revert** and **Done** in the optic's panel.
- No binding or kernel change.

## Verification

`bin/beam_test`, on scripted setups:

- **Rotate and carry**: a laser, a turning mirror at 90° and a chain of two lenses, a filter and a detector. Setting the turning angle to 60° leaves every downstream optic's beam-relative coordinates unchanged to 1 nm and 1 nrad, every post vertical, and every node **Hit**.
- **Translate and re-steer**: two mirrors and a lens. Sliding the second mirror 20 mm along x puts the beam on the second mirror's datum and on the lens's datum, each to 1 nm, and leaves the lens unmoved.
- **Walk through two irises**: after random adjuster turns on both mirrors, the preset puts the beam through both irises' centers to 1 nm, and its adjuster angles match the closed-form solution.
- Determinism: replaying the same sequence of driven values twice gives bit-identical poses.
- Refusal: a **Re-aim** node with a **Carry** child is rejected with its message, and nothing moves.
- **Revert** restores every pose exactly.

## Order of work

1. `LockRule`, the walk of step 3 with **Fixed** and **Carry**, and **Rotate and carry**.
2. **Re-aim**, the parent pre-pass, and **Translate and re-steer**.
3. The projection onto mounts and the residual report.
4. **Re-center**, **Slide along the beam**, and **Walk through two irises**.
5. Snapping carried posts, **Revert**.

## Open questions

- Whether **Carry** should optionally preserve the optical path length between nodes rather than the geometric distance, for setups where an arm's length matters to an interferometer. That is a different invariant from the request's, and depends on what the wave backend will want to keep fixed.
- Whether a locked edit should be able to drive a laser's pointing. The laser is the root, so everything downstream would follow by **Carry** or re-aim; it falls out of the same solve if the laser's pointing is exposed as a degree of freedom.

## Sources

Siegman 1986, *Lasers*, chapter 15 (ray matrices for misaligned elements); Thorlabs' kinematic mount and beam steering tutorials (the two-mirror, two-iris beam walk); the femtoPro alignment procedures in Brixner et al. 2023, §3.5 (coarse placement, then fine adjustment).
