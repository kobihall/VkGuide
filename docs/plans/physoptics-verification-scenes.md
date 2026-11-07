# Optics verification scenes

The request ends with what should be buildable, in four stages: a camera lens, a telescope, a microscope, Gaussian beam propagation and an off-axis parabola's focus first; then prisms, chromatic aberration, XUV optics and dichroics once spectral rendering exists; then wave plates, beamsplitters, Brewster windows and crystals once polarization exists; and diffraction and interference once the wave backend exists. This plan turns each into a scene in `assets/scenes/optics/`, a reference it is checked against (closed-form, OpticStudio, Optiland or LightPipes), a tolerance, and a script that runs the check. The unit tests of the other plans (`bin/optics_test`, `bin/beam_test`) check parts; these scenes check the system, and that the beam solver and the path tracer agree with each other and with the references.

*Partly implemented. Five geometric scenes (`gaussian_train`, `telescope_kepler`, `telescope_galilean`, `oap_focus`, `tilted_spherical_mirror`) were built with [physoptics-laser-sources.md](completed/physoptics-laser-sources.md) on 2026-11-02, checked on white screens against ABCD, and moved to probe planes and a beam profiler with [physoptics-detectors.md](completed/physoptics-detectors.md) on 2026-11-04; "Results so far" below has their numbers. On 2026-11-07 each got a beam axis along its beam ([physoptics-beam-axis.md](physoptics-beam-axis.md)), and two scenes of the beam axis plan joined them, `beam_axis_setup` and `beam_axis_double_pass`. Each scene depends on the plans its row names; the geometric section needs milestones 1 to 4 of the [laser optics plan](physoptics-laser-optics.md), the spectral section milestone 5, the polarization section milestone 6. Part of the [laser optics plan](physoptics-laser-optics.md).*

## How a scene is checked

- Every scene starts from the **Optical bench** template ([physoptics-scene-capabilities.md](completed/physoptics-scene-capabilities.md)) and is a saved scene file with its optics, mounts, beam axes, detectors and, where it renders an image, a camera.
- `tools/verify/optics_verification.py` (the plan first named it `tools/verify_optics.py`; the verification scripts all live in `tools/verify/`) opens each with the batch mode of [pbrt-aovs-and-image-output.md](completed/pbrt-aovs-and-image-output.md), renders to its sample target, reads its detectors (`--detectors`, which writes `detectors.json` and each film as EXR) and, once they exist, the solver's tables ([physoptics-data-export.md](physoptics-data-export.md)), computes the scene's quantities, and compares them with the reference within the tolerance. It writes the results tables below.
- Each scene is checked twice where it can be: by the beam solver's analytic readout, and by light tracing on its detectors. The two must agree within the Monte Carlo noise, which the script estimates from 20 independent seeds.
- References that need OpticStudio are run on the Windows machine through the comparison scripts' input and output files ([physoptics-data-export.md](physoptics-data-export.md)).

## Geometric goals

| Scene | Setup | Checked | Reference | Tolerance |
|---|---|---|---|---|
| `camera_35mm.gltf` | a 35 mm prime lens, imported from a prescription, as a realistic camera's lens, imaging a test chart scene | effective focal length; real ray intercepts per field; distortion; relative illumination; the rendered chart's sharpness at focus | OpticStudio's prescription and ray trace reports; Optiland's real trace | 0.5 % in focal length; 1e-3 of the image height in intercepts |
| `telescope_kepler.gltf` | two N-BK7 plano-convex lenses, 100 mm and 25 mm, as a Keplerian telescope after a HeNe laser | beam radius magnification and divergence reduction on two beam profilers; angular magnification of a tilted input | closed-form `M = f₁ / f₂`; ABCD; Optiland | 1 % |
| `telescope_galilean.gltf` | the same with a −25 mm plano-concave lens | as above | as above | 1 % |
| `microscope.gltf` | an infinity-corrected microscope of two achromats as objective and tube lens, imaging a backlit resolution target onto a camera sensor | lateral magnification on the sensor image; the target's image position | `M = f_tube / f_obj`; OpticStudio | 1 % |
| `gaussian_train.gltf` | a laser, two turning mirrors, a 100 mm lens, a probe plane scanned through the focus | waist radius and position, Rayleigh range, M² from second moments at 21 planes | closed-form ABCD; LightPipes' Fresnel propagation | 1 % in radii, 0.5 mm in position |
| `tilted_spherical_mirror.gltf` | a concave mirror at 10°, 20° and 45° incidence focusing a collimated beam | the separation of the tangential and sagittal foci | `R cos θ / 2` and `R / (2 cos θ)`; OpticStudio's skew Gaussian beam | 1e-3 relative |
| `oap_focus.gltf` | a collimated 2 mm beam on a 90° off-axis parabola, then misaligned by 0.1° and 0.5° | focal spot radii on a beam profiler; the solver's waist; the growth of the spot with misalignment | `w₀' = λ f / (π w)` aligned; OpticStudio's or Optiland's spot diagram and Gaussian analyses when misaligned | 1 % aligned; 5 % in spot growth |
| `beam_axis_setup.gltf` | the beam axis plan's setup: the HNL050LB, two turning mirrors, an open iris, an f = 100 mm lens, a plate beamsplitter and a power meter on each arm | the two arms' powers now; with the solver, every visit **Hit**, the lens **Missed** after one adjuster revolution of the second mirror, the iris **Clipped** when closed to 0.6 mm | the arms' sum against the laser's power less the losses; the miss geometry | 0.85 of the laser's power; 1 µm in the miss distance |
| `beam_axis_double_pass.gltf` | a cat's eye: the HNL050LB on the axis of a 2 in f = 100 mm lens, a 1/2 in mirror at its focus turned 4.3°, and a pickoff turning the return onto a power meter | the returned power now; with the solver, both visits of the lens solved | the laser's power less the losses | 0.85 of the laser's power |

Every geometric scene also checks that its beam axis is all **Hit** at its nominal alignment, and that nudging a named optic produces the status change the scene's notes predict. The axes exist since 2026-11-07, each ending where the scene's sensor stack begins, since an axis does not visit a stack's members; the statuses wait for the solver. `tools/verify/beam_axis_check.py` checks that each scene's axes survive the editor's round trip.

### Results so far

`tools/verify/optics_verification.py` measures each beam on probe planes, which record the beam where it crosses and leave it as it was, so one render measures every plane of a scene. Each telescope's last plane is a BC207VIS/M beam profiler, which absorbs. The radii are the renderer's ISO 11146 second-moment radii (`--detectors`); the script fits w(z)² through the planes for a waist, its position and M². The references are ABCD's (the embedded Gaussian of M² λ, thick lenses, the tilted mirror's tangential and sagittal focal lengths `R cos θ / 2` and `R / (2 cos θ)`). The lenses have ideal AR coatings: an uncoated lens's ghosts widen a second-moment radius by about 1 %, which a real profiler would see too. Each scene's `<name>.expected.json` holds its reference values. At 64 samples per pixel on 2026-11-04, on detectors:

- `gaussian_train`, through 21 planes from −3 to +3 Rayleigh ranges: the focus 102.520 and 102.524 mm past the lens (ABCD 102.525 mm), the waist 44.84 and 44.80 µm (44.83 µm), M² 1.0055 and 1.0054 (the laser's 1.0053).
- `telescope_kepler`: radii 0.2068 mm 0.1 m past the second lens and 1.1848 × 1.1844 mm on the profiler 0.6 m past it (ABCD 0.2068 and 1.1846 mm). `telescope_galilean`: 0.2956 and 1.2820 × 1.2815 mm (0.2955 and 1.2816 mm).
- `oap_focus`: the waist 20.45 µm (λ f / (π w) = 20.43 µm, ABCD 20.45 µm) and its focus 101.683 mm from the datum (101.683 mm). Turned 0.1° and 0.5°, the spot at the nominal focus is 1.002 and 1.08 times the aligned waist (x) and moves 0.355 and 1.775 mm: recorded, not checked, until OpticStudio's or Optiland's numbers exist. On screens on 2026-11-02 the same spots read 1.02 and 1.24 times the waist. The field and its 256 pixels are the same; the measure is not. The screens took the whole image's moments at pixel centers, and the detectors take ISO 11146's iterative area with Sheppard's correction. At 0.5° the field's pixels are 21 µm, as wide as the waist, which pixel-center moments widen most. A finer field would settle it.
- `tilted_spherical_mirror`: every focus within 0.01 mm of ABCD; the foci 3.067, 12.467 and 70.801 mm apart at 10°, 20° and 45° (`R / (2 cos θ) − R cos θ / 2` = 3.062, 12.449 and 70.711 mm). At 45° the tangential waist is 16.0 µm against ABCD's 14.2 µm: the sphere's other aberrations, which ABCD leaves out.

- `beam_axis_setup`, on 2026-11-07: the reflected arm 2.076 mW and the transmitted arm 2.547 mW of the laser's 5 mW, 0.925 of its power, the rest the two mirrors' and the splitter's losses. `beam_axis_double_pass`: the focus 94.159 mm past the lens with a 49.9 µm waist, the return 15.11 mm off the axis, and 4.647 mW on the meter, 0.929 of the laser's power. These powers say the authored order is the beam's, until the solver's statuses say it per visit.

The screen results of 2026-11-02 agreed with these to within their noise, apart from the misaligned parabola's spots. These checks found that catalog glasses were divided by the air's index twice ([physoptics-optical-materials.md](completed/physoptics-optical-materials.md)): before the fix the Gaussian train's focus was 0.11 mm long and the telescopes 0.3 % to 0.5 % wide.

Still to do for these scenes: the beam axes' **Hit** statuses and nudges, the solver's analytic readout beside the light tracing ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)), the 20-seed noise estimate (detector_check.py makes it for a power split), LightPipes' Fresnel propagation for `gaussian_train` (or `tools/verify/wave_optics.py`, which already propagates the laser's beam), and Optiland or OpticStudio for the misaligned parabola. `camera_35mm` and `microscope` wait for the realistic camera and the prescription import.

## Spectral goals

| Scene | Setup | Checked | Reference | Tolerance |
|---|---|---|---|---|
| `prism_dispersion.gltf` | an N-SF11 equilateral prism at minimum deviation for 587.6 nm, a broadband source, a screen | deviation per wavelength; the spectrum's position on the screen | `δ(λ) = 2 asin(n(λ) sin(A/2)) − A` | 1e-5 rad |
| `chromatic_singlet.gltf` | an N-BK7 100 mm singlet and an achromat, each focusing a broadband collimated beam | focal shift between the F and C lines | `Δf ≈ f / V_d` for the singlet; the achromat's data sheet | 2 % |
| `dichroic.gltf` | a long-pass dichroic at 45° between a 532 nm and a 1064 nm laser combined on one axis | power on the reflected and transmitted detectors per wavelength | the coating's table at 45° | 0.5 % |
| `xuv_spectrometer.gltf` | an XUV source with three harmonic lines, a gold toroidal mirror at grazing incidence, a VLS flat-field grating, a camera sensor in vacuum | line positions on the sensor; the toroid's focus | the grating equation; the toroidal mirror's radii; CXRO reflectances | one pixel; 1e-3 in reflectance |

## Polarization goals

| Scene | Setup | Checked | Reference | Tolerance |
|---|---|---|---|---|
| `waveplate_divider.gltf` | a linearly polarized laser, a half-wave plate in a rotation mount, a polarizing cube, two power meters | the two powers as the plate turns from 0° to 45° | `cos² 2θ`, `sin² 2θ` | 0.2 % |
| `quarter_wave.gltf` | a quarter-wave plate at 45° and a polarimeter detector | `s₃ / s₀` | ±1 | 1e-3 |
| `brewster_window.gltf` | an N-BK7 window set to Brewster's angle, s and p inputs | transmitted powers | Fresnel equations | 1e-4 |
| `calcite_displacer.gltf` | a calcite displacer and a beam profiler | separation of the two beams; their polarizations | `L tan ρ`; orthogonal linear | 1 µm; 1e-3 |

## Waiting for the wave backend

These need the phases and optical path lengths the wave backend will carry. When the rest of this plan is implemented, move this section to `docs/plans/waiting/physoptics-verification-scenes.md`.

Their references no longer need LightPipes alone: `tools/verify/wave_optics.py` (2026-11-02) propagates a laser's beam by the angular spectrum through masks and thin lenses, with M² above 1 and astigmatism, and its self-test places a circular aperture's first dark ring within 0.1 % of `1.22 λ z / D`. `iris_airy` and `truncated_gaussian` can be checked against it as soon as the wave backend renders them, and `laser_check.py`'s iris case already shows the ray tracer's side of `truncated_gaussian`: past a 1 mm iris the rays keep a clipped Gaussian 19 % brighter at its peak than the waves' Airy-ringed core.

| Scene | Setup | Checked | Reference |
|---|---|---|---|
| `double_slit.gltf` | a laser, a double slit, a screen far behind | fringe period `λ L / d` and envelope | closed-form; LightPipes |
| `iris_airy.gltf` | a collimated beam through a closed iris, a lens, a camera sensor at focus | the Airy disk's first dark ring at `1.22 λ f / D`, and its shift as the beam walks off the iris center | closed-form; POP |
| `michelson.gltf` and `mach_zehnder.gltf` | interferometers on the bench, with a delay mirror | fringe visibility against lateral misalignment and tilt, as femtoPro's Figure 2 shows for a 2 mm shift | femtoPro's visibility factor; POP |
| `truncated_gaussian.gltf` | a Gaussian beam clipped by a mirror's edge | the diffraction rings downstream | POP; LightPipes |

## Registry, bindings and data

- `assets/scenes/tests/optics_verification/*.gltf`, each with its expected values in `<name>.expected.json`.
- `tools/verify/optics_verification.py`, which renders, checks and (with `--write`) writes them; the results, filled in this file when the scenes run.

## Verification

This plan's own check is that it catches a planted error: changing N-BK7's first Sellmeier coefficient by 1e-3 in a scratch build must fail `chromatic_singlet` and `telescope_kepler`, and leave `oap_focus` (a mirror) passing.

## Order of work

1. `gaussian_train`, `tilted_spherical_mirror` and `oap_focus`, which need only the solver and detectors; `beam_axis_setup` and `beam_axis_double_pass`, whose statuses need the solver.
2. The two telescopes and the microscope.
3. `camera_35mm`, once the realistic camera exists.
4. The spectral scenes, after milestone 5.
5. The polarization scenes, after milestone 6.

## Open questions

### Pending answers

- The 35 mm prime. Which design to build: a prescription from a patent, a lens database the user has, or a `.zmx` the user supplies. pbrt's `wide.22mm.dat` is a stand-in until then.
- The microscope and telescope parts. Which Thorlabs lenses and objectives the lab's own setups use, so the scenes are the setups the user will compare against on the bench.
- The lab's setups. Any real experiment of the user's worth reproducing as a scene, with its measured values, which would be the strongest reference of all.

## Sources

Smith 2008, *Modern optical engineering* (telescopes, microscopes, chromatic aberration); Hecht 2017, *Optics* (prisms at minimum deviation, Brewster's angle, double slits, Airy disks); Siegman 1986, *Lasers* (Gaussian beams through ABCD systems); Palmer and Loewen 2014, *Diffraction grating handbook*; Brixner et al. 2023, figure 2 (interference visibility against misalignment); the comparison tools of [physoptics-data-export.md](physoptics-data-export.md).
