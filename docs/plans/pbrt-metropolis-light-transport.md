# Metropolis light transport

Every other integrator draws each sample independently, so a path that carries light through a hard-to-find route, such as the gap of a door left ajar, is found only as often as its probability says. Metropolis light transport (MLT) finds such a path once and then explores its neighborhood by mutating it, spending samples in proportion to each path's contribution to the image. pbrt 3 implements it in primary sample space (Kelemen et al. 2002), where a path is the vector of random numbers that generated it, and multiplexes the strategies of its BDPT (Hachisuka et al. 2014; PBR 3ed §16.4). The user chose primary-sample-space MLT over the existing path tracer first, and multiplexed MLT over pbrt 3's BDPT after it. This plan adds both as integrators.

*Not implemented. Depends on [pbrt-sample-generation.md](completed/pbrt-sample-generation.md) (the sample record the chains write), and the splat buffer of [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md). Multiplexed MLT also needs that plan's `bdpt` integrator. Part of the [roadmap](pbrt-roadmap.md).*

## Decision already made

On 2026-09-26 the user chose primary-sample-space MLT (PSSMLT) over the path tracer first, then multiplexed MLT (MMLT) over pbrt 3's BDPT, over either alone.

## What the renderer does today

- Samples are independent. Kernel 01 will write each bounce's random numbers into `RaySamples` ([pbrt-sample-generation.md](completed/pbrt-sample-generation.md)), from which every later kernel reads, and the dimension layout is fixed: 0 to 5 for the camera, then 12 per bounce.
- Every contribution lands in its path's own pixel; the splat buffer arrives with [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md).

## Design

### Primary sample space

A path is a deterministic function of its primary samples, the vector `X` of uniform numbers the sampler provides. Mutating `X` and re-tracing the path explores path space without knowing anything about the path tracer's internals. The sample record makes this a sampler change:

- `X` includes the pixel position (dimensions 0 and 1), so a chain moves across the image and its contributions are splats.
- Large steps, with probability 0.3 (pbrt's `largestepprobability`), replace `X` with fresh uniform numbers, which keeps the chain ergodic.
- Small steps perturb each coordinate by a normal offset with σ = 0.01 (pbrt's `sigma`), wrapping around [0, 1).
- Mutation is lazy, as in pbrt's `MLTSampler`: each coordinate records the iteration it was last changed in, and a coordinate is brought up to date only when a path reads it. Paths that end early never touch the dimensions of later bounces.

### PSSMLT, `pssmlt`

The integrator runs the path tracer's 00 and 02 to 08 unchanged, with the selected traversal, and adds two stages to the library ([integrator-layer.md](completed/integrator-layer.md)) as stages 22 and 23, which the roadmap reserves. They are stages rather than variants of 01 and 09, because a chain sampler or an accept step would break the path tracer if it could be selected there:

| Stage | Runs where the path tracer runs | What it does |
|---|---|---|
| 22 mutate chain samples | 01 | brings the chain's proposed `X` up to date for this bounce's dimensions and writes `RaySamples` from it, including the camera pass |
| 23 accept and splat | 09 | the proposal's contribution `I(X') = luminance(L')`, acceptance `a = min(1, I(X') / I(X))`, then splat both the current and the proposed path with Veach's expected-value weights, `(1 − a) L / I(X)` and `a L' / I(X')`, times `b / M`, and keep or restore `X` |

`b` is the image's mean contribution and `M` the mutations per pixel. Each chain is one pool slot, so a frame advances every chain by one mutation.

Before the chains start, a bootstrap traces 100,000 independent paths (pbrt's `bootstrapsamples`), measures each one's `I`, and estimates `b` as their mean. Each chain starts from a bootstrap path picked in proportion to its `I`, with an alias table, which removes start-up bias. The chain is seeded with that path's random seed, so its `X` regenerates exactly.

pbrt's default is 1000 chains. A GPU needs far more to fill the pool: 65,536 chains by default. Each chain keeps its `X` (up to 198 dimensions, 16 bytes each for the value, backup and modification counters) in binding 44, `MarkovChains`: about 200 MB at that count. More chains correlate less but spend more samples in bootstrap-like early phases; the **Chains** setting trades them.

The displayed image is the splat buffer scaled by `b` over the mutations per pixel so far. It converges to the path tracer's image, with noise that looks different: correlated streaks rather than grain.

### MMLT, `mmlt`

Hachisuka et al. 2014, as pbrt 3's `MLTIntegrator`, over the `bdpt` integrator:

- Each chain has a fixed path length `d`, chosen at bootstrap in proportion to each length's share of `b`.
- `X` has three streams, as pbrt's `MLTSampler` interleaves them: the camera subpath, the light subpath, and the connection. Its first number picks one strategy `(s, t)` with `s + t = d + 2`, uniformly.
- A proposal generates only `t` camera vertices and `s` light vertices, evaluates that one connection, and weighs it by the number of strategies times its MIS weight. The chain therefore moves between strategies as well as paths, and spends its time on whichever strategy captures the light best.
- It runs `bdpt`'s stages with stages 22 and 23 in place of 01 and 09.

### Later

- The mutations of Veach and Guibas 1997's path-space MLT (lens, caustic and bidirectional perturbations), which primary sample space cannot express. PBR 3ed exercise 16.15 points at them.
- Hoberock's noise-aware MLT (PBR 3ed exercise 16.16), which targets the image's perceptual error instead of its luminance.

## Registry, bindings and data

- Integrators `pssmlt` and `mmlt` ([integrator-layer.md](completed/integrator-layer.md)) and stages 22 and 23, whose shaders sit in `shaders/rt/` with the rest of the library. `pssmlt` runs the path tracer's 00 and 02 to 08 as they are, and the bootstrap's passes are plain compute passes in `shaders/rt/mlt/`.
- Binding 44 `MarkovChains`; binding 42 `SplatAccumulation`, shared.
- Settings: **Chains**, **Large step probability**, **Sigma**, **Bootstrap samples**.
- C++ mirror: `MLTSampler`'s lazy mutation in `rt_sampler`, with its test in `bin/sampler_test`.

## Importing pbrt scenes

[completed/pbrt-scene-import.md](completed/pbrt-scene-import.md) imports pbrt-v4 scenes today and substitutes for this plan's features. Each substitution is a line in the import report, so `./bin/pbrt_test --report <file.pbrt>` lists the scenes that need this plan. When this plan lands, change the importer as follows, then run `tools/verify/pbrt_compare.py` on the scenes named below.

- **`mlt`** renders with the path tracer, reported, and its parameters (`bootstrapsamples`, `chains`, `mutationsperpixel`, `largestepprobability`, `sigma`) are read and ignored (`settings()` in `src/pbrt_import.cpp`). No scene in `pbrt-v4-scenes` uses it. Select `mmlt` from it and map the parameters.

## Verification

- The path tracer and the BDPT integrators render bit-identical.
- `bin/sampler_test`: lazy mutation matches eager mutation coordinate for coordinate; a large step's coordinates are uniform (χ²); a small step's offsets are normal with the given σ.
- `pssmlt` and `mmlt` converge to the path tracer's reference on the Cornell box (means within 0.1 % after long runs).
- Veach's door-ajar scene (a room lit only through a slightly open door) and the mirrored caustic of [pbrt-photon-mapping.md](pbrt-photon-mapping.md): equal-time relative MSE for `path`, `lvc`, `bdpt`, `sppm`, `pssmlt` and `mmlt`.
- `mmlt` against pbrt-v4's `mlt` integrator on the same scene through [pbrt-scene-import.md](completed/pbrt-scene-import.md): the images converge together.

## Order of work

1. The chain sampler and lazy mutation in `rt_sampler`, with tests.
2. `pssmlt`: bootstrap, chains, the accept kernel and splats.
3. `mmlt`, once `bdpt` exists.

## Sources

PBR 3ed §16.4 (primary sample space MLT, multiplexed MLT, `MLTSampler`, `MLTIntegrator`) and exercises 16.15 and 16.16; Veach and Guibas 1997, *Metropolis light transport*; Kelemen et al. 2002, *A simple and robust mutation strategy for the Metropolis light transport algorithm*; Hachisuka, Kaplanyan and Dachsbacher 2014, *Multiplexed Metropolis light transport*; pbrt-v4 `src/pbrt/samplers.h` (`MLTSampler`), `cpu/integrators.cpp` (`MLTIntegrator`).
