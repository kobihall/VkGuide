# The Gaussian beam solver

Dragging a mirror's adjuster should move the beam in the viewport in the same frame, show the focus after the next lens shift, and say at once if the beam now clips an iris. That needs the beam's geometry and its size everywhere, for every beam axis, in a millisecond or two, which a Monte Carlo render cannot give. femtoPro solves the same problem with one stigmatic Gaussian beam per segment on the CPU, rebuilt downstream of each change. This plan does the same with three refinements: the chief ray is traced through the real surfaces in double precision, it carries its own exact first-order ray-transfer matrix by forward-mode automatic differentiation, and the beam is a general astigmatic Gaussian propagated through those matrices. Clipping, ghosts, back reflections, polarization and optical path length come out of the same trace.

*Not implemented. Depends on [physoptics-beam-axis.md](physoptics-beam-axis.md), [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md) (the `double` intersector), [physoptics-optical-materials.md](physoptics-optical-materials.md) (indices and coatings) and [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md) (where everything is, and what can block the beam). Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

On 2026-10-18 the user chose the general astigmatic model, with the beam carried by the chief ray's ray-transfer matrix in double precision on the CPU, over femtoPro's stigmatic `q` with an active-plane lens law.

## What femtoPro does

A segment holds a scalar complex `q` with `1/q = 1/R + i λ M² / (π w²)`. An element acts in one plane: `k_out = (k_in − d/f) / |k_in − d/f|` and `1/q_out = 1/q_in − 1/f`. A clipped beam becomes a new Gaussian whose cross section fits the aperture, and its energy comes from a lookup table of a Gaussian's transmission through a displaced circle. Astigmatism is ignored. A queue of dirty nodes rebuilds only the edges downstream of a change ("selective updating"), with a cap on bounces for cavities (Brixner et al. 2023 and 2025; Müller et al. 2025).

## Design

### Where it runs

On the CPU, in `double`, in `rt_optics` (`src/beam_solver.h/.cpp`), with no Vulkan. Its inputs are a snapshot of the scene: each optic's prescription and world transform (`opticWorldTransform()`), its materials, every laser and beam axis, and a `bvh_scene` TLAS over everything that is not an optic (mounts, posts, the table, meshes, shapes) for blockers. It runs on the main thread after the scene editor's panel and before the gizmo draws, so the beam drawn in a frame is the beam for that frame's poses.

### Tracing the chief ray

From each laser's aperture, along its output axis, the solver finds the nearest hit among the optic bodies (the `double` intersector) and the blockers (the `float` CPU traversal, whose precision is ample for deciding what is in the way). At an optic surface it reads the surface's two media and its material, and splits:

- The reflected and refracted directions follow from the surface normal, the two indices at the segment's wavelength, and total internal reflection when it applies.
- Each branch's power follows from the coating's complex `r` and `t` for s and p and the beam's polarization (below).
- A branch is followed if its power is at least the axis's ghost threshold times the laser's power (1e-4 by default), up to 64 interactions along one path and 4096 segments per axis. A branch that reaches either cap is marked, so a cavity reports that it was truncated, as femtoPro's does.

A segment is the beam between two interactions, inside a body as well as between bodies. It records its start point, direction, length, medium, the optic and surface (or blocker) it ends on, its parent segment, the branch it took (reflected or refracted at which surface), its power, its polarization, and its beam.

### The ray-transfer matrix by automatic differentiation

`rt_optics`'s geometry is templated on the scalar type ([physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)). The solver instantiates it with `Dual4<double>`, a forward-mode dual number carrying four derivatives: with respect to the chief ray's two transverse positions and two transverse reduced directions (`n` times the direction's transverse components) at the segment's start. Tracing the chief ray through a surface with dual numbers gives, at no extra cost in code, the exact Jacobian of the outgoing ray with respect to the incoming one: the segment's 4 × 4 ray-transfer matrix `S` in the beam's transverse frames. It is exact to rounding for every surface kind, including even aspheres and their Newton iterations, and it includes the oblique-incidence effects (the tangential and sagittal focal lengths of a tilted spherical mirror, `R cos θ / 2` and `R / (2 cos θ)`) without a formula for any of them. This is how the request's "single ray per propagation segment" carries the beam: one ray, and its derivatives.

Each segment's transverse frame is a pair of unit vectors perpendicular to its direction, carried from the parent by parallel transport, so the frame twists only when the geometry twists the beam.

### The beam

A general astigmatic Gaussian beam's field is `exp(−i k rᵀ Q⁻¹ r / 2)` for a complex symmetric 2 × 2 matrix `Q` (Arnaud and Kogelnik 1969). The solver carries it as Arnaud's two complex rays, the columns of a 4 × 2 complex matrix `[X; U]` with `Q = X U⁻¹`, and propagates them through each segment's `S` by one matrix product, the generalized ABCD law with no inversion along the way. From `Q` anywhere along a segment:

- the 1/e² intensity radii along the principal axes, `wᵢ = √(λ / (π μᵢ))` with `μᵢ` the eigenvalues of `−Im(Q⁻¹)`, and the principal axes' orientation, which rotates along z for a twisted beam;
- the wavefront curvatures, from `Re(Q⁻¹)`;
- each axis's waist position and radius and Rayleigh range, from propagating `Q` in closed form to where `Re(Q⁻¹)` vanishes along that axis.

A laser's M² enters as an embedded Gaussian: its two complex rays at the waist are scaled so that `wᵢ` uses `M²ᵢ λ` on each of the laser's axes (Siegman). For a beam with unequal M² whose axes later rotate, this is an approximation, which the readout flags.

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
- Analytic readouts at detectors, beside the Monte Carlo ones ([physoptics-detectors.md](physoptics-detectors.md)), and the analytic Gaussian field at a plane for ZBF export ([physoptics-data-export.md](physoptics-data-export.md)).
- Optical path length per segment, `n` times the length in `double`, summed from the laser, and the Gouy phase of each axis. Nothing in these plans uses them except the readout; they are kept for the wave backend.

### The Beam window

**Windows** → **Beam**, registered under the **Optical bench** capability ([physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)), shows the solved beam: the selected axis's segments as a table (start, end, length, medium, power, clipping), and a probe you drag along a segment, with the beam there: position, direction, the two radii and their axes' orientation, both waists and Rayleigh ranges, both wavefront radii, power, the polarization ellipse, and the optical path length. **Copy as CSV** puts the table on the clipboard.

## Registry, bindings and data

- `BeamSolver`, `BeamSegment`, `TracedBeam`, `Dual4<double>`, the complex-ray propagation and the Gauss–Hermite tables (`src/beam_solver.*`, `src/beam_gaussian.*`, `src/dual.h`), in `rt_optics`.
- `tests/beam_test.cpp` → `bin/beam_test`.
- The **Beam** window. No binding, no kernel, no scene-file change: the solver's settings (ghost threshold, caps) belong to the beam axis.

## Verification

- `bin/beam_test`:
    - free space: `w(z)`, the waist and the Rayleigh range of a laser match the closed forms to 1e-12 relative;
    - every segment's `S` is symplectic, `Sᵀ J S = J`, to 1e-12, and matches central finite differences of four parabasal rays to 1e-8;
    - a plano-convex lens focuses a collimated beam to the waist and distance that the paraxial thick-lens ABCD matrix predicts, to 1e-6 relative;
    - a spherical mirror at 45° gives tangential and sagittal waists at the distances `R cos θ / 2` and `R / (2 cos θ)` predict;
    - an off-axis parabola focuses a collimated beam aligned with its parent axis to `w₀' = M² λ f / (π w)` with no astigmatism, and a misalignment of 1 mrad produces the astigmatism the dual-number trace predicts;
    - power is conserved: the transmitted, reflected, absorbed, clipped and truncated powers add up to the laser's to 1e-12;
    - a Brewster window transmits p fully, and a polarizing cube splits a 45° linear input 50/50;
    - selective updating gives the same segments, bit for bit, as a full re-solve, over a scripted sequence of 1000 random edits.
- Against OpticStudio ([physoptics-data-export.md](physoptics-data-export.md)): the same lens train's paraxial Gaussian beam and skew Gaussian beam analyses agree with the solver's radii and waist positions to 1e-4 relative.
- Timing: the solve time per frame, shown in the Beam window, stays under 2 ms while dragging an adjuster in the 30-element test setup.

## Order of work

1. Chief-ray tracing through optic bodies, branching and power, with no beam yet.
2. `Dual4` and the ray-transfer matrices, with the finite-difference and symplecticity tests.
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

Arnaud and Kogelnik 1969, *Gaussian light beams with general astigmatism*, Applied Optics 8(8); Arnaud 1985, *Representation of Gaussian beams by complex rays*, Applied Optics 24(4); Kogelnik and Li 1966, *Laser beams and resonators*; Siegman 1986, *Lasers*, chapters 15, 17 and 20; Herloski, Marshall and Antos 1983, *Gaussian beam ray-equivalent modeling and optical design*, Applied Optics 22(8); Stone and Forbes 1990, *Optimal interpolants for Runge–Kutta ray tracing in inhomogeneous media* (differential ray tracing); Griewank and Walther 2008, *Evaluating derivatives*, chapter 3 (forward-mode automatic differentiation); Brixner et al. 2023 and 2025, and Müller et al. 2025 (femtoPro's beam model and solver).
