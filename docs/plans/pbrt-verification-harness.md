# Verification harness

Several plans add sampling code whose bugs do not show as crashes or obvious artifacts: a BSDF whose sampling density disagrees with its `pdf()` by a few percent renders a plausible image with the wrong brightness under MIS. `bin/light_test` already catches that class of bug for lights. This plan builds the equivalent for BSDFs and samplers, adds scene-level energy tests, and extends **Compare to reference** so that convergence rates, not only single errors, can be measured.

*Not implemented. Depends on [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) only for the GPU parity pass. Build it before [pbrt-bsdf-library.md](pbrt-bsdf-library.md), which relies on it. Part of the [roadmap](pbrt-roadmap.md).*

## What exists today

- `bin/light_test` checks every light's sampled density against its density function and its solid angle against a counted estimate, through the C++ mirror of `crt_light.glsl`.
- `bin/bvh_bench` checks every traversal against brute force.
- The materials in `crt_scatter.glsl` (lambertian, metal, phong, dielectric, pbr) have no C++ mirror and no test. Their `evalSurface()` and `pdfSurface()` were derived by hand to agree with `scatterSurface()`, and nothing checks that they do.
- **Compare to reference** reports RMSE and relative MSE against a stored render, one number per frame.

## Design

### A C++ mirror of the existing materials

Start the `rt_bsdf` static library (no Vulkan, `-O2`, like `rt_light`) by mirroring `crt_scatter.glsl` line for line: `scatterSurface()`, `evalSurface()`, `pdfSurface()` and the pbr helpers, with the same PCG. From then on every BSDF in [pbrt-bsdf-library.md](pbrt-bsdf-library.md), [pbrt-layered-materials.md](pbrt-layered-materials.md) and the other appearance plans lands in both places.

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

- `white_furnace.gltf`: a sphere of each material inside a uniform environment of radiance 1, no other lights. A passive material that conserves energy renders exactly 1 wherever it reflects everything and below 1 elsewhere; any pixel above 1 by more than the noise is a bug. Microfacet materials show the energy they lose to single scattering, which [pbrt-appearance-extras.md](pbrt-appearance-extras.md) then compensates.
- `cornell_reference.gltf`: the Cornell box with a published reference render. Its role is to catch errors every internal test shares, such as a wrong factor of π in the light's power.

### Convergence measurement

**Compare to reference** gains:

- A log–log plot of relative MSE against samples per pixel, kept for each render compared against the same reference, so that two strategies can be compared on one plot. An unbiased estimator draws a line of slope −1; a flatter line past about 1000 spp points to accumulation precision, which [pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md) fixes (PBR 4ed Figure 5.24).
- Mean absolute percentage error beside RMSE and relative MSE.
- Saving and loading the reference as an EXR (through [pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md)), so that a reference rendered once can be reused across sessions and compared against pbrt's own output ([pbrt-scene-import.md](pbrt-scene-import.md)).

### Optics

The laser optics plans follow the same pattern: `bin/optics_test` for surfaces, glasses, coatings and element builders, `bin/beam_test` for the Gaussian beam solver, and system-level scenes with references (OpticStudio, Optiland, LightPipes, closed forms) in [physoptics-verification-scenes.md](physoptics-verification-scenes.md). A coating test adds energy conservation, `R + T + A = 1`, to this plan's tests.

### GPU parity

The mirrors guarantee nothing if the GLSL drifts from the C++. A later step adds `shaders/test/bsdf_parity.comp`, which evaluates a fixed batch of (material, wo, wi, random numbers) on the GPU, and a **Run GPU self-tests** item under **Windows** that reads the results back and compares them with `rt_bsdf` to 1e-5. It runs inside the application, since the tests need the Vulkan device.

## Order of work

1. `rt_bsdf` with the mirror of `crt_scatter.glsl`, and `bin/bsdf_test` running tests 1 to 3 on the existing materials. Fix, or record as known, whatever they find. The phong and metal densities are the likeliest candidates, since they were derived by hand.
2. The white furnace and Cornell reference scenes.
3. The convergence plot and MAPE.
4. The GPU parity pass.

## Verification

This plan's own verification is that it finds a planted bug. Multiply `pdfPbr()` by 1.05 in a scratch build: test 1 must fail, and the white furnace must stay unchanged, since the sampling itself is still correct.

## Sources

PBR 4ed §2.1.4 (MSE and its estimation), Figure 5.24 (float precision flattening MSE), §9.5.2 (reciprocity of BTDFs); pbrt-v4 `src/pbrt/bsdfs_test.cpp` (χ² tests); Jung et al. 2020 and Subr and Arvo 2007 (statistical tests for Monte Carlo sampling), cited in PBR 4ed chapter 2 "Further Reading".
