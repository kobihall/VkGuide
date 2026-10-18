# Optics verification scenes

The request ends with what should be buildable, in four stages: a camera lens, a telescope, a microscope, Gaussian beam propagation and an off-axis parabola's focus first; then prisms, chromatic aberration, XUV optics and dichroics once spectral rendering exists; then wave plates, beamsplitters, Brewster windows and crystals once polarization exists; and diffraction and interference once the wave backend exists. This plan turns each into a scene in `assets/scenes/optics/`, a reference it is checked against (closed-form, OpticStudio, Optiland or LightPipes), a tolerance, and a script that runs the check. The unit tests of the other plans (`bin/optics_test`, `bin/beam_test`) check parts; these scenes check the system, and that the beam solver and the path tracer agree with each other and with the references.

*Not implemented. Each scene depends on the plans its row names; the geometric section needs milestones 1 to 4 of the [laser optics plan](physoptics-laser-optics.md), the spectral section milestone 5, the polarization section milestone 6. Part of the [laser optics plan](physoptics-laser-optics.md).*

## How a scene is checked

- Every scene starts from the **Optical bench** template ([physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)) and is a saved scene file with its optics, mounts, beam axes, detectors and, where it renders an image, a camera.
- `tools/verify_optics.py` opens each with the batch mode of [pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md), renders to its sample target, exports its detectors and the solver's tables ([physoptics-data-export.md](physoptics-data-export.md)), computes the scene's quantities, and compares them with the reference within the tolerance. It writes the results tables below.
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

Every geometric scene also checks that its beam axis is all **Hit** at its nominal alignment, and that nudging a named optic produces the status change the scene's notes predict.

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

| Scene | Setup | Checked | Reference |
|---|---|---|---|
| `double_slit.gltf` | a laser, a double slit, a screen far behind | fringe period `λ L / d` and envelope | closed-form; LightPipes |
| `iris_airy.gltf` | a collimated beam through a closed iris, a lens, a camera sensor at focus | the Airy disk's first dark ring at `1.22 λ f / D`, and its shift as the beam walks off the iris center | closed-form; POP |
| `michelson.gltf` and `mach_zehnder.gltf` | interferometers on the bench, with a delay mirror | fringe visibility against lateral misalignment and tilt, as femtoPro's Figure 2 shows for a 2 mm shift | femtoPro's visibility factor; POP |
| `truncated_gaussian.gltf` | a Gaussian beam clipped by a mirror's edge | the diffraction rings downstream | POP; LightPipes |

## Registry, bindings and data

- `assets/scenes/optics/*.gltf`, each with a short notes file of its expected values.
- `tools/verify_optics.py`; the results tables, filled in this file when the scenes run.

## Verification

This plan's own check is that it catches a planted error: changing N-BK7's first Sellmeier coefficient by 1e-3 in a scratch build must fail `chromatic_singlet` and `telescope_kepler`, and leave `oap_focus` (a mirror) passing.

## Order of work

1. `gaussian_train`, `tilted_spherical_mirror` and `oap_focus`, which need only the solver and detectors.
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
