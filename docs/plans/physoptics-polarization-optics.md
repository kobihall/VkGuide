# Polarization optics

Polarization is how a laser lab controls power. A half-wave plate in a rotation mount ahead of a polarizing beamsplitter divides a beam between two arms in any ratio; a quarter-wave plate makes it circular; a window at Brewster's angle passes one polarization without loss; a calcite displacer splits it into two parallel beams. The request lists these as its polarization goals, with birefringent crystals. [pbrt-polarization.md](pbrt-polarization.md) carries Stokes vectors and Mueller matrices through the path tracer and adds polarizer and retarder materials. This plan builds the optical elements on top: wave plates, polarizers and Brewster windows as catalog kinds, polarizing coatings put to work, the rotation mount driving each element's axis, uniaxial crystals that split rays into ordinary and extraordinary with walk-off, and the same physics in the beam solver.

*Not implemented. Depends on [pbrt-polarization.md](pbrt-polarization.md), [physoptics-optical-materials.md](physoptics-optical-materials.md) (complex coating amplitudes), [physoptics-optical-elements.md](physoptics-optical-elements.md), [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md) (rotation mounts) and [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md) (the chief ray's Jones vector). Part of the [laser optics plan](physoptics-laser-optics.md).*

## What the renderer will have

- [pbrt-polarization.md](pbrt-polarization.md): a polarized lane family whose throughput is, per lane, an adjoint Stokes row for camera paths; Mueller matrices for exact Fresnel, rough microfacets, depolarizers, an ideal `linear_polarizer` and a `retarder`; a polarizing filter on the camera.
- [physoptics-optical-materials.md](physoptics-optical-materials.md): coatings that return complex `rₛ, rₚ, tₛ, tₚ`, and the optical interface material, which averages s and p while the polarized family is off.
- [physoptics-laser-sources.md](physoptics-laser-sources.md): lasers with a Stokes vector.
- [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md): a Jones vector on every chief ray, through every coating.

## Design

### Light paths carry Stokes vectors forward

A camera path does not know the light's polarization until it reaches the light, so [pbrt-polarization.md](pbrt-polarization.md) carries the adjoint row. A light path from a laser knows it from the start. In the polarized family, the light integrator's throughput is therefore the Stokes vector itself, per lane, multiplied by each interaction's Mueller matrix in order; the storage is the same four floats per lane. A detector deposits `s₀`, or all four components for a polarimeter (below). The camera connections of the light integrator turn the forward Stokes vector into the camera's measurement with the camera's analyzer row at the connection. [pbrt-polarization.md](pbrt-polarization.md) records this addition.

### The elements

New `OPTIC_TRAITS` rows, all round optics in rotation mounts by default, each with its axis angle measured from the rotation mount's zero:

| Kind | Parameters | Model |
|---|---|---|
| Half-wave plate, quarter-wave plate | design wavelength, order (zero-order, or multi-order m), material (crystal quartz, MgF₂, polymer), thickness from the order | a uniaxial plate whose retardance is `δ(λ, θ) = 2π Δn(λ) d_eff(θ) / λ` |
| Linear polarizer | transmission of the pass axis and extinction ratio, each a spectrum | a diattenuator: `tₓ` and `tᵧ` along the pass and block axes |
| Brewster window | glass, thickness, polarization to pass (p by default) | an uncoated plate; **Set to Brewster** turns its mount to `atan(n₂ / n₁)` relative to the arriving beam at the design wavelength |
| Polarizing beamsplitter cube | as the cube beamsplitter, with a polarizing coating at the diagonal | `Rₛ ≈ 1`, `Tₚ ≈ 1` from the coating's table |
| Birefringent plate, wedge, displacer | crystal (calcite, α-BBO, YVO₄, crystal quartz), optic axis direction in the body frame, cut | a uniaxial medium (below) |

For a wave plate the retardance at off-normal incidence uses the extraordinary index along the refracted wave vector, so tilting a plate detunes it, which is how labs fine-tune a multi-order plate. A multi-order plate under a broadband source becomes depolarizing in the image of a spectral render without any coherence model, because every lane has its own retardance and the lanes average.

The rotation mount's rotor angle ([physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)) is the element's rotation about its optical axis, so turning the RSP's knob turns the plate's fast axis, and **Rotate and carry** or a typed angle sets it exactly ([physoptics-locked-transforms.md](physoptics-locked-transforms.md)).

### Uniaxial media

A crystal element's glass is a uniaxial medium: an ordinary and an extraordinary index, each a dispersion formula from the catalog, and an optic axis. Two models, chosen per element:

- Thin (retarder). The plate is thin enough that its two eigenwaves overlap when they leave it, as in a wave plate. The ray refracts once with the ordinary index's geometry, and at the exit surface the material applies the retarder Mueller matrix (Jones matrix in the solver) for the retardance accumulated across the plate. There is no walk-off.
- Thick (split rays). The plate separates its eigenwaves, as a beam displacer or a Wollaston prism does. At the entry surface the ray splits into an ordinary ray (Snell with `n_o`, polarized perpendicular to the plane of the optic axis and the wave vector) and an extraordinary ray, whose wave vector follows Snell with the direction-dependent index `n(θ) = (cos² θ / n_o² + sin² θ / n_e²)^(−1/2)` and whose ray direction, the energy flow, differs from it by the walk-off angle. The path tracer picks one with probability proportional to its transmitted power, as at any partial interface, and carries it with its eigenpolarization; the beam solver follows both, as the ports **Ordinary** and **Extraordinary**.

The split's amplitudes are each eigenpolarization's projection of the arriving field times the isotropic Fresnel transmission for its index. That is exact at normal incidence and a close approximation at the small angles a lab uses; the exact anisotropic Fresnel coefficients (Berreman's 4 × 4 method, or McClain, Hillman and Chipman's polarization ray tracing in anisotropic media) are a later step. Optical activity (crystal quartz along its axis) is not modeled.

`rt_optics` gains the uniaxial refraction, walk-off and eigenpolarizations in `src/optic_birefringence.cpp`, templated like the surfaces so the solver differentiates through them, with the GLSL mirror in `crt_birefringence.glsl`.

### Polarizing coatings at work

Nothing new is needed for a polarizing beamsplitter beyond the coating tables of [physoptics-optical-materials.md](physoptics-optical-materials.md): with the polarized family on, the interface material builds its Mueller matrix from the table's `rₛ, rₚ` (or `tₛ, tₚ`) and their phases, as [pbrt-polarization.md](pbrt-polarization.md) does for exact Fresnel, and the s and p powers go to their ports. The warning the materials plan shows for polarizing coatings with polarization off stays.

### Measuring polarization

- A detector option, **Polarimeter**, makes a film store all four Stokes components per pixel per channel, and its window shows `s₀`, the degree of polarization, the azimuth and the ellipticity as images ([physoptics-detectors.md](physoptics-detectors.md)).
- The Beam window shows the solver's polarization ellipse at the probe, and its power in each port of a splitter.

## Registry, bindings and data

- Kinds `half_wave_plate`, `quarter_wave_plate`, `linear_polarizer`, `brewster_window`, `birefringent_crystal`, and a polarizing coating preset for the cube beamsplitter.
- The uniaxial medium in the glass library (two indices and an optic axis); `crt_birefringence.glsl` and `src/optic_birefringence.cpp`, tested in `bin/optics_test`.
- Ports **Ordinary** and **Extraordinary**.
- **Polarimeter** on detectors: four times the film's channels.
- No new binding: crystal axes ride in the body's payload in binding 55.

## Verification

- With the polarized family off, every scene renders bit-identical; with it on and no polarization element, light paths' Stokes vectors stay proportional to the laser's through depolarizing materials, as [pbrt-polarization.md](pbrt-polarization.md)'s bit-identity check requires.
- Malus's law: a polarizer in a rotation mount after a linearly polarized laser transmits `cos²` of the angle within 0.1 % on a power meter, at 0° to 90° in 10° steps.
- A half-wave plate at angle θ turns linear polarization by 2θ; a quarter-wave plate at 45° gives `|s₃| / s₀ ≥ 0.999` at its design wavelength, in the solver and in a polarimeter's reading.
- A half-wave plate ahead of a polarizing cube divides power between the cube's two detectors as `cos² 2θ` and `sin² 2θ`, matching the solver to 1e-9 and light tracing within noise.
- **Set to Brewster** puts an N-BK7 window at `atan(n)` to 1e-9 rad; the window then transmits p-polarized light with no reflection at either face and s-polarized light with the two-face loss the Fresnel equations give.
- A calcite displacer cut at 45° to its axis separates the ordinary and extraordinary beams by `L tan ρ`, with `ρ` the walk-off angle, to 1 µm, with orthogonal polarizations, in the solver and on a beam profiler.
- A thin crystal between crossed polarizers under a broadband source shows the interference colors that its retardance predicts, in a spectral render, against Michel-Lévy's chart.

## Order of work

1. Forward Stokes vectors on light paths, and polarimeter detectors.
2. Polarizers and wave plates in the thin model, with their rotation mounts; the Malus and wave-plate tests.
3. The polarizing cube, and the half-wave-plate power divider.
4. Brewster windows and **Set to Brewster**.
5. Uniaxial media in the thick model: splitting, walk-off, the ports, the displacer test.

## Waiting for the wave backend

When this plan is implemented, move this section to `docs/plans/waiting/physoptics-polarization-optics.md`.

- Interference between polarization components that recombine after separate paths (a polarization interferometer, the fringes behind a Wollaston prism and an analyzer) needs their phases.

## Open questions

### Pending answers

- Which wave plates and polarizers. The default products for the presets: zero-order quartz or polymer wave plates, and film or crystal polarizers, with their Thorlabs part numbers and data (retardance tolerance, extinction ratio, transmission curves), for the user to choose.
- Which crystals. Which birefringent crystals the lab uses (calcite beam displacers, α-BBO or YVO₄ plates), with their cuts.

### Design questions

- Glan-type and Wollaston polarizers are birefringent prisms, which the thick model covers once their prism shapes are catalog kinds. Whether to add them as kinds now or let the user build them from a crystal and a prism prescription.
- Exact anisotropic Fresnel coefficients matter for steep incidence on crystals. Whether they are needed depends on the user's crystals.

## Sources

Born and Wolf 1999, *Principles of optics*, §15.3 to §15.5 (light in uniaxial crystals, wave and ray directions, walk-off); Yariv and Yeh 1984, *Optical waves in crystals*, chapters 4 and 5 (Jones calculus, wave plates, birefringent plates); McClain, Hillman and Chipman 1993, *Polarization ray tracing in anisotropic optically active media*, JOSA A 10(11); Berreman 1972, *Optics in stratified and anisotropic media: 4 × 4-matrix formulation*; Chipman, Lam and Young 2018, *Polarized light and optical systems*; Thorlabs' wave plate, polarizer and beam displacer pages.
