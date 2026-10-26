# Camera models

Every render uses Ray Tracing in One Weekend's thin-lens perspective camera: a pinhole when the aperture is zero, a uniformly sampled disk otherwise. pbrt adds orthographic and spherical projections, a realistic camera that traces rays through the lens elements of real lens prescriptions, and the camera measurement equation that gives a lens its vignetting. Its exercises add aperture shapes for bokeh and tilt-shift film planes. This plan adds all of them as camera data, and absorbs the one useful idea of the deleted `depth-of-field.md`, focusing by clicking.

*Not implemented. Depends on [pbrt-sample-generation.md](completed/pbrt-sample-generation.md). The realistic camera also depends on [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md), [physoptics-beam-axis.md](physoptics-beam-axis.md) (assemblies) and [physoptics-prescription-import.md](physoptics-prescription-import.md), and is item 28 of the [laser optics plan](physoptics-laser-optics.md)'s roadmap. Part of the [roadmap](pbrt-roadmap.md).*

## Decision already made

On 2026-09-26 the user chose to keep the projection as `SceneCamera` data, evaluated by a switch in kernel 00 that is uniform across the dispatch, over kernel 00 variants per camera type. Kernel 00 variants are how pixels are sampled: `thin_lens` and `filter_importance` ([pbrt-film-and-reconstruction.md](completed/pbrt-film-and-reconstruction.md)).

## What the renderer does today

- `SceneCamera` (`src/rt_scene_types.h`) holds a pose, a vertical field of view, an aperture, a focus distance and an exposure. `cameraSnapshot()` turns it into `RTCameraSnapshot` (look-from, look-at, up), and `GpuPathTracer` precomputes the image plane's lower-left corner and edge vectors into the push constants.
- Kernel 00 samples the lens with `randomInDisk()`, a polar mapping, where pbrt uses the concentric mapping that keeps stratified samples stratified (PBR 4ed §A.5.1).

## Design

### Camera kinds

`SceneCamera` gains `kind` and the parameters each kind uses. `CAMERA_TRAITS`, like `SHAPE_TRAITS`, lists them for the editor and the scene file.

| Kind | Parameters | Source |
|---|---|---|
| Perspective (thin lens) | field of view, aperture, focus distance, aperture shape | today's, PBR 4ed §5.2.2 and §5.2.3 |
| Orthographic | the width of the view volume, aperture, focus distance | PBR 4ed §5.2.1 |
| Spherical | mapping: equirectangular or equal-area | PBR 4ed §5.3 |
| Realistic | a lens assembly (an optical assembly in the scene), aperture stop diameter, focus distance, film size | PBR 3ed §6.4, pbrt-v4 `RealisticCamera` |

`crt_camera.glsl` holds `generateCameraRay(CameraParams, filmSample, lensSample, time)`, with a `switch` on the kind; the camera's parameters move from push constants into a small uniform block (or stay in push constants, which have 4 KB on MoltenVK). Both kernel 00 variants call it.

### Spherical cameras

An equirectangular render of the scene can be loaded back as its environment map, and an equal-area one can be used directly by the equal-area environment lights of [pbrt-environment-lighting.md](pbrt-environment-lighting.md). The raster viewport's frustum preview draws a sphere marker for this kind instead of a frustum.

### Realistic lens systems

pbrt's realistic camera traces each camera ray backwards through a stack of spherical lens elements and an aperture stop, read from a prescription (radius, thickness, IOR, aperture diameter per element, the format of pbrt's `lenses/*.dat`). On 2026-10-18 the user asked that the lens be the laser optics plans' own representation instead of a separate table. A realistic camera therefore names a lens **assembly** ([physoptics-beam-axis.md](physoptics-beam-axis.md)): a rigid group of optical bodies of the surface-list kind ([physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)) with an aperture stop as an iris element. An assembly comes from [physoptics-prescription-import.md](physoptics-prescription-import.md), which reads pbrt's `.dat` files and OpticStudio's `.zmx` files into it, or is built by hand from catalog lenses on the bench. The camera's film is the assembly's image plane, and the camera's pose is the assembly's datum, so a camera can stand on the breadboard behind the optics it looks through. Kernel 00 traces each camera ray sequentially through the assembly's bodies in reverse order, with `testOpticBody()` from `crt_optic.glsl` and the optical interface material's refraction ([physoptics-optical-materials.md](physoptics-optical-materials.md)), and never walks the scene's BVH inside the lens: a ray that misses an aperture is vignetted, as in pbrt. The scene sees the lens's front element as ordinary geometry, so a camera on the bench also casts its lens's shadow and reflections. As pbrt does:

- Focusing moves the lens stack along the axis; pbrt computes the position with the thick-lens approximation and then refines it (PBR 3ed §6.4.3 and §6.4.4).
- For efficiency it precomputes the exit pupil's bounds over 64 radial positions on the film, and samples the rear element only inside them (§6.4.5).
- Each ray gets a weight of `cos⁴ θ / z²` times the exit pupil's area, the camera measurement equation (PBR 4ed §5.4.1), which produces natural vignetting. The weight starts as the path's throughput, as pbrt multiplies `cameraRay.weight` into the radiance before the film sees it; the film's weight stays the filter's sign ([pbrt-film-and-reconstruction.md](completed/pbrt-film-and-reconstruction.md)).
- In spectral modes each element's glass disperses by its catalog formula, which gives chromatic aberration for free ([pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md), [physoptics-optical-materials.md](physoptics-optical-materials.md)). In exact RGB every glass is read at the design wavelength.
- Coatings on the lens surfaces apply, so an uncoated lens shows its ghosts when the light integrator renders through it.

The prescriptions pbrt ships (wide-angle, fisheye, double Gauss, telephoto) are in `assets/models/pbrt-v4-scenes/lenses/`; [physoptics-prescription-import.md](physoptics-prescription-import.md) imports them, and copies the ones the scenes use into `assets/lenses/` with their license noted.

### Aperture shapes

The thin lens and the realistic camera's aperture stop sample a shape rather than a disk:

- Circle (default, today's result with the concentric mapping).
- Regular polygon with n blades and a rotation, sampled by picking a triangle of the polygon and a point in it.
- An image, sampled with a piecewise-constant 2D distribution over its luminance: stars, hearts, the doughnut of a catadioptric lens (PBR 3ed exercise 6.4 and pbrt-v4's aperture images).

Out-of-focus highlights then take the aperture's shape (PBR 4ed Figure 5.10 and pbrt's `watercolor-aperture-*` images).

### Tilt and shift

The film plane of the perspective camera can be shifted off the optical axis and tilted about it (PBR 4ed exercise 5.5, after Kensler 2021). A tilt makes the plane of focus oblique (the Scheimpflug principle), which lets a landscape stay sharp from near to far at a wide aperture; a shift keeps a tall building's verticals parallel.

### Focusing by clicking

From `depth-of-field.md`: a **Focus on** button, then a click in the raster viewport, traces one ray on the CPU (`bvh_bench`'s CPU traversal already exists for exactly this kind of query) and sets the focus distance to its hit distance. The raster preview draws the focal plane briefly as a translucent quad.

### Time

With [pbrt-motion-blur.md](pbrt-motion-blur.md), every camera kind takes a time sample and interpolates its pose between the shutter's open and close poses. A rolling shutter (PBR 4ed exercise 5.1) offsets each row's time by its position in the image; it is a camera parameter of that plan, not this one.

### Camera importance

Light tracing and bidirectional path tracing need the camera's response to a ray arriving at it: `We()`, `Pdf_We()` and `Sample_Wi()`. [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md) adds them to `crt_camera.glsl` for the thin-lens perspective camera only, as pbrt does, and those integrators are unavailable with the other kinds.

## Importing pbrt scenes

[completed/pbrt-scene-import.md](completed/pbrt-scene-import.md) imports pbrt-v4 scenes today and substitutes for this plan's features. Each substitution is a line in the import report, so `./bin/pbrt_test --report <file.pbrt>` lists the scenes that need this plan. When this plan lands, change the importer as follows, then run `tools/verify/pbrt_compare.py` on the scenes named below.

- **Perspective cameras** import with their pose, `fov` (pbrt's spans the shorter image axis; `camera()` in `src/pbrt_import.cpp` converts it to a vertical field of view), `frameaspectratio`, `lensradius` (as the aperture's diameter, `2 * lensradius`) and `focaldistance`. `bin/pbrt_test` checks that a point lands at the same place on the screen in both renderers, with and without a mirroring `Scale -1 1 1` in front of `LookAt`.
- **Other cameras** (`orthographic`, `spherical`, `realistic` in `sanmiguel-realistic-courtyard`) are perspective cameras with the same pose, and `screenwindow` is ignored. Map each to this plan's `SceneCamera` kinds, and `realistic`'s `lensfile`, `aperturediameter` and `focusdistance` to its lens assembly.

## Verification

- The perspective camera with a circular aperture and the concentric mapping converges to the same image as today; with the old polar mapping, retained for comparison, it renders bit-identical.
- An orthographic render of the Cornell box shows parallel walls parallel.
- An equirectangular render of a scene, loaded as that scene's environment map, lights a mirror sphere at the camera's position identically to the scene itself.
- A realistic camera with the wide-angle prescription shows barrel distortion and corner vignetting, and focuses at the requested distance to within 1 %.
- The double Gauss imported from `dgauss.50mm.dat` renders, through its assembly, the same image as pbrt-v4's realistic camera with the same file, through [pbrt-scene-import.md](completed/pbrt-scene-import.md)'s comparison workflow, and its real ray intercepts match OpticStudio's for the same lens ([physoptics-verification-scenes.md](physoptics-verification-scenes.md), `camera_35mm`).

## Order of work

1. `crt_camera.glsl`, the concentric disk mapping, and `SceneCamera.kind` with perspective only.
2. Orthographic and spherical.
3. Aperture shapes.
4. Focus by clicking.
5. Tilt and shift.
6. The realistic camera, after [physoptics-prescription-import.md](physoptics-prescription-import.md): assemblies as lenses, sequential tracing in kernel 00, focusing, the exit pupil table and the camera weight.

## Sources

PBR 4ed chapter 5 (§5.1 camera interface and coordinate spaces, §5.2 projective cameras and the thin lens, §5.3 the spherical camera, §5.4.1 the camera measurement equation, exercises 5.1, 5.5), §A.5.1 (concentric disk sampling); PBR 3ed §6.3 (environment camera), §6.4 (realistic cameras: lens systems, thick lens, focusing, exit pupil), exercises 6.3 and 6.4 (aperture shapes, bokeh); Kolb, Mitchell and Hanrahan 1995; Kensler 2021 (tilt-shift); pbrt-v4 `src/pbrt/cameras.h`; Shirley, *Ray Tracing in One Weekend*, "Defocus Blur".
