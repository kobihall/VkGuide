# Polarization

The tracer carries intensity alone, as pbrt does: its Fresnel terms average the s- and p-polarized reflectances on the assumption that light is unpolarized (PBR 4ed §9.3.5), and the polarization that each reflection imposes is lost. Polarization is what a photographer's polarizing filter acts on, what makes reflections off glass vanish at Brewster's angle, and what successive reflections between glass panes change. PBR 4ed exercise 9.3 asks for polarized Fresnel equations and for optical elements such as polarizers and retarders. The user chose to plan this as groundwork toward their long-term goal of electromagnetic simulation. This plan carries Stokes vectors through the spectral lanes, evaluates Mueller matrices at surfaces, and adds polarizers and retarders.

*Not implemented, research groundwork. Depends on [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) (the lane families) and [pbrt-bsdf-library.md](pbrt-bsdf-library.md) (Fresnel with separate s and p amplitudes). The [laser optics plan](physoptics-laser-optics.md)'s polarization goals build on it through [physoptics-polarization-optics.md](physoptics-polarization-optics.md), which makes it item 31 of that plan's roadmap. Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- Throughput is an RGB `vec3`; [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) makes it a `Spectrum` of `CRT_LANES` scalars.
- The legacy `dielectric` uses Schlick's approximation, which has no s and p components. The `pbr` material also uses Schlick. [pbrt-bsdf-library.md](pbrt-bsdf-library.md) adds pbrt's exact dielectric and conductor Fresnel equations, whose complex amplitudes `rs` and `rp` this plan needs.
- Emitters and the environment emit unpolarized light. The lasers of [physoptics-laser-sources.md](physoptics-laser-sources.md) will be the first polarized emitters, each with a Stokes vector.

## Design

### What a path carries

A Stokes vector `s = (s0, s1, s2, s3)` describes intensity, linear polarization along two axis pairs, and circular polarization. An optical interaction is a 4 × 4 Mueller matrix acting on it, expressed in a reference frame perpendicular to the ray.

A camera path is traced backward from the sensor, so the light's Stokes vector is only known at the end. The measured intensity is `d · M₁ M₂ ⋯ Mₖ · s_light`, where `d` is the sensor's analyzer row: `(1, 0, 0, 0)` for a plain sensor. The path therefore carries the row vector `r = d M₁ ⋯ Mᵥ`, an adjoint Stokes vector, instead of the full matrix product. That is 4 values per lane rather than 16, and the lanes stay independent.

- Light paths, which the light integrator of [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md) traces from the lights, know the light's polarization from the start. In the polarized family they carry the forward Stokes vector itself per lane, multiplied by each Mueller matrix in order, in the same four floats per lane; a connection to the camera applies the camera's analyzer row there. [physoptics-polarization-optics.md](physoptics-polarization-optics.md) adds this, and polarimeter detectors that record all four components.
- A polarized family (`CRT_POLARIZED`, beside `CRT_LANES`) makes `PathState.throughput` a `PolarizedSpectrum`: `CRT_LANES` rows of 4 floats. With 4 lanes that is 64 bytes instead of 16, and the path state grows from 96 to 144 bytes, in that family only.
- Only the 4- and 8-lane families get a polarized build. A Mueller matrix per lane is 16 floats in registers, and 32 lanes of them would spill.
- Next-event estimation computes `r · M_bsdf · s_light` per lane, a scalar, so shadow rays and everything after them are unchanged. Kernels 03 and 04 compute `r · s_emitted`, which for an unpolarized emitter is `r₀ Le`.
- The MIS pdfs and the light and BSDF sampling densities stay scalar. Polarization changes the weights, not where samples go.

### Reference frames

Every Stokes vector and Mueller matrix is expressed in a frame. The path's frame is a canonical basis derived deterministically from the ray direction, as Mitsuba 3's polarized mode does, so no basis is stored. At a surface, kernel 06 rotates the row into the plane of incidence (the s and p axes), applies the interaction's Mueller matrix, and rotates into the outgoing ray's canonical frame. The rotation Mueller matrices and the frame helpers go into `crt_mueller.glsl`.

PBR 4ed §9.3.6 warns that sign conventions differ between sources (the sign of a conductor's `k`, the sign of `r∥`) and call such details a common source of bugs in polarized renderers. `crt_mueller.glsl` states its conventions at the top, follows Mitsuba 3's, and the tests below exercise every rotation and both signs.

### Interactions

| Material | Mueller matrix |
|---|---|
| Smooth dielectric and conductor ([pbrt-bsdf-library.md](pbrt-bsdf-library.md)) | the exact Fresnel matrix from the complex `rs` and `rp` |
| Rough dielectric and conductor | the Fresnel matrix at the sampled microfacet normal, rotated between the macrosurface frames, times the scalar microfacet terms, as Mitsuba 3's polarized rough BSDFs do |
| Diffuse, and every legacy material | a depolarizer, `diag(f, 0, 0, 0)` |
| `optical_interface` with a coating ([physoptics-optical-materials.md](physoptics-optical-materials.md)) | the Fresnel-form matrix built from the coating table's complex `rₛ, rₚ` or `tₛ, tₚ` and their phases, which is how a polarizing beamsplitter splits |
| Wave plates, polarizers and uniaxial crystals ([physoptics-polarization-optics.md](physoptics-polarization-optics.md)) | a retarder with a wavelength- and angle-dependent retardance, a diattenuator, or the eigenpolarizations of an ordinary and an extraordinary ray |
| `linear_polarizer` (new) | an ideal linear polarizer sheet, with its transmission axis at an angle in the surface's tangent frame |
| `retarder` (new) | a wave plate with retardance δ (a quarter-wave plate at δ = π/2) and a fast-axis angle |

pbrt's `FrComplex()` already computes `rs` and `rp` internally before averaging their squared magnitudes; the polarized family keeps them apart.

Legacy materials as depolarizers are an approximation. It has one useful property: with the sensor analyzer off and every emitter unpolarized, a row that meets only depolarizers stays `(x, 0, 0, 0)`, and `x` follows exactly the arithmetic of the scalar throughput.

### The sensor

A **Polarizing filter** setting on the camera ([pbrt-cameras.md](pbrt-cameras.md)): none, or a linear polarizer at an angle to the camera's up vector. Kernel 00 sets `d` to the filter's first row, rotated into each camera ray's canonical frame. Rotating the filter dims reflections off water and glass as a real filter does, and darkens a polarized sky once there is one.

A polarimetric camera, which outputs the four Stokes images `s0` to `s3` as auxiliary outputs ([pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md)), needs all four rows of the product instead of one. That is a later `CRT_POLARIZED = 2` family carrying 16 floats per lane.

### Toward the electromagnetic goal

Stokes vectors describe partially polarized, incoherent light: intensities add. Diffraction and interference need amplitudes and phases that add before squaring. On 2026-10-18 the user decided that the coherent wave backend which will do that is planned by no file here: they will design and build it last, after every pbrt and physoptics plan. What this plan owes it is to keep what it will need where it is cheap. The Fresnel and coating terms are kept as complex amplitudes, not only their squared magnitudes, and the reference frames defined here are the ones the backend's polarization will be expressed in. Nothing in this plan depends on the backend. Thin-film iridescence ([pbrt-appearance-extras.md](pbrt-appearance-extras.md)) is the one wave effect the tracer gets without it, because Belcour and Barla solve the interference analytically inside the BRDF. The grid-based [wave-simulation.md](wave-simulation.md) is a separate route to wave effects, and a comparison between the two is a natural test once both exist.

## Registry, bindings and data

- `CRT_POLARIZED` builds of the lane-dependent kernels for the 4- and 8-lane families, named like `0604_surface_scatter_mis_power.lanes4.pol.comp.spv`.
- `crt_mueller.glsl`, mirrored in `rt_bsdf` with its tests in `bin/bsdf_test`.
- Materials `linear_polarizer` and `retarder` ([pbrt-material-system.md](completed/pbrt-material-system.md)); the camera's **Polarizing filter**.
- A **Polarization** checkbox in the spectral settings, available in exact RGB too: each RGB channel is then polarized with its own Fresnel terms.
- No new binding.

## Verification

- With polarization off, every scene renders bit-identical.
- With it on, the analyzer off and only legacy materials and unpolarized emitters, the image is bit-identical to the unpolarized family's.
- `bin/bsdf_test`: for unpolarized input, the first column of every Fresnel Mueller matrix reproduces pbrt's `FrDielectric()` and `FrComplex()` to 1e-6; reflection off n = 1.5 glass at Brewster's angle (56.3°) is fully s-polarized; every rotation composed with its inverse is the identity.
- Malus's law: two polarizer sheets in front of an area light transmit in proportion to cos² of the angle between them, within 0.1 %.
- A quarter-wave plate at 45° between crossed polarizers transmits half the light a single polarizer does.
- A glass sphere lit from the side and viewed at Brewster's angle through the camera filter: rotating the filter removes the reflection.
- A simple scene rendered in Mitsuba 3's polarized mode matches this renderer's Stokes images within noise, once the polarimetric camera exists.

## Order of work

1. `crt_mueller.glsl` and the Fresnel Mueller matrices, with their tests.
2. The polarized family for 4 lanes with depolarizing legacy materials, and the bit-identical check.
3. Smooth dielectric and conductor, the camera filter, and the Brewster and Malus scenes.
4. Polarizer and retarder materials.
5. Rough microfacet Mueller matrices.
6. The polarimetric camera, and a polarized sky.

## Sources

PBR 4ed §9.3.5 and §9.3.6 (the Fresnel equations, and the assumption of unpolarized light), chapter 9 "Further Reading" and exercise 9.3; Wolff and Kurlander 1990, *Ray tracing with polarization parameters*; Wilkie and Weidlich 2012, *Polarised light in computer graphics* (SIGGRAPH Asia course); Collett 1993, *Polarized light: fundamentals and applications*; Mojzík et al. 2016, *Bi-directional polarised light transport*; Jarabo and Arellano 2018, *Bidirectional rendering of vector light transport*; Baek et al. 2020, *Image-based acquisition and modeling of polarimetric reflectance*; Mitsuba 3 documentation, polarized variants and `mueller.h`; Belcour and Barla 2017, *A practical extension to microfacet theory for the modeling of varying iridescence*.
