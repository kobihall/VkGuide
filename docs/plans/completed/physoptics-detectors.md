# Detectors

A simulated experiment is only as useful as what it lets you measure. In the lab that is a power meter's reading, a camera's frame, a beam profiler's widths, a spectrometer's trace, or the spot on an alignment card. This plan adds those as optical elements whose surfaces record the light that reaches them: light paths from the lasers deposit their power on a detector's film as they hit it, in a new stage after kernel 02, and a measurement panel turns each film into the numbers a lab reports, with the beam solver's analytic prediction beside each one.

*Implemented 2026-11-04, apart from what "Not built" lists; "As built" says where the code differs from the design below, and [the waiting file](../waiting/physoptics-detectors.md) holds what waits for the wave backend. Depends on [physoptics-optical-elements.md](physoptics-optical-elements.md), [physoptics-laser-sources.md](physoptics-laser-sources.md), and the light integrator of [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md). Spectrometers need [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md). Part of the [laser optics plan](../physoptics-laser-optics.md).*

## What the renderer does today

- The only film is the camera's: kernel 09 folds each frame's samples into the accumulation image, and the tonemap pass displays it.
- Float atomics on storage buffers are available (`shaderBufferFloat32AtomicAdd`); on images they are not.
- A light path to a detector is importance transport, which needs every BSDF it scatters from to be reciprocal. Phong, the fuzzy metal and the Lambertian lobe of `pbr` are not (relative asymmetry 0.5 to 0.95, measured by [pbrt-verification-harness.md](pbrt-verification-harness.md)), so stray light that scatters from a mount or a table made of them reads the wrong power at a detector. Mounts and the table use the types of [pbrt-bsdf-library.md](pbrt-bsdf-library.md), and the detector verification scenes include a diffuse wall read from both sides.
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
- With a beam axis in a mode that tracks the graph ([physoptics-guided-tracing.md](../physoptics-guided-tracing.md)), **Separate stray light** doubles the channels: one set for paths on the graph, one for paths off it, so a detector shows the main beam and the ghosts and scatter apart.

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

An alignment card or a screen scatters what reaches it, so the light integrator's camera connections show the spot on it in a render, as in a real lab when you hold a card in the beam. With a fog medium ([pbrt-participating-media.md](../pbrt-participating-media.md)), the beam itself shows.

## Registry, bindings and data

- Detector kinds in `OPTIC_TRAITS`; the `detector` material type (absorbing, diffuse or transparent by kind).
- Queue 10 `CRT_QUEUE_DETECTOR`; stage 26 `deposit_detector`, variant `2601_deposit_detector`; the per-frame fold, a plain compute pass in `shaders/rt/light/`.
- Bindings 59 `Detectors` and 60 `DetectorFilm`.
- `src/optic_detectors.h/.cpp`: the records, the readouts and the measurement code (moments, ISO 11146 widths, encircled power), the last in `rt_optics` for testing.
- Scene file: each detector's kind, size, pixels and spectral range, in its optic record. Films are not saved; [physoptics-data-export.md](../physoptics-data-export.md) writes them out.

## Verification

- A scene without detectors renders bit-identical, and stage 26 is never dispatched.
- A power meter covering the whole beam a meter from the laser reads the laser's power within 0.5 % after 10⁶ paths, and its standard error matches the spread over 20 independent runs. Until detectors exist, `tools/verify/laser_check.py` makes this check with a white screen seen square-on by a pinhole camera (`tools/verify/beam_probe.py`), which reads 5.0000 mW of the HNL050LB's 5 mW; the detector version replaces the screen in the same scenes (`assets/scenes/optics/laser_*.gltf`), and the beam profilers of `optics_verification.py` likewise.
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

## Open questions

### Answers (2026-11-04)

- Scope: steps 1 to 3 and 5 of "Order of work". Step 4, the solver's prediction, waits for the beam solver and moves to [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md). Step 6, **Separate stray light**, waits for guided tracing and moves to [physoptics-guided-tracing.md](../physoptics-guided-tracing.md).
- Placement: a detector is placed freely, as a laser is. While the bench snaps, its sensor goes on the nearest hole with its axis along the grid, and the gizmo turns it in quarter turns. The Viewport outlines its housing, and the tracer sees only the sensor.
- Sizes: the sensor's true size and pitch, with a **binning** parameter that keeps a camera's or a profiler's film at most 512 pixels across by default. The numbers are Thorlabs' pages as read on 2026-11-04 and still wait for the user's confirmation (see "Pending answers").
- The batch mode writes `detectors.json` and an EXR film per detector (`--detectors <dir>`), ahead of [physoptics-data-export.md](../physoptics-data-export.md).

### Answers (2026-11-04, second round: detectors are sensors)

After the first build the user asked that detectors not be a system of their own beside the cameras, with its own windows, plots and rendering, but sensors a render targets as it targets a camera:

- **Sensors.** **Add** → **Sensor** lists the perspective, orthographic and spherical cameras, then, while the scene has Optical bench, the detector kinds. The Scene panel's **Sensors** folder holds every camera and detector, a folder per kind. Detectors are still placed and moved as optics are, on the bench with snapping: "similar to a camera" meant the Add list and the panel.
- **One render target.** Render Settings' **Render target** (once **Camera**) lists the cameras and the detectors. Light tracing records every detector beside the target, and the Raytraced Output window can show any of them. A render with a detector target needs no camera.
- **A detector target's film** is its own pixels. Under light tracing, with no camera to size the render, **Light paths per sample** (512 × 512 by default) takes the resolution's place.
- **No separate windows.** The Raytraced Output window shows the target's picture, or a detector's film and readings while it shows one. The Detectors window, each detector's window and the panel's reading are gone.
- **Sizes and pixels first.** A detector's size in millimeters and the pixels the renderer records are its parameters. A camera sensor or a beam profiler can also name a **physical sensor**, its own pixels and pitch, which set the size and which the film downsamples. No kind has presets, and there is no binning.
- **Bare sensors render under the path tracer too**, from camera paths cosine-weighted over the sensor's front hemisphere.
- **No spatial resolution where there is none.** A power meter, a spectrometer and a beam dump record one pixel: power, or a spectrum.
- **The realistic camera** is a camera sensor behind a beam axis of lens elements, recorded in [pbrt-cameras.md](pbrt-cameras.md) and [physoptics-prescription-import.md](../physoptics-prescription-import.md).

### Pending answers

- Named sensor models. The user chose plain defaults (a 10 × 10 mm camera sensor and profiler at 512 × 512, a 10 mm power meter and beam dump, a 3 mm spectrometer entrance over 350 to 700 nm in 3500 bins, an 85.6 × 54 mm card, a 100 × 100 mm screen, a 10 × 10 mm probe plane) and will name the lab's cameras and meters later, which then become presets of the physical-sensor section.

### Design questions

- A camera sensor model (quantum efficiency curve, gain, full well, read noise, bit depth) would turn the power image into what the lab's camera software shows. It is a post-process on the film and could be added without changing anything above.
- Whether detectors should be able to record angle as well as position (a far-field or angle-resolved detector, OpticStudio's polar detector). A detector at the focal plane of a lens does it optically, which the scene can already build.
- Whether a probe plane should also record light crossing it backwards, as a ghost travelling toward the laser does. Today every detector records only what arrives at its front.
- Rendering to several cameras at once. Light tracing records every detector in one pass; camera paths follow one camera, so several cameras would be several passes, or several pools.

## As built

- **The kinds.** Eight `OPTIC_TRAITS` rows after the laser (`src/optic_catalog.cpp`): `power_meter`, `camera_sensor`, `beam_profiler`, `alignment_card`, `screen`, `spectrometer`, `beam_dump` and `probe_plane`, none with presets. `src/optic_detectors.h` (in `rt_optics`) holds what each records: `detectorGeometry()` (the sensor's shape and size, the film's pixels, a physical sensor's pixels, a spectrometer's bins and range, the housing the Viewport outlines) and `buildDetector()`, the one builder. Every body is a single flat face at z = 0 facing -z, so light travelling along the body's +z reaches it; the film's columns run along +x and its rows along +y. A sheet's parameters are its `width`, `height`, `pixelsX` and `pixelsY`; a camera sensor's and a profiler's add `physicalSensor` with `nativeX`, `nativeY` and `pitch`, which then set the size, the film at most as fine (turning it on sets the film to every pixel). A power meter, a spectrometer and a beam dump are a `diameter` and one pixel.
- **Materials.** An absorbing sensor's slot holds the new `detector` material (`MaterialType::Detector`, `CRT_MATERIAL_DETECTOR` 14): black, with neither the scatter flag nor the emit flag, so kernel 02 queues a hit on it nowhere and the path ends. A card's and a screen's slot, `face`, holds pbrt's `diffuse`, 0.8 for the card and 1.0 for the screen. A probe plane has no slot.
- **Placement.** Free, as a laser is (`OpticTraits::snapsFree`), at the user's choice. The Optical bench overlays outline each housing's placeholder box behind its sensor and draw a probe plane's rectangle dashed. The raster preview draws a probe plane not at all.
- **Sensors.** **Add** → **Sensor** lists a camera of each projection, then the detector kinds through Optical bench's new `sensorMenu` hook; the Scene panel's **Sensors** folder (once **Cameras**) a folder per camera projection, then the detector kinds' through the `sensorFolders` hook. The Optics folder keeps the other optics. A detector's panel has **Render to this sensor**.
- **The render target.** `RaytraceRenderer` keeps the render camera and, beside it, `m_renderDetectorId`: while it names a detector, that detector is the target (`resolveTarget()`). Render Settings' **Render target** combo lists the cameras, then the detectors. A target needs no camera, so **Render** is enabled while the scene has any sensor. Light tracing toward a detector skips stages 12 and 14 (`GpuRenderSnapshot::cameraConnections`) and emits **Light paths per sample** (`RenderSettings::lightPathsPerSample`, a square of 256 to 2048 a side) in place of a resolution. Every detector records beside it. The light vertex cache and pbrt 3's BDPT are refused for a detector target, and greyed in the combo.
- **The sensor camera.** Under the path tracer a detector target is its own camera, `CameraKind::Sensor` (`src/camera_model.h`, `crt_camera.glsl`): kernel 00 starts a ray at a point of a pixel, 10 µm in front of the sensor, and draws its direction cosine-weighted over the front hemisphere from the lens dimensions, under `CRT_CAMERA_MODEL`. A pixel's mean radiance times π is its irradiance, and times its area its power: `DetectorFilms` reads the render's own film back for the readings (`recordSensorCopy()`), masking a round sensor's corners. Only the target records then. `bin/camera_test` checks the frame, the start just in front of the sensor and the cosine distribution, and the camera parity self-test gains the case. A scene camera can never be of the kind (`CameraTraits::sceneCamera`).
- **No queue 10.** Stage 26, `2601_deposit_detector.comp`, runs over the current ray queue after kernel 02, beside kernel 12, and reads each light path's segment as kernel 02 left it, from the path's origin to its hit or to infinity. A segment that ends on a sensor's front deposits there; one that crosses a probe plane's front deposits there on its way. Kernel 02 is unchanged: it already ends a path on a `detector` material and sends a card's or a screen's hit to the surface queue. A probe plane is therefore no geometry at all. Camera paths and shadow rays never see it, and it costs a light path no bounce, which a chain of 21 planes through a focus would otherwise exhaust (`CRT_MAX_DEPTH` is 16). Queue 10 stays reserved.
- **Only the front records.** A path that reaches a detector from behind passes a probe plane unrecorded, and is absorbed or scattered by a sensor unrecorded, as its housing would.
- **Binding 59** is a header (the detectors, the frame sums' floats, where the totals and the counts start), the radiometric width of each band of a band representation, then 96-byte `GpuDetector` records (`src/rt_accel.h`): the sensor's world-space frame and half sizes, its instance, its flags, its film and its offsets. `buildSceneAccel()` makes them in the order of the scene's optics; at most 32 (`MAX_DETECTORS`), the rest left out and the panel says so. A record finds its sensor's hits by instance; a probe plane's is intersected analytically.
- **Binding 60** and its fold, `shaders/rt/light/detector_fold.comp`, as `src/rt_detectors.h` (`DetectorFilms`, engine side) lays them out: each detector's frame sums, then every running sum and its Neumaier compensation, then per-frame totals per channel and counts of the paths that reached each detector. **A frame's sums are kept in copies** (`frameCopies()`), a path adding to the copy its index picks. A frame can add a million paths to the pixels a beam lands on, and a single float that many equal terms are added to lost 0.1 % of them: the power meter read 4.995 mW of 5 mW before the copies, and 5.0000005 mW after, when meters had a 64 × 64 film. The fold adds the copies into the running sum, once per detector after kernel 09. A film keeps up to 4096 copies (a one-pixel meter's), as many as fit in 2^20 floats; a meter then reads 5.0001 mW, the residue of a thousand adds a copy.
- **Readings.** `GpuRenderer` copies the totals back every frame and the running sums twice a second and when a render ends. A pixel's watts are its sum over the light paths emitted, one per pixel sample. The power is the frames' totals over their paths; its standard error is the per-frame readings' weighted spread (`FrameStatistics`). In exact RGB the power is the three channels' mean. In a spectral mode a film has one channel, the lanes' power integrated over wavelength: a hero lane weighted by 1 / (pdf × lanes), a band by its width. That is exact for box bands; the RGB Gaussians are no partition of unity over a narrow line and read 60 % of a 632.8 nm laser. A monochromatic render reads W/nm at its wavelength.
- **A spectrometer** bins each lane's power by its wavelength in a spectral mode, eight copies a bin, and reports W/nm; in exact RGB it reads the power alone and its window says to use a spectral mode.
- **Measurements** (`src/optic_detectors.h`, tested by `bin/optics_test`): ISO 11146-1's moments and D4σ diameters along x, y and the principal axes; ISO 11146-3's iterative integration area, three widths across; peak irradiance; encircled power; lineouts along the principal axes. One difference from the standard: **Sheppard's correction**, pitch² / 12, comes off each second moment. The film holds exact pixel powers, which the standard's sums at pixel centers spread by a pixel's own second moment. Without it the card's 0.2 mm pixels widened a 0.57 mm beam by 4 %, and with it the card reads w(z) to 0.09 %. The iterative area needs no baseline subtraction, since a film has no dark offset.
- **The Raytraced Output window** is drawn by the renderer now (`RaytraceRenderer::drawOutputWindow()`, an `AppWindow`; the display image is a registry texture). It shows the render target's picture, and while it shows a detector, the detector's readings below it (`src/detectors_ui.h`): the power, its error and the paths; the centroid, the ISO widths and the peak; encircled power and profiles as ImPlot curves; a spectrometer's spectrum; a cursor readout in watts and W/m². A light-traced detector's film shows through `shaders/rt/light/detector_view.comp`, linear or logarithmic, gray or false color, made only while it shows. A **Showing** combo picks any detector light tracing recorded beside the target. A first version had a **Detectors** window and a window per detector, which the user rejected.
- **Batch mode.** `--out` writes a light-traced detector target's film. `--detectors <dir>` writes `detectors.json` (every reading, each detector's frame, its moments and its spectrum) and `<name>.exr` films in watts per pixel, R, G and B in exact RGB or P in a spectral mode, row 0 at the sensor's +y. `vkscene.run_detectors()` reads them.
- **The Optical bench template** now holds the HNL050LB at the beam height and a power meter 200 mm along its beam, and runs light tracing. A render of a new bench reads 5.0000 mW.
- **Scene file:** payload version 29, the eight optic types, `renderDetector` (the target's optic index) and `render.lightPathsPerSample`. Version 29 was never committed before the redesign, so it was redefined rather than bumped.
- **A bug in the laser sources** surfaced here: a band representation projected every spectrum onto its bands at 0.25 nm steps, and a laser's 0.002 nm line fell between them, so a laser rendered nothing in the spectral bands. A line now projects from its profile's closed form (`projectLineOntoBand()`, `src/spectral.h`), checked by `bin/spectrum_test`.

### Checks

- `bin/optics_test`: every kind's film and sensor, the face square to the light; a Gaussian's centroid and D4σ (1e-3), a Gaussian over pixels of w / 4 with Sheppard's correction (2e-3), a rotated elliptical beam's principal widths and angle, a top hat's 2R, a hot pixel 7 mm out that biases the whole image's width and that the iterative area leaves out, encircled power at w, peak irradiance, a lineout, and the frames' standard error. `--selfcheck` catches them.
- `tools/verify/detector_check.py` on the scenes `tools/verify/detector_scenes.py` writes to `assets/scenes/optics/detector_*.gltf`, at 64 samples (4.2 million light paths) on 2026-11-04, after the redesign:
    - a 10 mm power meter 1 m from the HNL050LB reads 5.00011 mW, every path reaching it; since each path carries the laser's power, its error is 1e-9 W;
    - an ideal 50:50 cube sends 2.48452 and 2.48950 mW to its two meters, of 2.48732 mW each (half the beam, less the cube's 25.4 mm of N-BK7), 2.2 and 1.7 of their 1.30 µW errors away; the binomial error is 1.22 µW; over 20 seeds the readings spread by 1.45 µW and the meters report 1.19 µW on average; a probe plane before the cube changes neither reading by more than 2e-8;
    - a 10 mm beam profiler at an f = 500 mm lens's focus reads radii of 234.48 × 234.50 µm, ABCD's waist 234.44 µm (0.03 %);
    - an alignment card 0.8 m out reads 5.00014 mW and radii within 0.08 % of w(z), and a camera's picture of the card sees 3.99999 mW of the 4 mW its white 0.8 sends back, at the same radius;
    - a diffuse transmission wall (0.5 and 0.3) sends 17.445 and 10.424 µW into 10 mm meters 50 mm away at 45 degrees on either side, against a Lambertian spot's 17.503 and 10.502 µW (1.5 and 1.4 standard errors);
    - a spectrometer of 0.1 nm bins sees the 632.8 and 532 nm lines in the bins at 632.85 and 531.95 nm, with 4.998 and 2.000 mW;
    - a beam dump absorbs 5.00011 mW, and a meter behind it reads 0;
    - with no camera, the meter the render target, 512 × 512 light paths a sample give 16.8 million paths and 5.00009 mW;
    - a 10 mm camera sensor 30 mm from a glowing 20 mm square of radiance 2 reads 75.233 µW from camera paths (256 samples) and 75.278 µW by light tracing, against the closed form's 75.351 µW (-0.16 % and -0.10 %);
    - under a uniform 0.5 sky every pixel of the sensor reads π × 0.5 W/m², 157.0796 µW in all.
- `tools/verify/laser_check.py` and `tools/verify/optics_verification.py` measure on detectors now, in one render a scene (see [physoptics-verification-scenes.md](../physoptics-verification-scenes.md)).
- Scenes without detectors: stage 26 and the fold are never recorded. Twelve scenes rendered bit-identical to the last commit's build (the Cornell box, Veach's scene, all materials, the glass caustic, four optics scenes, a projection light, a textured plane, bokeh, thin glass and the camera looking into a laser), and after the sensor camera, twelve again, among them four through the camera model (bokeh, a tilted ground, an orthographic box, depth of field). `optics_bench.gltf` differs in 28 pixels, as the last commit's build differs from itself run to run.
- The validation layers report nothing new on four detector scenes with every detector window open, nor on a profiler target with no camera or a camera-path sensor target; the one message, a binary semaphore signaled twice, comes from the last commit's build too.

## Not built

- The solver's prediction beside each reading (step 4): there is no beam solver yet. It moves to [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md), whose analytic readouts at detectors are the prediction.
- **Separate stray light** (step 6): there is no beam axis or guided tracing yet. It moves to [physoptics-guided-tracing.md](../physoptics-guided-tracing.md).
- Detectors under the light vertex cache and pbrt 3's BDPT, and any detector but the render target under the path tracer: only light tracing starts paths at the lasers (see [physoptics-laser-sources.md](physoptics-laser-sources.md)), so only it runs stage 26, and camera paths follow one sensor. A path-traced sensor's readings have no standard error, and its spectral modes give the film's RGB, not watts.
- Several camera targets in one render (see "Design questions").
- Housings as geometry: the user chose free placement with outlines. A beam that misses a sensor passes its housing.
- A beam dump's inner cone: the dump is an ideal absorbing entrance disk.
- Polarimeter films: [physoptics-polarization-optics.md](../physoptics-polarization-optics.md).
- Exporting films as ZBF or NPY, and loading a ZBF as a reference: [physoptics-data-export.md](../physoptics-data-export.md), which can build on `writeDetectorReadings()`.
- Radiometric power in the RGB Gaussians and Gaussian bands, whose bases are no partition of unity over a narrow line.

## Sources

ISO 11146-1:2021, *Lasers and laser-related equipment: test methods for laser beam widths, divergence angles and beam propagation ratios*; the Zemax OpticStudio manual, non-sequential detectors (rectangle, color, polar, volume; transparent detectors); Brixner et al. 2023 (femtoPro's power meters, spectrometers and alignment cards); PBR 3ed §16.1 (light tracing and its flux normalization); Neumaier 1974 (compensated summation).
