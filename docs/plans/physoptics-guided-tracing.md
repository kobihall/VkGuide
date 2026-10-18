# Guided tracing

A light path from a laser through a twenty-element setup meets twenty-odd surfaces, and at each the path tracer must find what it hits next and decide whether it reflects or refracts. The beam axis already says what it should hit and which way it should go. This plan lets the path tracer use that, in four modes chosen per beam axis: **Full** ignores the graph; **Seeded** uses it only to speed up the search, and gives the same result as **Full**; **Guided** also biases each reflect-or-refract choice toward the graph and reweights, which stays unbiased and still finds every ghost; **Sequential** traces the graph's surfaces alone, as a lens design program's sequential mode does, which is fast and ignores clipping by anything outside the graph and every ghost.

*Not implemented. Depends on [physoptics-beam-axis.md](physoptics-beam-axis.md), [physoptics-laser-sources.md](physoptics-laser-sources.md), the light integrator of [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md) and [physoptics-detectors.md](physoptics-detectors.md), which measures what the modes change. Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

On 2026-10-18 the user chose all four modes. The request asks for a maximal mode in which no ray traverses the full scene, a proper render that also finds clipping, stray light, back reflections and ghosts, and something weighted between them.

## What the renderer does today

- Kernel 02 traces every ray with `traceScene()` over the full TLAS, through the selected traversal ([pbrt-roadmap.md](pbrt-roadmap.md), integrator layer).
- [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md)'s light integrator runs stage 11 (emit), the shared 01 and 02, stage 13 (scatter light paths) and the camera connections; [physoptics-detectors.md](physoptics-detectors.md) adds stage 26, which deposits on detectors.
- A path carries no notion of where it is in an optical layout.

## What other simulators do

Non-sequential optical software faces the same trade. OpticStudio's non-sequential mode splits rays at every partial interface (deterministically, by power) or picks a branch at random, and can bias scattered rays toward "importance sampling targets", with weights that keep the result unbiased. FRED does the same with "importance sampling toward specified surfaces" and ray splitting controls. Both also offer sequential tracing, where a ray meets only the next surface in a list and is lost if it misses its aperture. This plan's modes are those, expressed through the beam axis instead of a surface list or target list.

## Design

### Per-path beam state

Binding 57, `OpticPathState`, 16 bytes per pool slot:

```glsl
struct OpticPathState {
    uint node;          // the beam-axis node the path is at or heading for, or CRT_OFF_GRAPH
    uint entry;         // the surface it entered the node's optic through, or none
    uint bounces;       // reflections inside the current body
    uint flags;         // inside a body; the axis's mode
};
```

It is allocated only while some beam axis has a mode other than **Full**, as `MediumPathState` is allocated only when a scene has a medium ([pbrt-participating-media.md](pbrt-participating-media.md)). `PathState` does not grow. Stage 11 sets `node` to the root of the laser's axis when it emits.

Binding 58, `BeamGraph`, is the axes flattened by the CPU when the scene is built: per node, its optic's instance slot, its mode and guide strength, and its ports, each a child node and the (entry surface, exit surface, internal reflections) triples that identify it ([physoptics-optical-elements.md](physoptics-optical-elements.md) defines ports by such sequences).

### Following the graph

The scatter stage that handles a light path's hit on an optic (stage 13 for light paths) updates the state:

- Entering the node's optic records the entry surface; each internal reflection counts a bounce.
- Leaving it looks up the (entry, exit, bounces) triple among the node's ports. A match moves `node` to that port's child, or off the graph if the port has none. No match (a ghost) moves the path off the graph.
- Hitting anything that is not the node's optic or the expected child's (a mount, the table, another optic) moves the path off the graph.

A path off the graph stays off it, and is traced as in **Full**.

### The four modes

| Mode | Traversal | Reflect or refract | Exact |
|---|---|---|---|
| **Full** | `traceScene()` over the TLAS, every ray | by the coating's R and T | yes |
| **Seeded** | on the graph: the expected optic first, then `traceScene()` with `tMax` at that hit | by R and T | yes, the same result as **Full** |
| **Guided** | as **Seeded** | biased toward the graph's ports, reweighted | yes, unbiased |
| **Sequential** | on the graph: the expected optic only, no TLAS | the graph's branch, weighted by its power | no: no clipping by anything else, no ghosts |

In **Seeded** mode, for a path on the graph, kernel 02 intersects the expected optic's body first with `testOpticBody()` (the current node's optic while the path is inside it, the child's otherwise), then calls `traceScene()` with `tMax` set just past that hit. The BVH walk then culls everything beyond the expected surface and usually ends after a few nodes; if it finds something closer, a mount or another optic, that is the hit, exactly as **Full** would find. The image and the detector readings are identical to **Full** with the same seed: only the counted work changes.

In **Guided** mode, at a partial interface, the scatter stage picks reflection with probability

```text
p_R = (1 − |g|) · R / (R + T) + |g| · q_R
```

where `g` is the axis's guide strength and `q_R` is 1 when only the reflected branch stays on the graph, 0 when only the refracted branch does, and `R / (R + T)` when both do or neither does. The branch's weight is divided by its probability, so the estimate is unbiased for every detector. Every branch keeps a probability of at least `(1 − |g|)` times its physical share, so ghosts and stray light still converge, more slowly. A negative `g` inverts the bias toward the branches that leave the graph, for a stray-light study: the main beam is then the rare event, and the ghosts are sampled often. The default is `g = 0.8`.

In **Sequential** mode, on the graph, kernel 02 tests only the expected body. A path that misses it, outside its clear aperture, is lost and counted as vignetted. At a partial interface the path takes the graph's branch with its power as weight (`R` or `T`), the way sequential lens software reports transmitted power, and at a splitter whose two ports both have children it picks one by power. Nothing off the graph exists in this mode: no mount clips, no ghost forms. It is for design work on a long train, where it costs a fraction of a BVH walk per ray, and the panel says what it leaves out.

The mode is a uniform per-dispatch flag, `CRT_FLAG_BEAM_GRAPH`, with the per-axis mode in `BeamGraph`. Kernel 02 reads binding 57 and 58 only under the flag, so camera paths, which have no beam state, never touch them; the bindings stay declared, and the renderer publishes a 16-byte placeholder for each while no axis needs them, since a declared binding nobody published aborts. This is a uniform branch in the shared kernel 02, not a variant, so it works in every integrator that runs kernel 02.

### Counters and the render guard

The light integrator's counters gain, per frame: paths on the graph and off it, vignetted paths (**Sequential**), and the mean guided weight per branch kind, so a badly chosen guide strength shows as a spread of weights. The render guard prices a ray in **Sequential** at the expected body's surface count, and in **Seeded** and **Guided** at the full traversal's cost, its upper bound.

## Registry, bindings and data

- Binding 57 `OpticPathState`; binding 58 `BeamGraph`; `CrtBinding::OpticPathState`, `CrtBinding::BeamGraph`.
- `CRT_FLAG_BEAM_GRAPH`; the seeded and sequential branch in kernel 02 (`crt_intersect.glsl`); the state update and the guided choice in the light paths' scatter stage and in `optical_interface`'s sampling ([physoptics-optical-materials.md](physoptics-optical-materials.md)).
- `buildBeamGraph()` (`src/beam_graph.h/.cpp`).
- The mode and guide strength are the beam axis's, saved with it ([physoptics-beam-axis.md](physoptics-beam-axis.md)).

## Verification

- With every axis in **Full**, every scene renders bit-identical, and no graph buffer is allocated.
- **Seeded** gives bit-identical detector films and images to **Full** with a fixed seed, on the 30-element test setup with mounts in the beam, and the traversal counters fall. The ratio of nodes visited goes into this file's results table.
- **Guided** converges to **Full**'s detector readings within noise for every detector, on-graph and off-graph, at `g` = 0.5, 0.8, 0.95 and −0.8; its relative MSE on the on-graph detectors at equal sample count is lower than **Full**'s, and on a ghost detector higher, by amounts recorded here.
- **Sequential** matches the beam solver's transmitted power at each on-graph detector to 1e-3 relative, and misses exactly the clipping and ghost power the solver attributes to off-graph objects.
- A path that hits a mount is off the graph from then on, in every mode but **Sequential**.

## Order of work

1. `OpticPathState`, `BeamGraph`, and the on-graph tracking, with every axis still in **Full**. Bit-identical.
2. **Seeded**, and its bit-identical check.
3. **Sequential**.
4. **Guided**, including negative guide strengths.
5. The counters and the results table.

## Open questions

- Whether **Guided** should also guide the light integrator's choice of which laser to emit from when a scene has several, toward the lasers whose axes reach a selected detector. The light integrator picks by power today.
- Whether camera paths, in a scene viewed through a realistic camera, should follow the camera's lens assembly in **Sequential** inside kernel 00 or as light-path-style graph state. [pbrt-cameras.md](pbrt-cameras.md) traces the assembly sequentially inside kernel 00, which needs none of this plan's state.

## Sources

The Zemax OpticStudio manual, non-sequential ray tracing (ray splitting, importance sampling toward targets) and sequential ray tracing; Photon Engineering, FRED documentation, ray splitting and importance sampling; Breault Research, ASAP technical guides on stray-light analysis; PBR 4ed §2.2 (importance sampling, splitting and Russian roulette); Arvo and Kirk 1990, *Particle transport and image synthesis* (splitting and roulette in particle tracing).
