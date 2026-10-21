# Film and image reconstruction

A pixel's value was a running mean of its samples in a float image, and each sample landed uniformly inside its own pixel. That is a box filter, which PBR 4ed calls the least desirable choice because it lets aliasing through. The float running mean also drifts as the sample count grows. This plan adds pixel reconstruction filters, compensated accumulation, per-pixel variance and two ways to suppress fireflies.

*Implemented 2026-10-21. Depends on [pbrt-sample-generation.md](pbrt-sample-generation.md). Part of the [roadmap](../pbrt-roadmap.md).*

## Decisions

On 2026-09-26 the user accepted two defaults: filter importance sampling in kernel 00, and compensated accumulation, since the Radeon Pro 560X has no `shaderFloat64`.

On 2026-10-21, when implementation started, the user answered four more questions:

- A new scene selects the Gaussian filter (pbrt-v4's default, radius 1.5, sigma 0.5) and the compensated film. Its converged image is slightly softer than before. A file saved earlier keeps its box filter and running mean.
- A sample's filter weight rides in the `w` of its radiance slot, which kernel 00 already wrote as a spawned flag, so binding 29 (`PixelState`) is not allocated.
- The `sampleCount` image becomes the count of samples taken under the compensated film, and the count of samples accepted moves to the film record. Kernel 00 and the seven samplers read the image unchanged.
- All five steps of the plan are built, including outlier reweighting.

## What each pixel does now

Three kernels carry the film:

- Kernel 00 has a second variant, `0002_generate_camera_rays_filtered.comp` (`filter_importance`), that draws each sample's offset from the selected filter. `0001` stays, and a scene file that names it renders as before.
- Kernel 09 has two new variants beside `0901_update_film.comp` (`running_mean`): `0902_update_film_compensated.comp` (`compensated_mean`) and `0903_update_film_outliers.comp` (`outlier_reweighted`).
- The pixel value is the weighted mean `sum(w L) / sum(w)` (PBR 4ed equation 5.13), written into the accumulation image every frame. The tonemap, the reference comparison and the analysis views read the image they always did.

A new scene selects `filter_importance` and `compensated_mean`. A file with no `"00"` or `"09"` entry in `"kernels"` (every file older than version 8, and anything written by hand) reads `thin_lens` and `running_mean`, as a file with no `"01"` entry reads `pcg_hash`. Every file from version 8 on names its stages, so it keeps what it saved.

## Pixel filters

pbrt-v4 samples the filter instead of splatting samples into neighboring pixels (PBR 4ed §8.8.3). A sample's position comes from the filter's own distribution, so a filter that is never negative needs no weight and each sample still touches only its own pixel. Kernel 09 keeps its per-pixel independence and needs no atomics.

`src/film.h` (static library `rt_film`) holds the five filters with pbrt-v4's formulas and defaults:

| Id | Radius | Parameters |
|---|---|---|
| `box` | 0.5 | |
| `triangle` | 2 | |
| `gaussian` | 1.5 | sigma 0.5 |
| `mitchell` | 2 | B = C = 1/3 |
| `lanczos` | 4 | tau 3 |

`buildFilterTable()` tabulates a filter as pbrt's `FilterSampler` does: 32 cells per unit of radius along each axis, over the filter's whole width, as a piecewise-constant 2D distribution of `|f|` (`buildPiecewiseConstant2D()` from `rt_light`). `packFilterTable()` writes it to binding 30 as the width and height, the radius, the integral, the marginal cdf, the row cdfs, `|f|` and the signed `f`. Two uniform numbers from the sampler's pixel dimensions invert the cdfs, and `crt_filter.glsl` mirrors `sampleFilterTable()` line for line.

Mitchell and Lanczos go negative. A sample takes the sign of the filter in its cell, +1 or -1, in the slot's `w`. pbrt's weight is that sign times the integral of `|f|`, and the integral divides out of `sum(w L) / sum(w)`, so the shader uses the sign alone and the products stay exact. Kernel 09's `0902` and `0903` weigh by it. `0901` ignores it, so a render that pairs a filter with negative lobes with `running_mean` runs `compensated_mean` instead, and the **Film** section says so (`filmFallback()` in `rt_gpu.cpp`, the same pattern as the sampler's fallback).

A sample can land outside its own pixel, up to the radius away, and outside the image for an edge pixel. Kernel 00 projects it through the extended image plane as pbrt does. With antialiasing off every sample is the center of its pixel and the weight is 1.

## Samples taken and samples accepted

Finding 7 of [pbrt-verification-harness.md](pbrt-verification-harness.md) was a dropped sample drawn again every frame. Kernel 00 seeded a path from `sampleCount + k`, and kernel 09 counted only the samples it accepted, so a sample dropped as non-finite left the count where it was. At one sample per frame the pixel never got past it.

Under the compensated film the image in binding 9, renamed `samplesTaken` in the code (`CrtBinding::SamplesTaken`), counts every slot kernel 09 saw spawned, accepted or dropped. The accepted count is in the film record. `0901` keeps today's behavior, so a scene that selects it renders bit-identical and keeps the bug.

The self-test feeds eight frames of one slot per frame with a NaN in the fourth. The compensated film takes 8 samples and accepts 7, the dropped-sample counter reads 1, and the mean is the mean of the 7. The running mean counts 7 taken, as before.

## Compensated sums

`0902` keeps, per pixel, `sum(w L)` per channel and `sum(w)`, each with a Neumaier compensation term, in a 48-byte record (binding 31, `CrtFilmPixel`):

| Field | Meaning |
|---|---|
| `sum`, `compensation` | two `vec4`: the four sums and their compensation terms |
| `welfordMean`, `welfordM2` | the luminance's running mean and sum of squared deviations |
| `accepted` | samples accepted |

The compensation term must not be reassociated away. `crt_film.glsl` marks the operations `precise`, and the GPU self-test measures whether Metal kept it: the mean of one sample of 10000 followed by 4095 samples of 0.01 has relative error 2e-8 on the GPU, where a plain float sum has 9.6e-5.

The compensation is itself a float sum, so it holds only while the number of terms times float's epsilon stays well below 1. Adding 0.1 repeatedly gives a relative error of 3e-7 after 2^16 terms and 6.7e-5 after 2^20, where a plain float sum gives 6.2e-4 and 9.9e-3. At 2^24 terms the compensation no longer helps. A pixel takes thousands of samples, but a very long reference render should keep that range in mind.

Against the running mean on real renders, which see the same samples so that their difference is accumulation error alone (Cornell box, 32 × 32, relative RMS difference):

| Samples per pixel | 1 sample per frame | 8 samples per frame |
|---|---|---|
| 1024 | 2.8e-6 | |
| 4096 | 1.0e-5 | |
| 16384 | 4.8e-5 | 5.4e-6 |
| 65536 | | 2.3e-5 |
| 262144 | | 1.1e-4 |

The running mean's drift grows in proportion to the sample count and depends on the samples per frame. It stays two orders of magnitude below the noise on this scene at these counts, so a convergence plot of relative MSE against a reference does not bend within 16384 samples, where the plan expected it to. The benefit is for pixels with almost no variance and for renders far past 100,000 samples.

## Per-pixel variance

`0902` and `0903` feed Welford's update with each accepted sample's luminance, after the clamp and without the filter weight (pbrt's `VarianceEstimator`, PBR 4ed §B.2.11). **Analysis view** → **Standard error** shows `sqrt(M2 / (n - 1) / n)` over `max(|mean|, 0.1)` on the error view's log scale, from 0.1 percent (blue) to 100 percent (red). It needs no reference, and it is grey under the running mean, which keeps no variance. [pbrt-adaptive-sampling.md](../pbrt-adaptive-sampling.md) reads the same numbers.

## Firefly handling

Both are off by default, since both bias the image.

**Clamp** is pbrt's `maxComponentValue`: a sample whose largest component exceeds the limit is scaled down to it, keeping its color, before it reaches the sums, the variance and the cascade. `RenderSettings::clampSamples` and `clampValue` (default 10) are saved with the scene, and `CrtParams::filmClamp` carries the limit. Only the film variants that keep a record clamp; selecting the running mean with the clamp on runs the compensated mean.

**Outlier reweighting** (`0903`, Zirr, Hanika and Dachsbacher 2018) keeps the plain sums too, and beside them a cascade of six buffers by brightness (binding 62, `FilmCascade`, 6 × 16 bytes a pixel). A sample of luminance S goes to the two buffers around `j = floor(log_8 S)`, split by the hyperbolic weight `alpha = (8^j / S - 1/8) / (1 - 1/8)` so that dividing a buffer's luminance sum by `8^j` counts the samples of that brightness (equation 19). A sample dimmer than 1 goes whole to the first buffer and one past the top whole to the last, so the cascade spans up to 8^5 = 32768 before the last buffer absorbs the rest.

The resolve weighs each buffer by how well its brightness was sampled. A buffer's weight is the larger of two:

- the count weight `min(1, n / kappa)`, where `n` is the pixel's own count of that brightness, and zero where the average count over the pixel and its 3 × 3 neighbors is below 1, which drops a brightness nobody nearby has seen;
- the value weight `min(1, N E / (kappa 8^j))`, where `N` is the samples accepted and `E` the expected value composed from the buffers below, which keeps every sample too dim to be a firefly.

`0903` runs in two passes (`KernelVariant::passes`, the pass number in `params.bounce`): pass 0 adds the frame's samples, and pass 1 resolves each pixel from its own cascade and its neighbors', which pass 0 had to finish writing. `src/film.cpp` mirrors the arithmetic as `cascadeAddSample()`, `cascadeCount()` and `outlierResolve()`.

A glass sphere on a floor under a small lamp (`assets/scenes/caustic_glass.gltf`) has a caustic made of fireflies. At 256 samples against a 32768-sample reference, mean of three seeds:

| Film | Relative MSE | Mean, as a fraction of the reference |
|---|---|---|
| unbiased | 0.0693 | 0.995 |
| outlier, kappa 1 | 0.0084 | 0.972 |
| outlier, kappa 2 | 0.0050 | 0.966 |
| outlier, kappa 4 | 0.0045 | 0.957 |
| outlier, kappa 8 | 0.0061 | 0.919 |
| outlier, kappa 16 | 0.0114 | 0.880 |
| clamp 10 | 0.0055 | 0.905 |
| clamp 30 | 0.0060 | 0.912 |

Kappa 4 lowers the error 15-fold for 4 percent of the energy, where a clamp of 10 lowers it 13-fold for 10 percent. The default is 4, which suits a few hundred samples. Kappa has to stay small against the samples per pixel: Zirr et al. use 20 to 32 at 1000 samples and more. On the Cornell box at 64 samples, kappa 32 loses 33 percent of the energy and raises the relative MSE twentyfold, and kappa 8 loses 10 percent.

On the Cornell box and the Veach scene, which have next-event estimation and no extreme paths, the reweighting lowers no error. Kappa 1 and 2 change nothing, and a larger kappa trades energy for noise it does not have. The method takes a rare bright path for a firefly, since the two look alike in a pixel, and the energy of the real ones is what it gives up. A clamp of 10 loses 25 percent of the Veach scene's mean for the same reason: its lights are brighter than 10.

## Splats

Light tracing and the bidirectional methods add contributions to arbitrary pixels ([pbrt-bidirectional-path-tracing.md](../pbrt-bidirectional-path-tracing.md)). Their splats go in a buffer of their own, and the compensated variants' resolve is where `splatSum / samplesPerPixel` joins the weighted mean, as pbrt's `RGBFilm::GetPixelRGB()` does. `crt_film_update.glsl` marks the place with a comment. No splat buffer exists yet.

## Registry, bindings and data

- Binding 9 is `CrtBinding::SamplesTaken`, the samples taken of each pixel.
- Binding 30 `FilterTable`: the packed table, a few kilobytes up to 192 × 192 cells for the widest filter.
- Binding 31 `AccumulationError`: `CrtFilmPixel`, 48 bytes a pixel, 100 MB at 1920 × 1080. A one-pixel stub while the running mean is selected.
- Binding 62 `FilmCascade`: 96 bytes a pixel, 200 MB at 1920 × 1080, only for `0903`. Binding 29 stays reserved for `PixelState`.
- A film record past `FILM_RECORD_BYTES_LIMIT` (1 GiB) falls back to the running mean, and the panel says so.
- New variants: kernel 00 `filter_importance`, kernel 09 `compensated_mean` and `outlier_reweighted`. New settings: `KernelSettings::PixelFilter` and `KernelSettings::Film`, `KernelVariant::keepsFilmRecord`, `keepsFilmCascade` and `passes`.
- `CrtParams` keeps its 208 bytes: `filmClamp` and `outlierKappa` take two of the padding words.
- `RenderSettings` gains `pixelFilter`, `clampSamples`, `clampValue` and `outlierKappa`. Scene files are payload version 15. A file with no `"pixelFilter"` reads a box.
- The **Film** section of **Raytrace Render** holds **Pixel sampling** (kernel 00), **Pixel filter** with its parameters, **Film** (kernel 09), **Clamp samples** and **Outlier kappa**, and the film's memory.
- `rt_film` (`src/film.h/.cpp`) is the C++ mirror of `crt_filter.glsl` and `crt_film.glsl`, and `bin/film_test` checks it.

## Verification

- With `thin_lens` and `running_mean` selected, the Cornell box, the sphere scene, the Veach scene, the thin glass scene, `all_materials.gltf` and the white furnace gallery render bit-identical to the build before this plan. Kernel 00's SPIR-V differs from the old one in its declarations only, and its instructions are the same once the ids are stripped.
- Kernel 00 with a box filter through the table gives the Cornell box at 1024 samples within an RMS difference of 0.00000 of `0001`, where two seeds of `0001` differ by 0.0205: the same distribution, reached through a table. `tools/verify/filter_check.py` runs the comparison.
- Every filter's samples follow the table: a chi-square test of 2 million offsets per filter against `|f|` gives p between 0.09 and 0.97, with no sample of the wrong sign. The weighted mean of a function over filtered samples agrees with the filter-weighted average within its standard error, and the mean of a constant is exactly the constant, signs included. `bin/film_test --selfcheck` plants a sampler 2 percent off and a sum without compensation, and fails unless both are caught.
- The GPU self-tests (**Windows** → **GPU self-tests**, or `VKGUIDE_SELFTEST=1`) pass. Filter sampling matches the mirror to 3.2e-7 of the radius at worst over 40,000 samples per filter, with no weight differing. The film sums match over 402 sequences, including signed weights and the clamp, to 7.8e-6. Kernel 09's accounting runs the real shaders over 65536 frames. Kernel `0903` matches the mirror, neighborhood included, to 3.3e-7 on a 12 × 8 image with planted fireflies.
- The white furnace stays exact under every filter. A sphere of a mirror or of glass, whose samples are all exactly 1, renders exactly 1 in every pixel of the image under box, triangle, Gaussian, Mitchell and Lanczos, so the negative lobes' weights cancel without residue. Lambertian and `pbr` spheres agree with the box and the running mean within their noise. `tools/verify/furnace_check.py` runs it.
- A checkerboard running to the horizon (`filter_check.py`) at 64 samples per pixel: the mean squared difference between neighboring pixels in the band near the horizon is 0.127 for the box filter, 0.103 for Mitchell-Netravali and 0.080 for the Gaussian, so the Gaussian is the smoothest. At one sample per pixel no filter shows: a pixel is then a single point sample. The plan said one sample, which was wrong.
- The validation layers report nothing for every combination of `thin_lens` or `filter_importance` with the three films, nor for the six analysis views over each.

GPU time of a 640 × 360, 64-sample render of the Cornell box with ZSobol on the Radeon Pro 560X, median of four interleaved runs: 1066 ms for `thin_lens` with `running_mean`, 1051 ms for `filter_importance` with a Gaussian and `compensated_mean`, and 1295 ms for the same with `outlier_reweighted`. The cascade's two read-modify-writes per sample and its second pass cost 21 percent.

## Where it departs from the plan and from pbrt

- A sample's weight is its sign, not pbrt's `f / pdf`. The two differ by the integral of `|f|`, which the weighted mean divides out, and the sign keeps the products exact.
- `PixelState` is not allocated. The camera's ray weight ([pbrt-cameras.md](../pbrt-cameras.md)) can start as the path's throughput, since pbrt multiplies it into the radiance before the film sees it and not into the film weight.
- The plan expected a convergence plot to bend for the running mean within 16384 samples. It does not on these scenes; see the table above.
- The plan gave the record as 32 bytes in one place and 48 in another. It is 48, with the variance and the accepted count in it.
- `0903` needs two passes, which the plan did not foresee, because the neighborhood criterion reads other pixels' cascades.
- The outlier film resolves while the render runs. Changing kappa restarts the render, where Zirr et al. apply it to the finished buffers. The cascade keeps what a display-time resolve needs, so a later change can add one.
- The cascade's sums are plain float, as in the paper. Only the film record's are compensated.
- The variance reads the sample after the clamp only while the clamp is on. [pbrt-adaptive-sampling.md](../pbrt-adaptive-sampling.md) wants the clamped sample for its estimate even when the image does not clamp, and has to add that.

## Sources

PBR 4ed §5.4.3 to §5.4.6 (filtering image samples, the weighted importance sampling estimator, RGBFilm with double-precision pixels and `maxComponentValue`), §8.8 (filters and `FilterSampler`), §B.2.11 (robust variance estimation), Figure 5.24 (32-bit references), Figure 8.48 (filters on a checkerboard); Zirr, Hanika and Dachsbacher 2018, *Re-weighting firefly samples for improved finite-sample Monte Carlo estimates*; Neumaier 1974, *Rundungsfehleranalyse einiger Verfahren zur Summation endlicher Summen*; pbrt-v4 `src/pbrt/film.*`, `filters.*`.
