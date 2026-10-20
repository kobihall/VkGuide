# Low-discrepancy sample generation

Every random number the tracer used came from one PCG stream per path, drawn lazily inside kernels 00 and 06. Independent samples are PBR's baseline: they have the worst possible discrepancy, spread their error as white noise, and converge at O(1/n) in MSE. Stratified and low-discrepancy samples converge faster, and blue-noise variants distribute the remaining error in a way the eye forgives. Slot 01 was created for this; this plan fills it.

*Implemented 2026-10-19. No prerequisites. Part of the [roadmap](../pbrt-roadmap.md).*

## Decisions

On 2026-09-26 the user chose:

- Kernel 01 writes a fixed per-bounce sample record that later kernels read, over sampling dimensions on the fly.
- New scenes default to ZSobol. Older files keep PCG, so they render bit-identical.

On 2026-10-19, when implementation started, the user answered four more questions:

- Padded Sobol and ZSobol need a samples-per-pixel count to interleave pixels with, and a render can run with **Unlimited samples**. They work in blocks of 2^L samples: L is the base-2 logarithm of **Max samples** rounded up, or 6 (64 samples) for an unlimited render. Sample n is element n mod 2^L of block n >> L, and the blocks are consecutive stretches of the sequence, so a render can run on forever. Only `stratified` and `pmj02bn`, which need the exact count, are disabled under unlimited samples.
- The Sobol and ZSobol sequence indices are 64-bit, emulated in GLSL as `uvec2`, since the device does not enable `shaderInt64`. The Sobol matrices keep pbrt's 52 columns.
- pbrt's tables ship as binary files in `assets/sampler/`, converted by `tools/sampler_tables/convert_pbrt_tables.py`, and are loaded at start-up.
- The record is allocated only while a sampler that writes one is selected. `pcg_hash` binds a 16-byte stub, and `CRT_MAX_POOL` stays at 4M paths.

## The sample record

```glsl
// 48 bytes per pool slot, binding 23 (RaySamples)
struct RaySamples {
    vec4 direct;     // x: light choice, yz: point on the light, w: spare
    vec4 indirect;   // x: lobe choice, yz: direction, w: roulette
    vec4 medium;     // x: distance, y: interaction type, zw: phase function direction
};
```

Kernel 01 writes it for every live path at the top of each bounce, and kernels 00 and 06 read it. The dimensions are allocated explicitly, as pbrt-v4's wavefront does (PBR 4ed §15.3.5), so every sample at a given bounce comes from the same dimension of the sequence whatever branch the path takes:

| Dimensions | Use |
|---|---|
| 0, 1 | offset in the pixel |
| 2, 3 | lens position |
| 4 | time ([pbrt-motion-blur.md](../pbrt-motion-blur.md)) |
| 5 | wavelengths ([pbrt-spectral-rendering.md](../pbrt-spectral-rendering.md)) |
| 6 + 12 b to 17 + 12 b | bounce b, in field order: `direct.xyzw`, `indirect.xyzw`, `medium.xyzw` |

A field's index in the record is its dimension's offset within the bounce, so `direct.w` is the spare. The record is regenerated per bounce, not stored for every bounce: one dispatch per bounce against 48 bytes × depth per path. Twelve dimensions per bounce and a maximum depth of 16 use 198 dimensions, within the 1024 of pbrt's Sobol matrices.

Kernel 01 does not generate the spare or the four medium fields, and writes zeros there. [pbrt-participating-media.md](../pbrt-participating-media.md) switches the medium fields on when kernels 05 and 07 read them.

Consumers that need more numbers than the record holds draw them from the path's PCG stream, as pbrt-v4 does for `LayeredBxDF`. The metal's fuzz radius is the first such number: the record gives the ball's direction and the stream its radius.

### The camera pass

Kernel 00 needs dimensions 0 to 5 before any path exists. A record-writing variant of kernel 01 (`KernelVariant::cameraPass`) therefore runs once over the pool before kernel 00, with `Params.bounce == CRT_CAMERA_PASS`, and writes the camera's dimensions into the same 48 bytes: the pixel offset and the lens position in `direct`, time and wavelength in `indirect.xy`. It then runs once per bounce over the live ray queue. `WavefrontPool::dispatchSize()` sizes the camera pass over the pool. A path's pixel and sample index follow from its pool slot and the film's sample count, so the generators are pure functions of (pixel, sample index, dimension, seed).

With a record, kernel 00 mixes `CRT_RECORD_STREAM_SALT` into the path's PCG seed. The independent sampler fills the camera's dimensions from the unsalted stream, and the salt keeps the numbers a path later spills into from repeating them.

### Keeping PCG bit-identical

`crt_samples.glsl` gives every consumer one accessor:

```glsl
float sampleDimension(uint pathIndex, inout uint rng, uint field);
```

Without a record it returns `randomFloat(rng)` and ignores the field, so a consumer that asks in the order it always drew gets the numbers it always got. With a record it returns the record's value.

The plan proposed a run-time flag, `CRT_FLAG_SAMPLE_RECORD`. That was not enough. The metal case of `scatterSurface()`, written around a branch on the flag, rounded differently on Metal by one ulp: 11 percent of the Cornell box's pixels differed from the old build in the last bit. The record's presence is instead the specialization constant `CRT_SAMPLE_RECORD` (`constant_id = 0`), set when the pipeline is built (`ComputePassBuilder::addSpecialization()`). Kernels 00 and 06 declare `KernelVariant::readsRecord`, and `GpuRenderer` builds each of them once per value, keyed in its pipeline cache. Without a record the branches are gone before the compiler optimizes, and the kernel is the code it was.

## The samplers

Each is a kernel 01 variant. Their body is `crt_sampler.glsl`; each `.comp` defines one `CRT_SAMPLER_*` and includes it.

| Variant | Id | Notes |
|---|---|---|
| 0101 | `pcg_hash` | no record; bit-identical to the tracer before this plan |
| 0102 | `independent` | the path's PCG stream written into the record |
| 0103 | `stratified` | jittered strata of **Max samples**, permuted per pixel and dimension; disabled under unlimited samples |
| 0104 | `halton` | scaled over the image with Grünschloß's index mapping |
| 0105 | `sobol` | one Sobol sequence over the image |
| 0106 | `padded_sobol` | a (0, 2)-sequence per pixel and request, padded within blocks |
| 0107 | `zsobol` | padded Sobol indexed by the pixel's Morton code; the new-scene default |
| 0108 | `pmj02bn` | pbrt-v4's five PMJ02BN sets with blue-noise rotation; disabled under unlimited samples |

The **Sampler** section of **Raytrace Render** chooses the variant (`KernelSettings::Sampler` shows **Randomization** for the Sobol and Halton samplers): none, random digit permutations, fast Owen scrambling (Laine and Karras), or full Owen scrambling, the default. It is `RenderSettings::samplerRandomization`, saved in `"render"`. The render's seed is the sampler's seed.

`GpuRenderer::start()` runs the independent sampler in place of the selected one when the selected one cannot run the render, and the panel says why: a sampler that needs the sample count under unlimited samples, a Sobol or PMJ02BN sampler without its tables, a sampler that interleaves pixels on an image wider than 65535 pixels, or PMJ02BN above 65536 samples per pixel.

### Where the generators depart from pbrt-v4

- pbrt hashes with 64-bit `MixBits` and `MurmurHash64A`. These are 32-bit (`samplerMix`, Wellons's lowbias32): the Owen scrambler calls the mixer once per bit, and a 64-bit multiply is several 32-bit ones on the GPU. Samples are equal to pbrt's in distribution, not in bits.
- Owen scrambling makes 23 decisions, not 31. A value keeps its top 24 bits as a float (`samplerToFloat()`), and the decision for a bit depends only on the bits above it, so the result is the same.
- pbrt's Owen-scrambled Halton hashes `1 + (dimension << 4)` alone, which leaves the render's seed out, and every render of a scene came out the same. The seed is mixed in.
- Halton's radical inverses accumulate in 32 bits with pbrt's overflow limit. The largest bases keep three digits where pbrt keeps four, about 6e-10, below a float's resolution. The index wraps after about 138,000 samples per pixel.
- A GPU's float division is not IEEE, so no shader divides. Halton reads each base's reciprocal from a generated table (`tools/sampler_tables/generate_halton_tables.py` writes `src/sampler_halton_tables.h` and `crt_halton_tables.glsl`), and the stratified and PMJ02BN samplers multiply by reciprocals the CPU computes into `CrtParams`. The digit counts of pbrt's scrambled radical inverse are tabulated for the same reason: they come from a float comparison a compiler may fuse.
- Padded Sobol and ZSobol work in blocks, as decided above.

## Tables

`assets/sampler/` holds pbrt's Sobol matrices (1024 dimensions × 52 columns, and the van der Corput pair `SobolIntervalToIndex` needs), the five PMJ02BN sets of 65536 points, and the 48 blue-noise textures, 4.4 MB in all. `assets/sampler/NOTICE.md` gives their licenses. `rt_sampler` loads them (`loadSamplerTables()`), and `GpuRenderer` uploads them once into binding 46, `SamplerTables`, 4.7 MB with a 16-word header (`packSamplerTablesForGpu()`). PMJ02BN's pixel samples, set 0 sorted into the cells of a tile of pixels, depend on the sample count. They are rebuilt in a reserved region of the same buffer when a render with another count starts.

## Registry, bindings and data

- Binding 23 `RaySamples`, 48 bytes × pool size (192 MB at the 4M cap), or a 16-byte stub.
- Binding 46 `SamplerTables`.
- `KernelVariant::writesRecord`, `cameraPass`, `needsSampleCount`, `sampler` (the `SamplerKind`) and `readsRecord`; `KernelSettings::Sampler`.
- `CrtParams` grows to 208 bytes: `samplerOptions`, `samplerHalton` and `samplerStrata` (`PackedSamplerParams`), and the stratified sampler's three reciprocals.
- Scene files are payload version 14, with `"samplerRandomization"` in `"render"`. New scenes select `zsobol`. A file without a `"01"` entry in `"kernels"` reads as `pcg_hash`; every file saved since version 8 names it already.
- `rt_sampler` (`src/sampler.h/.cpp`) is the C++ mirror of `crt_sampler.glsl`, and `bin/sampler_test` checks it.

## Verification

- `pcg_hash` renders bit-identical to the build before this plan: the Cornell box, the sphere scene, the Veach scene, the thin glass scene, and `assets/scenes/all_materials.gltf`, with every material type (Lambertian, fuzzy metal, Phong, dielectric, two PBR), at 1 and 4 samples per frame, under all four direct-lighting strategies. A scene file older than version 8 opens with `pcg_hash` and renders bit-identical too.
- The GPU's records equal the C++ mirror's bit for bit: every sampler and randomization, the camera pass and the bounce passes, at image sizes from 64 × 36 to 1000 × 300, sample counts from 7 to 300, and every bounce up to 15 (dimensions up to 197).
- `bin/sampler_test` checks determinism and range; (0, m, 2)-nets for padded Sobol, ZSobol and PMJ02BN, across blocks and under unlimited samples; global Sobol's net over the whole image; Halton's index tiling; one sample per stratum; L2 star discrepancy against independent sampling (15 percent of it for padded Sobol and ZSobol); and ZSobol's blue noise (its one-sample error has 1/14 of padded Sobol's power at low frequencies).
- Every sampler converges to the reference. Relative MSE against a 4096-sample `pcg_hash` reference, as a fraction of `pcg_hash`'s at the same sample count, averaged over four seeds:

| Sampler | Cornell box, 64 spp | Veach scene, 16 spp |
|---|---|---|
| `independent` | 1.04 | 1.01 |
| `stratified` | 0.73 | 0.30 |
| `halton` | 0.77 | 0.45 |
| `sobol` | 0.76 | 0.33 |
| `padded_sobol` | 0.71 | 0.31 |
| `zsobol` | 0.70 | 0.36 |
| `pmj02bn` | 0.62 | 0.18 |

- Global Sobol leaves the pixel dimensions unscrambled, as pbrt does, so its sub-pixel positions do not change with the seed. A small, very bright feature then gets a fixed coverage error rather than noise: 10 percent of the Veach scene's mean at 16 samples, from its brightest light alone, 0.9 percent at 64 and 0.07 percent at 2048.
- At one sample per pixel on the Veach scene and equal MSE, ZSobol's error has 3.0 percent of its power below 0.1 cycles per pixel, against 4.5 percent for padded Sobol and PCG.
- The validation layers report nothing for five samplers.

GPU time of a 640 × 360, 64-sample, 8-bounce render of the Cornell box on the Radeon Pro 560X: `pcg_hash` 680 ms; `independent` 793 ms, the record's own cost; `stratified` 800 ms; `padded_sobol` 833 ms; `pmj02bn` 825 ms; `sobol` 834 ms; `halton` 963 ms; `zsobol` 965 ms with Owen scrambling, 857 ms with fast Owen, 823 ms without. The Cornell box is all shapes, so its traversal is cheap and the sampler's share is large; ZSobol's cost is mostly the Morton digit permutation per request. The convergence plots of [pbrt-verification-harness.md](../pbrt-verification-harness.md) are left to that plan.

## Sources

PBR 4ed chapter 8, §8.2 to §8.7 (discrepancy, the sampler interface, independent, stratified, Halton, Sobol, padded Sobol and ZSobol samplers, randomization by scrambling), §15.3.5 (sample generation in the wavefront integrator); PBR 3ed §7.5 and §7.6; pbrt-v4 `src/pbrt/samplers.h`, `samplers.cpp`, `util/lowdiscrepancy.h`, `util/sobolmatrices.cpp`, `util/pmj02tables.cpp`, `util/bluenoise.cpp`, `wavefront/samples.cpp`; Christensen, Kensler and Kilpatrick 2018, *Progressive multi-jittered sample sequences*; Ahmed and Wonka 2020, *Screen-space blue-noise diffusion of Monte Carlo sampling error via hierarchical ordering of pixels*; Shirley, *Ray Tracing: The Rest of Your Life*, "Stratified Samples (Jittering)".
