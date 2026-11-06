# Camera models

Every render uses Ray Tracing in One Weekend's thin-lens perspective camera: a pinhole when the aperture is zero, a uniformly sampled disk otherwise. pbrt adds orthographic and spherical projections, a realistic camera that traces rays through the lens elements of real lens prescriptions, and the camera measurement equation that gives a lens its vignetting. Its exercises add aperture shapes for bokeh and tilt-shift film planes. This plan adds all of them as camera data, and absorbs the one useful idea of the deleted `depth-of-field.md`, focusing by clicking.

*Implemented 2026-11-01, steps 1 to 5, before [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md), which needs it. The realistic camera, step 6, is not built: it depends on [physoptics-beam-axis.md](../physoptics-beam-axis.md) (assemblies) and [physoptics-prescription-import.md](../physoptics-prescription-import.md), and is item 28 of the [laser optics plan](../physoptics-laser-optics.md)'s roadmap. "As built" records what differs from the design below, and "Not built" what is left. Depends on [pbrt-sample-generation.md](pbrt-sample-generation.md). Part of the [roadmap](../pbrt-roadmap.md).*

## Decision already made

On 2026-09-26 the user chose to keep the projection as `SceneCamera` data, evaluated by a switch in kernel 00 that is uniform across the dispatch, over kernel 00 variants per camera type. Kernel 00 variants are how pixels are sampled: `thin_lens` and `filter_importance` ([pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md)).

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

An equirectangular render of the scene can be loaded back as its environment map, and an equal-area one can be used directly by the equal-area environment lights of [pbrt-environment-lighting.md](../pbrt-environment-lighting.md). The raster viewport's frustum preview draws a sphere marker for this kind instead of a frustum.

### Realistic lens systems

pbrt's realistic camera traces each camera ray backwards through a stack of spherical lens elements and an aperture stop, read from a prescription (radius, thickness, IOR, aperture diameter per element, the format of pbrt's `lenses/*.dat`). On 2026-10-18 the user asked that the lens be the laser optics plans' own representation instead of a separate table. A realistic camera therefore names a lens **assembly** ([physoptics-beam-axis.md](../physoptics-beam-axis.md)): a rigid group of optical bodies of the surface-list kind ([physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)) with an aperture stop as an iris element. An assembly comes from [physoptics-prescription-import.md](../physoptics-prescription-import.md), which reads pbrt's `.dat` files and OpticStudio's `.zmx` files into it, or is built by hand from catalog lenses on the bench. The camera's film is the assembly's image plane, and the camera's pose is the assembly's datum, so a camera can stand on the breadboard behind the optics it looks through. Kernel 00 traces each camera ray sequentially through the assembly's bodies in reverse order, with `testOpticBody()` from `crt_optic.glsl` and the optical interface material's refraction ([physoptics-optical-materials.md](physoptics-optical-materials.md)), and never walks the scene's BVH inside the lens: a ray that misses an aperture is vignetted, as in pbrt. The scene sees the lens's front element as ordinary geometry, so a camera on the bench also casts its lens's shadow and reflections. As pbrt does:

- Focusing moves the lens stack along the axis; pbrt computes the position with the thick-lens approximation and then refines it (PBR 3ed §6.4.3 and §6.4.4).
- For efficiency it precomputes the exit pupil's bounds over 64 radial positions on the film, and samples the rear element only inside them (§6.4.5).
- Each ray gets a weight of `cos⁴ θ / z²` times the exit pupil's area, the camera measurement equation (PBR 4ed §5.4.1), which produces natural vignetting. The weight starts as the path's throughput, as pbrt multiplies `cameraRay.weight` into the radiance before the film sees it; the film's weight stays the filter's sign ([pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md)).
- In spectral modes each element's glass disperses by its catalog formula, which gives chromatic aberration for free ([pbrt-spectral-rendering.md](pbrt-spectral-rendering.md), [physoptics-optical-materials.md](physoptics-optical-materials.md)). In exact RGB every glass is read at the design wavelength.
- Coatings on the lens surfaces apply, so an uncoated lens shows its ghosts when the light integrator renders through it.

The prescriptions pbrt ships (wide-angle, fisheye, double Gauss, telephoto) are in `assets/models/pbrt-v4-scenes/lenses/`; [physoptics-prescription-import.md](../physoptics-prescription-import.md) imports them, and copies the ones the scenes use into `assets/lenses/` with their license noted.

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

With [pbrt-motion-blur.md](../pbrt-motion-blur.md), every camera kind takes a time sample and interpolates its pose between the shutter's open and close poses. A rolling shutter (PBR 4ed exercise 5.1) offsets each row's time by its position in the image; it is a camera parameter of that plan, not this one.

### Camera importance

Light tracing and bidirectional path tracing need the camera's response to a ray arriving at it: `We()`, `Pdf_We()` and `Sample_Wi()`. [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md) adds them to `crt_camera.glsl` for the thin-lens perspective camera only, as pbrt does, and those integrators are unavailable with the other kinds.

## Importing pbrt scenes

[completed/pbrt-scene-import.md](pbrt-scene-import.md) imports pbrt-v4 scenes today and substitutes for this plan's features. Each substitution is a line in the import report, so `./bin/pbrt_test --report <file.pbrt>` lists the scenes that need this plan. When this plan lands, change the importer as follows, then run `tools/verify/pbrt_compare.py` on the scenes named below.

- **Perspective cameras** import with their pose, `fov` (pbrt's spans the shorter image axis; `camera()` in `src/pbrt_import.cpp` converts it to a vertical field of view), `frameaspectratio`, `lensradius` (as the aperture's diameter, `2 * lensradius`) and `focaldistance`. `bin/pbrt_test` checks that a point lands at the same place on the screen in both renderers, with and without a mirroring `Scale -1 1 1` in front of `LookAt`.
- **Other cameras** (`orthographic`, `spherical`, `realistic` in `sanmiguel-realistic-courtyard`) are perspective cameras with the same pose, and `screenwindow` is ignored. Map each to this plan's `SceneCamera` kinds, and `realistic`'s `lensfile`, `aperturediameter` and `focusdistance` to its lens assembly.

## Verification

- The perspective camera with a circular aperture and the concentric mapping converges to the same image as today; with the old polar mapping, retained for comparison, it renders bit-identical.
- An orthographic render of the Cornell box shows parallel walls parallel.
- An equirectangular render of a scene, loaded as that scene's environment map, lights a mirror sphere at the camera's position identically to the scene itself.
- A realistic camera with the wide-angle prescription shows barrel distortion and corner vignetting, and focuses at the requested distance to within 1 %.
- The double Gauss imported from `dgauss.50mm.dat` renders, through its assembly, the same image as pbrt-v4's realistic camera with the same file, through [pbrt-scene-import.md](pbrt-scene-import.md)'s comparison workflow, and its real ray intercepts match OpticStudio's for the same lens ([physoptics-verification-scenes.md](../physoptics-verification-scenes.md), `camera_35mm`).

## Order of work

1. `crt_camera.glsl`, the concentric disk mapping, and `SceneCamera.kind` with perspective only.
2. Orthographic and spherical.
3. Aperture shapes.
4. Focus by clicking.
5. Tilt and shift.
6. The realistic camera, after [physoptics-prescription-import.md](../physoptics-prescription-import.md): assemblies as lenses, sequential tracing in kernel 00, focusing, the exit pupil table and the camera weight.

## As built

On 2026-11-01 the user chose to build this plan before [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md), rather than write that plan's camera importance against Ray Tracing in One Weekend's camera alone. Steps 1 to 5 are built. These differ from the design above, or add to it:

- **Where the code is.** `src/camera_model.h` and `camera_model.cpp`, the static library `rt_camera`, hold the kinds, `CAMERA_TRAITS`, `RTCameraSnapshot` (moved out of `rt_scene_types.h`), `CameraFrame` (what the shaders read) and the C++ mirror of `generateCameraRay()`. `shaders/rt/include/crt_camera.glsl` is the camera model, and kernel 00's body, which that file used to hold, is `crt_camera_rays.glsl`. The warps the lights and the cameras share (the concentric disk, the uniform triangle, the equal-area square) moved from `crt_light.glsl` to `crt_warp.glsl`, and `equirectDirectionOf()` to `shaders/equirect.glsl`.
- **The camera stays in the push constants.** `CrtParams` grows from 240 to 320 bytes: camera z with the focus distance, the tilted plane of focus, the kind, the flags, the polygon's blades, where the aperture image's table starts, the aperture's rotation, the lens's and the film's areas, and an orthographic camera's pixel height.
- **`CRT_CAMERA_MODEL`**, specialization constant 7, is on only for a camera that is more than Ray Tracing in One Weekend's: another kind, a tilt, or a lens with another shape or the concentric mapping. Kernel 00 variants set `KernelVariant::readsCamera` to be built with it. Without it, a lens's rays moved by an ulp under Metal's fast math once the camera became a function with branches. With it, two scenes given a lens render bit-identical.
- **The orthographic camera** takes the view volume's height, not its width, as the perspective camera takes a vertical field of view, so a change of the render's aspect keeps the framing. Its lens is centered on each film point's ray, parallel to the axis. pbrt-v4's `OrthographicCamera` starts every lens ray on a disk at the camera's origin instead, whatever the film point, which makes the image out of focus converge like a perspective camera's. The importer reports a pbrt orthographic camera with a lens.
- **The spherical camera's equirectangular image** is the environment map's layout (`equirect.glsl`): its center looks down the camera's -z, and u turns right. A render can be loaded back as the map. pbrt's equirectangular layout runs the other way round, and the importer says so. Its equal-area image is pbrt's, pole on camera y.
- **The aperture image** is read as a projection light's image, through the texture library (`VulkanEngine::syncSceneImages()`), and its luminance is the aperture's transmission over the square the aperture's circle fits in. Its piecewise-constant table is binding 65, `ImageDistributions` (`crt_image_table.glsl`, `packPiecewiseConstant2D()`), a stub while no camera has one; [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md) added the light images' tables after it. A polygon's corners lie on the aperture's circle.
- **Every camera ray carries weight 1**, as pbrt's thin lens does, so an aperture's shape changes the bokeh and not the brightness. Nothing about the camera reaches the path's throughput.
- **The lens mapping** is concentric for a new camera and polar for a camera in a scene file before version 26, as the sampler's default changed (**concentric lens mapping** in the camera panel, `"lensMapping"` in the file).
- **The tilt** is the plane of focus's: two angles about the camera's x and y axes, up to 80° either way, pivoting at the point on the axis at the focus distance. The plan left open whether a tilt names the lens or the plane; the plane is what a user is after. **The shift** is in image heights on both axes, and a perspective camera's chief ray and field of view are measured from the shifted film.
- **Focus on...**, in the camera panel, waits for a click in the viewport, traces the ray under the cursor through the scene on the CPU (`RaytraceRenderer::pickDistance()`, `traceScene()` over `RaytraceSceneAccel::sceneInstances`) and sets the focus distance that puts the hit point on the plane of focus, tilted or not. The film's frame on the plane of focus shows for 1.5 seconds as a translucent quad drawn over the viewport, not in the raster pass. Escape cancels.
- **The viewport's outlines** follow the kind: a box as large as an orthographic camera's view volume, three rings around a spherical camera with a tick down its axis, and a perspective frame moved by the shift.
- **The scene file** is payload version 26: a camera's `"kind"`, `"viewHeight"`, `"mapping"`, `"lensMapping"`, `"apertureShape"`, `"apertureBlades"`, `"apertureRotation"`, `"apertureImage"`, `"tilt"` and `"shift"`.
- **The importer** maps pbrt's `orthographic` (its screen window's height is the view height) and `spherical` cameras, and every camera's `screenwindow`: its center becomes the shift and its height the field of view. A window of another shape than the film is reported, since the film keeps the render's aspect.
- **An EXR export's** `NDCFromWorld` is the shifted frustum or the orthographic box, and a spherical camera writes none, as pbrt does.
- First-person edit flies an orthographic or spherical camera's pose, and the viewport still shows its own perspective view.

Verification:

- Nine scenes with a pinhole and two given a lens of 0.15 and 0.3 render bit-identical to the build before (`cornell_box`, `veach_mis`, `all_materials`, `caustic_glass`, `thin_glass`, `sphere_scene`, `lt_projection`, `tx_checker_plane`, `white_furnace`).
- `bin/camera_test` checks Ray Tracing in One Weekend's camera bit for bit, each aperture's sampling by chi-square against its density (the circle by both mappings, polygons of 3 to 9 blades, a ring image), that a film point's rays meet on the plane of focus for every kind with and without a tilt, the orthographic view volume, the shift, and both spherical layouts. `--selfcheck` plants a polygon sampler that favors one triangle and a focus point a millimeter off, and catches both.
- **Windows** → **GPU self-tests** → **Camera parity** compares `crt_camera.glsl` with the mirror for nine cameras over 8192 rays each: the worst difference is 3.6e-7 of the vector's length.
- `tools/verify/camera_scenes.py` writes seven scenes to `assets/scenes/` (`cam_*.gltf`) and `assets/textures/aperture_star.png`, and `tools/verify/camera_check.py` checks them. The orthographic box shows 0.001% of its pixels on a side wall, against 20% in perspective, and its back wall is the 160 pixels wide the view volume makes it. The concentric mapping's image differs from the polar one's by 0.87 of the noise between two seeds, the ratio noise alone gives. Through a hexagon and a star the out-of-focus emitter keeps 1.0025 and 0.9946 of the circle's energy, and the hexagon is 1.125 wider across its corners than its flats. The tilted lens keeps the near and far ground as sharp as a pinhole, where untilted it blurs the far ground to 0.15. An equirectangular render of a room, loaded as the environment map, lights a mirror sphere to 1.0018 of the room's own mean, and to 0.0023 per pixel after tonemapping.
- `bin/pbrt_test --scan` imports all 98 scene files of pbrt-v4-scenes.
- `tools/verify/pbrt_camera_scenes.py` writes pbrt-v4 scenes of an orthographic camera with an off-center window, an equal-area spherical camera, a shifted perspective camera and a thin lens to `assets/models/pbrt_cameras/`. `pbrt_compare.py` puts each at its noise floor: relative MSE 2.48e-5 against a floor of 2.45e-5, 1.86e-5 against 1.83e-5, 2.44e-5 against 2.49e-5, and 2.92e-5 against 2.96e-5.

## Not built

- **The realistic camera**, step 6, waits for [physoptics-beam-axis.md](../physoptics-beam-axis.md) and [physoptics-prescription-import.md](../physoptics-prescription-import.md). A pbrt `realistic` camera still imports as a perspective camera with its pose, and the report says so.
- **The realistic camera's shape, as the user set it on 2026-11-04.** It is not a new kind of scene camera. It is a **camera sensor** detector ([physoptics-detectors.md](physoptics-detectors.md)) standing behind a beam axis of lens elements, and the sensor is what the render targets. The sensor already renders two ways. Light tracing deposits on it, through whatever optics stand in front, so a laser imaged by a lens already lands on it. Camera paths start at its pixels (`CameraKind::Sensor`, `generateCameraRay()` in `src/camera_model.cpp`), today cosine-weighted over the front hemisphere, a bare irradiance sensor. The realistic camera replaces that hemisphere with pbrt's exit pupil: a direction toward the rear element's bounds, traced sequentially through the assembly in kernel 00 as "Realistic lens systems" above describes, with the camera weight of PBR 3ed §6.4.7. The sensor's size and pixels are the film; its physical-sensor section is a real camera's. Nothing else the plan describes changes.
- A color aperture image: the aperture is the image's luminance.
- pbrt's equirectangular layout, and pbrt's orthographic lens.

## Sources

PBR 4ed chapter 5 (§5.1 camera interface and coordinate spaces, §5.2 projective cameras and the thin lens, §5.3 the spherical camera, §5.4.1 the camera measurement equation, exercises 5.1, 5.5), §A.5.1 (concentric disk sampling); PBR 3ed §6.3 (environment camera), §6.4 (realistic cameras: lens systems, thick lens, focusing, exit pupil), exercises 6.3 and 6.4 (aperture shapes, bokeh); Kolb, Mitchell and Hanrahan 1995; Kensler 2021 (tilt-shift); pbrt-v4 `src/pbrt/cameras.h`; Shirley, *Ray Tracing in One Weekend*, "Defocus Blur".
