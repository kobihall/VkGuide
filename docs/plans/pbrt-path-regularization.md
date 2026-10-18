# Path regularization

A diffuse floor next to a chrome wheel under a sky with a small bright sun is the case PBR 4ed Figure 13.12 shows: a BSDF sample from the floor rarely hits the wheel in a direction that reflects to the sun, and when it does, the pixel spikes. Light sampling cannot help, because the wheel is specular. pbrt-v4's `PathIntegrator` offers path regularization (Kaplanyan and Dachsbacher 2013): once a path has scattered off a non-specular surface, every later BSDF is made rougher, so that next-event estimation and MIS can reach the paths BSDF sampling alone finds too rarely. It is biased, and off by default. This plan adds it as a setting.

*Not implemented. Most of its effect needs the microfacet BxDFs of [pbrt-bsdf-library.md](pbrt-bsdf-library.md); before them it applies to the `pbr` and `metal` materials only. Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- Kernel 06 has no record of what earlier bounces scattered off. `isSpecularMaterial()` (`crt_scatter.glsl`) treats `dielectric` and `metal` with fuzz below `CRT_SPECULAR_FUZZ` as specular, and those vertices skip next-event estimation.
- Nothing changes a material's roughness during a path.

## Design

### The flag

`PathState.flags` (the roadmap assigns `pad1`) gains `CRT_PATH_ANY_NON_SPECULAR`, set by kernel 06 whenever it samples a non-specular lobe, and never cleared. pbrt tracks the same thing as `anyNonSpecularBounces`. Kernel 00 clears it.

### Roughening

A **Regularize** setting. When it is on and the flag is set, kernel 06 roughens the material before sampling or evaluating it:

| Material | Roughening |
|---|---|
| New microfacet BxDFs (conductor, dielectric, coated layers) | pbrt's `TrowbridgeReitzDistribution::Regularize()`: each of `αx`, `αy` below 0.3 becomes `clamp(2α, 0.1, 0.3)` |
| `pbr` | the same rule on its GGX `α` |
| `metal` | the same rule on its fuzz radius, by analogy; a specular metal becomes a fuzz-0.1 metal, which has a pdf (`fuzzBallPdf()`) |
| `dielectric` (legacy), `lambertian`, `phong`, thin dielectric | unchanged: the legacy glass has no rough form, and pbrt leaves `ThinDielectricBxDF::Regularize()` unimplemented |

A roughened specular surface is no longer specular, so it takes a light sample and writes a real BSDF pdf into the MIS record. `isSpecularMaterial()` and the MIS record read the roughened material, not the original.

Camera rays and purely specular chains are never roughened, so mirrors and glass seen directly or through each other look as authored. PBR 4ed Figure 13.13 compares roughening everything with roughening after the first non-specular bounce: only the second keeps the car's appearance.

### Consistency

pbrt's regularization is biased and stays biased at any sample count. Kaplanyan and Dachsbacher's original shrinks the regularization as samples accumulate, which makes it consistent. A **Shrink with samples** option shrinks the roughening as the pixel's sample count grows, at the rate their paper prescribes, which resembles a progressive photon-mapping radius. In the limit no roughening remains. It is off by default, since pbrt does not do it and the decay trades some of the variance reduction back.

## Registry, bindings and data

- `CRT_PATH_ANY_NON_SPECULAR` in `PathState.flags`.
- Settings: **Regularize**, **Shrink with samples**.
- No new binding or variant.

## Verification

- With **Regularize** off, every scene renders bit-identical.
- A test scene after Figure 13.12: a diffuse floor, a chrome sphere and a small bright environment sun ([pbrt-environment-lighting.md](pbrt-environment-lighting.md) supplies the sun). With regularization, the floor's fireflies go away at equal samples, and **Compare to reference** against an unregularized 64k-spp render shows the bias, which is the price.
- With **Shrink with samples**, the error against that reference keeps falling as samples accumulate, where plain regularization levels off.
- Mirrors and glass seen directly render identically with and without regularization.

## Order of work

1. The flag. Bit-identical.
2. Roughening for `pbr` and `metal`.
3. The microfacet BxDFs, as [pbrt-bsdf-library.md](pbrt-bsdf-library.md) adds them.
4. **Shrink with samples**.

## Sources

PBR 4ed §13.4.1 (path regularization, Figures 13.12 and 13.13), §9.6.1 (`TrowbridgeReitzDistribution`); Kaplanyan and Dachsbacher 2013, *Path space regularization for holistic and robust light transport*; pbrt-v4 `src/pbrt/cpu/integrators.cpp`, `util/scattering.h`, `bxdfs.h`.
