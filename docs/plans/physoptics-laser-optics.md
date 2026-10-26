# Laser optics simulator

This plan turns the path tracer into a simulator of optical experiments on a laser table. You place Thorlabs-style optics on posts on a breadboard, string them together with a beam axis, and see where a Gaussian beam goes, how large it is everywhere, what it clips, where its ghosts land, and what a detector or a camera behind the optics records. The point is to preview how a real setup deviates from the ideal one: astigmatism from a tilted spherical mirror, a beam walking off a lens center, an iris clipping a misaligned beam, a back reflection heading for the laser.

The request this plan answers is `docs/00-laser-optics.md`. It takes its approach from femtoPro, the University of Würzburg's virtual ultrafast laser laboratory, without copying its feature list. The 18 `physoptics-*.md` plans below implement it, on top of the `pbrt-*.md` plans of the [pbrt roadmap](pbrt-roadmap.md). This file holds the decisions that shape all of them, the resources they reserve, and one linear order to build everything in.

*Written 2026-10-18. Nothing here is implemented. Read the [pbrt roadmap](pbrt-roadmap.md), `docs/raytracing-overview.md` and `docs/codebase-map.md` §8 first; every plan here assumes them.*

## What femtoPro does, and what this plan takes from it

femtoPro (Brixner et al. 2023; Müller et al. 2025; Brixner et al. 2025) simulates one beam per segment, not millions of rays. Each segment between two optical elements carries a stigmatic Gaussian beam (a complex beam parameter `q`), a sampled spectral and temporal field, and a pulse energy. Elements are "general optical elements" that act in one active plane: `k_out = (k_in − d/f) / |k_in − d/f|` for a hit at offset `d` from an element of focal length `f`, and `1/q_out = 1/q_in − 1/f`. A clipped beam becomes a new Gaussian whose diameter fits the aperture, with the transmitted energy from a precomputed Gaussian-over-aperture table. The segments form a dynamic directed graph that a CPU solver rebuilds every frame by "selective updating": only edges downstream of a change are recomputed. Detectors integrate interference over their area instead of resolving fringes. Fine alignment happens through mount adjusters, separately from coarse placement.

What this plan keeps:

- The beam as a few rays per segment for interactive work, with Gaussian beam parameters attached, solved on the CPU, and recomputed only downstream of a change ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)).
- The split between coarse placement and fine adjustment ([physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)).
- A beam that is visible as a volume in the viewport, colored by its spectrum ([physoptics-beam-preview.md](physoptics-beam-preview.md)).
- Detectors that measure rather than display: power meters, cameras, cards ([physoptics-detectors.md](physoptics-detectors.md)).

What it does differently:

- Every optic is real geometry with exact surfaces, not an active plane, so thick lenses, prisms, wedges, off-axis parabolas and cube beamsplitters behave as glass does ([physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)).
- The Gaussian beam is general astigmatic, propagated by the ray-transfer matrix of the real surfaces at the chief ray, which femtoPro leaves out.
- Behind the interactive solver sits the physically based path tracer. Light tracing from the laser through the full scene finds clipping by mounts, stray reflections, ghosts and back reflections, which femtoPro's single-beam graph only partly captures.
- The beam axis is authored by you, as the intended path, and the solver reports where the real beam departs from it ([physoptics-beam-axis.md](physoptics-beam-axis.md)).
- Spectral, polarized and grating optics come from the spectral and polarization plans of the pbrt roadmap.
- No virtual reality, no tutorials or training missions, no multiplayer. These are out of scope at the user's request.

## Decisions that shape every plan

The user settled these on 2026-10-18. Treat them as fixed.

| Topic | Decision |
|---|---|
| Optic geometry | Surface-list optical bodies: a new compound shape kind holding an ordered list of exact bounded surfaces and the media between them, internal interfaces included. Catalog builders, `.zmx` import and pbrt's lens `.dat` files all produce the same list. Not CSG, not meshes. |
| Beam model | General astigmatic Gaussian beams. A chief ray per segment carries its differential ray-transfer matrix through the real surfaces, in double precision on the CPU, and a complex 2 × 2 beam matrix propagates through it. |
| Trace modes | Four, per beam axis: full scene, seeded (an exact speed-up), guided (port choices biased toward the authored graph, reweighted, unbiased), and sequential (graph only). |
| Mount placement | Breadboard-physical: the gizmo slides a post in x and z (continuously or snapped to 25 mm holes), turns it about the post's axis and changes its height. Tip and tilt come only from fine adjusters. A per-optic **Free placement** toggle unlocks six degrees of freedom. |
| Fine adjustment | Real kinematics: an adjuster turns the optic about the axis through the mount's pivot ball and the other adjuster's ball, at Thorlabs' angle per revolution. |
| Table | A procedurally generated plate with the B3030A's hole pattern, thickness and finish, resizable in 25 mm steps, 300 × 300 mm by default. |
| Beam height | A per-scene setting, 100 mm by default, which every new mount's post is cut to. |
| Wedge prisms | The 2°, 4°, 6° and 10° angles are beam deviations, as Thorlabs specifies its PS81x prisms. |
| Off-axis parabolas | Off-axis angle picked from Thorlabs' set (15°, 30°, 45°, 60°, 90°; 90° by default), reflected focal length continuous. |
| Plate beamsplitters | Thorlabs' 30 arcmin wedge and AR-coated back face by default, both editable. |
| Prisms | Height equals the leg (right angle) or the side (equilateral), editable. |
| Curved mirror substrates | Thorlabs CM-series and MPD-series catalog thicknesses (CM254: 6.0 mm at the center). |
| Square filters | Held in an FP01 plate holder (`assets/optical_models/Filter_mount-FP01.step`), which has no upper size limit and holds plates up to 14.7 mm thick. The SFH3 model is not used. |
| Missing models | Lasers and detectors get procedural placeholders now, and [physoptics-optomechanical-models.md](physoptics-optomechanical-models.md) lists Thorlabs parts whose STEP files replace them later. |
| References | Zemax OpticStudio, open-source Python ray tracers (Optiland, RayOptics) and open-source wave optics (LightPipes, POPPY) are all available for comparison. |
| Wave optics | Not planned here. The user builds the coherent backend last. See "The wave backend" below. |
| Scene modes | The optics editor is a scene capability, **Optical bench**, which a scene lists in its file and which adds the optics' folders, windows, menus and recommended settings without changing how anything renders. Capabilities combine; **New Scene** offers templates that turn them on; a capability cannot be turned off while its objects exist; its recommended settings are offered once, never forced; named workspaces come later ([physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)). |

Technical defaults the plans state and the user can object to:

- The world unit is the meter, as in glTF. Every optics panel shows millimeters, degrees and microradians.
- The CPU side of the optics is a Vulkan-free static library, `rt_optics`, built at `-O2` like `rt_bvh` and `rt_light`. Its geometry is templated on the scalar type: the beam solver runs in `double`, and the `float` instantiation is the line-for-line mirror of the GLSL.
- In exact RGB mode a glass's index is read at the scene's **Design wavelength** (587.6 nm by default, or the laser's wavelength when the scene has one laser), not at 550 nm. [pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md) added the setting; [physoptics-laser-sources.md](physoptics-laser-sources.md) makes it follow a single laser.
- An optic's materials are library entries the optic owns, the way a glTF model owns its imported materials in [pbrt-material-system.md](completed/pbrt-material-system.md).
- Ports on beam-axis nodes are general (any number of outputs), so beamsplitters, acousto-optic modulators and nonlinear crystals fit the graph. Only the linear optics of the element list get physics in these plans.

## How the parts fit together

```text
SceneOptic (catalog kind, sizes, coatings, mount)            physoptics-optical-elements
   │  builds
   ▼
OpticPrescription (surfaces, apertures, media)  ──────────►  CPU rt_optics (double)   physoptics-optical-surfaces
   │  packs                                                    │
   ▼                                                          ▼
GPU OpticSurfaces, one Optic instance per element         Gaussian beam solver ──► beam preview
   │                                                          │      ▲              locked transforms
   ▼                                                          │      │              diagnostics
path tracer: light paths from lasers (light tracing),         │   BeamAxis graph (authored ports)
detectors, camera splats, four trace modes                    │
                                                              ▼
mount library (STEP → meshes + connection nodes) ──► raster preview, and BLASes the tracer clips against
```

Two consumers read the same optics. The solver traces one chief ray per segment through the full scene on the CPU, fast enough to run on every gizmo drag. The path tracer traces millions of light paths on the GPU for detectors, camera images and stray light. Both use the same surface prescriptions, the same materials and the same scene geometry, and [physoptics-verification-scenes.md](physoptics-verification-scenes.md) checks that they agree.

## The plans

| Plan | What it adds |
|---|---|
| [physoptics-scene-capabilities.md](physoptics-scene-capabilities.md) | Scene capabilities (**Optical bench** first), templates for **New Scene**, and later workspaces, so the optics editor never shows in a plain render scene |
| [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md) | The surface-list optical body: surface kinds, apertures, media, the `CRT_SHAPE_OPTIC` instance, `crt_optic.glsl`, `rt_optics` |
| [physoptics-optical-materials.md](physoptics-optical-materials.md) | Glass catalogs (AGF, Sellmeier), coatings (ideal, tabulated, thin-film stacks, metals, absorbers), the optical interface material |
| [physoptics-optical-elements.md](physoptics-optical-elements.md) | `SceneOptic` and `OPTIC_TRAITS`: every element of the request with its Thorlabs sizes, the Optics folder, the scene file |
| [physoptics-optomechanical-models.md](physoptics-optomechanical-models.md) | The offline STEP-to-mesh pipeline, cleanup, connection nodes, the mount library, placeholders and the parts list |
| [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md) | The breadboard, posts, post holders and clamps, mount poses, hole snapping, fine adjusters, mounts as traced geometry |
| [physoptics-laser-sources.md](physoptics-laser-sources.md) | The laser element: Gaussian emission, line spectra, power, polarization, emitted-ray sampling |
| [physoptics-beam-axis.md](physoptics-beam-axis.md) | The authored graph of elements and ports, its editor, the traced graph beside it |
| [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md) | Chief rays with differential ray transfer, astigmatic beam matrices, clipping, ghosts, selective updating |
| [physoptics-beam-preview.md](physoptics-beam-preview.md) | Beams in the raster viewport as at most three conical sections per segment |
| [physoptics-locked-transforms.md](physoptics-locked-transforms.md) | Moving one degree of freedom while the beam path holds still: carry, re-aim, fixed |
| [physoptics-guided-tracing.md](physoptics-guided-tracing.md) | The four trace modes in the path tracer, and the per-path beam state |
| [physoptics-detectors.md](physoptics-detectors.md) | Detector elements, the detector film, the deposit stage, measurement readouts |
| [physoptics-prescription-import.md](physoptics-prescription-import.md) | `.zmx` and pbrt lens `.dat` import into optics and assemblies, with the glass catalogs they name |
| [physoptics-data-export.md](physoptics-data-export.md) | ZBF, ZRD and array exports, and the comparison scripts for OpticStudio, Optiland and LightPipes |
| [physoptics-polarization-optics.md](physoptics-polarization-optics.md) | Wave plates, polarizers, polarizing beamsplitters, Brewster windows, birefringent crystals |
| [physoptics-gratings-and-xuv.md](physoptics-gratings-and-xuv.md) | Toroidal and cylindrical surfaces, ruled gratings, grazing-incidence XUV mirrors, non-visible spectra |
| [physoptics-verification-scenes.md](physoptics-verification-scenes.md) | The request's goal setups as test scenes, each with its reference |

## The roadmap

One linear order for every plan, pbrt and physoptics alike. Each item lists its own prerequisites, and this order satisfies them all. The milestones are the request's goal lists: after milestone 4 the geometric goals can be built, after 5 the spectral ones, after 6 the polarization ones. A pbrt plan that the laser work needs is pulled forward to where it is first needed; the rest of the pbrt roadmap follows in milestone 7.

### Milestone 0: foundations

1. [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md). Implemented 2026-10-18.
2. [pbrt-robust-intersections.md](completed/pbrt-robust-intersections.md). Implemented 2026-10-19. Its camera-relative space is split off to [pbrt-camera-relative-space.md](pbrt-camera-relative-space.md), which no laser optics plan needs.
3. [pbrt-verification-harness.md](completed/pbrt-verification-harness.md). Implemented 2026-10-20, ahead of items 4 and 6, which it does not depend on.
4. [pbrt-material-system.md](completed/pbrt-material-system.md). Implemented 2026-10-19, ahead of items 2 and 3, which it does not depend on.
5. [pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md). Implemented 2026-10-23, with a monochromatic representation for one laser wavelength, laser lines as spectra, wavelength sampling from the emitters' spectra, spectral images, and the **Design wavelength**.
6. [pbrt-sample-generation.md](completed/pbrt-sample-generation.md). Implemented 2026-10-19, ahead of items 3 and 5, which it does not depend on.
7. [pbrt-film-and-reconstruction.md](completed/pbrt-film-and-reconstruction.md). Implemented 2026-10-21.
8. [pbrt-bsdf-library.md](pbrt-bsdf-library.md). Its `glass` (dispersive, smooth and rough) and `conductor` were built with item 5; the rest is not implemented.

### Milestone 1: the bench

9. [physoptics-scene-capabilities.md](physoptics-scene-capabilities.md). Capabilities and templates, so every later plan registers its UI under **Optical bench** and a plain render scene never shows it.
10. [physoptics-optomechanical-models.md](physoptics-optomechanical-models.md). Offline work, which can start in parallel with milestone 0.
11. [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)
12. [physoptics-optical-materials.md](physoptics-optical-materials.md)
13. [physoptics-optical-elements.md](physoptics-optical-elements.md)
14. [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)

### Milestone 2: beams

15. [pbrt-light-types.md](pbrt-light-types.md). Emitted-ray sampling, which the laser's emission joins.
16. [physoptics-laser-sources.md](physoptics-laser-sources.md)
17. [physoptics-beam-axis.md](physoptics-beam-axis.md)
18. [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)
19. [physoptics-beam-preview.md](physoptics-beam-preview.md)
20. [physoptics-locked-transforms.md](physoptics-locked-transforms.md)

### Milestone 3: light from lasers, measured

21. [pbrt-aovs-and-image-output.md](completed/pbrt-aovs-and-image-output.md). Implemented 2026-10-21. Image export and the batch mode the verification scripts use.
22. [pbrt-display-and-color.md](pbrt-display-and-color.md). The sRGB curve and false color for wavelengths outside the visible range.
23. [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md). Its light tracing integrator (steps 1 and 2 of that plan) is what renders laser light; `lvc` and `bdpt` can follow at any later point.
24. [physoptics-detectors.md](physoptics-detectors.md)
25. [physoptics-guided-tracing.md](physoptics-guided-tracing.md)
26. [physoptics-data-export.md](physoptics-data-export.md)

### Milestone 4: imaging systems and the geometric goals

27. [physoptics-prescription-import.md](physoptics-prescription-import.md)
28. [pbrt-cameras.md](pbrt-cameras.md). Its realistic camera traces through a lens assembly from item 27.
29. [physoptics-verification-scenes.md](physoptics-verification-scenes.md). The geometric section: a 35 mm prime lens, a telescope, a microscope, Gaussian beam propagation through mirrors and lenses, an off-axis parabola's focal spot. Its spectral and polarization sections are verified as milestones 5 and 6 land.

### Milestone 5: the spectral goals

30. [physoptics-gratings-and-xuv.md](physoptics-gratings-and-xuv.md). Prism dispersion and chromatic aberration need nothing beyond items 5, 8 and 12; dichroic mirrors are coatings from item 12.

### Milestone 6: the polarization goals

31. [pbrt-polarization.md](pbrt-polarization.md)
32. [physoptics-polarization-optics.md](physoptics-polarization-optics.md)

### Milestone 7: the rest of the pbrt roadmap

In the order below, which respects the [pbrt roadmap](pbrt-roadmap.md)'s dependency graph. None of these blocks a laser optics goal.

33. [pbrt-russian-roulette.md](pbrt-russian-roulette.md)
34. [pbrt-light-shape-sampling.md](pbrt-light-shape-sampling.md)
35. [pbrt-bvh-light-sampler.md](pbrt-bvh-light-sampler.md)
36. [pbrt-direct-lighting-strategies.md](pbrt-direct-lighting-strategies.md)
37. [pbrt-textures.md](pbrt-textures.md)
38. [pbrt-bump-mapping.md](pbrt-bump-mapping.md)
39. [pbrt-layered-materials.md](pbrt-layered-materials.md)
40. [pbrt-appearance-extras.md](pbrt-appearance-extras.md)
41. [pbrt-measured-bsdfs.md](pbrt-measured-bsdfs.md)
42. [pbrt-environment-lighting.md](pbrt-environment-lighting.md)
43. [pbrt-participating-media.md](pbrt-participating-media.md). A fog medium also makes laser beams visible in a render.
44. [pbrt-subsurface-scattering.md](pbrt-subsurface-scattering.md)
45. [pbrt-analytic-shapes.md](pbrt-analytic-shapes.md)
46. [pbrt-csg.md](pbrt-csg.md)
47. [pbrt-motion-blur.md](pbrt-motion-blur.md)
48. [pbrt-curves-and-hair.md](pbrt-curves-and-hair.md)
49. [pbrt-subdivision-and-displacement.md](pbrt-subdivision-and-displacement.md)
50. [pbrt-procedural-textures.md](pbrt-procedural-textures.md). Blocked on the user's choice of representation.
51. [pbrt-alternative-accelerators.md](pbrt-alternative-accelerators.md)
52. [pbrt-gpu-bvh-construction.md](pbrt-gpu-bvh-construction.md)
53. [pbrt-wavefront-performance.md](pbrt-wavefront-performance.md)
54. [pbrt-adaptive-sampling.md](pbrt-adaptive-sampling.md)
55. [pbrt-path-regularization.md](pbrt-path-regularization.md)
56. [pbrt-photon-mapping.md](pbrt-photon-mapping.md)
57. [pbrt-metropolis-light-transport.md](pbrt-metropolis-light-transport.md)
58. [pbrt-path-guiding.md](pbrt-path-guiding.md)
59. [pbrt-resampled-direct-lighting.md](pbrt-resampled-direct-lighting.md)
60. [pbrt-scene-import.md](completed/pbrt-scene-import.md)

[pbrt-camera-relative-space.md](pbrt-camera-relative-space.md) is not in this order. It starts when a scene has coordinates beyond about 10 km, which no laser optics goal does.

### Milestone 8: the wave backend, by the user

61. The coherent wave backend, which the user designs and builds after everything above.
62. The "waiting" files every plan above split off for it (see below), in the order their plans appear here.

[simulation-domain.md](simulation-domain.md) and [wave-simulation.md](wave-simulation.md) are an older, separate 2D wave-equation solver on a placed plane. They are not part of this roadmap and nothing here depends on them.

## The wave backend

The user will design a coherent backend last: paths that carry a phase and an optical path length, and detectors that sum amplitudes before squaring, toward a Monte Carlo solution over the dyadic Green's function. No plan here designs it. To keep its door open, the plans hold to three rules.

- Dependencies are stated abstractly. A plan that needs coherent light says "the wave backend's phase and optical path length per path", never a struct field, a binding or a kernel.
- Data that the backend will need is kept where it is cheap. The solver records optical path length per segment in double precision, coatings keep complex amplitudes `r` and `t` (as well as `|r|²`), gratings keep their order phases, and detectors keep their pixel geometry in the detector's own frame.
- Wave-dependent features are split off when their plan is implemented. Each plan that has such features lists them under a heading "Waiting for the wave backend". When the implementing session finishes the rest, it moves the implemented plan to `docs/plans/completed/<name>.md` as usual and writes the waiting section to `docs/plans/waiting/<name>.md`, with a pointer from each file to the other.

The request's wave goals (a double slit, the Airy rings of an iris used for beam walking, interferometers, diffraction from finite optics) are all such sections, gathered in [physoptics-verification-scenes.md](physoptics-verification-scenes.md).

## Shared reservations

Every rule of the [pbrt roadmap](pbrt-roadmap.md) holds here: exact RGB stays bit-identical for a scene that uses none of a plan's features, binding numbers and stages are append-only, every GLSL math module gets a line-for-line C++ mirror and a command-line test, and every plan that saves something bumps `SCENE_FILE_FORMAT_VERSION` by one when it lands. The reservations below continue the pbrt roadmap's tables, so the two sets of plans never collide.

### Binding numbers

| Binding | Resource | Plan |
|---|---|---|
| 55 | `OpticSurfaces` | optical-surfaces |
| 56 | `OpticCoatings` | optical-materials |
| 57 | `OpticPathState` | guided-tracing |
| 58 | `BeamGraph` | guided-tracing |
| 59 | `Detectors` | detectors |
| 60 | `DetectorFilm` | detectors |
| 61 | `RayRecords` | data-export |

The limit of 31 storage buffers per stage still applies. Kernel 02 gains three bindings (55, 57, 58), the light paths' scatter stage four (55, 56, 57, 58) and stage 26 three (55, 59, 60), which leaves every kernel well under it.

### Queues and stages

| Kind | Number | Name | Plan |
|---|---|---|---|
| Queue | 10 | `CRT_QUEUE_DETECTOR` | detectors |
| Stage | 26 | deposit on detectors | detectors |

The realistic camera's trace through its lens runs inside kernel 00, which [pbrt-cameras.md](pbrt-cameras.md) already owns, so it needs no stage.

### Instances and path state

- An optic is a shape instance of kind `CRT_SHAPE_OPTIC`. It uses `GpuInstance`'s existing fields: `attributeBase` is its first record in `OpticSurfaces` and `triangleCount` its surface count, `materialBase` indexes `instanceMaterials[]` by surface as a mesh's does, and `lightBase` is `CRT_NO_LIGHT`. The struct does not grow, and its padding words stay the media's and motion blur's.
- `PathState` does not grow. The beam-axis state a light path needs (its graph node and port, and the guided mode's bookkeeping) lives in `OpticPathState`, allocated only while a beam axis requests a mode other than full, as [pbrt-participating-media.md](pbrt-participating-media.md) allocates `MediumPathState` only when a scene has a medium.

### Libraries and tests

| Library | GLSL it mirrors | Test | Plan |
|---|---|---|---|
| `rt_optics` | `crt_optic.glsl`, `crt_coating.glsl`, `crt_tmm.glsl`, `crt_birefringence.glsl` | `bin/optics_test` | optical-surfaces, optical-materials, polarization-optics, gratings-and-xuv |
| `rt_optics` (solver) | none: CPU only | `bin/beam_test` | gaussian-beam-solver |

### Scene file keys

The plans add, each in the payload version it lands in: `"capabilities"` and `"template"`, `"bench"` (table size, beam height, snapping), `"optics"`, `"beamAxes"`, and the optics' owned entries in `"materials"`. They do not pre-assign versions.

### Files and directories

- `src/scene_capabilities.{h,cpp}` for the capability registry, and `assets/templates/` for the templates.
- `src/optic_*.{h,cpp}` for the optics code and `src/beam_*.{h,cpp}` for the beam axis and solver, flat in `src/` as the rest of the codebase is.
- `shaders/rt/include/crt_optic.glsl`, `crt_coating.glsl`, `crt_detector.glsl`; `shaders/rt/2601_deposit_detector.comp`; `shaders/optics/beam_preview.vert` and `.frag` for the raster beams.
- `tools/optical_models/` for the FreeCAD conversion scripts, and `assets/optical_models/meshes/` for their committed output.
- `assets/glass/` and `assets/coatings/` for the bundled material data, and `assets/lenses/` for lens prescriptions.
- `assets/scenes/optics/` for the verification scenes.

## Notes on the request's source files

Reading `assets/optical_models/` against the request turned up three differences, which the plans follow:

- The 2 in kinematic mount is `Kinematic_mount-KM200.step` (KM200). The request's table names `Kinematic_mount-KM100.step` for both the 1 in and the 2 in optic.
- The post holder files are `Post_holder-PH50E_M.step` and `Post_holder_clamp-CF125C_M.STEP`, without the `ePost_` prefix the request uses.
- The STEP files mix units. The KM100, the three LMR and three RSP mounts, the ID8 to ID20 irises, the PH50E/M and the KM200PM/M are authored in inches; the rest in millimeters. The conversion pipeline must read each file's own unit, which FreeCAD does.

The three mirror blanks are distinct files: PF05-03 and PF10-03 are 6 mm thick with 0.2 mm and 0.25 mm chamfers, and PF20-03 is 12 mm thick.

## Not planned

- The wave backend, by the user's choice; see above.
- Virtual reality, tutorials and training missions, and multiplayer, at the user's request.
- The physics of nonlinear crystals and acousto-optic modulators. The graph's ports accept them, so a later plan can add their transformations without changing the graph.
- Resonator mode solving. The solver caps the number of round trips in a cavity, as femtoPro does, and reports that it did.
- Ultrashort pulse propagation (dispersion of a sampled temporal field, chirp). femtoPro's spectral-temporal envelopes are a natural later plan once spectral rendering and the wave backend exist.

## Sources

Brixner et al. 2023, *femtoPro: virtual-reality interactive training simulator of an ultrafast laser laboratory*, Applied Physics B 129, 78; Müller, Mueller, Brixner and von Mammen 2025, *A graph-based laser path solver algorithm for virtual reality laboratory simulations*, Simulation Modelling Practice and Theory, doi 10.1016/j.simpat.2025.103194; Brixner et al. 2025, *femtoPro: real-time linear and nonlinear optics simulations*, JOSA B 42(8), 1663; the femtoPro feature list at uni-wuerzburg.de/en/femtopro/features; Kogelnik and Li 1966, *Laser beams and resonators*; Siegman 1986, *Lasers*, chapters 15 to 21; Arnaud 1985, *Representation of Gaussian beams by complex rays*; the Zemax OpticStudio user manual (sequential and non-sequential ray tracing, physical optics propagation, file formats); Thorlabs product pages and drawings for every part the plans name.
