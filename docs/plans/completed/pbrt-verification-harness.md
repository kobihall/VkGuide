# Verification harness

Several plans add sampling code whose bugs do not show as crashes or obvious artifacts: a BSDF whose sampling density disagrees with its `pdf()` by a few percent renders a plausible image with the wrong brightness under MIS. `bin/light_test` already catches that class of bug for lights. This plan builds the equivalent for BSDFs and samplers, adds scene-level energy tests, and extends **Compare to reference** so that convergence rates, not only single errors, can be measured.

*Implemented 2026-10-20. Depends on [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md) only for the GPU parity pass. Built ahead of [pbrt-bsdf-library.md](../pbrt-bsdf-library.md), which relies on it. Part of the [roadmap](../pbrt-roadmap.md).*

## Decisions

On 2026-10-20 the user chose:

- The Cornell reference is an independent CPU solve and closed forms, plus a render by their own pbrt-v4 build. No published image exists on disk.
- Reference images are saved and loaded as EXR now, so tinyexr is added in this plan instead of in [pbrt-aovs-and-image-output.md](../pbrt-aovs-and-image-output.md). PFM comes with it.
- The scene-level checks render through a small permanent headless mode, `VKGUIDE_VERIFY`, instead of a temporary hook. [pbrt-aovs-and-image-output.md](../pbrt-aovs-and-image-output.md) can replace it with its batch mode.
- A bug the harness finds is reported first, and the user decides on each finding. On 2026-10-20 they chose to fix findings 1, 2 and 5 and to note 3 and 4 in the plans that will address them.

## What exists today

- `bin/light_test` checks every light's sampled density against its density function and its solid angle against a counted estimate, through the C++ mirror of `crt_light.glsl`.
- `bin/bvh_bench` checks every traversal against brute force.
- The materials in `crt_scatter.glsl` (lambertian, metal, phong, dielectric, pbr) have no C++ mirror and no test. Their `evalSurface()` and `pdfSurface()` were derived by hand to agree with `scatterSurface()`, and nothing checks that they do.
- **Compare to reference** reports RMSE and relative MSE against a stored render, one number per frame.

## Design

### A C++ mirror of the existing materials

Start the `rt_bsdf` static library (no Vulkan, `-O2`, like `rt_light`) by mirroring `crt_scatter.glsl` line for line: `scatterSurface()`, `evalSurface()`, `pdfSurface()` and the pbr helpers, with the same PCG. From then on every BSDF in [pbrt-bsdf-library.md](../pbrt-bsdf-library.md), [pbrt-layered-materials.md](pbrt-layered-materials.md) and the other appearance plans lands in both places.

### `bin/bsdf_test`

For each material and a grid of parameters (roughness, IOR, metallic, incident angle), the test runs:

1. Samples and the pdf agree: draw about one million directions with the sampling routine and histogram them over a (cos θ, φ) grid of, for example, 64 × 128 cells. Integrate `pdf()` over each cell with a fine quadrature. Compare with Pearson's χ² test, pooling cells whose expected count is below 5, and report a p-value; fail below 0.01 after Šidák correction for the number of configurations. This is pbrt-v4's `bsdfs_test.cpp` approach (after Jung et al. 2020).
2. The weight equals f cos / pdf: for every sampled direction, the weight the sampling routine returns must equal `eval() / pdf()` to within 1e-4 relative.
3. Energy: the directional albedo, estimated with 64k cosine-weighted samples, must not exceed 1 for any passive material, and must match the analytic value where one exists (1 for a white lambertian, the Fresnel average for a smooth dielectric).
4. Reciprocity, for materials that should have it: `f(wo, wi) = f(wi, wo)` for reflection, `η_o² f(wo, wi) = η_i² f(wi, wo)` for transmission (PBR 4ed §9.5.2).

Delta materials skip tests 1 and 4 and check instead that the sampled direction is the mirror or refraction direction.

### Sampler checks

[pbrt-sample-generation.md](pbrt-sample-generation.md) adds `bin/sampler_test`. The harness provides the common machinery: star discrepancy of 2D projections, a check that every pixel's samples are stratified over the elementary intervals a (0, 2)-sequence promises, and determinism (the same pixel and sample index give the same values twice).

### Scene-level energy tests

Two scenes in `assets/scenes/`, rendered by a script through the headless smoke-test hook (memory note "Headless smoke testing recipe"):

- `white_furnace.gltf`: a sphere of each material inside a uniform environment of radiance 1, no other lights. A passive material that conserves energy renders exactly 1 wherever it reflects everything and below 1 elsewhere; any pixel above 1 by more than the noise is a bug. Microfacet materials show the energy they lose to single scattering, which [pbrt-appearance-extras.md](../pbrt-appearance-extras.md) then compensates.
- `cornell_reference.gltf`: the Cornell box with a published reference render. Its role is to catch errors every internal test shares, such as a wrong factor of π in the light's power.

### Convergence measurement

**Compare to reference** gains:

- A log–log plot of relative MSE against samples per pixel, kept for each render compared against the same reference, so that two strategies can be compared on one plot. An unbiased estimator draws a line of slope −1; a flatter line past about 1000 spp points to accumulation precision, which [pbrt-film-and-reconstruction.md](../pbrt-film-and-reconstruction.md) fixes (PBR 4ed Figure 5.24).
- Mean absolute percentage error beside RMSE and relative MSE.
- Saving and loading the reference as an EXR (through [pbrt-aovs-and-image-output.md](../pbrt-aovs-and-image-output.md)), so that a reference rendered once can be reused across sessions and compared against pbrt's own output ([pbrt-scene-import.md](../pbrt-scene-import.md)).

### Optics

The laser optics plans follow the same pattern: `bin/optics_test` for surfaces, glasses, coatings and element builders, `bin/beam_test` for the Gaussian beam solver, and system-level scenes with references (OpticStudio, Optiland, LightPipes, closed forms) in [physoptics-verification-scenes.md](../physoptics-verification-scenes.md). A coating test adds energy conservation, `R + T + A = 1`, to this plan's tests.

### GPU parity

The mirrors guarantee nothing if the GLSL drifts from the C++. A later step adds `shaders/test/bsdf_parity.comp`, which evaluates a fixed batch of (material, wo, wi, random numbers) on the GPU, and a **Run GPU self-tests** item under **Windows** that reads the results back and compares them with `rt_bsdf` to 1e-5. It runs inside the application, since the tests need the Vulkan device.

## Order of work

1. `rt_bsdf` with the mirror of `crt_scatter.glsl`, and `bin/bsdf_test` running tests 1 to 3 on the existing materials. Fix, or record as known, whatever they find. The phong and metal densities are the likeliest candidates, since they were derived by hand.
2. The white furnace and Cornell reference scenes.
3. The convergence plot and MAPE.
4. The GPU parity pass.

## Verification

This plan's own verification is that it finds a planted bug. Multiply `pdfPbr()` by 1.05 in a scratch build: test 1 must fail, and the white furnace must stay unchanged, since the sampling itself is still correct.

## As built

### What exists

- `src/bsdf.h/.cpp`, the static library `rt_bsdf`: `scatterSurface()`, `evalSurface()`, `pdfSurface()` and their helpers, mirroring `crt_scatter.glsl` for the six original materials, with the random number routines they draw from. `BsdfDraws` holds the PCG state or a sample record, so the mirror runs both ways kernel 06 draws. Textures and the normal map are not mirrored.
- `tests/bsdf_test.cpp` and `tests/test_stats.h`, `bin/bsdf_test`: 130 configurations (lambertian, four phong, five fuzzy metals and a mirror at two settings, two dielectrics, twelve pbr, each at 0, 30, 60, 80 and 88 degrees).
- `src/gpu_selftest.h/.cpp`, `src/gpu_selftest_bsdf.cpp` and `shaders/test/bsdf_parity.comp`: **Windows** → **GPU self-tests**, or `VKGUIDE_SELFTEST=1`.
- `src/image_io.h/.cpp` (`rt_image`), `tests/image_diff.cpp` (`bin/image_diff`), and tinyexr in `../CPPLibraries/tinyexr/`.
- **Compare to reference**: **Save...**, **Load...**, MAPE, the mean difference, the convergence plot with each curve's slope, and up to eight kept curves.
- `VKGUIDE_VERIFY` in `VulkanEngine`, and `tools/verify/`: `make_scenes.py`, `furnace_check.py`, `cornell_check.py`, `radiosity.py`, `pbrt_cornell.py`, `cornell.py`, `vkscene.py` and `run_all.py`.
- `assets/scenes/white_furnace.gltf`, `cornell_reference.gltf`, `cornell_empty.gltf` and `assets/environment_maps/uniform_white.hdr`.

### Where it differs from the design

- **Test 2 holds per sample only for some materials.** `scatterSurface()` for `pbr` returns the weight of the lobe it chose, `f_lobe cos / (chance pdf_lobe)`, not `f cos / pdf` of the mixture. The estimator is still unbiased: the expected weight of the samples in a region equals the integral of `f cos` over it, because the MIS weight depends on the direction alone. The test therefore compares the mean weight per coarse cell with the integral of `evalSurface()` for every material, and the per-sample ratio only for lambertian, phong and metal. The comment at the top of `crt_scatter.glsl` says the attenuation is exactly `evalSurface() / pdfSurface()` for the picked direction. That holds for those three and not for `pbr`.
- **The grid follows the lobe.** A 64 x 128 grid in the surface's frame cannot resolve a lobe narrower than one cell. A case with a narrow lobe lays its rings around the mirror direction, with ring edges that thicken as the cube of the ring number, and each cell's density is integrated adaptively. The samples a material absorbs are one more cell, so a density that integrates to the wrong probability of keeping a sample fails the same test.
- **Per-sample counts are exact only to quadrature.** The albedo from `evalSurface()` and the one from sampling agree to 1 percent, the quadrature's accuracy on a lobe 0.1 radians wide. The cell-by-cell weight test is the sharp one.
- **The furnace is one sphere per render.** A gallery of spheres reflects its neighbors, which breaks the exact values. `furnace_check.py` writes a scene with one sphere per material and strategy, and `assets/scenes/white_furnace.gltf` is a gallery for looking at.
- **The Cornell box has two references**, as the user chose. `radiosity.py` solves the empty box with exact point-to-polygon form factors and a final gather from each pixel's hit point. pbrt-v4 renders the box with its two blocks. The solve takes one sample at a pixel's center and the renderer averages the pixel, so the comparison uses the pixels whose 3 x 3 neighborhood shows one wall.
- **The sampler checks were already there.** `bin/sampler_test` has the star discrepancy, the elementary-interval strata and the determinism checks, from [pbrt-sample-generation.md](pbrt-sample-generation.md). This plan adds `tests/test_stats.h`, whose chi-square test the light and media plans can use.
- **Parity runs in both record modes** and compares directions to 1e-4, weights to 1e-3 and densities to 1e-3, not 1e-5 everywhere. A GGX density at roughness 0.2 divides by `(cos^2 (a^2 - 1) + 1)^2`, which cancels, so the GPU and the CPU differ by 2e-4 there. The plan's 1e-5 holds for the other quantities.
- **EXR comes with this plan**, as the user chose, with PFM beside it. The plot's slope is fitted over two octaves ending at a quarter of the reference's samples, since past that the reference's own noise bends the line.

### Findings

Reported to the user, who chose what to fix. Each has its evidence.

1. **A fully metallic `pbr` material divided 0 by 0. Fixed.** With `metallic = 1` the diffuse lobe has weight 0, so `specularChance` is exactly 1, and `scatterPbr()` took the diffuse branch when the lobe draw was not below it. That branch computes `diffuse * (1 - fresnelView) / (1 - specularChance)`, which is `0 / 0`. `bin/bsdf_test` found it at corner samples, and on the GPU kernel 09 dropped the NaN sample. A white fully metallic sphere dropped 1 sample in 16.8 million paths at 4, 16 and 64 samples per frame, 2 at 2 samples per frame, and 1403 at 64 x 64 pixels, 4096 samples and 1 sample per frame. The cause of the larger count was never understood: the guard removes it (that render now drops 0), so the NaN was the source, but the draw behind it was not exactly 1.0 as often as the arithmetic predicts. The fix is a guard, `diffuseWeight <= 0.0 ||`, before the lobe test in `scatterPbr()` and `src/bsdf.cpp`, with the lobe draw made before it so the path's random stream is unchanged. The first version of the guard skipped the draw, shifted the stream of every `pcg_hash` render of a fully metallic `pbr`, and the bit-identity comparison caught it. The guard is the robust part: on a GPU whose division is not IEEE, `specularChance` can come out a rounding step below 1.
2. **`randomFloat()` returned 1.0. Fixed.** For 128 of the 2^32 PCG words it converted the word to a float that rounds up to 2^32, though its documentation said `[0, 1)`. It now clamps to 0.99999994, which changes only those 128 words. `src/bsdf.cpp` mirrors it, and `bin/bsdf_test` checks the conversion of the top words and drives every material with PCG states whose first draw is one.
3. **Three materials are not reciprocal. Noted in [pbrt-bsdf-library.md](../pbrt-bsdf-library.md), [pbrt-bidirectional-path-tracing.md](../pbrt-bidirectional-path-tracing.md) and [physoptics-detectors.md](../physoptics-detectors.md).** The relative asymmetry of `f(wo, wi)` against `f(wi, wo)` is 0.7 to 0.95 for phong, 0.07 to 0.95 for the fuzzy metal, and 0.5 to 0.8 for the Lambertian lobe of `pbr` (the `1 - F(wo)` factor depends on one direction only). Lambertian and the GGX lobe are reciprocal to 1e-6. A unidirectional path tracer does not need it. Bidirectional integrators and the importance-transport light paths of the laser optics plans do. `bin/bsdf_test` reports the asymmetry for every case and fails only where a material is expected to be reciprocal.
4. **Schlick's approximation underestimates reflection inside glass. Noted in [pbrt-bsdf-library.md](../pbrt-bsdf-library.md) and [physoptics-optical-materials.md](../physoptics-optical-materials.md).** `scatterSurface()` calls `schlick()` with the incident cosine in both directions. Leaving glass of index 1.5 it gives 0.040 at 30 degrees and 0.041 at 40, where the exact Fresnel reflectance is 0.055 and 0.245 (0.380 at 41 degrees, just under the critical angle). From outside it is within 0.013 up to 70 degrees and 0.036 at 85. Light that should reflect inside a lens or a prism is transmitted. The dielectric also does not scale radiance by the square of the index on refraction, as the BSDF library plan says.
5. **The comment at the top of `crt_scatter.glsl` was wrong for `pbr`. Fixed.** It said the attenuation `scatterSurface()` returns is exactly `evalSurface() / pdfSurface()` for the direction it picked. That holds for lambertian, phong and the fuzzy metal. For `pbr` it is the weight of the chosen lobe, and it equals the ratio in expectation. The estimator is unbiased either way, which `bin/bsdf_test` checks per cell. The comment now says so.
6. **A microfacet material loses energy to single scattering**, as expected and as [pbrt-appearance-extras.md](../pbrt-appearance-extras.md) plans to compensate: in the furnace a white `pbr` sphere renders 0.9917 at roughness 0.5 and 0.9633 at roughness 1, and a metallic one 0.8854 and 0.3600. At roughness 0.2 the non-metallic one renders 1.0001 +- 0.0001, which the cheap layering of a Lambertian lobe under the specular one allows. No material renders above 1 by more than the noise.

## Verification

This plan's own verification is that it finds a planted bug. Multiply `pdfPbr()` by 1.05 in a scratch build: test 1 must fail, and the white furnace must stay unchanged, since the sampling itself is still correct.

## As built

### What exists

- `src/bsdf.h/.cpp`, the static library `rt_bsdf`: `scatterSurface()`, `evalSurface()`, `pdfSurface()` and their helpers, mirroring `crt_scatter.glsl` for the six original materials, with the random number routines they draw from. `BsdfDraws` holds the PCG state or a sample record, so the mirror runs both ways kernel 06 draws. Textures and the normal map are not mirrored.
- `tests/bsdf_test.cpp` and `tests/test_stats.h`, `bin/bsdf_test`: 130 configurations (lambertian, four phong, five fuzzy metals and a mirror at two settings, two dielectrics, twelve pbr, each at 0, 30, 60, 80 and 88 degrees).
- `src/gpu_selftest.h/.cpp`, `src/gpu_selftest_bsdf.cpp` and `shaders/test/bsdf_parity.comp`: **Windows** → **GPU self-tests**, or `VKGUIDE_SELFTEST=1`.
- `src/image_io.h/.cpp` (`rt_image`), `tests/image_diff.cpp` (`bin/image_diff`), and tinyexr in `../CPPLibraries/tinyexr/`.
- **Compare to reference**: **Save...**, **Load...**, MAPE, the mean difference, the convergence plot with each curve's slope, and up to eight kept curves.
- `VKGUIDE_VERIFY` in `VulkanEngine`, and `tools/verify/`: `make_scenes.py`, `furnace_check.py`, `cornell_check.py`, `radiosity.py`, `pbrt_cornell.py`, `cornell.py`, `vkscene.py` and `run_all.py`.
- `assets/scenes/white_furnace.gltf`, `cornell_reference.gltf`, `cornell_empty.gltf` and `assets/environment_maps/uniform_white.hdr`.

### Where it differs from the design

- **Test 2 holds per sample only for some materials.** `scatterSurface()` for `pbr` returns the weight of the lobe it chose, `f_lobe cos / (chance pdf_lobe)`, not `f cos / pdf` of the mixture. The estimator is still unbiased: the expected weight of the samples in a region equals the integral of `f cos` over it, because the MIS weight depends on the direction alone. The test therefore compares the mean weight per coarse cell with the integral of `evalSurface()` for every material, and the per-sample ratio only for lambertian, phong and metal. The comment at the top of `crt_scatter.glsl` says the attenuation is exactly `evalSurface() / pdfSurface()` for the picked direction. That holds for those three and not for `pbr`.
- **The grid follows the lobe.** A 64 x 128 grid in the surface's frame cannot resolve a lobe narrower than one cell. A case with a narrow lobe lays its rings around the mirror direction, with ring edges that thicken as the cube of the ring number, and each cell's density is integrated adaptively. The samples a material absorbs are one more cell, so a density that integrates to the wrong probability of keeping a sample fails the same test.
- **Per-sample counts are exact only to quadrature.** The albedo from `evalSurface()` and the one from sampling agree to 1 percent, the quadrature's accuracy on a lobe 0.1 radians wide. The cell-by-cell weight test is the sharp one.
- **The furnace is one sphere per render.** A gallery of spheres reflects its neighbors, which breaks the exact values. `furnace_check.py` writes a scene with one sphere per material and strategy, and `assets/scenes/white_furnace.gltf` is a gallery for looking at.
- **The Cornell box has two references**, as the user chose. `radiosity.py` solves the empty box with exact point-to-polygon form factors and a final gather from each pixel's hit point. pbrt-v4 renders the box with its two blocks. The solve takes one sample at a pixel's center and the renderer averages the pixel, so the comparison uses the pixels whose 3 x 3 neighborhood shows one wall.
- **The sampler checks were already there.** `bin/sampler_test` has the star discrepancy, the elementary-interval strata and the determinism checks, from [pbrt-sample-generation.md](pbrt-sample-generation.md). This plan adds `tests/test_stats.h`, whose chi-square test the light and media plans can use.
- **Parity runs in both record modes** and compares directions to 1e-4, weights to 1e-3 and densities to 1e-3, not 1e-5 everywhere. A GGX density at roughness 0.2 divides by `(cos^2 (a^2 - 1) + 1)^2`, which cancels, so the GPU and the CPU differ by 2e-4 there. The plan's 1e-5 holds for the other quantities.
- **EXR comes with this plan**, as the user chose, with PFM beside it. The plot's slope is fitted over two octaves ending at a quarter of the reference's samples, since past that the reference's own noise bends the line.

### Findings

Reported to the user, and not fixed, as they chose. Each has its evidence.

1. **A fully metallic `pbr` material divides 0 by 0.** With `metallic = 1` the diffuse lobe has weight 0, so `specularChance` is exactly 1, and `scatterPbr()` takes the diffuse branch when the lobe draw is not below it. That branch computes `diffuse * (1 - fresnelView) / (1 - specularChance)`, which is `0 / 0`. `bin/bsdf_test` finds it at corner samples (a lobe draw of exactly 1.0, which `randomFloat()` returns for the top 128 of 2^32 PCG words) and prints it as KNOWN; **--strict** fails on it. On the GPU, kernel 09 drops the NaN sample and counts it. A white fully metallic sphere drops 1 sample in 16.8 million paths at 4, 16 and 64 samples per frame, 2 at 2 samples per frame, and 1403 at 64 x 64 pixels, 4096 samples and 1 sample per frame. The cause of the larger count is not understood: with the one-line guard below it drops 0 in that last render, so the NaN is the source, but the draw behind it is not exactly 1.0 as often as the arithmetic predicts. The pixels stay finite, and each dropped sample biases its pixel by one sample's worth. A guard that costs nothing elsewhere is `diffuseWeight <= 0.0 ||` before the lobe test in `scatterPbr()` and in `src/bsdf.cpp`, but any edit near that branch can move the bits of renders that do not use it (the sample-generation plan's trap), so it needs the bit-identity comparison.
2. **`randomFloat()` returns 1.0** for 128 of the 2^32 PCG words, because it converts the whole word to a float, and `float(0xFFFFFF80)` rounds up to 2^32. Its documentation says `[0, 1)`. Every consumer except finding 1 handles it: the alias table clamps its index, roulette terminates, the dielectric reflects. Converting the top 24 bits, as `samplerToFloat()` does, would remove it and change the stream, so every `pcg_hash` render would differ.
3. **Three materials are not reciprocal.** The relative asymmetry of `f(wo, wi)` against `f(wi, wo)` is 0.7 to 0.95 for phong, 0.07 to 0.95 for the fuzzy metal, and 0.5 to 0.8 for the Lambertian lobe of `pbr` (the `1 - F(wo)` factor depends on one direction only). Lambertian and the GGX lobe are reciprocal to 1e-6. A unidirectional path tracer does not need it. The bidirectional integrators of [pbrt-bidirectional-path-tracing.md](../pbrt-bidirectional-path-tracing.md) and the importance-transport light paths of the laser optics plans do. `bin/bsdf_test` reports the asymmetry for every case and fails only where a material is expected to be reciprocal.
4. **Schlick's approximation underestimates reflection inside glass.** `scatterSurface()` calls `schlick()` with the incident cosine in both directions. Leaving glass of index 1.5 it gives 0.040 at 30 degrees and 0.041 at 40, where the exact Fresnel reflectance is 0.055 and 0.245 (0.380 at 41 degrees, just under the critical angle). From outside it is within 0.013 up to 70 degrees and 0.036 at 85. Light that should reflect inside a lens or a prism is transmitted. [pbrt-bsdf-library.md](../pbrt-bsdf-library.md) replaces it with the exact term. The dielectric also does not scale radiance by the square of the index on refraction, as that plan says.
5. **The comment at the top of `crt_scatter.glsl` is wrong for `pbr`.** It says the attenuation `scatterSurface()` returns is exactly `evalSurface() / pdfSurface()` for the direction it picked. That holds for lambertian, phong and the fuzzy metal. For `pbr` it is the weight of the chosen lobe. The estimator is unbiased either way, which `bin/bsdf_test` checks per cell.
6. **A microfacet material loses energy to single scattering**, as expected and as [pbrt-appearance-extras.md](../pbrt-appearance-extras.md) plans to compensate: in the furnace a white `pbr` sphere renders 0.9917 at roughness 0.5 and 0.9633 at roughness 1, and a metallic one 0.8854 and 0.3600. At roughness 0.2 the non-metallic one renders 1.0001 +- 0.0001, which the cheap layering of a Lambertian lobe under the specular one allows. No material renders above 1 by more than the noise.

## Verification

The fixes to findings 1, 2 and 5 changed `crt_scatter.glsl` and `crt_random.glsl`. 42 renders (the five older scenes under seven sampler and strategy selections, 160 x 90 at 8 samples, and a fully metallic `pbr` sphere under the same seven) are bit-identical before and after. The other edited shader is `image_error.comp`, and the added ones are under `shaders/test/`. After the fixes the GPU self-test passes with the same differences, `bin/bsdf_test --strict` passes with no known finding, and the 64 x 64, 4096-sample, 1-sample-per-frame render of the fully metallic sphere drops 0 samples, where it dropped 1403.

- `bin/bsdf_test` runs 880 chi-square tests at one million samples (Sidak threshold 1.1e-5 for a family error of 0.01; the smallest p is 0.0042) and finds no failure in 29 s on the development machine. **--selfcheck** plants a density 5% high (151 checks fail with **--quick**), an evaluation 5% high (428) and a sampling roughness 10% high (440), and the unplanted model passes.
- The GPU self-test compares 38912 inputs per mode. No kept or absorbed decision and no count of random draws differs. The worst relative differences are 1.6e-4 in attenuation, 6.5e-5 in direction and 1.9e-4 in density, inside the tolerances. The plan's planted check, `pdfPbr()` times 1.05 in `crt_scatter.glsl`, fails 14726 inputs of each mode with a density difference of 0.048.
- The white furnace (`furnace_check.py`, 512 samples at 64 x 64) passes. A white Lambertian renders 1 and 0.5 for reflectance 1 and 0.5, a mirror and glass render 1, to the printed precision of 1e-5. Every other material renders at most 1. The 10 groups of the matrix (strategy and background, sampler, traversal and intersection flags) agree within 4 standard errors and 0.001.

| Material | Renders |
|---|---|
| phong, smoothness 0.5 | 0.9643 |
| `pbr`, roughness 0.2, 0.5, 1, non-metallic | 1.0001, 0.9917, 0.9633 |
| `pbr`, roughness 0.2, 0.5, 1, metallic | 0.9970, 0.8854, 0.3600 |

- The planted density in the furnace: with `pdfPbr()` times 1.05 in `crt_scatter.glsl`, the furnace of the non-metallic `pbr` sphere at roughness 0.5 renders the same in every strategy, background and sampler (0.9917 to 0.9920, against 0.9916 to 0.9919). Both MIS weights use the one wrong density, so they still sum to 1, as the plan expected.
- The Cornell box (`cornell_check.py`, 128 x 128 at 1024 samples, in 16 x 16 pixel blocks, away from edges). Against the radiosity solve of the empty box the mean radiance is within 0.08 percent in every channel and no block is outside 4 standard errors plus 2 percent (the worst is 0.28 of its allowance). pbrt-v4 agrees with the solve to 0.6 percent in red and 0.2 percent or less in green and blue, and its direct view of the light reads 14.993 for 15. The renderer with its two blocks agrees with pbrt to 0.3 percent in red and 0.2 percent or less in green and blue, with no block outside 4 standard errors plus 4 percent. The first version of the solve let the light shine on the ceiling above it, which the light's black back side blocks, and read 26 percent high: the comparison found the solve's own error.
- `bin/light_test`, `bin/sampler_test` and `bin/bvh_bench` on `structure.glb` pass.
- An EXR and a PFM written and read back are identical to the bit. Saving, loading and the convergence history were run through a temporary hook in a render of the Cornell box: a curve is kept when a new render starts, and loading a reference clears the kept curves. The panel's plot was drawn but not looked at.

## Sources

PBR 4ed §2.1.4 (MSE and its estimation), Figure 5.24 (float precision flattening MSE), §9.5.2 (reciprocity of BTDFs); pbrt-v4 `src/pbrt/bsdfs_test.cpp` (χ² tests); Jung et al. 2020 and Subr and Arvo 2007 (statistical tests for Monte Carlo sampling), cited in PBR 4ed chapter 2 "Further Reading".
