# Depth of Field

A short to-do for making the CPU raytracer's camera lens (aperture and focus distance) usable. It's small enough to do by hand in one sitting.

## Why it never worked in the old project

In the sibling project's `Renderer.cpp`:

```cpp
double dist_to_focus = (lookfrom - lookat).length();
```

On a glm vector, `.length()` doesn't return the distance. It returns the **number of components**, which is always `3`. The real distance from (3,3,2) to (0,0,−1) is about 5.2, so the focus plane sat about 2 units in front of the thing you were looking at, and nothing you aimed at was ever sharp.

The distance you want is `glm::length(lookfrom - lookat)`.

## What's already in place

- **The lens maths is done and correct.** `RTCamera::get_ray()` in `src/rt_job.cpp` already offsets rays across the lens when the aperture is above zero.
- **The data is there.** `RTCameraSnapshot` in `src/rt_types.h` has `aperture` (default `0.0`) and `focusDistance` (default `1.0`).
- **Nothing sets them.** There's no UI, and `buildRaytraceScene()` in `src/rt_scene.cpp` never fills them in, so the lens is always closed.

## Steps

1. **Add the two settings.** The easiest home is `RenderSettings` in `src/rt_types.h`, next to `rayDepth`, as `double aperture` and `double focusDistance`. That puts them next to the other render settings, and they're already copied into every render snapshot.
2. **Add two sliders** in `RaytraceJob::drawControlPanel()` (`src/rt_job.cpp`), under "Settings":
   - Aperture from `0.0` to about `1.0`.
   - Focus distance from about `0.1` to `50`. A logarithmic slider feels nicer here (`ImGuiSliderFlags_Logarithmic`, the same flag the sphere radius uses).
3. **Copy them into the camera snapshot** at the end of `buildRaytraceScene()`:
   `scene.camera.aperture = settings.aperture;` and the same for `focusDistance`.
4. **Don't derive the focus distance from `lookAt`.** In this project `lookAt` is always exactly one unit in front of the camera (`position + forward`), so `glm::length(lookAt - lookFrom)` is always `1`. That's the same class of mistake as the old bug. Use the slider value.
5. **Keep the focus distance above zero.** `RTCamera` scales the whole image plane by it, so `0` collapses every ray onto one point. Clamp it to something like `0.01` when copying it into the snapshot.

## Optional: click-to-focus

Instead of guessing a distance, add a "Focus on centre" button that fires one ray straight out of the camera and uses how far it travelled:

```cpp
ray r(position, forward);
hit_record rec;
if (scene.hit(r, 0.001, RT_INFINITY, rec))
    focusDistance = rec.t;   // forward is unit length, so t is the distance
```

## How to check it

- With the default camera at (0, 0, 5), the centre sphere is **6 units** away. Set focus distance to 6 and aperture to about 0.3, then render. The red centre sphere should be sharp, the glass and gold spheres beside it a little soft, and the far ground clearly blurred.
- **Regression check:** turn on **Fixed seed** and render once with aperture 0 before your change and once after. The images should be identical, because a closed lens skips lens sampling and uses no extra random numbers.

## Later

The planned GPU raytracer (`docs/plans/compute-pipeline-raytracing.md`) will need the same two values in its ray-generation push constant, plus the same random point on the lens, so its output keeps matching the CPU raytracer's.
