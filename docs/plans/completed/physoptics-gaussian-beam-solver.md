# The Gaussian beam solver

Dragging a mirror's adjuster should move the beam in the viewport in the same frame, show the focus after the next lens shift, and say at once if the beam now clips an iris. That needs the beam's geometry and its size everywhere, for every beam axis, in a millisecond or two, which a Monte Carlo render cannot give. femtoPro solves the same problem with one stigmatic Gaussian beam per segment on the CPU, rebuilt downstream of each change. This plan does the same with three refinements: the chief ray is traced through the real surfaces in double precision, it carries its own first-order ray-transfer matrix from the surfaces' curvatures at each hit, and the beam is a general astigmatic Gaussian propagated through those matrices. Clipping, ghosts, back reflections, polarization and optical path length come out of the same trace.

*Implemented 2026-11-07, apart from what "Not built" lists; "As built" says where the code differs from the design below, and [the waiting file](../waiting/physoptics-gaussian-beam-solver.md) holds what waits for the wave backend. Polarization moved to [physoptics-polarization-optics.md](../physoptics-polarization-optics.md). Depends on [physoptics-beam-axis.md](../physoptics-beam-axis.md), [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md) (the `double` intersector), [physoptics-optical-materials.md](physoptics-optical-materials.md) (indices and coatings) and [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md) (where everything is). Part of the [laser optics plan](../physoptics-laser-optics.md).*

## Decisions already made

- On 2026-10-18 the user chose the general astigmatic model, with the beam carried by the chief ray's ray-transfer matrix in double precision on the CPU, over femtoPro's stigmatic `q` with an active-plane lens law.
- On 2026-11-07 the user chose to compute each segment's ray-transfer matrix from the surfaces' principal curvatures (generalized Coddington equations, below), and to add the automatic differentiation this plan first described only if the oblique cases fail their tests. A finite-difference matrix, from four parabasal rays traced in `double`, is the reference both are checked against.
- On 2026-11-07 the user first chose to write the solver's equations and the Beam window's layout themselves, then asked for this plan and the preview to be implemented whole. The surface matrix, the Gaussian readouts, the clipping integral and the Beam window were written for the user to review.
- On 2026-11-07 the user chose that the solver traces the optics' own surfaces only. Mounts, posts, the breadboard, meshes and shapes are left to the path tracer, and a beam that misses an optic, or meets a lens's rim from outside, carries on past it.
- Polarization waits for [physoptics-polarization-optics.md](../physoptics-polarization-optics.md), which now holds this plan's polarization work (the user, 2026-11-07). Until then the solver averages s and p at every coating, as the renderer does.

## What femtoPro does

A segment holds a scalar complex `q` with `1/q = 1/R + i λ M² / (π w²)`. An element acts in one plane: `k_out = (k_in − d/f) / |k_in − d/f|` and `1/q_out = 1/q_in − 1/f`. A clipped beam becomes a new Gaussian whose cross section fits the aperture, and its energy comes from a lookup table of a Gaussian's transmission through a displaced circle. Astigmatism is ignored. A queue of dirty nodes rebuilds only the edges downstream of a change ("selective updating"), with a cap on bounces for cavities (Brixner et al. 2023 and 2025; Müller et al. 2025).

## Design

### Where it runs

On the CPU, in `double`, in `rt_optics` (`src/beam_solver.h/.cpp`, the Gaussian math in `src/beam_gaussian.h/.cpp`), with no Vulkan. Its input (`BeamSolverInput`) is the editor's optics, each with its prescription, its slots' materials and its world transform (`SceneOptic::world`, which `opticWorldTransform()` sets as the editor resolves the bench each frame), the optical library, the surrounding medium, the beam axes, and the cameras an axis starts from. Nothing else in the scene is traced: the bodies are what light meets in the solver. `RaytraceSceneEditor::solveBeams()` runs it from `VulkanEngine::updateBench()`, right after `resolveBench()`, so the beam drawn in a frame is the beam for that frame's poses.

### Tracing the chief ray

From every laser of the scene, and from each camera a beam axis starts from (its chief ray leaves the camera's position along its view axis; [physoptics-beam-axis.md](../physoptics-beam-axis.md)), the solver finds the nearest hit among the optic bodies with the `double` intersector. A ray outside every body tests them all, behind a slab test of each body's world box, and passes a surface marked `OPTIC_SURFACE_EDGE` (a lens's rim, a mirror's side) as if it were not there. A ray inside a body tests that body alone. An iris's opening and a probe plane are crossed rather than hit: the segment ends where the chief ray crosses the opening's plane inside it, or the probe plane inside its sensor, and one child carries on unchanged.

At a surface the slot's material decides:

- A polished optical interface splits the beam. The reflected and refracted directions follow from the normal, the two indices at the beam's wavelength and total internal reflection. Each branch's power follows from the coating (`evaluateCoating()`, at the angle in the incident medium, the average of s and p), and the coating's absorptance is lost.
- A ground interface scatters the beam, a detector's sensor or a card detects it, a black or diffuse surface (an iris's blade) absorbs it, and a laser's aperture takes it back: each ends the branch.
- A medium's absorption, `exp(−α L)` at the beam's wavelength, takes its share along each segment inside a body.

A branch is followed while its power is at least the ghost threshold times the source's power (1e-4 by default; a laser several axes start from takes the smallest of their thresholds), up to `BEAM_MAX_INTERACTIONS` = 64 along one path and `BEAM_MAX_SEGMENTS` = 4096 per tree. A branch that reaches a cap ends in a `Truncated` segment.

A segment (`BeamSegment`) is the beam between two interactions, inside a body as well as between bodies. It records the state it starts with (`BeamBranchState`: origin, direction, transverse frame, complex rays, power, the body and medium it runs in, the optical path and distance from the source, the clipping upstream, the events since it entered its body, the port it left its last optic by, and whether it is a ghost), its length, what it ends on and where, the power arriving, the share of it the end's aperture took, and what the end did with the power.

The events (surface, reflect or refract) from entering a body to leaving it are matched against the kind's ports (`matchOpticPort()`), forward or reversed, so a double pass through a lens matches its Transmitted port both ways. A branch that matches no port is a ghost, as is everything after it.

### The ray-transfer matrix

At each hit the solver computes the surface's 4 × 4 matrix from its principal curvatures (`principalCurvatures()`, `src/optic_surface.h`; the user's choice, 2026-11-07), in reduced coordinates: a neighbouring ray is a transverse offset `x` and `u`, the index times the transverse direction. The neighbour meets the surface's tangent plane at `t = E x − d (N · E x) / (N · d)`, with `E` the incoming frame, `d` the chief ray's direction and `N` the normal turned along `d`, where the normal has changed by `δN = W t`, `W = −Σ kᵢ dᵢ dᵢᵀ` the shape operator. Snell's law `n' d' = n d + γ N`, with `γ = n' cos θ' − n cos θ` (or `−2 n cos θ` for a reflection), perturbed and projected along `N` onto the plane square to `d'` by `Q' = I − N d'ᵀ / (d' · N)`, gives (`surfaceTransfer()`, `src/beam_gaussian.cpp`):

```text
A = E'ᵀ T        B = 0
C = γ E'ᵀ Q' W T  D = E'ᵀ Q' E
```

Free space between surfaces is `B = (L / n) I`. These are the generalized Coddington equations in matrix form (Stavroudis 1972; Herloski, Marshall and Antos 1983). Every matrix is symplectic to 4e-15, and matches central differences of parabasal rays traced through the real surface to 1.5e-8, on a sphere, an even asphere hit off its axis with its principal directions 53 degrees from the plane of incidence and an off-axis paraboloid, at 0, 10, 45 and 70 degrees, reflected and refracted. The automatic differentiation the plan held in reserve for oblique cases (a `Dual4<double>` through the templated geometry) was not needed.

Each segment's transverse frame is carried from its parent by parallel transport (`transportFrame()`): mirrored at a reflection, turned by the rotation that takes `d` to `d'` at a refraction, so the frame twists only when the geometry twists the beam.

### The beam

A general astigmatic Gaussian beam's field is `exp(−i k₀ rᵀ Q⁻¹ r / 2)`, with `Q` the reduced complex curvature matrix (Arnaud and Kogelnik 1969): `q / n` for a stigmatic beam, so `1/Q = n / R − i λ₀ / (π w²)` with the vacuum wavelength in any medium. The solver carries it as Arnaud's two complex rays, the columns of `[X; U]` with `Q = X U⁻¹` (`ComplexRays`), and propagates them through each segment's matrix by one product. Over a distance `s` in a medium of index `n`, `Q(s) = Q + (s / n) I`. From `Q` anywhere along a segment (`crossSection()`, `beamProfile()`):

- the 1/e² radii along the principal axes, `wᵢ² = λ₀ / (π μᵢ)` with `μᵢ` the eigenvalues of `−Im(Q⁻¹)`, and their angle from the frame;
- the wavefront radii, `n` over the eigenvalues of `Re(Q⁻¹)` along those axes;
- each axis's waist, its position and its Rayleigh range, from the diagonal of `Q` in the frame of the intensity ellipse at the segment's start; `BeamProfile::twisted` says when the off-diagonal is more than 1e-6 of it, and the profiles are approximate.

A laser's M² enters as an embedded Gaussian: the complex rays carry the embedded beam, and the real beam's w² tensor is the embedded one's scaled by `diag(M²ₓ, M²ᵧ)` in the transported frame. The scale is the laser's M² times `n₀ λ₀ / λ`, so the solver gives back the laser's own `w(z)` in the surrounding medium exactly, as the renderer's light paths and `tools/verify/beam_probe.py` have it. For unequal M² whose axes turn from the frame, this is an approximation, which `twisted` flags.

The root segment comes from `laserBeam()` (`src/optic_laser.h`): the beam's center, axis and x axis in the laser's body frame, each axis's waist, waist position and M², the wavelength and the power. A camera's chief ray carries a notional collimated beam of 1 mm at 550 nm and 1 W, which nothing draws.

### Power and clipping

At each polished surface the solver integrates the beam's intensity over the surface's clear aperture, the elliptical Gaussian carried along the beam onto the surface's tangent plane at the hit's obliquity (`fractionInside()`). In coordinates where the Gaussian is a unit normal, each line of the first coordinate is cut into the runs that land inside, found on a 49-point grid and their ends refined by bisection, and each run is integrated exactly with `erf`. Across the first coordinate, the places where the number of runs changes split the span, and each piece takes 24 Gauss-Legendre nodes under `a = m + h sin t`, which smooths the square-root edge a run opens or closes with. A beam whose 6-sigma ellipse lies inside a convex aperture at 32 points counts as wholly inside without the integral. The map from the beam's transverse plane to the surface's local coordinates is affine, so it is evaluated once per hit, and a test costs a 2 × 2 product. The lost share is recorded as clipping on that surface.

An iris clips the same way where the beam crosses its opening, and a beam whose chief ray lands on the blades ends there, its visit Clipped by the blades' share.

A clipped beam keeps its Gaussian parameters and loses power, as femtoPro's does not (femtoPro fits a smaller Gaussian into the aperture); neither is physical, since the real clipped beam diffracts. The power budget (`BeamPowerBudget`) records where every watt went: detected, absorbed by coatings and black surfaces, absorbed in glass, clipped, scattered, escaped, back into a laser, dropped below the threshold, and truncated. It adds up to the source's power to 1e-12.

### Ghosts and back reflections

Every branch above the ghost threshold is traced, so the second-surface reflection of a lens, the etalon pair of an unwedged plate and the leak through a dielectric mirror appear as dim segments. Two cases get a warning (`BeamWarning`), shown in the axis's panel:

- Back reflection into a laser: a branch that reaches a laser's aperture, with its power and the angle between it and the output axis.
- Ghost focus: a ghost whose waist lies inside a body, or within a Rayleigh range of the surface it ends on, with its peak intensity `2P / (π wₓ wᵧ)`.

### Selective updating

The solver keeps last solve's trees and optics. A solve compares each optic and its slots' materials with what it traced before, and collects the changed ones' old and new world boxes. In each tree it re-traces, from its stored start state, every segment that ends on or runs inside a changed optic, or whose envelope (three times the beam's larger radius at either end, around the chief ray) overlaps a changed box, with everything after it; an escaped segment is re-traced at any change. A subtree is contiguous in depth-first order, so the kept and re-traced segments make the new tree in place. A changed laser, threshold, library, surrounding medium or design wavelength re-solves its tree whole. The trees, budgets, readings and statuses equal a full solve's bit for bit, over 1000 random edits in `bin/beam_test`.

Moving one mirror re-traces the segments after it; a change nowhere near a beam re-traces nothing, and the solution's revision does not move. A solve also re-reads every axis, which costs about 0.1 ms on the verification scenes.

### Outputs

- The traced trees (`BeamSolution::trees`), each a source's segments with their beams, powers, ends and ports, its budget, its warnings, and the beam where it meets a detector's sensor or crosses a probe plane (`BeamDetectorReading`: power on the sensor, centroid, radii and angle in the detector's own frame).
- A report per visit of every beam axis (`BeamNodeReport`, `src/beam_axis.h`): its status, the power reaching it, the share clipped, the miss distance and the direction to steer, the optic hit instead, and the segment along which the beam arrives, which the Viewport's arrows follow. Follow beam reads the trees (`followBeam()`).
- Segments for the raster preview ([physoptics-beam-preview.md](physoptics-beam-preview.md)), and chief rays and hit points for [physoptics-locked-transforms.md](../physoptics-locked-transforms.md).
- Optical path length per segment, `n L` summed from the source, and each axis's Gouy phase in the Beam window's probe; kept for the wave backend.
- `--solve-beams <file>` in the batch mode writes the solution as JSON, every optic by name, and `--follow-beams` first runs Follow beam on every axis without a visit.

### The Beam window

**Windows** → **Beam**, registered under the **Optical bench** capability and docked below the Viewport in the Optics workspace, shows one beam: the selected axis's, or the one its combo picks. Its segments are a table (length, medium, power, clipping, what each ends on), ghosts on request, and **Copy as CSV** puts every segment on the clipboard. A probe dragged along the selected segment shows the beam there: position, direction, distance from the source, both radii and their axes' angle, both wavefront radii, each axis's waist, its position and Rayleigh range and the Gouy phase, the power and the clipping upstream, and the optical path. The preview's settings are in **Optics** → **Beam Preview** ([physoptics-beam-preview.md](physoptics-beam-preview.md)), since they are display, not results.

The solve time per frame is a live measurement, so it goes in the **Stats** window's **Beam solver** section with a sparkline, beside the segments the last solve traced.

## Registry, bindings and data

- `BeamSolver`, `BeamSolution`, `BeamTree`, `BeamSegment`, `BeamBranchState`, `BeamPowerBudget`, `BeamDetectorReading`, `BeamWarning`, `matchOpticPort()` and `followBeam()` in `src/beam_solver.h/.cpp`; `ComplexRays`, `RayTransfer`, `surfaceTransfer()`, `transportFrame()`, `crossSection()`, `beamProfile()` and `fractionInside()` in `src/beam_gaussian.h/.cpp`; both in `rt_optics`.
- `RaytraceSceneEditor::solveBeams()`, `beamSolver()` and `followBeamFrom()`; the Beam window, `src/rt_scene_editor_beam_window.cpp`.
- `bin/beam_test` and `tools/verify/beam_solver_check.py`.
- No binding, no kernel. The scene file adds nothing for the solver: its settings (the ghost threshold) belong to the beam axis.

## Verification

Done on 2026-11-07. `bin/beam_test` (its `--selfcheck` catches 159 planted errors):

- Free space: `w(z)` of an astigmatic laser with unequal M² matches the closed form to 1e-12 relative, and its waists, their positions and Rayleigh ranges to 1e-12.
- Every surface matrix is symplectic to 3.6e-15, and matches central differences of parabasal rays traced through the real surface to 1.5e-8, in 24 cases: a sphere, an even asphere off its axis with its principal directions turned 53 degrees against the plane of incidence, and an off-axis paraboloid, at 0, 10, 45 and 70 degrees, reflected and refracted.
- A spherical mirror of R = 0.2 m at 45 degrees: the tangential and sagittal waists where `R cos θ / 2` and `R / (2 cos θ)` put them, to 1e-10. A 90-degree off-axis parabola focuses a collimated beam on its parent axis to one waist, the two axes' foci equal to 1e-10, at `f + y² / 4f` from the hit.
- A plano-convex lens's focus against the thick lens's ABCD law, with the library's indices at 632.8 nm: the waist's position and size to 1e-6.
- A bench of a laser, a turning mirror, an iris, a lens, a plate beamsplitter and two power meters: Follow beam builds the axis, every visit Hit; the budget adds up to the laser's power to 1e-12; each meter's reading matches the beam's cross section there. Closing the iris clips it by a Gaussian's loss through the opening, `exp(−2 a² / w²)`, to 1e-6, and the visits after it stay Hit. Moving the lens 2 mm past the beam reads Missed by 2 mm, steered back toward it; a mirror put in front of the iris reads Blocked.
- 1000 random moves, turns and iris openings, each solved selectively and fully: the same trees, budgets, readings and statuses every time, 149 of them re-tracing only part of the tree.
- Timing, the plan's 30-element setup (a laser, 28 turning mirrors in a zigzag and a meter): a full solve 0.29 ms; dragging the 20th mirror's tilt over 30 frames, 0.45 ms a frame on average and 0.86 ms at worst, under the 2 ms budget.

`tools/verify/beam_solver_check.py`, on the scenes of [physoptics-verification-scenes.md](../physoptics-verification-scenes.md):

- Every beam axis reads Hit at every visit, and every budget adds up to 1e-12.
- `gaussian_train`: the waist after the lens, 44.8315 µm at 102.5253 mm past the back vertex, against the ABCD law's to 1e-4 and 1 µm. The telescopes' radii on the probe plane and the profiler against ABCD to 1e-4. `oap_focus`: one waist of 20.4486 µm at 101.6830 mm, no astigmatism. `tilted_spherical_mirror` at 45 degrees: the foci at 70.7571 and 141.5499 mm, against ABCD to 1 µm.
- `beam_axis_setup`: Follow beam on the emptied axis builds it with every visit Hit. With the iris opened wide, M2's adjuster turned five revolutions (40 mrad) marks the lens Missed by the distance the turned beam passes its edge, to 1 µm. One revolution, as the beam axis plan had it, turns the beam 16 mrad, 3.2 mm at the lens, which still hits it; and the open iris's blades take the beam past 1.6 revolutions. The iris closed to 0.6 mm (it clamps to its 1.4 mm minimum) reads Clipped by 0.82 %, and the visits after it Hit.
- `beam_axis_double_pass`: both visits of the lens Hit, the focus 94.1589 mm past it as ABCD has it, and the return 15.0 mm off the lens's axis against the 15.1 mm of `2 f tan 2α`.
- The meters against the path tracer: within 3 %. `beam_axis_setup`'s PM_R reads 2.132 mW against the render's 2.076 (+2.7 %), converged at 1024 samples per pixel over two seeds, and PM_T 2.530 against 2.547. Exact RGB tabulates a coating over 360 to 830 nm and projects it onto RGB, so the splitter's split is not the one at 632.8 nm. A monochromatic render at 632.8 nm divides the beam 0.4548 to the reflected arm, the solver 0.4573 and exact RGB 0.4490.

Still to check: against OpticStudio's paraxial and skew Gaussian beam analyses ([physoptics-data-export.md](../physoptics-data-export.md)), which needs OpticStudio.

## Order of work

1. ~~Chief-ray tracing through optic bodies, branching and power, with no beam yet.~~
2. ~~The ray-transfer matrices from principal curvatures, with the finite-difference and symplecticity tests.~~ `Dual4` was not needed.
3. ~~Complex rays, the beam, waists and the readouts; the Beam window.~~
4. ~~Clipping by apertures.~~ Clipping by blockers was dropped (the user, 2026-11-07).
5. Polarization: moved to [physoptics-polarization-optics.md](../physoptics-polarization-optics.md).
6. ~~Ghosts, back reflections and ghost foci.~~
7. ~~Selective updating~~ and the budget.

All but 5 done on 2026-11-07.

## As built

- No blockers. The plan traced the chief ray against a TLAS of everything that is not an optic, and cast the Gauss-Hermite points of each segment's envelope against it. The user chose to leave all of that to the path tracer, and a beam that misses an optic carries on.
- The clipping integral is not a 32 × 32 Gauss-Hermite grid. A grid of points cannot follow an aperture's edge, which cuts the Gaussian discontinuously; the runs and `erf` above are exact along one axis, and the split pieces converge along the other. Closing an iris to 1.4 mm on a 0.45 mm beam agrees with `exp(−2 a² / w²)` to 1e-6 relative; a 96-node rule over the whole span was off by 0.5 %.
- The budget's deferral is not built: if a solve exceeded 2 ms during a drag, ghosts below 1 % of the laser's power were to wait for later frames. The 30-element setup's slowest drag frame is 0.86 ms.
- A camera's chief ray carries a notional 1 mm beam at 550 nm, which the preview does not draw: a camera has no beam of its own until a realistic camera's assembly gives it one.
- Statuses: **Clipped** means the end's aperture took at least `BEAM_CLIP_REPORT` = 1e-4 of the beam. **Blocked** names another optic the beam reaches first; with no blockers, a mount in the way is not reported.

## Not built

- The solver's prediction in each detector's readings, and a sensor stack sized by the beam. The solver computes the beam where it meets every sensor and probe plane (`BeamDetectorReading`, and `--solve-beams`), but the detectors' readings in the Raytraced Output window and `detectors.json` do not show it yet, and sensor stacks keep their one size. Both change code the detectors plan built, and wait for the user's decision.
- Polarization (Jones vectors, polarizing coatings' ports, the Brewster and polarizing cube checks): [physoptics-polarization-optics.md](../physoptics-polarization-optics.md).
- The analytic Gaussian field at a plane for ZBF export: [physoptics-data-export.md](../physoptics-data-export.md).

## Sources

Stavroudis 1972, *The optics of rays, wavefronts, and caustics* (the generalized Coddington equations); Arnaud and Kogelnik 1969, *Gaussian light beams with general astigmatism*, Applied Optics 8(8); Arnaud 1985, *Representation of Gaussian beams by complex rays*, Applied Optics 24(4); Kogelnik and Li 1966, *Laser beams and resonators*; Siegman 1986, *Lasers*, chapters 15, 17 and 20; Herloski, Marshall and Antos 1983, *Gaussian beam ray-equivalent modeling and optical design*, Applied Optics 22(8); Stone and Forbes 1990, *Optimal interpolants for Runge–Kutta ray tracing in inhomogeneous media* (differential ray tracing); Griewank and Walther 2008, *Evaluating derivatives*, chapter 3 (forward-mode automatic differentiation); Brixner et al. 2023 and 2025, and Müller et al. 2025 (femtoPro's beam model and solver).
