# Laser sources

Every beam axis starts at a laser. To the beam solver a laser is a chief ray and a complex beam parameter at its waist; to the path tracer it is a light that emits a few milliwatts into a cone a milliradian wide from a spot a millimeter across. Neither exists today: the tracer's lights are punctual lights, emitting surfaces and the environment. This plan adds the laser as an optical element and as a light: its Gaussian beam with M², astigmatism and a waist that need not sit at the output, its spectrum from a single line to a femtosecond pulse's bandwidth, its power and its polarization, and the emitted-ray sampling that light tracing draws from.

*Implemented 2026-11-02, apart from what "Not built" lists; "As built" says where the code differs from the design below. Depends on [physoptics-optical-elements.md](physoptics-optical-elements.md) (the element), [pbrt-light-types.md](pbrt-light-types.md) (emitted-ray sampling, `sampleLe()` and `pdfLe()`) and [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) (line spectra, wavelength sampling by the emitter). Rendering its light needs the light integrator of [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md); the beam solver needs only the element. Part of the [laser optics plan](../physoptics-laser-optics.md).*

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

Sampling starts at the waist and carries each ray straight to the housing's aperture plane, so no ray starts inside the housing whatever the waist position. Positions use the sample record's light dimensions ([pbrt-sample-generation.md](pbrt-sample-generation.md)) through the Box–Muller transform, so stratified and low-discrepancy samplers stratify the beam's cross section.

### As a light

- `LightKind` and `GpuLightKind` gain `Laser`. Its 96-byte `GpuLight` record holds the aperture center, the axis, the x axis, and per axis the waist radius, the waist's distance from the aperture and the far-field half angle.
- `sampleLe()` for a laser is the sampling above, and `pdfLe()` its product of Gaussian densities over position and direction ([pbrt-light-types.md](pbrt-light-types.md)'s emission table gains the row). The light integrator picks lights by power, so a laser of a few milliwatts beside a 100 W room light gets its share by power; when every light is a laser, as on an optical bench, every light path starts at one.
- Next-event estimation from a camera path never samples a laser: its emission is confined to a cone of milliradians, so a shadow ray toward it would almost never land inside the cone. `LightListBuilder` keeps lasers out of both alias tables, with a flag, as the infinite plane is kept out today.
- Looking into the laser still works. The aperture is a one-sided emitting disk whose material, `laser_aperture`, names the laser's light record, and kernel 04 evaluates its radiance for a camera ray that hits it, at full weight since no light sample can reach it, as the beam's phase-space density at that point and direction: `P` times the two normal densities in position and in angle, divided by the cosine. The spot is as bright as it should be and as small.

### Spectrum and color

- In spectral modes, a light path from a laser draws its wavelengths from the laser's spectrum. A laser's spectrum is a `Line` `SpectrumRef` (a center, a width, a Gaussian or Lorentzian profile, its integral as the scale), and camera paths can already sample wavelengths from the emitters' lines (**Sampling** → **Emitters' spectra**, [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)); light paths from an emitter are this plan's and the light integrator's to add. A single line with a linewidth below the sampling resolution puts every lane at the line's center with one density, so no lane is wasted outside the line.
- In exact RGB, a laser carries a fixed RGB color: its spectrum projected to linear Rec. 709 when it is visible, or the false color of its wavelength from [pbrt-display-and-color.md](../pbrt-display-and-color.md) when it is not, normalized so the three channels average to 1 W per watt of laser power. A detector in exact RGB then reads the right power through optics that treat every channel alike, which is everything except colored glass and dichroics; the detector panel says to use a spectral mode for those. The glass indices use the design wavelength, which is the laser's ([physoptics-optical-materials.md](physoptics-optical-materials.md)).

### Polarization

Not built: the user deferred it on 2026-11-02, and it lands with [pbrt-polarization.md](../pbrt-polarization.md), whose "Lasers" section now holds this design. The laser's Stokes vector is `P (1, p cos 2ψ cos 2χ, p sin 2ψ cos 2χ, p sin 2χ)` for azimuth ψ, ellipticity χ and degree of polarization p, in the frame of its x axis. [pbrt-polarization.md](../pbrt-polarization.md) carries it forward along light paths in the polarized family; in other families it is reduced to its intensity. The beam solver carries the fully polarized part as a Jones vector on the chief ray ([physoptics-gaussian-beam-solver.md](../physoptics-gaussian-beam-solver.md)). The laser's second light record has room for the Stokes vector.

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
- A camera looking into the aperture from on axis, a distance L from the waist, sees the irradiance `2 P / (π w(L)²)` there, summed over its image. Its spot is not the beam's radius at that distance: a pinhole at L sees the waist plane's points x along the slopes −x / L, whose density falls off with the half angle `w0 θ0 / w(L)`, the size of the waist seen from L. The plan first said the beam's radius, which the Wigner function shows is wrong. Off axis by five radii, the camera sees nothing.

## Order of work

1. The element, its parameters and the housings: enough for the beam solver.
2. `LightKind::Laser`, emission sampling, the light list flag, with `bin/light_test`.
3. Spectra and the RGB color rule, after the spectral plan.
4. The aperture's radiance for camera rays.
5. Polarization, after [pbrt-polarization.md](../pbrt-polarization.md).

## Open questions

### Answers (2026-11-02)

- The default laser is the HNL050LB, from the data sheet the user gave: 632.8 nm, a linewidth of 0.002 nm (1.5 GHz), 5.0 mW, a 0.81 mm 1/e² diameter, a 1.0 mrad full divergence, a 500:1 polarization ratio, TEM00 above 95 %. The user could not confirm M² = 1; the diameter and divergence with the waist at the output give M² = 1.005, which the preset holds. The CPS635R and F220APC-633 presets wait for the user's numbers, and the user may add the lab's own sources.
- Pulsed sources wait: average power only.
- Light tracing alone renders a laser's light. Polarization is deferred to [pbrt-polarization.md](../pbrt-polarization.md).
- A laser is placed freely, with no housing geometry yet. While the bench snaps, its output goes on the nearest hole and its beam along the grid, as a post is. The Viewport outlines its housing and draws an arrow along its beam.
- In the spectral modes a laser joins the emitters' spectra that **Lights** wavelength sampling follows; a light path does not draw its wavelengths from its own light.

### Design questions

- Partially coherent and multimode beams with structure (a top hat, a donut mode, a fiber's LP modes) are not Gaussian Wigner functions, and some are not positive. A tabulated near-field intensity with a divergence would cover a top hat as rays; the rest waits for the wave backend.

## As built

- `OpticKind::Laser` (`src/optic_catalog.cpp`) is one row: the parameters of the table above but polarization, the HNL050LB preset (the sizes combo, labeled **preset**), one material slot, **aperture**, and one port, **Output**. `src/optic_laser.h` turns the parameters into a `LaserBeam`: the beam's frame in the body frame (the pointing applied), each axis's `LaserAxisBeam` (waist, waist position, M², half angle, `radius(z)`), the spectrum, the exact RGB color, the coherence length and the aperture's radius. It is what the light list and the panel read, and what the beam solver will take its root segment from. `fitLaserAxis()` turns a data sheet's diameter and divergence into the waist and M², which the panel's **As a data sheet gives it** fields use.
- The housing is a parameter (laser head, diode module, fiber collimator) that names its placeholder model and its nominal output aperture, and nothing the tracer sees. The aperture disk is three of the beam's radii at the aperture across when that is wider than the housing's, so it holds all but 1.5e-8 of the power.
- No `LightKind::Laser`: a laser is an optic, and `buildSceneAccel()` adds its light from it. There is no `SceneLight` of the kind.
- A laser is **two** `GpuLight` records: the first as every light's (the aperture's center and radius, the axis, the x axis, the power's color, the price, the lasers' own alias table, the spectrum), the second its beam (the waists, their distances behind the plane, the half angles). One record held too few words for an astigmatic beam, and the second has room for the Stokes vector. The lasers follow every other light from `lightHeader.laserBase`, out of both next-event tables; the header's three spare words became `laserBase`, `laserCount` and `invEmitPower`, which a list without a laser leaves 0, so its bytes are unchanged.
- The beam's reference plane is the aperture's, 10 µm (and 1e-6 of its distance from the origin) in front of it, with the waists that much further behind. Light paths that started on the aperture's own plane hit it again and were absorbed: the first test render got 1e-8 W of 5 mW.
- The sampling and densities are `sampleLaserLe()`, `laserPdfLe()` and `laserRadiance()` in `src/light_sampling.cpp`, mirrored by `shaders/rt/include/crt_laser.glsl` (not `crt_light.glsl`: emitted rays live in `crt_light_emission.glsl`, which includes it). The position density is the beam's marginal at the plane, and the direction's the slope's conditional Gaussian given the point, so no ratio of two underflowing tails is ever taken. Light tracing picks among all lights by power with `sampleEmitterIndex()`.
- `CRT_LASERS` (specialization constant 13, `CRT_FEATURE_LASERS`) is on for every kernel that reads the instances while the light list holds a laser. Kernel 11 starts light paths at lasers and evaluates the beam toward the camera for the light's own point; kernel 04 lights a `laser_aperture` hit by the beam of the laser its instance's `lightBase` names, at full weight. Off, both are the code they were: 17 scenes without optics rendered bit-identical to the HEAD build. The bidirectional scenes differ from it in the last bit, as HEAD differs from itself (their splats add with float atomics).
- `MaterialType::LaserAperture` (`laser_aperture`, `CRT_MATERIAL_LASER_APERTURE` 13) is a classic record with no emission and the emits flag, so every other emitter reader finds it black.
- Spectra: a laser's spectrum is a `Line` reference integrating to 1 over nanometers, in the `Unbounded` role, scaled by the power. Its exact RGB color is `wavelengthDisplayColor()` of its center scaled so the channels average to 1, which shows a line outside the visible range in the display's false colors without waiting for [pbrt-display-and-color.md](../pbrt-display-and-color.md). The design wavelength follows the scene's one laser (`designWavelength()`, `SpectralSettings::designFollowsLaser`), and **Optical bench** recommends **Lights** wavelength sampling.
- Placement: a laser is unmounted and free. `snapFreeToBench()` puts its output on the nearest hole with its beam level along the grid; the gizmo slides it across the table, raises it and turns it about the vertical, stepping in quarter turns while the bench snaps (Alt inverts it).
- Payload version 28: the optic type `laser`, and `designFollowsLaser` when false.
- Checks: `bin/light_test` (second-moment radii at the waist, one Rayleigh range and ten within 0.13 % for M² = 1, M² = 2 and an astigmatic diode; the densities; their integrals; chi-square per axis; the power), `bin/optics_test` (the preset against the data sheet, the fit, the aperture, the colors, the design wavelength, snapping), and `tools/verify/laser_check.py` on the scenes `tools/verify/laser_scenes.py` writes to `assets/scenes/optics/`. There a white screen seen square-on by a pinhole camera stands in for a detector (`tools/verify/beam_probe.py`): the screen 1 m from the HNL050LB reads 5.0000 mW of 5 mW and its radius is within 0.12 % of w(1 m); looking into the laser the path tracer and light tracing see the irradiance within 0.1 %; the spectral modes read the power within 0.05 %.
- A wave reference, `tools/verify/wave_optics.py`, which the user asked for on 2026-11-02, propagates the same beam by diffraction (diffractsim's angular spectrum method, with M² > 1 as a Gaussian Schell-model beam's coherent modes, astigmatism, masks and thin lenses). Its self-test checks it against the closed forms and, run with diffractsim's venv, against diffractsim to 4e-11. `laser_check.py` compares every screen's irradiance with it: in free space the rays and the waves agree to within the rays' noise (0.3 % to 0.6 % of the power), as they must, since the rays are the beam's Wigner function. Past a 1 mm iris they part 2 m on: the rays keep a clipped Gaussian 19 % brighter at its peak, the waves spread into a wider core with Airy rings.
- The laser checks found that the optical materials divided the air out twice: a catalog's index is relative to air, and the renderer treated it as absolute and divided it by Ciddor's air, so every lens was 0.05 % long. Catalog glasses are now relative to air (`OpticGlass::relativeToAir`, `absoluteIndexRef()` for the GPU), at the user's choice.

## Not built

- Polarization: deferred by the user to [pbrt-polarization.md](../pbrt-polarization.md), whose "Lasers" section holds the design above.
- Light paths under the light vertex cache and pbrt 3's BDPT. Their MIS assumes next-event estimation can reach every light, which a laser defeats; teaching both that a laser's light-sampling strategy has density 0 is the work. Until then they, like the path tracer, show a laser's aperture seen directly and leave what its beam lights dark, and Render Settings says so with a **Use Light tracing** button.
- A tabulated spectrum from an `.spd` file: an optic parameter cannot hold a spectrum, and reading a file on every build of the body needs a cache. A Gaussian line wide enough is a broadband or pulsed source's band.
- Light paths that draw their wavelengths from their own light's spectrum rather than the scene's **Lights** mixture: the user chose the mixture.
- Presets for the CPS635R and the F220APC-633, and housing geometry: they wait for the user's numbers and STEP files.
- `pdfLe()` for a laser in `lightPdfLe()`, which only the light vertex cache and BDPT's MIS read.
- A GPU self-test of `crt_laser.glsl` against `light_sampling.cpp`: the render checks above exercise it end to end, to 0.1 %.

## Sources

Siegman 1986, *Lasers*, chapter 17 (Gaussian beams) and Siegman 1993, *Defining, measuring, and optimizing laser beam quality* (M² by second moments); ISO 11146-1:2021 (beam widths, divergence and propagation ratios); Bastiaans 1979, *Wigner distribution function and its application to first-order optics*; Saleh and Teich 2019, *Fundamentals of photonics*, chapters 3 and 6 (Gaussian beams, polarization); PBR 3ed §16.1.2 (emitted-ray sampling); the Thorlabs HNL050LB, CPS635R and F220APC-633 pages; Gori 1980, *Collett-Wolf sources and multimode lasers*, and Mandel and Wolf 1995, *Optical coherence and quantum optics*, §5.8 (the Gaussian Schell-model beam and its coherent modes); Matsushima and Shimobaba 2009, *Band-limited angular spectrum method*; diffractsim (Rafael de la Fuente, MPL 2.0).
