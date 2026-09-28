# Adaptive sampling

Every pixel receives the same number of samples, though a pixel of flat sky converges long before one under a caustic. The path tracer was built with a per-pixel sample budget for exactly this, and nothing fills it yet. pbrt deliberately has no adaptive sampling: its authors found few methods robust across scenes (PBR 4ed chapter 8, "Further Reading"), and Kirk and Arvo 1991 showed that deciding from samples that are also added to the image biases it. This plan adds a policy with both problems handled explicitly: a choice of error estimate, a floor that keeps sampling everywhere, and an unbiased mode that decides from samples kept out of the image.

*Not implemented. Depends on [film-and-reconstruction.md](film-and-reconstruction.md) (per-pixel variance in `0902_compensated_mean`). This revision replaces the earlier barebones draft of this file. Part of the [roadmap](roadmap.md).*

## What the renderer does today

- `sampleBudget` (binding 5, one `uint` per pixel) exists. Kernel 00 spawns `min(sampleBudget[pixel], samplesPerFrame)` paths for each pixel, and a fresh render fills every budget with the samples per frame (`rt_gpu.cpp`).
- `sampleCount` (`r32ui`) holds each pixel's count, and the running mean divides by it, so pixels may hold different numbers of samples.
- The **Sample count heat** debug view shows `sampleCount`.
- Nothing writes the budget after the first frame.

## Design

### Error estimates

An **Error estimate** setting:

- **Variance** uses the relative standard error of each pixel's mean, `√(M2 / (n − 1) / n) / max(mean, floor)`, from the Welford sums of `0902_compensated_mean`.
- **Half-buffer difference** accumulates samples with even and odd indices into two halves and takes their difference relative to their mean, averaged over an 8 × 8 tile. A pixel whose few samples happen to agree fools it less, and it needs one more rgba32f image. Cycles' adaptive sampling uses this estimate.

Either estimate treats fireflies as high variance forever, so the estimate reads the clamped sample (the **Clamp** of [film-and-reconstruction.md](film-and-reconstruction.md)) even when the image does not clamp.

### The policy pass

`crt_budget.comp`, a pass over the pixels after kernel 09 every `F` frames (default 4):

- A pixel with fewer than **Minimum samples** (default 64) keeps the full budget. A pixel whose first samples all missed a rare feature, such as a glint through a gap, otherwise looks converged.
- A pixel whose error is below the **Tolerance** (default 1 %) gets a budget of 0, except that it still receives one sample every **Floor interval** frames (default 8), so no pixel stops sampling entirely.
- The rest keep the full budget. A graded budget proportional to the error ranking would need a normalisation pass to keep the frame's total bounded; binary comes first.

When every pixel is at its floor, the render panel shows **Converged**. The queue-header readback, `FRAME_OVERLAP` frames late, gives the number of spawned paths.

### Bias

A pixel that stops early because its samples so far happen to be quiet has a value that depends on those samples, which is Kirk and Arvo's bias. The minimum and the floor bound it; they do not remove it. **Unbiased decisions** removes it at the cost of samples:

- Frames alternate between two roles per pixel: decision samples, which update only the error estimate, and image samples, which update only the image.
- The budget decides how many image samples a pixel receives from decision statistics alone, so a pixel's image samples are independent of how many of them there are.
- About half the samples never reach the image, so this mode converges more slowly per sample. Its purpose is to measure the bias of the fast modes.

### Interactions

- Each sample touches only its own pixel under filter importance sampling ([film-and-reconstruction.md](film-and-reconstruction.md)), so Mitchell's problem does not arise: a dense clump of samples inside a pixel filter's extent never outweighs its neighbours.
- ZSobol, Sobol and the other global samplers take each pixel's own sample index, which stays correct when counts differ. The stratified sampler needs a fixed count and is disabled while adaptive sampling is on ([sample-generation.md](sample-generation.md)).
- Path regeneration ([wavefront-performance.md](wavefront-performance.md)) takes its next (pixel, sample) pairs only from pixels with budget left.
- Every render restart clears the budget, the variance and the half buffers along with the mean.

### Other uses of the budget

- A region of interest, painted on the output through the `DisplayRegistry`'s `onInteract` callback (the same hook [wave-simulation.md](wave-simulation.md) uses for mouse input), sets the budget directly, for inspecting one area without waiting for the rest.
- A time budget scales the samples per frame to hold a target frame time, since the render runs beside the raster viewport and should not make it stutter.

## Registry, bindings and data

- `crt_budget.comp`, a `ComputePass` after kernel 09.
- The half-buffer image, allocated only for the half-buffer estimate.
- Settings: **Adaptive sampling**, **Error estimate**, **Tolerance**, **Minimum samples**, **Floor interval**, **Unbiased decisions**.
- No new binding number: the budget is binding 5.

## Verification

- With adaptive sampling off, every scene renders bit-identical.
- On the Cornell box, the Veach scene and the spheres under `nowhere_road_4k.hdr`: equal-time relative MSE against uniform sampling, for each error estimate.
- The mean relative error against a 16384-spp reference measures each mode's bias; with **Unbiased decisions** it is consistent with zero.
- The sample-count heat view shows the budget concentrating where the error does: caustics, soft shadows, glossy reflections.

## Order of work

1. The variance estimate, the policy pass with the minimum and the floor, and **Converged**.
2. The half-buffer estimate.
3. **Unbiased decisions**, and the bias measurements.
4. Region of interest and time budgeting.

## Sources

PBR 4ed §8.1.5 ("Adaptive Sampling") and chapter 8 "Further Reading" (adaptive sampling and reconstruction: Mitchell's multi-stage filter, Kirk and Arvo's bias, Zwicker et al.'s survey); Kirk and Arvo 1991, *Unbiased sampling techniques for image synthesis*; Mitchell 1987, *Generating antialiased images at low sampling densities*; Zwicker et al. 2015, *Recent advances in adaptive sampling and reconstruction for Monte Carlo rendering*; the Cycles manual, "Adaptive sampling".
