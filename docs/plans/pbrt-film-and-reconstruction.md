# Film and image reconstruction

Each pixel's value is a running mean of its samples, kept in an rgba32f image, and each sample is jittered uniformly inside its pixel. That is a box filter, which PBR 4ed calls the least desirable choice because it lets aliasing through. The float running mean also stops improving at high sample counts: pbrt measured MSE flattening after about 1000 spp with a 32-bit reference, which undermines **Compare to reference** exactly where it matters. This plan adds pixel reconstruction filters, compensated accumulation, per-pixel variance and optional firefly suppression.

*Not implemented. Depends on [pbrt-sample-generation.md](completed/pbrt-sample-generation.md). Part of the [roadmap](pbrt-roadmap.md).*

## Decisions already made

On 2026-09-26 the user accepted two defaults: filter importance sampling in kernel 00, and compensated accumulation, since the Radeon Pro 560X has no `shaderFloat64`.

## What the renderer does today

- Kernel 00 jitters each sample uniformly in its pixel, or places it at the center when antialiasing is off.
- Kernel 09 (`0901_update_film.comp`, `running_mean`) folds the frame's K slots into `accumulation` as `μ_{n+m} = (n μ_n + Σ L_k) / (n + m)`, skipping unspawned and non-finite samples and counting the non-finite ones.
- `sampleCount` (r32ui) keeps each pixel's count for the mean and for adaptive sampling. It also gives kernel 00 each path's sample index, which has a bug when a sample is dropped.[^1]
- Nothing estimates per-pixel variance, and nothing clamps outliers.

## Design

### Pixel filters by importance sampling

pbrt-v4 samples each pixel's filter instead of splatting samples into neighboring pixels (PBR 4ed §5.4.3, §8.8). A sample's position is drawn from the filter's own distribution, so a positive filter needs no weight at all and each sample still touches only its own pixel. That keeps kernel 09's per-pixel independence and needs no atomics.

- A new kernel 00 variant, `0002_filter_importance`, warps the sampler's pixel dimensions through a tabulated distribution of the selected filter (pbrt's `FilterSampler`: the filter evaluated 32 times per unit of radius, sampled as a piecewise-constant 2D distribution). The camera projection is shared with 0001 through `crt_camera.glsl` ([pbrt-cameras.md](pbrt-cameras.md)).
- A **Pixel filter** settings section appears with this variant: box, triangle, Gaussian (pbrt's default, radius 1.5, σ 0.5), Mitchell–Netravali (B = C = 1/3) and Lanczos windowed sinc (τ = 3), each with its radius.
- The filter's table goes into `FilterTable` (binding 30), rebuilt when the setting changes.
- Filters with negative lobes (Mitchell, Lanczos) return a weight of sign(f) × ∫|f|. Kernel 00 writes it to `PixelState.filterWeight` (binding 29), and kernel 09 uses pbrt's weighted estimator `Σ w L / Σ w` (PBR 4ed Equation 5.13), which removes the variance a varying weight would otherwise add at a small bias.

0001 stays, unchanged, so older files and the bit-identical comparison keep today's box filter.

### Compensated accumulation

A new kernel 09 variant, `0902_compensated_mean`, keeps sums instead of a running mean:

- Per pixel, `Σ w L` (rgb) and `Σ w`, each with a Neumaier compensation term, in a 32-byte-per-pixel `AccumulationError` buffer (binding 31). Compensated float sums hold roughly twice float's precision, which is what pbrt's double-precision pixels exist to provide.
- It also writes the resolved mean into `accumulation` every frame, so the tonemap pass, the reference comparison and the auxiliary outputs read the same image as today.

`0901_running_mean` stays for bit-identical comparisons. New scenes select `0902`.

`0902` is also where the sample-index bug in footnote 1 gets fixed, since it changes what the film counts: the count the mean divides by and the number of samples taken become two numbers.

### Per-pixel variance

`0902` also keeps Welford's running mean and M2 of each sample's luminance (pbrt's `VarianceEstimator`, PBR 4ed §B.2.11), unweighted. It is the input [pbrt-adaptive-sampling.md](pbrt-adaptive-sampling.md) needs and a debug view of its own: standard error of the mean, `sqrt(M2 / (n − 1) / n)`, as a heat map relative to the mean.

### Firefly suppression

Both are off by default, since both bias the image:

- **Clamp** is pbrt's `maxComponentValue`: kernel 09 scales any sample whose largest component exceeds a threshold down to it. PBR 4ed Figure 5.25 shows its effect on a sun-lit scene, and §5.4.6 its cost: lost energy.
- **Outlier reweighting** follows Zirr et al. 2018. Samples are sorted by magnitude into a small cascade of buffers (about six, each a power of the base apart); at resolve time, a buffer whose contribution is not supported by its neighbors in the cascade is down-weighted. It removes fireflies with far less energy loss than clamping, and needs no stored samples. It is a third kernel 09 variant, `0903_outlier_reweighted`, since it changes the buffers kernel 09 owns.

### Splats

Light tracing and bidirectional methods add contributions to arbitrary pixels ([pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md)). Their splats go into their own buffer; the film's resolve step adds `splatSum / samplesPerPixel` to the weighted mean, as pbrt's `RGBFilm::GetPixelRGB()` does. This plan leaves a hook in the resolve for it.

## Registry, bindings and data

| Binding | Resource | Size |
|---|---|---|
| 29 `PixelState` | per pool slot: filter weight, camera ray weight, and later the auxiliary-output data | 16 bytes a slot to start |
| 30 `FilterTable` | the selected filter's piecewise-constant 2D distribution | a few KB |
| 31 `AccumulationError` | per pixel: compensated sums, weight sum, Welford mean and M2 | 48 bytes a pixel |

New variants: kernel 00 `filter_importance`; kernel 09 `compensated_mean`, `outlier_reweighted`. Settings: **Pixel filter**, **Clamp**. Scene file: the filter and its parameters, and the clamp.

## Order of work

1. `0902_compensated_mean` with the box filter. Test the convergence plot of [pbrt-verification-harness.md](completed/pbrt-verification-harness.md) out to 16k spp against `0901`: `0902`'s line must stay straight where `0901`'s bends.
2. `PixelState` and `0002_filter_importance` with box, triangle and Gaussian.
3. Mitchell and Lanczos with the weighted estimator.
4. Welford variance and its debug view.
5. Clamp, then outlier reweighting.

## Verification

- With `0001` and `0901` selected, every scene renders bit-identical to today.
- `0002` with the box filter converges to the same image as `0001` (it is the same distribution, reached through a table).
- A checkerboard plane receding to the horizon at 1 spp shows the aliasing difference between box, Gaussian and Mitchell, as in PBR 4ed Figure 8.48.
- The white furnace scene converges to exactly 1 under every filter.
- A sample forced to a NaN in one pixel of a render at 1 sample per frame is dropped once and the pixel carries on: its count reaches the render's sample target, minus the one dropped sample, instead of freezing (footnote 1). The **Dropped samples** analysis view shows exactly one red pixel.

## Open questions

- Whether `PixelState` should hold the filter weight per slot at all. For positive filters sampled exactly the weight is constant, so it could be a push constant. Only negative-lobed filters and realistic-camera vignetting ([pbrt-cameras.md](pbrt-cameras.md)) vary per sample; the auxiliary outputs need `PixelState` anyway, which is why the plan keeps it.

## Sources

PBR 4ed §5.4.3 to §5.4.6 (filtering image samples, the weighted importance sampling estimator, RGBFilm with double-precision pixels and `maxComponentValue`), §8.8 (filters and `FilterSampler`), §B.2.11 (robust variance estimation), Figure 5.24 (32-bit references); Zirr, Hanika and Dachsbacher 2018, *Re-weighting firefly samples for improved finite-sample Monte Carlo estimates*; pbrt-v4 `src/pbrt/film.*`, `filters.*`.

[^1]: **A dropped sample is drawn again every frame.** Found 2026-10-20 by the verification harness ([pbrt-verification-harness.md](completed/pbrt-verification-harness.md), finding 7).

    *The bug.* Kernel 00 seeds a path from `sampleIndex = sampleCount[pixel] + k`, where `k` is the path's slot within the frame. Kernel 09 adds only the samples it accepts to `sampleCount`, and skips a non-finite sample without counting it. So a dropped sample leaves the count where it was, and the next frame starts from the same index, which gives the same random stream and, for a deterministic fault, the same non-finite sample. At 1 sample per frame the pixel never gets past it. In a 64 x 64 render of 4096 samples, a fully metallic `pbr` sphere (before its 0/0 was fixed) dropped one sample 1403 times at pixel (39, 17), and that pixel finished at 2693 samples. At several samples per frame the retry happens once or twice, because the next frame starts a little past the bad index. Any non-finite sample from any cause does this, and the pixel's mean is then over fewer distinct samples, so it converges more slowly than its neighbors. Kernel 09's dropped-sample counter reports the repeats as new drops, so a single bad sample reads as a stream of them.

    *The fix.* The count the mean divides by and the number of samples taken have to be separate. Give kernel 00 its sample index from a second per-pixel counter of samples taken, which kernel 09 advances by the number of slots it spawned for the pixel, accepted or not, and keep `sampleCount` as the count of accepted samples that the mean divides by. That is one more `r32ui` image beside `sampleCount` (or a second channel of a wider one), one more image binding for kernels 00 and 09, and `0901_running_mean` has to keep today's behavior for bit-identical comparisons, so the change belongs to `0902_compensated_mean` and its kernel 00 partner. [pbrt-adaptive-sampling.md](pbrt-adaptive-sampling.md) draws each pixel's budget from the same counter and needs the same split: its budget compares samples taken against the target, and its variance estimate uses the accepted count. Do not make kernel 09 count a dropped sample as accepted: the sum would then divide by a sample it does not contain, which biases the pixel low by one sample's share.
