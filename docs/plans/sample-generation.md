# Low-discrepancy sample generation

Every random number the tracer uses comes from one PCG stream per path, drawn lazily inside kernels 00 and 06. Independent samples are PBR's baseline: they have the worst possible discrepancy, spread their error as white noise, and converge at O(1/n) in MSE. Stratified and low-discrepancy samples converge faster, and blue-noise variants distribute the remaining error in a way the eye forgives. Slot 01 was created for exactly this (its header comment says so); this plan fills it.

*Not implemented. Depends on [integrator-layer.md](integrator-layer.md). Part of the [roadmap](roadmap.md).*

## Decisions already made

On 2026-09-26 the user chose:

- Kernel 01 writes a fixed per-bounce sample record that later kernels read, over sampling dimensions on the fly.
- New scenes default to ZSobol. Older files keep PCG, so they render bit-identical.

## What the renderer does today

- Kernel 00 seeds `rng = pcgHash(pixel ^ pcgHash(sampleIndex ^ pcgHash(seed)))` and draws the pixel jitter and lens position from it.
- Kernel 01 (`0101_generate_samples.comp`, id `pcg_hash`) rehashes each live path's state with the bounce index.
- Kernel 06 draws as it goes: the BSDF sample (two or three numbers, depending on the material), then the roulette number, then the light index and a 2D light sample. How many numbers a path uses depends on which branch it took.

## Design

### The sample record

```glsl
// 48 bytes per pool slot, binding 23 (RaySamples)
struct RaySamples {
    vec4 direct;     // x: light choice, yz: point on the light, w: spare
    vec4 indirect;   // x: lobe choice, yz: direction, w: roulette
    vec4 medium;     // x: distance, y: interaction type, zw: phase function direction
};
```

Kernel 01 writes it for every live path at the top of each bounce. Kernels 06 and 07 read it. The dimensions are allocated explicitly, as pbrt-v4's wavefront does (PBR 4ed §15.3.5), so every sample at a given bounce comes from the same dimension of the sequence whatever branch the path takes:

| Dimensions | Use |
|---|---|
| 0, 1 | pixel position |
| 2, 3 | lens position |
| 4 | time ([motion-blur.md](motion-blur.md)) |
| 5 | wavelengths ([spectral-rendering.md](spectral-rendering.md)) |
| 6 + 12 b to 17 + 12 b | bounce b: `direct.xyz`, `indirect.xyzw`, `medium.xyzw`, one spare |

Twelve dimensions per bounce and a maximum depth of 16 use 198 dimensions, well within the 1024 of pbrt's Sobol matrices.

Consumers that need more numbers than the record holds (a layered BSDF's random walk, extra light samples when splitting) draw them from the path's PCG stream, as pbrt-v4 does for `LayeredBxDF`.

### Camera dimensions

Kernel 00 needs dimensions 0 to 5 before any path exists. The kernel 01 variant therefore runs twice per frame: once over the pool before kernel 00, writing the camera dimensions into the record, and then once per bounce over the live queue. `KernelVariant` gains `cameraPass`, and the schedule dispatches that pass with `pc.bounce = CRT_CAMERA_PASS`. The sampler stays one file.

### Keeping PCG bit-identical

`crt_samples.glsl` gives every consumer one accessor:

```glsl
float sampleDimension(uint pathIndex, inout uint rng, uint dimension);
```

When the selected variant writes no record (`pc.flags & CRT_FLAG_SAMPLE_RECORD` clear), it returns `randomFloat(rng)` and ignores the dimension. Because the calls stay in today's order, the `pcg_hash` variant renders exactly as before. When the flag is set, it returns the record's value.

### The samplers

Each is a kernel 01 variant. Numbers follow the slot's next free `VV`:

| Variant | Id | Notes |
|---|---|---|
| 0101 | `pcg_hash` | today's, no record, bit-identical |
| 0102 | `independent` | PCG written into the record, to measure the record's own cost |
| 0103 | `stratified` | jittered strata with random padding; needs a known sample count, so it is disabled with unlimited samples |
| 0104 | `halton` | scaled over the image so that neighbouring pixels share a well-distributed set (Grünschloß's index mapping) |
| 0105 | `sobol` | global Sobol over the image |
| 0106 | `padded_sobol` | per-pixel (0, 2)-sequences for each 1D and 2D request, randomly padded |
| 0107 | `zsobol` | padded Sobol indexed by the pixel's Morton code, giving blue-noise error (Ahmed and Wonka 2020); pbrt-v4's default |
| 0108 | `pmj02bn` | progressive multi-jittered (0, 2) with blue noise, from pbrt-v4's tables |

A **Sampler** settings section (a new `KernelSettings` value) appears for the variants that use it:

- **Randomisation** chooses none, random digit permutations, fast Owen scrambling (Laine and Karras; Vegdahl's hash) or full Owen scrambling, the default.
- **Seed**, which the render seed already provides.

pbrt's evaluation (PBR 4ed §8.7.7) gives an idea of what to expect on a defocus-blur scene at 16 spp, as MSE relative to the stratified sampler: Halton 1.44, padded Sobol 0.96, Sobol 0.64, ZSobol 0.84. On its complex test scene both Halton and Sobol were about 10 % below independent sampling.

### Tables

Sobol generator matrices (pbrt's Joe and Kuo matrices, 1024 dimensions × 32 bits), Halton digit permutations and the PMJ02BN point tables go into a read-only `SamplerTables` buffer (binding 46), uploaded once at start-up. The C++ side keeps them in `rt_sampler`.

### Consumers

- Kernel 00 reads the pixel, lens, time and wavelength dimensions. [film-and-reconstruction.md](film-and-reconstruction.md) warps the pixel dimensions through the pixel filter.
- Kernel 06 reads `direct` for next-event estimation, `indirect.x` to choose a lobe or a transmission branch, `indirect.yz` for the direction and `indirect.w` for Russian roulette.
- Kernels 05 and 07 read `medium` ([participating-media.md](participating-media.md)).

Code must request dimensions in the same order on every path (PBR 4ed §8.3); the explicit layout guarantees it for everything the record covers.

## Registry, bindings and data

- Binding 23 `RaySamples`, 48 bytes × pool size (192 MB at the 4M cap; the cap may need lowering, see the open question).
- Binding 46 `SamplerTables`, about 140 KB of Sobol matrices plus the Halton and PMJ tables. The roadmap's binding table gains this row.
- `KernelVariant::cameraPass`; `KernelSettings::Sampler`.
- New scenes select `zsobol`; a file without an entry for slot 01 keeps `pcg_hash`, which is what every file saved before this change says.

## Order of work

1. `crt_samples.glsl`, the record buffer and the camera pass, with `pcg_hash` unchanged. Bit-identical.
2. `independent`, to validate the record path end to end.
3. `rt_sampler` and `bin/sampler_test`: determinism, stratification of every 2D projection the (0, 2) property promises, star discrepancy against pbrt's published values.
4. `sobol` and `padded_sobol` with the three randomizers, then `zsobol`.
5. `stratified`, `halton`, `pmj02bn`.
6. Default for new scenes.

## Verification

- `pcg_hash` renders bit-identical before and after.
- `independent` converges to the same image as `pcg_hash` (**Compare to reference**, relative MSE within noise).
- Every sampler converges to the same reference, and the convergence plot of [verification-harness.md](verification-harness.md) shows each one's slope. A defocus scene (the sphere scene with a wide aperture) and the Veach scene are the two test scenes, mirroring PBR's Figure 8.45 and its glossy test.
- A 1 spp render with `zsobol` shows visibly blue-noise error next to `padded_sobol` at equal MSE, as in PBR Figure 8.38.

## Open questions

- Whether to store the record per path at all bounces' worth of dimensions, or regenerate it per bounce (as designed). Per bounce costs one dispatch per bounce and keeps 48 bytes per path; the alternative would cost 48 bytes × depth.
- The record adds 192 MB at the 4M-path cap. Lowering `CRT_MAX_POOL` to 2M, or packing the record as 16-bit unorm values (12 × 2 bytes = 24 bytes) are both open. 16 bits of a sample value lose the low bits Owen scrambling relies on, so full floats are the safer default.

## Sources

PBR 4ed chapter 8, §8.2 to §8.7 (discrepancy, the sampler interface, independent, stratified, Halton, Sobol, padded Sobol and ZSobol samplers, randomisation by scrambling), §15.3.5 (sample generation in the wavefront integrator); PBR 3ed §7.5 and §7.6 ((0, 2)-sequence and maximized-minimal-distance samplers); pbrt-v4 `src/pbrt/samplers.h`, `util/lowdiscrepancy.h`, `util/sobolmatrices.cpp`, `util/pmj02tables.cpp`, `wavefront/samples.cpp`; Shirley, *Ray Tracing: The Rest of Your Life*, "Stratified Samples (Jittering)".
