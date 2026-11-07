# The Gaussian beam solver

Dragging a mirror's adjuster should move the beam in the viewport in the same frame, show the focus after the next lens shift, and say at once if the beam now clips an iris. That needs the beam's geometry and its size everywhere, for every beam axis, in a millisecond or two, which a Monte Carlo render cannot give. femtoPro solves the same problem with one stigmatic Gaussian beam per segment on the CPU, rebuilt downstream of each change. This plan does the same with three refinements: the chief ray is traced through the real surfaces in double precision, it carries its own first-order ray-transfer matrix from the surfaces' curvatures at each hit, and the beam is a general astigmatic Gaussian propagated through those matrices. Clipping, ghosts, back reflections, polarization and optical path length come out of the same trace.

*Not implemented. The beam axes it reports against were built on 2026-11-07, and the scenes it is checked on are in `assets/scenes/tests/optics_verification/`. Depends on [physoptics-beam-axis.md](physoptics-beam-axis.md), [physoptics-optical-surfaces.md](completed/physoptics-optical-surfaces.md) (the `double` intersector), [physoptics-optical-materials.md](completed/physoptics-optical-materials.md) (indices and coatings) and [physoptics-bench-and-mounts.md](completed/physoptics-bench-and-mounts.md) (where everything is, and what can block the beam). Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

- On 2026-10-18 the user chose the general astigmatic model, with the beam carried by the chief ray's ray-transfer matrix in double precision on the CPU, over femtoPro's stigmatic `q` with an active-plane lens law.
- On 2026-11-07 the user chose to compute each segment's ray-transfer matrix from the surfaces' principal curvatures (generalized Coddington equations, below), and to add the automatic differentiation this plan first described only if the oblique cases fail their tests. A finite-difference matrix, from four parabasal rays traced in `double`, is the reference both are checked against.
- On 2026-11-07 the user chose to write the solver's equations and the Beam window's layout themselves, with the plumbing around them built for them.

## What femtoPro does

A segment holds a scalar complex `q` with `1/q = 1/R + i λ M² / (π w²)`. An element acts in one plane: `k_out = (k_in − d/f) / |k_in − d/f|` and `1/q_out = 1/q_in − 1/f`. A clipped beam becomes a new Gaussian whose cross section fits the aperture, and its energy comes from a lookup table of a Gaussian's transmission through a displaced circle. Astigmatism is ignored. A queue of dirty nodes rebuilds only the edges downstream of a change ("selective updating"), with a cap on bounces for cavities (Brixner et al. 2023 and 2025; Müller et al. 2025).

## Design

### Where it runs

On the CPU, in `double`, in `rt_optics` (`src/beam_solver.h/.cpp`), with no Vulkan. Its inputs are a snapshot of the scene: each optic's prescription and world transform (`SceneOptic::world`, which `opticWorldTransform()` sets as the editor resolves the bench each frame, so a lens is where its origin puts it), its materials, every laser and beam axis, and a `bvh_scene` TLAS over everything that is not an optic (mounts, posts, the table, meshes, shapes) for blockers. The mounts, posts and table are engine-side today (`BenchScene`, which hands `buildSceneAccel()` its tagged instances), so the snapshot takes their meshes and transforms from the engine and builds the TLAS in `rt_optics` from them; the solver links no Vulkan. It runs on the main thread after the scene editor's panel and before the gizmo draws, so the beam drawn in a frame is the beam for that frame's poses.

### Tracing the chief ray

From each beam axis's source (a laser's aperture along its output axis, or a camera's chief ray leaving its sensor's center outward along the view axis; [physoptics-beam-axis.md](physoptics-beam-axis.md)), and from every laser of the scene whether an axis names it or not, the solver finds the nearest hit among the optic bodies (the `double` intersector) and the blockers (the `float` CPU traversal, whose precision is ample for deciding what is in the way). At an optic surface it reads the surface's two media and its material, and splits:

- The reflected and refracted directions follow from the surface normal, the two indices at the segment's wavelength, and total internal reflection when it applies.
- Each branch's power follows from the coating's complex `r` and `t` for s and p and the beam's polarization (below).
- A branch is followed if its power is at least the axis's ghost threshold times the laser's power (1e-4 by default), up to 64 interactions along one path and 4096 segments per axis. A branch that reaches either cap is marked, so a cavity reports that it was truncated, as femtoPro's does.

A segment is the beam between two interactions, inside a body as well as between bodies. It records its start point, direction, length, medium, the optic and surface (or blocker) it ends on, its parent segment, the branch it took (reflected or refracted at which surface), its power, its polarization, and its beam.

### The ray-transfer matrix

The matrix comes from the surface's principal curvatures first (the user's choice, 2026-11-07). At a hit, `principalCurvatures()` (`src/optic_surface.h`) gives the two curvatures and their directions; projecting the beam's transverse frame onto the tangent plane at the hit's obliquity, and refracting or reflecting the curvature matrix by the generalized Coddington equations (Stavroudis 1972; Herloski, Marshall and Antos 1983), gives the surface's 4 × 4 matrix, and free space between surfaces is the usual drift. The equations are short and visible, and a new surface kind needs only its curvature tensor. Their weak point is the oblique case whose principal directions do not line up with the plane of incidence, which is where the tests below look hardest.

If those tests fail there, the solver falls back to automatic differentiation, as this plan first described it. glm's geometric functions `static_assert` an IEEE 754 scalar, but a `Dual4<double>` can meet them without forking glm: the standard allows specializing `std::numeric_limits` for it, and glm calls `sqrt` unqualified, so argument-dependent lookup finds the dual's. The geometry's 30-odd `std::sqrt` and `std::abs` calls would become unqualified, and `reflectDirection()` and `refractDirection()`, which take `glm::dvec3` today, templated.

The automatic differentiation route: `rt_optics`'s geometry is templated on the scalar type ([physoptics-optical-surfaces.md](completed/physoptics-optical-surfaces.md)). The solver instantiates it with `Dual4<double>`, a forward-mode dual number carrying four derivatives: with respect to the chief ray's two transverse positions and two transverse reduced directions (`n` times the direction's transverse components) at the segment's start. Tracing the chief ray through a surface with dual numbers gives, at no extra cost in code, the exact Jacobian of the outgoing ray with respect to the incoming one: the segment's 4 × 4 ray-transfer matrix `S` in the beam's transverse frames. It is exact to rounding for every surface kind, including even aspheres and their Newton iterations, and it includes the oblique-incidence effects (the tangential and sagittal focal lengths of a tilted spherical mirror, `R cos θ / 2` and `R / (2 cos θ)`) without a formula for any of them. This is how the request's "single ray per propagation segment" carries the beam: one ray, and its derivatives.

Each segment's transverse frame is a pair of unit vectors perpendicular to its direction, carried from the parent by parallel transport, so the frame twists only when the geometry twists the beam.

### The beam

A general astigmatic Gaussian beam's field is `exp(−i k rᵀ Q⁻¹ r / 2)` for a complex symmetric 2 × 2 matrix `Q` (Arnaud and Kogelnik 1969). The solver carries it as Arnaud's two complex rays, the columns of a 4 × 2 complex matrix `[X; U]` with `Q = X U⁻¹`, and propagates them through each segment's `S` by one matrix product, the generalized ABCD law with no inversion along the way. From `Q` anywhere along a segment:

- the 1/e² intensity radii along the principal axes, `wᵢ = √(λ / (π μᵢ))` with `μᵢ` the eigenvalues of `−Im(Q⁻¹)`, and the principal axes' orientation, which rotates along z for a twisted beam;
- the wavefront curvatures, from `Re(Q⁻¹)`;
- each axis's waist position and radius and Rayleigh range, from propagating `Q` in closed form to where `Re(Q⁻¹)` vanishes along that axis.

A laser's M² enters as an embedded Gaussian: its two complex rays at the waist are scaled so that `wᵢ` uses `M²ᵢ λ` on each of the laser's axes (Siegman). For a beam with unequal M² whose axes later rotate, this is an approximation, which the readout flags.

The root segment comes from `laserBeam()` (`src/optic_laser.h`, built 2026-11-02): the beam's center, axis and x axis in the laser's body frame (the pointing applied), each axis's waist, waist position and M², the center wavelength and the power. Its Jones vector waits for the laser's polarization ([pbrt-polarization.md](pbrt-polarization.md), "Lasers"); until then the solver carries an unpolarized laser as two orthogonal Jones vectors of half the power each, as below. `tools/verify/optics_verification.py` already checks the renderer's beams against ABCD through lenses and mirrors, and its closed-form code (`tools/verify/beam_probe.py`) is the reference `bin/beam_test` can share.

### Polarization

The chief ray carries a Jones vector in its transverse frame. At each surface the solver rotates it into the plane of incidence, multiplies by the coating's `rₛ, rₚ` or `tₛ, tₚ`, and rotates back, so a polarizing beamsplitter's two ports carry their real powers and a quarter-wave plate turns linear into circular ([physoptics-polarization-optics.md](physoptics-polarization-optics.md) adds retarders and birefringent crystals to this). An unpolarized or partially polarized laser is carried as two orthogonal Jones vectors whose powers add, which is exact for the powers. This works with or without the polarized path-tracer family, since the solver is a few complex multiplies per surface.

### Power and clipping

At each surface the solver integrates the beam's intensity over the surface's clear aperture, projecting the elliptical Gaussian from `Q` onto the surface's plane at the hit's obliquity. A 32 × 32 Gauss–Hermite grid over the Gaussian, each point tested against the aperture shape, gives the transmitted fraction to better than 1e-4 for any aperture. The lost fraction is recorded as clipping on that surface.

Between surfaces, the solver tests what the beam's envelope crosses: the same Gauss–Hermite points, carried along the segment by `S`, are cast against the blocker TLAS out to the segment's end, and the weight of those that hit is the fraction lost to that blocker, recorded with its optic and part. A beam scraping the edge of a KM100's front plate is caught this way although its chief ray misses the plate.

A clipped beam keeps its Gaussian parameters and loses power. femtoPro instead fits a new, smaller Gaussian into the aperture; neither is physical, since the real clipped beam diffracts. The readout shows the fraction lost and marks the beam downstream as "clipped upstream", and the diffraction it would show belongs to the wave backend (see the last section).

### Ghosts and back reflections

Every branch above the ghost threshold is traced, so the second-surface reflection of a lens, the etalon pair of an unwedged plate and the leak through a dielectric mirror appear as dim segments. Two cases get their own warnings:

- Back reflection into the laser. A branch that reaches the laser's aperture is reported with its power and the angle between it and the output axis, since feedback into a laser destabilizes it.
- Ghost focus. A ghost branch that comes to a waist on or inside an optic is reported with its peak intensity, `2P / (π w_x w_y)`, since a focused ghost can damage a coating.

### Selective updating

The solver keeps last frame's segments. An edit changes some optics, materials, lasers or blockers; the solver invalidates:

- every segment that ends on a changed optic, starts from one, or is inside one;
- every segment whose envelope, a capsule of three times the beam's largest radius around the chief ray, overlaps a changed object's old or new bounds;
- every descendant of those.

It re-traces from the first invalid segment of each branch, and leaves everything else. Moving the last mirror before a detector re-solves two segments; changing the laser re-solves everything, as in femtoPro. A full re-solve of a 30-element axis with ghosts is expected to take well under 5 ms; if one exceeds the 2 ms budget during a drag, ghost branches below 1 % of the laser's power are deferred to the next frames and the panel marks them as pending.

### Outputs

- The traced graph and the node statuses of [physoptics-beam-axis.md](physoptics-beam-axis.md).
- Segments with their beams, powers and polarizations, for [physoptics-beam-preview.md](physoptics-beam-preview.md).
- Chief rays and hit points, for [physoptics-locked-transforms.md](physoptics-locked-transforms.md).
- Analytic readouts at detectors, beside the Monte Carlo ones ([physoptics-detectors.md](completed/physoptics-detectors.md)), and the analytic Gaussian field at a plane for ZBF export ([physoptics-data-export.md](physoptics-data-export.md)).
- Statuses for every node of every beam axis ([physoptics-beam-axis.md](physoptics-beam-axis.md), `BeamNodeReport` in `src/beam_axis.h`), the inputs of **Follow beam**, and the overlay's colors. An axis that changed says so by its revision (`RaytraceSceneEditor::beamAxisRevision()`), which moves on every edit through `editBeamAxis()`, while `revision()` does not.
- **The solver's prediction at each detector**, step 4 of the detectors plan, which moved here when the detectors were built without a solver (2026-11-04): the solved power reaching the detector, and its two Gaussian radii, their axes and its centroid at the sensor's plane, in the detector's own frame (`GpuDetector`'s origin and axes, `detectorGeometry()`), shown in each detector's window beside the measured `BeamMoments` with their relative differences, and written to `detectors.json` by `--detectors`. Agreement within the measured standard error says the setup behaves paraxially; disagreement is aberration, clipping or stray light. Until then `tools/verify/beam_probe.py`'s ABCD stands in for it in the checks.
- **A sensor stack sized by the beam.** A sensor stack (`SensorStack`, `src/optic_detectors.h`, added 2026-11-05) gives every member the same size and film, as one detector slid along a translation stage has. With a solved beam, the stack gains a size option that follows the beam along its axis: each member's width and height set to a multiple of the solved radii at its plane (4 w by default, the half field the verification scripts use), with the film's pixels unchanged, so a fit through a focus samples the narrow planes as finely as the wide ones. The stack keeps the fixed size as its default, since a real stage's sensor does not change size. The member sizes are derived each time the solver runs, as the members' places are now, and never saved; the scene file saves the multiple. `gaussian_train`, `oap_focus` and `tilted_spherical_mirror` (`tools/verify/optics_verification.py`) then go back to sizing each plane by the beam, through the stack.
- Optical path length per segment, `n` times the length in `double`, summed from the laser, and the Gouy phase of each axis. Nothing in these plans uses them except the readout; they are kept for the wave backend.

### The Beam window

**Windows** → **Beam**, registered under the **Optical bench** capability ([physoptics-scene-capabilities.md](completed/physoptics-scene-capabilities.md)), shows the solved beam: the selected axis's segments as a table (start, end, length, medium, power, clipping), and a probe you drag along a segment, with the beam there: position, direction, the two radii and their axes' orientation, both waists and Rayleigh ranges, both wavefront radii, power, the polarization ellipse, and the optical path length. **Copy as CSV** puts the table on the clipboard.

The solve time per frame is a live measurement, and so goes in the **Stats** window with a sparkline, as CLAUDE.md asks of every measurement that moves frame to frame, not in the Beam window.

## Registry, bindings and data

- `BeamSolver`, `BeamSegment`, `TracedBeam`, the ray-transfer matrices, the complex-ray propagation and the Gauss–Hermite tables (`src/beam_solver.*`, `src/beam_gaussian.*`), in `rt_optics`; `Dual4<double>` (`src/dual.h`) only if the curvature route fails its oblique tests.
- `tests/beam_test.cpp` → `bin/beam_test`, which already holds the beam axis's checks (2026-11-07).
- The **Beam** window. No binding, no kernel, no scene-file change: the solver's settings (ghost threshold, caps) belong to the beam axis.

## Verification

- `bin/beam_test`:
    - free space: `w(z)`, the waist and the Rayleigh range of a laser match the closed forms to 1e-12 relative;
    - every segment's `S` is symplectic, `Sᵀ J S = J`, to 1e-12, and matches central finite differences of four parabasal rays to 1e-8, at normal incidence and at 10°, 45° and 70° on a sphere, a toric patch of an asphere and a tilted off-axis parabola, with the principal directions turned against the plane of incidence;
    - a plano-convex lens focuses a collimated beam to the waist and distance that the paraxial thick-lens ABCD matrix predicts, to 1e-6 relative;
    - a spherical mirror at 45° gives tangential and sagittal waists at the distances `R cos θ / 2` and `R / (2 cos θ)` predict;
    - an off-axis parabola focuses a collimated beam aligned with its parent axis to `w₀' = M² λ f / (π w)` with no astigmatism, and a misalignment of 1 mrad produces the astigmatism the dual-number trace predicts;
    - power is conserved: the transmitted, reflected, absorbed, clipped and truncated powers add up to the laser's to 1e-12;
    - a Brewster window transmits p fully, and a polarizing cube splits a 45° linear input 50/50;
    - selective updating gives the same segments, bit for bit, as a full re-solve, over a scripted sequence of 1000 random edits.
- The scenes of [physoptics-verification-scenes.md](physoptics-verification-scenes.md): each beam axis all **Hit** at its nominal alignment, the solver's radii at each probe plane against the detectors' within their noise, and `beam_axis_setup`'s and `beam_axis_double_pass`'s nudges (`<name>.expected.json`).
- Against OpticStudio ([physoptics-data-export.md](physoptics-data-export.md)): the same lens train's paraxial Gaussian beam and skew Gaussian beam analyses agree with the solver's radii and waist positions to 1e-4 relative.
- Timing: the solve time per frame, shown in the Beam window, stays under 2 ms while dragging an adjuster in the 30-element test setup.

## Order of work

1. Chief-ray tracing through optic bodies, branching and power, with no beam yet.
2. The ray-transfer matrices from principal curvatures, with the finite-difference and symplecticity tests; `Dual4` only if the oblique cases fail.
3. Complex rays, the beam, waists and the readouts; the Beam window.
4. Clipping by apertures, then by blockers.
5. Polarization.
6. Ghosts, back reflections and ghost foci.
7. Selective updating and the budget.

## Waiting for the wave backend

These need the phases and optical path lengths the wave backend will carry. When this plan is implemented, move this section to `docs/plans/waiting/physoptics-gaussian-beam-solver.md`.

- Diffraction from clipping: the rings and side lobes a beam develops after a hard aperture or a mount's edge, in place of the power loss alone.
- Interference between segments that overlap on a detector: femtoPro's integrated visibility factor between two Gaussian beams of different radii, positions and directions is a closed form the solver could evaluate from its `Q`, its optical path lengths and its Jones vectors. Whether this cheap analytic interference belongs in the solver, or waits to be replaced by the wave backend's, is the user's call.
- Gouy phase and optical path length in readouts that compare arms of an interferometer.

## Sources

Stavroudis 1972, *The optics of rays, wavefronts, and caustics* (the generalized Coddington equations); Arnaud and Kogelnik 1969, *Gaussian light beams with general astigmatism*, Applied Optics 8(8); Arnaud 1985, *Representation of Gaussian beams by complex rays*, Applied Optics 24(4); Kogelnik and Li 1966, *Laser beams and resonators*; Siegman 1986, *Lasers*, chapters 15, 17 and 20; Herloski, Marshall and Antos 1983, *Gaussian beam ray-equivalent modeling and optical design*, Applied Optics 22(8); Stone and Forbes 1990, *Optimal interpolants for Runge–Kutta ray tracing in inhomogeneous media* (differential ray tracing); Griewank and Walther 2008, *Evaluating derivatives*, chapter 3 (forward-mode automatic differentiation); Brixner et al. 2023 and 2025, and Müller et al. 2025 (femtoPro's beam model and solver).
