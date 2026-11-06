# Detectors: waiting for the wave backend

These are the parts of [physoptics-detectors.md](../completed/physoptics-detectors.md) that need the phase and optical path length per path the wave backend will carry ([the laser optics plan](../physoptics-laser-optics.md), "The wave backend"). The rest of the plan was implemented on 2026-11-04.

- Coherent detection: films that sum complex amplitudes per pixel before squaring, so interference fringes, Airy rings and speckle appear on camera sensors and screens, and a power meter's reading depends on the relative phase of the beams it collects.
- Spectral interference on a spectrometer: two delayed pulses on one spectrometer show fringes in the spectrum, as in femtoPro's Mach–Zehnder example (Brixner et al. 2023, figure 2).
- The detector frame, pixel geometry and per-pixel accumulation the implemented plan defines are what the coherent films reuse: each detector's world-space frame is `GpuDetector` (`src/rt_accel.h`), its pixels `detectorGeometry()` (`src/optic_detectors.h`), and stage 26 finds a path's pixel from the point where its segment meets the sensor's plane (`shaders/rt/2601_deposit_detector.comp`). A coherent film needs two floats a pixel a channel in place of one, and a fold that adds amplitudes before it squares them.
