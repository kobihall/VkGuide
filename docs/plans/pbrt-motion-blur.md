# Motion blur

A real camera's shutter stays open for a while, and anything that moves during it smears. The tracer renders one instant. pbrt gives each camera ray a time between the shutter's opening and closing, and lets primitives carry an `AnimatedTransform` that interpolates between two transforms (PBR 3ed §2.9; PBR 4ed §5.1 and §7.1.2). *The Next Week* opens with the same idea, a sphere moving between two centers. The user chose two poses per object and camera, at shutter open and close, and no animation timeline. This plan adds shutter times, per-ray time, two-pose instances and cameras, and motion bounds in the TLAS.

*Not implemented. Depends on [pbrt-sample-generation.md](completed/pbrt-sample-generation.md) (the time dimension) and [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) (object-to-world rows in `GpuInstance`). Part of the [roadmap](pbrt-roadmap.md).*

## Decision already made

On 2026-09-26 the user chose two poses per object and camera, at shutter open and close, over a keyframe timeline or deferring motion.

## What the renderer does today

- Rays have no time. `SceneCamera`, `SceneShape`, mesh objects and `SceneLight` each have one pose.
- `GpuInstance` holds one world-to-object transform, which `instanceRay()` in `crt_traverse.glsl` applies for every traversal strategy.
- The TLAS is built over each instance's world bounding box.

## Design

### Shutter and time

- `SceneCamera` gains **Shutter open** and **Shutter close**, 0 and 1 by default, as pbrt's cameras have. When they are equal, nothing moves and no time sample is drawn.
- Kernel 00 takes time dimension 4 of the sample record and sets `t = lerp(u, open, close)` (pbrt's `SampleTime()`), stored in `PathState.time` (roadmap).
- Every ray the path spawns carries that time, and so does every shadow ray: the shadow-ray table's spare word becomes `time`.
- **Rolling shutter** (PBR 4ed exercise 5.1): an optional readout time. Each row's exposure starts `readout × row / height` after the previous row's, so a fast pan skews verticals as a phone camera does. [pbrt-cameras.md](pbrt-cameras.md) deferred this here.

When nothing in the scene moves and the camera has no second pose, kernel 00 draws no time sample and `PathState.time` stays 0. Scenes without motion keep their random-number sequence, and so their image.

### Two poses

Each `SceneShape`, mesh object and `SceneCamera` gains an optional end pose: a position, an orientation and, for shapes, a size. An object without one does not move.

- Interpolation follows pbrt's `AnimatedTransform`: each pose is decomposed into translation, rotation and scale, translation and scale interpolate linearly, and rotation by quaternion slerp. Interpolating the matrices directly would shrink a rotating object halfway through its turn (PBR 3ed §2.9.2).
- On the GPU, a moving instance's two decomposed poses (48 bytes each) go into binding 39, `MotionTransforms`. `GpuInstance`'s second padding word holds the instance's index there, or none. The medium interface of [pbrt-participating-media.md](pbrt-participating-media.md) takes the first.
- `instanceRay()` builds the interpolated world-to-object transform at the ray's time when the instance moves. It is shared by every traversal, so the traversal contract gains a time argument and no strategy file changes otherwise. `crt_interaction.glsl` builds the object-to-world transform the same way.
- The camera interpolates its pose in kernel 00 before generating the ray.

*The Next Week*'s moving sphere is a shape with an end pose.

### Motion bounds

The TLAS is built over each moving instance's bounds over the whole shutter interval, which pbrt's `AnimatedTransform::MotionBounds()` computes by finding where each box corner's path along the interpolated motion reaches its extremes (PBR 3ed §2.9.4). `bvh_build` takes the widened boxes; the BLAS does not change, since motion here is rigid.

A fast-moving instance's box can span a large part of the scene and slow every ray that passes near it. The render guard prices a frame from the TLAS actually built, so it accounts for this.

### Lights

Punctual lights keep one pose. Emitting shapes and meshes that move are left out of the light list in the first version, and kernel 04 counts what BSDF samples find on them at full weight, as it does for the infinite plane. Sampling them properly means transforming an object-space light sample by the instance's pose at the path's time. That is the second step, and it needs the light records to refer to instances rather than hold world-space geometry.

### Editing

- A **Moves** checkbox per object and camera enables the end pose.
- **Edit Transform** gains a start and end toggle, so the gizmo edits either pose.
- The viewport draws the path between the two poses with the ImGui overlay the camera frustums use, since the raster preview has no line pipeline.
- The raster preview draws objects at their start pose.

### Not planned

Deformation motion blur, where each vertex has its own start and end position (PBR 4ed exercise 6.15), is not in this plan: the user's choice is rigid poses. It would need per-vertex end positions and a BLAS refit per frame ([pbrt-gpu-bvh-construction.md](pbrt-gpu-bvh-construction.md)).

## Registry, bindings and data

- Binding 39 `MotionTransforms`.
- `PathState.time`; `ShadowRay.time` in the shadow-ray table's spare word; `GpuInstance`'s second padding word.
- The traversal contract's `traceScene()` and `occluded()` take a time.
- Scene file: `"shutterOpen"`, `"shutterClose"` and `"rollingShutter"` per camera; `"endPose"` per shape, mesh object and camera.

## Verification

- A scene with no end pose and equal shutter times renders bit-identical.
- `bin/bvh_bench` gains moving instances and random ray times: the traversal matches brute force with 0 mismatches, with `--perturb`.
- A sphere moving across the frame converges to the average of 64 static renders at evenly spaced times (**Compare to reference**).
- A bar rotating 90° about its center blurs along an arc and keeps its length, which matrix interpolation would not.
- *The Next Week*'s bouncing-spheres scene reproduces the book's image.
- A camera pan with a rolling shutter skews a vertical pole by the angle the readout time predicts.

## Order of work

1. Shutter times, the time sample and `PathState.time`, drawn only when something moves. Bit-identical.
2. Two-pose instances, `MotionTransforms`, `instanceRay()` at time `t`, and motion bounds in the TLAS.
3. The moving camera and the rolling shutter.
4. Editing: the end pose, the gizmo toggle and the motion path overlay.
5. Light sampling on moving emitters.

## Sources

PBR 4ed §5.1 (camera rays carry a time between shutter open and close), §7.1.2 (primitives in motion, `AnimatedPrimitive`), exercises 5.1 and 6.15; PBR 3ed §2.9 (animating transformations: quaternions, slerp, `AnimatedTransform`, bounding moving boxes); Shirley, *Ray Tracing: The Next Week*, "Motion Blur"; pbrt-v4 `src/pbrt/util/transform.h` (`AnimatedTransform`).
