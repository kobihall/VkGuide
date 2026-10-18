# Detectors

A simulated experiment is only as useful as what it lets you measure. In the lab that is a power meter's reading, a camera's frame, a beam profiler's widths, a spectrometer's trace, or the spot on an alignment card. This plan adds those as optical elements whose surfaces record the light that reaches them: light paths from the lasers deposit their power on a detector's film as they hit it, in a new stage after kernel 02, and a measurement panel turns each film into the numbers a lab reports, with the beam solver's analytic prediction beside each one.

*Not implemented. Depends on [physoptics-optical-elements.md](physoptics-optical-elements.md), [physoptics-laser-sources.md](physoptics-laser-sources.md), and the light integrator of [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md). Spectrometers need [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md). Part of the [laser optics plan](physoptics-laser-optics.md).*

## What the renderer does today

- The only film is the camera's: kernel 09 folds each frame's samples into the accumulation image, and the tonemap pass displays it.
- Float atomics on storage buffers are available (`shaderBufferFloat32AtomicAdd`); on images they are not.
- [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md)'s light integrator traces paths from lights and splats connections to the camera into binding 42.

## Design

### Detector kinds

Each is an `OPTIC_TRAITS` row with a sensor surface in its body and a placeholder model ([physoptics-optomechanical-models.md](physoptics-optomechanical-models.md)):

| Kind | Sensor | After depositing | Reports |
|---|---|---|---|
| Power meter | a disk | absorbs | power |
| Camera sensor | a rectangle of nx × ny pixels at a pitch | absorbs | an image in W per pixel |
| Beam profiler | as a camera sensor | absorbs | an image, centroid, second-moment widths |
| Alignment card | an 85.6 × 54 mm rectangle | scatters as a diffuse white card | an image, and the card is visible in renders |
| Screen | a rectangle of any size | scatters diffusely | an image |
| Spectrometer | a small disk (a fiber's or a slit's entrance) | absorbs | a spectrum |
| Beam dump | the dump's inner cone | absorbs | absorbed power |
| Probe plane | a rectangle, invisible | lets the ray through unchanged | an image of the flux crossing it |

The probe plane has no counterpart on a bench: it measures the beam anywhere without disturbing it, as OpticStudio's transparent detector rectangle does.

### Recording

- Kernel 02 classifies a light path's hit on a sensor surface onto queue 10, `CRT_QUEUE_DETECTOR`. Camera paths see sensors as their ordinary material (black, or the card's white) and are never queued there.
- Stage 26, deposit on detectors (`2601_deposit_detector.comp`), drains queue 10. It finds the hit's position in the detector's own frame, its pixel, and adds the path's power (its weight times its throughput) to that pixel with a float atomic. Then it ends the path (absorbing kinds), appends it to the surface queue for the light paths' scatter stage (cards and screens), or appends it to the next ray queue from the hit point with its direction unchanged (probe planes).
- The schedule runs stage 26 after kernel 02 and before the scatter stage in every bounce, only when the scene has a detector.

Each detector's record in binding 59, `Detectors`, holds its instance slot, its frame (origin and two axes), its size, its pixel counts, its channel count and its offset in the film. Binding 60, `DetectorFilm`, holds every detector's frame sums: float, one value per pixel per channel. A pass at the end of each frame folds the frame sums into a running, Neumaier-compensated sum per pixel and clears them, as `0902_compensated_mean` does for the camera ([pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md)), so a reading still improves after 10⁹ paths.

The light integrator emits each path with power `P_laser / N`, for N paths emitted from that laser so far, so a pixel's running sum divided by the paths emitted is power in watts.

### Channels

- In exact RGB a film has three channels, in watts per channel as the RGB laser color defines them ([physoptics-laser-sources.md](physoptics-laser-sources.md)), and a power reading is their mean.
- In a spectral mode a film has the representation's bands, or for a spectrometer its own set of bins over its range, and a power reading is their sum.
- With a beam axis in a mode that tracks the graph ([physoptics-guided-tracing.md](physoptics-guided-tracing.md)), **Separate stray light** doubles the channels: one set for paths on the graph, one for paths off it, so a detector shows the main beam and the ghosts and scatter apart.

### Measurements

**Windows** → **Detectors**, registered under the **Optical bench** capability ([physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)), lists every detector with its reading, and each detector's own window shows:

- **Power**, with its standard error from the spread of per-frame readings, and the paths that reached it.
- **Image** through `DisplayRegistry`: linear or logarithmic, false color, with a cursor readout in W per pixel and W/m².
- **Centroid and widths**: the first and second moments of the image, the second-moment (D4σ) diameters along the principal axes and their orientation, ISO 11146's definitions, with its iterative aperture cut for noisy images.
- **Peak irradiance** and **encircled power** as a curve of radius.
- **Profiles** along two lines through the centroid.
- **Spectrum**, for a spectrometer, in W per nm.
- **Solver prediction**: the beam solver's power on this detector and its Gaussian radii and centroid at the sensor's plane, beside the measured ones, with their relative differences. Agreement within noise says the setup behaves paraxially; disagreement is aberration, clipping or stray light, which is what the request wants to see.

A camera sensor's image is power, not a photograph. Converting to digital numbers through a quantum efficiency, a gain and noise is a sensor model this plan does not add (see "Open questions").

### A detector in the camera's picture

An alignment card or a screen scatters what reaches it, so the light integrator's camera connections show the spot on it in a render, as in a real lab when you hold a card in the beam. With a fog medium ([pbrt-participating-media.md](pbrt-participating-media.md)), the beam itself shows.

## Registry, bindings and data

- Detector kinds in `OPTIC_TRAITS`; the `detector` material type (absorbing, diffuse or transparent by kind).
- Queue 10 `CRT_QUEUE_DETECTOR`; stage 26 `deposit_detector`, variant `2601_deposit_detector`; the per-frame fold, a plain compute pass in `shaders/rt/light/`.
- Bindings 59 `Detectors` and 60 `DetectorFilm`.
- `src/optic_detectors.h/.cpp`: the records, the readouts and the measurement code (moments, ISO 11146 widths, encircled power), the last in `rt_optics` for testing.
- Scene file: each detector's kind, size, pixels and spectral range, in its optic record. Films are not saved; [physoptics-data-export.md](physoptics-data-export.md) writes them out.

## Verification

- A scene without detectors renders bit-identical, and stage 26 is never dispatched.
- A power meter covering the whole beam a meter from the laser reads the laser's power within 0.5 % after 10⁶ paths, and its standard error matches the spread over 20 independent runs.
- A beam profiler at a lens's focus measures second-moment radii within 1 % of the solver's, for an aberration-free case (a paraxial beam through a long-focal-length lens).
- An ideal 50:50 splitter between two power meters gives each half the power within noise; a probe plane before them changes neither reading beyond float rounding.
- An alignment card in the beam shows a spot in a camera render, of the radius the solver predicts at the card.
- `bin/optics_test`: the moment and ISO 11146 width code reproduces analytic widths for synthetic Gaussian, elliptical and top-hat images.

## Order of work

1. Power meters: the detector kind, the record, queue 10, stage 26, the film and the fold.
2. Camera sensors and beam profilers, the image window, moments and widths.
3. Cards, screens and probe planes.
4. The solver prediction beside each reading.
5. Spectrometers, after spectral rendering.
6. **Separate stray light**, after guided tracing.

## Waiting for the wave backend

When this plan is implemented, move this section to `docs/plans/waiting/physoptics-detectors.md`.

- Coherent detection: films that sum complex amplitudes per pixel before squaring, so interference fringes, Airy rings and speckle appear on camera sensors and screens, and a power meter's reading depends on the relative phase of the beams it collects.
- Spectral interference on a spectrometer: two delayed pulses on one spectrometer show fringes in the spectrum, as in femtoPro's Mach–Zehnder example (Brixner et al. 2023, figure 2).
- The detector frame, pixel geometry and per-pixel accumulation defined here are what the coherent films will reuse.

## Open questions

### Pending answers

- Real detector sizes. The default sizes are placeholders: the S120C's sensor aperture, the CS165MU's pixel count and pitch, and the BC207VIS/M's sensor, from the Thorlabs pages, for the user to confirm.

### Design questions

- A camera sensor model (quantum efficiency curve, gain, full well, read noise, bit depth) would turn the power image into what the lab's camera software shows. It is a post-process on the film and could be added without changing anything above.
- Whether detectors should be able to record angle as well as position (a far-field or angle-resolved detector, OpticStudio's polar detector). A detector at the focal plane of a lens does it optically, which the scene can already build.

## Sources

ISO 11146-1:2021, *Lasers and laser-related equipment: test methods for laser beam widths, divergence angles and beam propagation ratios*; the Zemax OpticStudio manual, non-sequential detectors (rectangle, color, polar, volume; transparent detectors); Brixner et al. 2023 (femtoPro's power meters, spectrometers and alignment cards); PBR 3ed §16.1 (light tracing and its flux normalization); Neumaier 1974 (compensated summation).
