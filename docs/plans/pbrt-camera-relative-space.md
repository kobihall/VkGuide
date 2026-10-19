# Camera-relative rendering space

A scene far from the origin loses precision in every float the tracer computes, and when the coordinates themselves are floats it loses them before the first ray. PBR 4ed 5.1.1 shows a scene 1,000,000 units from the origin breaking up in world space and rendering cleanly in camera-world space, where the camera is at the origin and the axes keep the world's orientation. This plan renders in that space: the scene keeps its world coordinates in double precision, and every world-space value the GPU reads is made relative to a render origin first.

*Not implemented. Split off from [pbrt-robust-intersections.md](completed/pbrt-robust-intersections.md) on 2026-10-19, whose steps 1 to 4 are built. Nothing in the [laser optics plan](physoptics-laser-optics.md) or the [pbrt roadmap](pbrt-roadmap.md) depends on it. Part of the roadmap's geometry group.*

## When to build it

Build it when a scene you want to render has coordinates beyond about 10 km, or about 100 m with detail under a millimeter. The robust-intersection work measured the edge: a Cornell box 55 units wide moved 10,000 units from the origin renders the same as the unshifted box once error-bounded offsets are on, and moved 100,000 units it does not, with or without them. The likely sources are the pbrt-v4 scenes in `assets/models/` that [pbrt-scene-import.md](pbrt-scene-import.md) brings in, and terrain or architecture authored in absolute coordinates. The laser optics scenes sit within a meter of the origin and do not need it.

## What the renderer does today

- The scene model is float. `SceneShape::position`, `SceneCamera::position`, `SceneLight::position` and `SceneMeshObject::transform` are `glm::vec3` and `glm::mat4`, and `scene_io` writes them as floats. A scene moved 100,000 units has its positions rounded to 7.8e-3 when it is authored or loaded, so the damage is done before any render starts.
- `buildSceneAccel()` builds the TLAS, the instance records and the light list in world space from those floats. The BLASes are in object space.
- `ensureSceneAccel()` compares the scene's objects, shapes, lights and materials by value and rebuilds only when they differ, so a camera drag, which restarts the render every frame, rebuilds nothing.
- The camera reaches the GPU as `CrtParams.origin`, `lowerLeft`, `horizontal` and `vertical`, in world space.

## Design

### Double-precision source data

Subtracting a render origin from rounded positions fixes the arithmetic and leaves the geometry misaligned. So the world-space data in the scene model becomes double: shape, camera and light positions, and the translation of a mesh object's transform (its rotation and scale stay float, since a mesh object is its BLAS under a transform). `scene_io` writes these with the digits a double needs, and an older file reads as it does today. glTF import takes node translations as double where fastgltf provides them.

The first step of the work is an experiment that separates the two causes. It builds the shifted Cornell box with positions kept in double and the translation applied on the CPU, and measures how much of the 100,000-unit error comes from stored positions and how much from the arithmetic. If almost all of it is storage, the plan reduces to the load-time recentering described under "Open questions".

### The render origin

A `RenderOrigin` holds a `glm::dvec3` offset and converts world values to render space: `toRender(dvec3) -> vec3`, and `toRender(dmat4) -> mat4` for transforms. The offset is the camera position when a render starts. Every upload of world-space data goes through it, and with the setting off the offset is zero and the output is bit-identical to today.

`buildSceneAccel()` takes the origin and applies it before rounding to float:

- Each instance's `objectToWorld` and `worldToObject` are formed in double, translated, then rounded. The inverse is computed in double, which also tightens the round trip that `toWorldPoint()` measures.
- The TLAS is built from the translated instance boxes. The BLASes do not change.
- The light list's positions, rectangle corners and emissive triangle vertices are translated: punctual lights, `addShape()` and `addTriangle()` all take render-space values. Directional lights and the environment map have only directions.
- `GpuRenderer::baseParams()` computes the camera in render space, where `origin` is zero.

The shaders do not change. They see a smaller world, and every `pError` is proportional to the coordinates, so the error bounds shrink with it.

### When the origin moves

`ensureSceneAccel()` gains the origin as part of its key. The origin is kept until the camera is farther from it than a threshold, then re-chosen, so a drag rebuilds the TLAS, instances and lights once per threshold crossing and not every frame. The threshold is a power of two sized so the camera's distance from the origin stays well inside the precision the scene needs: 1,024 units to start. A render that starts at one pose is exact at that pose and degrades gently as the camera moves away, until the threshold restarts it.

### Settings

**Raytrace Render** → **Intersection precision** gains **Camera-relative space**, off by default. It sits in `RenderSettings`, in the render key, and in the scene file in the payload version that lands with it. Off, nothing in this plan runs.

## What later plans must do

Every plan that puts a world-space position in a GPU buffer routes it through `RenderOrigin`. A value it forgets is wrong only in a scene far from the origin, which nobody will be looking at when the plan lands, so each plan's verification section names the one place it converts. The ones that exist today:

- [pbrt-participating-media.md](pbrt-participating-media.md): grid and medium transforms.
- [pbrt-motion-blur.md](pbrt-motion-blur.md): the two poses per object and camera.
- [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md), [physoptics-detectors.md](physoptics-detectors.md) and [physoptics-beam-axis.md](physoptics-beam-axis.md): surface frames, detector frames and the beam graph's world positions.
- [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md): the light vertex cache holds render-space positions, which is consistent because everything GPU-side shares one origin.
- [physoptics-data-export.md](physoptics-data-export.md): ray records and detector data leave in world coordinates, so the exporter adds the origin back.

The CPU solver in [physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md) already runs in double and works in world space.

## Verification

- With the setting off, the eight renders of the robust-intersection checks (Cornell under three traversals, the sphere scene, the Veach scene, Sponza under two layouts) are bit-identical to before.
- A copy of the Cornell box moved to (1e6, 1e6, 1e6), with positions in double, matches the original within noise with the setting on, and does not with it off.
- A camera that moves 10,000 units across a Sponza render restarts it once per threshold crossing, and the image after each restart matches a render made from a fresh origin.
- `bvh_bench` checks the double-precision transform path: instances formed in double and rounded match the same instances formed in float to within the float's own rounding.
- Zero validation messages.

## Order of work

1. The experiment above, to separate stored positions from arithmetic.
2. Double positions in the scene model and `scene_io`, with older files reading unchanged.
3. `RenderOrigin`, and `buildSceneAccel()` and `baseParams()` taking it.
4. The re-choice threshold in `ensureSceneAccel()`, the setting, the panel checkbox and the file version.
5. Verification.

## Open questions

- Scene storage or per-render origin. The cheaper alternative is to recenter once when a scene loads: read positions in double, subtract a scene origin chosen then, and keep the scene model in float relative to it. It covers a static scene with no per-render machinery and no obligation on later plans, and it does not follow a camera that travels far across a large scene. If the experiment shows storage is nearly all of the error, this may be enough on its own.
- The threshold. 1,024 units is a guess. It depends on how far a camera may drift before precision matters for the scene being rendered.
- Whether the editor and gizmo should display world coordinates, which would then need doubles in the UI, or render-space ones.

## Not planned

- Double-precision BLASes. A mesh's vertices stay float in object space, and a mesh far from its own origin loses precision in the glTF file itself.
- Double precision on the GPU. The Radeon Pro 560X has no `shaderFloat64`.

## Sources

PBR 4ed §5.1.1 (rendering space: world space, camera space and camera-world space) and §6.8.5 (transformations with error bounds); [pbrt-robust-intersections.md](completed/pbrt-robust-intersections.md), whose verification section records the 10,000 and 100,000 unit measurements.
