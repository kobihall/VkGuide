# Direct lighting strategies

Kernel 06 has four variants: BSDF sampling, light sampling, and MIS with the balance or the power heuristic. All four take one light sample (or none) and one BSDF sample per vertex. The sources describe three more ways to spend samples: *The Rest of Your Life*'s one-sample MIS over a mixture density, which needs no shadow ray; pbrt's light-sample splitting (PBR 4ed §2.2.5), several light samples per BSDF sample; and pbrt 3rd edition's `UniformSampleAllLights`, a sample from every light at every vertex. This plan adds the first as a kernel 06 variant and the other two as settings that the existing variants honour.

*Not implemented. Uses the light selection setting of [bvh-light-sampler.md](bvh-light-sampler.md). Part of the [roadmap](roadmap.md).*

## What the renderer does today

- `crt_surface.glsl` is one body, compiled per variant with `CRT_DIRECT_LIGHTING` (BSDF, light, MIS) and `CRT_MIS_EXPONENT` (1 or 2).
- Every variant light-samples delta lights, so that all four converge to one image.
- Kernel 06 writes `PathState.misPdf`: the BSDF sample's density, `CRT_MIS_FULL` for full weight, or 0 when light sampling covers emission alone. Kernels 03 and 04 weigh emission with `misWeight(path.misPdf, lightPdf, path.misExponent)`.
- The shadow queue holds one ray per path slot.
- The render guard counts two traversals per bounce when shadow rays are on.

## Design

### One-sample MIS with a mixture density

*The Rest of Your Life* ("Mixing densities") samples the next direction from `p = f p_light + (1 − f) p_bsdf`, choosing the light with probability `f` and the BSDF otherwise, and divides by the mixture's density. No shadow ray is cast: the path continues along the chosen direction, and whatever emission its next hit finds counts at full weight, because the mixture density already accounts for both ways of finding it. This is Veach's one-sample model with the balance heuristic.

A new variant, `0605_surface_scatter_mis_mixture`, is `crt_surface.glsl` with `CRT_DIRECT_LIGHTING` set to a new `CRT_DIRECT_MIXTURE`:

- It writes `misPdf = CRT_MIS_FULL`, so kernels 03 and 04 need no change.
- Delta lights have no density to mix, so they still get a shadow ray, as in the other variants.
- A specular BSDF skips the mixture and samples itself.
- A **Light fraction** setting sets `f`, 0.5 by default as in the book.

Per sample it costs one ray fewer than MIS, and it is noisier per sample. Which wins at equal time is the question the verification answers.

### Light-sample splitting

A **Light samples** setting, N from 1 to 8, for the light and MIS variants: kernel 06 takes N light samples, each with its own shadow ray carrying `1/N` of its contribution. The BSDF sample still counts once.

Veach's multi-sample MIS weighs the light technique by its count, `w_bsdf = p_bsdf / (p_bsdf + N p_light)` for the balance heuristic. Both heuristics depend only on the ratio of the two densities, so kernel 06 writes `misPdf = p_bsdf / N`, and kernels 03 and 04 compute the right weight without knowing N.

The extra samples need random numbers. [sample-generation.md](sample-generation.md)'s `RaySamples` holds one light sample per bounce, and consumers that need more draw them from the path's PCG stream, so for N above 1 the first light sample comes from the sampler and the rest from PCG.

### Sampling every light

**All lights** joins the light selection setting of [bvh-light-sampler.md](bvh-light-sampler.md): every light is sampled at every vertex with probability 1, and `lightPmf()` returns 1, so kernels 03 and 04 weigh BSDF hits correctly. With **Light samples** at N, each light gets N samples, which is pbrt 3rd edition's `UniformSampleAllLights` with `nSamples` per light.

It suits a scene with a few lights where every light matters everywhere, such as the Veach scene. Past 16 lights the panel greys it out, since the shadow queue would need 16 rays per slot.

### Shadow queue capacity

Splitting and all-lights sampling put up to `N × lights` shadow rays in the queue per vertex. The pool is sized so the shadow queue fits the memory budget: with 48-byte shadow rays ([thin-hit-record.md](thin-hit-record.md)) and 4 shadow rays per slot, a 4M-slot pool would need 768 MB, so the pool shrinks to fit. The render guard counts `1 + shadow rays per vertex` traversals per bounce.

### Later

- Variance-aware MIS weights (Grittmann et al. 2019), and the optimal weights of Kondapaneni et al. 2019, which can be negative. Both change the weights, not the samples, and both would be kernel 06 variants.
- Adapting the light fraction or N per pixel from the variance each technique shows (Pajot et al. 2011, Lu et al. 2013).

## Registry, bindings and data

- `0605_surface_scatter_mis_mixture`: the same bindings as 0604.
- Settings: **Light fraction** (0605), **Light samples** (0602 to 0604), and **All lights** in the light selection setting. All saved with the render settings.
- No new binding.

## Verification

- With **Light samples** at 1 and a variant that exists today, every scene renders bit-identical.
- Every strategy converges to the same 16384-spp reference on the four scenes of `shadow-rays-nee.md` §9, with means within 0.1 %.
- That section's relative-MSE table at 4096 spp gains a column for 0605, and rows for N = 2 and 4 and for all-lights sampling, with the frame time of each, so the table reads at equal time as well as equal samples.

## Order of work

1. Splitting, since it changes only kernel 06's loop and the queue sizing.
2. The mixture variant.
3. All-lights sampling, after the light selection setting exists.

## Sources

Shirley, *Ray Tracing: The Rest of Your Life*, "Mixing densities" and "Some architectural decisions"; PBR 4ed §2.2.3 (MIS, one-sample model), §2.2.5 (splitting), §13.4 (the path integrator's direct lighting); PBR 3ed §14.3 (`DirectLightingIntegrator`, `UniformSampleAllLights`); Veach 1997 §9.2 (multi-sample and one-sample MIS); Grittmann et al. 2019, *Variance-aware multiple importance sampling*; Kondapaneni et al. 2019, *Optimal multiple importance sampling*; Pajot et al. 2011, *Representativity for robust and adaptive multiple importance sampling*; Lu, Pacanowski and Granier 2013, *Second-order approximation for variance reduction in multiple importance sampling*.
