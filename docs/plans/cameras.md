# Camera models

Every render uses Ray Tracing in One Weekend's thin-lens perspective camera: a pinhole when the aperture is zero, a uniformly sampled disk otherwise. pbrt adds orthographic and spherical projections, a realistic camera that traces rays through the lens elements of real lens prescriptions, and the camera measurement equation that gives a lens its vignetting. Its exercises add aperture shapes for bokeh and tilt-shift film planes. This plan adds all of them as camera data, and absorbs the one useful idea of the deleted `depth-of-field.md`, focusing by clicking.

*Not implemented. Depends on [sample-generation.md](sample-generation.md). Part of the [roadmap](roadmap.md).*

## Decision already made

On 2026-09-26 the user chose to keep the projection as `SceneCamera` data, evaluated by a switch in kernel 00 that is uniform across the dispatch, over kernel 00 variants per camera type. Kernel 00 variants stay reserved for how pixels are sampled ([film-and-reconstruction.md](film-and-reconstruction.md)).

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
| Realistic | lens prescription file, aperture diameter, focus distance, film diagonal | PBR 3ed §6.4, pbrt-v4 `RealisticCamera` |

`crt_camera.glsl` holds `generateCameraRay(CameraParams, filmSample, lensSample, time)`, with a `switch` on the kind; the camera's parameters move from push constants into a small uniform block (or stay in push constants, which have 4 KB on MoltenVK). Both kernel 00 variants call it.

### Spherical cameras

An equirectangular render of the scene can be loaded back as its environment map, and an equal-area one can be used directly by the equal-area environment lights of [environment-lighting.md](environment-lighting.md). The raster viewport's frustum preview draws a sphere marker for this kind instead of a frustum.

### Realistic lens systems

pbrt's realistic camera traces each camera ray backwards through a stack of spherical lens elements and an aperture stop, read from a prescription (radius, thickness, IOR, aperture diameter per element, the format of pbrt's `lenses/*.dat`):

- Focusing moves the lens stack along the axis; pbrt computes the position with the thick-lens approximation and then refines it (PBR 3ed §6.4.3 and §6.4.4).
- For efficiency it precomputes the exit pupil's bounds over 64 radial positions on the film, and samples the rear element only inside them (§6.4.5).
- Each ray gets a weight of `cos⁴ θ / z²` times the exit pupil's area, the camera measurement equation (PBR 4ed §5.4.1), which produces natural vignetting. The weight goes to `PixelState.cameraWeight` ([film-and-reconstruction.md](film-and-reconstruction.md)).
- In spectral modes the IOR of each element can be dispersive, which gives chromatic aberration for free ([spectral-rendering.md](spectral-rendering.md)).

The prescriptions pbrt ships (wide-angle, fisheye, double Gauss, telephoto) go into `assets/lenses/` with their licence noted.

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

With [motion-blur.md](motion-blur.md), every camera kind takes a time sample and interpolates its pose between the shutter's open and close poses. A rolling shutter (PBR 4ed exercise 5.1) offsets each row's time by its position in the image; it is a camera parameter of that plan, not this one.

### Camera importance

Light tracing and bidirectional path tracing need the camera's response to a ray arriving at it: `We()`, `Pdf_We()` and `Sample_Wi()`. [bidirectional-path-tracing.md](bidirectional-path-tracing.md) adds them to `crt_camera.glsl` for the thin-lens perspective camera only, as pbrt does, and those integrators are unavailable with the other kinds.

## Verification

- The perspective camera with a circular aperture and the concentric mapping converges to the same image as today; with the old polar mapping, retained for comparison, it renders bit-identical.
- An orthographic render of the Cornell box shows parallel walls parallel.
- An equirectangular render of a scene, loaded as that scene's environment map, lights a mirror sphere at the camera's position identically to the scene itself.
- A realistic camera with the wide-angle prescription shows barrel distortion and corner vignetting, and focuses at the requested distance to within 1 %.

## Order of work

1. `crt_camera.glsl`, the concentric disk mapping, and `SceneCamera.kind` with perspective only.
2. Orthographic and spherical.
3. Aperture shapes.
4. Focus by clicking.
5. Tilt and shift.
6. The realistic camera.

## Sources

PBR 4ed chapter 5 (§5.1 camera interface and coordinate spaces, §5.2 projective cameras and the thin lens, §5.3 the spherical camera, §5.4.1 the camera measurement equation, exercises 5.1, 5.5), §A.5.1 (concentric disk sampling); PBR 3ed §6.3 (environment camera), §6.4 (realistic cameras: lens systems, thick lens, focusing, exit pupil), exercises 6.3 and 6.4 (aperture shapes, bokeh); Kolb, Mitchell and Hanrahan 1995; Kensler 2021 (tilt-shift); pbrt-v4 `src/pbrt/cameras.h`; Shirley, *Ray Tracing in One Weekend*, "Defocus Blur".
