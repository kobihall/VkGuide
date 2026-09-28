# Resampled direct lighting

Every direct-lighting strategy today draws its light sample from a density that ignores the BSDF and the cosine at the shading point, so on a glossy surface most light samples land where the BSDF is near zero. Resampled importance sampling (RIS, Talbot et al. 2005) draws several cheap candidates, weighs each by its unshadowed contribution, and keeps one, which approximates sampling the full product with a single shadow ray. ReSTIR (Bitterli et al. 2020) goes further and shares the kept candidates between neighbouring pixels and across frames. PBR 4ed covers the reservoir sampling both are built on (§A.2) and cites both in its "Further Reading", and neither is in pbrt. The user chose to plan them as research. This plan adds RIS and ReSTIR as kernel 06 variants.

*Not implemented, research. Benefits from [bvh-light-sampler.md](bvh-light-sampler.md) (a better candidate density) and relies on [thin-hit-record.md](thin-hit-record.md) (a 24-byte hit to store per pixel). Part of the [roadmap](roadmap.md).*

## What the renderer does today

- Kernel 06's MIS variants take one light sample from the light selection and the shape sampler, weigh it against the BSDF with the balance or power heuristic, and emit one shadow ray.
- Kernels 03 and 04 weigh emission that a BSDF sample finds with `misWeight(path.misPdf, lightPdf, path.misExponent)`, where `lightPdf` is the selection and shape density.
- Nothing is shared between pixels or frames. Each frame's samples are independent, which is what makes the progressive mean converge.

## Design

### Weighted reservoirs

`crt_reservoir.glsl` holds pbrt's `WeightedReservoirSampler` (PBR 4ed §A.2): a streaming choice of one item from a sequence with probability proportional to its weight, keeping only the chosen item, the weight sum and the count. Two reservoirs merge in O(1). The C++ mirror goes into `rt_light`, and `bin/light_test` checks that the chosen index follows the weights (χ²) and that merging two reservoirs chooses as one reservoir over both streams would.

### RIS

A new variant, `0606_surface_scatter_ris`:

1. Draw M candidates `xᵢ` from the source density `p`, the light selection times the shape sampler, exactly what the MIS variants draw one sample from.
2. Weigh each by `wᵢ = p̂(xᵢ) / p(xᵢ)`, where the target `p̂` is the unshadowed contribution `f(ωo, ωi) Le |cos θ| / d²` (for the environment, without `d²`), and stream it into a reservoir.
3. Trace one shadow ray to the chosen candidate `y` and add `f(y) Le(y) V(y) / p̂(y) × (1/M) Σ wᵢ`.

The estimator is unbiased wherever `p̂` is positive when the integrand is (Talbot 2005), and leaving visibility out of `p̂` keeps it so. Because the candidates include the environment, RIS also approximates the environment × BSDF product sampling that [environment-lighting.md](environment-lighting.md) leaves as research.

RIS against BSDF sampling: the chosen candidate's contribution is multiplied by the MIS weight computed from the source density, `misWeight(p(y), p_bsdf(y))`, and `misPdf` is written as in the MIS variants. Kernel 04 weighs a BSDF hit with the same two densities, so the two weights still sum to 1 for every path and the combination stays unbiased. The RIS density itself has no closed form and is never needed. Kernels 03 and 04 do not change.

Settings: **Candidates** (M, 1 to 32, default 8) and **Target**: with the BSDF (the full unshadowed contribution) or without it (`Le |cos θ| / d²`, cheaper when the material evaluation is expensive).

Each candidate needs its own random numbers. The first comes from `RaySamples.direct` ([sample-generation.md](sample-generation.md)) and the rest from the path's PCG stream, as the record's spill rule says.

### ReSTIR

A second variant, `0607_surface_scatter_restir`, applies reuse at the first bounce, where neighbouring paths are neighbouring pixels. Later bounces behave as `0606`.

- At bounce 0, kernel 06 runs RIS as above but stores the reservoir instead of tracing its shadow ray: the chosen light sample, the weight sum, the candidate count and the target value at the owner. It also stores the owner's thin hit record and outgoing direction, so another pixel can rebuild this surface with `crt_interaction.glsl` and evaluate its BSDF.
- Temporal reuse merges in the previous frame's reservoir of the same pixel, which is valid because the camera does not move during a render (any camera change restarts it). Its count is capped at 20 × M, as Bitterli et al. do, so that old samples do not dominate.
- For spatial reuse, a reuse pass merges k reservoirs (default 5) from random pixels within a radius (default 30 pixels) whose first hit is similar: normals within 25° and depths within 10 %.
- The reuse pass then emits the one shadow ray for the pixel's final reservoir.

The reuse pass must run after every pixel's initial reservoir exists and before kernel 08. `KernelVariant` gains `reusePass`, the shader of a second pipeline that the variant owns (`0607_surface_scatter_restir_reuse.comp`). The path schedule dispatches it at bounce 0, between kernel 06 and kernel 08, when the selected kernel 06 variant has one. This follows `cameraPass` in [sample-generation.md](sample-generation.md), and needs no new slot.

A reservoir's sample is valid for its owner's target, not a neighbour's. Merging with plain weights, as in Bitterli et al.'s biased variant, darkens contact shadows and edges. A **Combination** setting offers:

- **Pairwise MIS** (default): the generalized RIS weights of Lin et al. 2022 with Bitterli's pairwise MIS, which evaluate each merged sample's target at the other pixels involved. Unbiased without extra shadow rays, since the targets carry no visibility, at O(k) target evaluations per pixel.
- **Biased** is Bitterli et al. 2020's simple merge, cheaper, for comparison.

Each frame's estimate stays unbiased with pairwise MIS, so the progressive mean still converges to the reference. Temporal reuse correlates successive frames, which can slow that convergence even while it lowers each frame's noise. Measuring both effects is the point of the research.

ReSTIR runs with one sample per pixel per frame: the panel forces **Samples per frame** to 1 while it is selected, since a pixel's neighbours are other pixels' first samples.

### Later

- ReSTIR GI (Ouyang et al. 2021) reuses indirect samples the same way, with reconnection shifts.
- Resampling distances in media, the product sampling of Wrenninge and Villemin 2020 that PBR 4ed exercise 14.5 describes, belongs to [participating-media.md](participating-media.md).

## Registry, bindings and data

- Variants `0606_surface_scatter_ris` and `0607_surface_scatter_restir`, with `0607`'s reuse pipeline. `KernelVariant::reusePass`.
- Binding 38 `Reservoirs`: per pixel, two reservoirs (this frame's and the previous), each 32 bytes of reservoir and 32 bytes of the owner's hit record and direction. 256 MB at 1920 × 1080; the panel shows the size.
- Settings: **Candidates**, **Target**, and for `0607` **Neighbours**, **Radius**, **Temporal reuse**, **History cap** and **Combination**.

## Verification

- No existing variant changes: every scene renders bit-identical under 0601 to 0605.
- `bin/light_test`: the reservoir sampler's choice probabilities and merges, as above.
- RIS with M = 1 converges to the same image as MIS with the same heuristic, and so does every M (**Compare to reference**).
- ReSTIR with pairwise MIS converges to the 16384-spp reference on the Veach scene and the 980-light scene; the biased combination shows its bias as a darkening measured by the relative mean error.
- Relative MSE after 1, 4 and 16 frames for MIS, RIS (M = 8, 32) and ReSTIR on the same two scenes, at equal frame count and equal time. This is where ReSTIR should win.

## Order of work

1. The reservoir and its tests.
2. RIS.
3. ReSTIR's initial candidates and spatial reuse with the biased combination.
4. Pairwise MIS.
5. Temporal reuse.

## Sources

PBR 4ed §A.2 (weighted reservoir sampling), chapter 2 "Further Reading" (resampled importance sampling), chapter 12 "Further Reading" (spatiotemporal resampling of light samples), exercise 14.5; Talbot et al. 2005, *Importance resampling for global illumination*; Bitterli et al. 2020, *Spatiotemporal reservoir resampling for real-time ray tracing with dynamic direct lighting*; Bitterli 2022, *Correlations and reuse for fast and accurate physically based light transport* (pairwise MIS); Lin et al. 2022, *Generalized resampled importance sampling: foundations of ReSTIR*; Ouyang et al. 2021, *ReSTIR GI*; Wrenninge and Villemin 2020, *Product importance sampling of the volume rendering equation using virtual density segments*.
