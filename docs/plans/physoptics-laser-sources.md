# Laser sources

Every beam axis starts at a laser. To the beam solver a laser is a chief ray and a complex beam parameter at its waist; to the path tracer it is a light that emits a few milliwatts into a cone a milliradian wide from a spot a millimeter across. Neither exists today: the tracer's lights are punctual lights, emitting surfaces and the environment. This plan adds the laser as an optical element and as a light: its Gaussian beam with M², astigmatism and a waist that need not sit at the output, its spectrum from a single line to a femtosecond pulse's bandwidth, its power and its polarization, and the emitted-ray sampling that light tracing draws from.

*Not implemented. Depends on [physoptics-optical-elements.md](physoptics-optical-elements.md) (the element), [pbrt-light-types.md](pbrt-light-types.md) (emitted-ray sampling, `sampleLe()` and `pdfLe()`) and [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) (line spectra, wavelength sampling by the emitter). Rendering its light needs the light integrator of [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md); the beam solver needs only the element. Part of the [laser optics plan](physoptics-laser-optics.md).*

## What the renderer does today

- `SceneLight` (`src/rt_scene_types.h`) is a point, spot or directional light; `LightListBuilder` (`src/rt_lights.cpp`) prices every light by power for two alias tables that kernel 06 samples.
- Every light is reached by next-event estimation or by a BSDF sample landing on an emitter. Nothing traces from a light, which [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md)'s light integrator adds.
- Emitters are unpolarized and carry RGB.

## Design

### The element

`laser` is an `OPTIC_TRAITS` row. Its housing is a parameter, one of the placeholders of [physoptics-optomechanical-models.md](physoptics-optomechanical-models.md) (laser head, diode module, fiber collimator), which sets its model, its mount and where its output aperture sits. Its one port is **Output**, along the body frame's +z from the aperture's center. Its parameters:

| Group | Parameters |
|---|---|
| Beam | waist radius w₀ (1/e² intensity) or output diameter and full-angle divergence, which the panel converts both ways; waist position relative to the aperture (negative inside the housing); M²; a separate x and y set of all three for an elliptical or astigmatic beam, with the orientation of its x axis about +z |
| Spectrum | single line (center wavelength, linewidth), Gaussian band (center, FWHM, for a broadband or pulsed source), or a tabulated spectrum |
| Power | average power in watts |
| Polarization | linear at an angle, circular left or right, elliptical (azimuth and ellipticity), or unpolarized, and a degree of polarization |
| Pointing | an optional angular and lateral offset of the beam from the housing's axis, for a laser that does not leave its housing straight |

Presets fill the beam and spectrum groups from a data sheet. The first preset is Thorlabs' HNL050LB 632.8 nm HeNe, with its published output diameter and divergence, to be confirmed with the user (see "Pending answers").

The coherence length is stored with the spectrum (`c / (π Δν)` for a Lorentzian line) but nothing in these plans uses it: it is for the future wave backend.

### The beam as rays

A Gaussian beam's Wigner distribution is itself Gaussian, and positive, so rays drawn from it reproduce the beam's intensity through any paraxial optical system exactly, including the focus. At the waist, for each transverse axis independently:

```text
position  x ~ Normal(0, w₀ / 2)
angle     θ ~ Normal(0, θ₀ / 2),   θ₀ = M² λ / (π w₀)   (the far-field half angle at 1/e²)
```

A bundle of such rays has the second-moment radius `w(z)² = w₀² + θ₀² z²`, which is the beam's, for M² = 1 and, by Siegman's definition of M², for an embedded Gaussian of any M². Through real optics the rays pick up the aberrations the optics have, which is what the request wants previewed. What rays cannot carry is diffraction at a hard aperture, whose Wigner function goes negative; that belongs to the wave backend.

Sampling starts at the waist and carries each ray straight to the housing's aperture plane, so no ray starts inside the housing whatever the waist position. Positions use the sample record's light dimensions ([pbrt-sample-generation.md](completed/pbrt-sample-generation.md)) through the Box–Muller transform, so stratified and low-discrepancy samplers stratify the beam's cross section.

### As a light

- `LightKind` and `GpuLightKind` gain `Laser`. Its 96-byte `GpuLight` record holds the aperture center, the axis, the x axis, and per axis the waist radius, the waist's distance from the aperture and the far-field half angle.
- `sampleLe()` for a laser is the sampling above, and `pdfLe()` its product of Gaussian densities over position and direction ([pbrt-light-types.md](pbrt-light-types.md)'s emission table gains the row). The light integrator picks lights by power, so a laser of a few milliwatts beside a 100 W room light gets its share by power; when every light is a laser, as on an optical bench, every light path starts at one.
- Next-event estimation from a camera path never samples a laser: its emission is confined to a cone of milliradians, so a shadow ray toward it would almost never land inside the cone. `LightListBuilder` keeps lasers out of both alias tables, with a flag, as the infinite plane is kept out today.
- Looking into the laser still works. The aperture is a one-sided emitting disk whose material, `laser_aperture`, names the laser's light record, and kernel 04 evaluates its radiance for a camera ray that hits it, at full weight since no light sample can reach it, as the beam's phase-space density at that point and direction: `P` times the two normal densities in position and in angle, divided by the cosine. The spot is as bright as it should be and as small.

### Spectrum and color

- In spectral modes, a light path from a laser draws its wavelengths from the laser's spectrum ([pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) gains this for light paths). A single line with a linewidth below the sampling resolution puts every lane at the line's center with one density, so no lane is wasted outside the line.
- In exact RGB, a laser carries a fixed RGB color: its spectrum projected to linear Rec. 709 when it is visible, or the false color of its wavelength from [pbrt-display-and-color.md](pbrt-display-and-color.md) when it is not, normalized so the three channels average to 1 W per watt of laser power. A detector in exact RGB then reads the right power through optics that treat every channel alike, which is everything except colored glass and dichroics; the detector panel says to use a spectral mode for those. The glass indices use the design wavelength, which is the laser's ([physoptics-optical-materials.md](physoptics-optical-materials.md)).

### Polarization

The laser's Stokes vector is `P (1, p cos 2ψ cos 2χ, p sin 2ψ cos 2χ, p sin 2χ)` for azimuth ψ, ellipticity χ and degree of polarization p, in the frame of its x axis. [pbrt-polarization.md](pbrt-polarization.md) carries it forward along light paths in the polarized family; in other families it is reduced to its intensity. The beam solver carries the fully polarized part as a Jones vector on the chief ray ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)).

### In the beam solver

The laser gives the solver its root segment: the chief ray from the aperture's center along the (possibly offset) output axis, the beam's complex 2 × 2 matrix at the aperture from the two waists, their positions and orientation, the power, the spectrum's center wavelength, and the Jones vector.

## Registry, bindings and data

- `OpticKind::Laser`, its traits, presets and housings.
- `LightKind::Laser`, `GpuLightKind::Laser` (`CRT_LIGHT_LASER`, the next free kind number after the ones [pbrt-light-types.md](pbrt-light-types.md) adds), the record layout above, the no-NEE flag.
- `sampleLaserLe()` and `pdfLaserLe()` in `crt_light.glsl`, mirrored in `light_sampling.cpp`; the `laser_aperture` material type and its radiance in kernel 04.
- Scene file: the laser's parameters, by name, in the optic record.
- No new binding.

## Verification

- Scenes without a laser render bit-identical, and their light lists are unchanged.
- `bin/light_test`:
    - 10⁶ emitted rays, propagated to z = 0, z_R and 10 z_R, have second-moment radii within 0.5 % of `w(z)`, for M² = 1 and 2, and for an astigmatic diode beam with separate x and y waists;
    - `pdfLe()` integrates to 1 over the aperture and directions, and matches the histogram of samples (χ²);
    - the emitted Stokes vectors average to the laser's.
- A detector covering the whole beam, a meter away, reads the laser's power within 0.5 % after 10⁶ light paths ([physoptics-detectors.md](physoptics-detectors.md)).
- A camera looking into the aperture from on axis sees a spot whose radius is the beam's at that distance, and no light from off axis.

## Order of work

1. The element, its parameters and the housings: enough for the beam solver.
2. `LightKind::Laser`, emission sampling, the light list flag, with `bin/light_test`.
3. Spectra and the RGB color rule, after the spectral plan.
4. The aperture's radiance for camera rays.
5. Polarization, after [pbrt-polarization.md](pbrt-polarization.md).

## Open questions

### Pending answers

- The default laser. HNL050LB's output beam diameter, divergence, power and polarization from the Thorlabs page, as the default preset; and which other presets the lab's lasers need (the user's own sources: wavelengths, powers, beam parameters).
- Pulsed sources. Whether pulse energy, repetition rate and pulse duration should be parameters now (for power readouts and nonlinear thresholds later) or wait for the ultrashort pulse work the laser optics plan leaves out.

### Design questions

- Partially coherent and multimode beams with structure (a top hat, a donut mode, a fiber's LP modes) are not Gaussian Wigner functions, and some are not positive. A tabulated near-field intensity with a divergence would cover a top hat as rays; the rest waits for the wave backend.

## Sources

Siegman 1986, *Lasers*, chapter 17 (Gaussian beams) and Siegman 1993, *Defining, measuring, and optimizing laser beam quality* (M² by second moments); ISO 11146-1:2021 (beam widths, divergence and propagation ratios); Bastiaans 1979, *Wigner distribution function and its application to first-order optics*; Saleh and Teich 2019, *Fundamentals of photonics*, chapters 3 and 6 (Gaussian beams, polarization); PBR 3ed §16.1.2 (emitted-ray sampling); the Thorlabs HNL050LB, CPS635R and F220APC-633 pages.
