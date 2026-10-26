# Russian roulette

After three bounces, kernel 06 lets a path survive with probability `clamp(max(throughput), 0.05, 1)` and divides the survivors' throughput by it. pbrt-v4 uses a different rule: it starts after the first bounce, has no floor, and removes the radiance scaling that refraction puts into the throughput, so that a ray entering glass is not killed for a factor that cancels when it leaves (PBR 4ed §13.4, Figure 13.11). pbrt's chapter 13 exercises add roulette for shadow rays and Veach's efficiency-optimized roulette. This plan makes the roulette rule a setting with each of these, and leaves adjoint-driven roulette and splitting for after path guiding.

*Not implemented. `etaScale` needs [pbrt-bsdf-library.md](pbrt-bsdf-library.md)'s transmission to scale radiance by 1/η². Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- `RenderSettings::russianRoulette` (on by default) and `minBouncesBeforeRoulette` (3), in `src/rt_scene_types.h`, reach kernel 06 as `CRT_FLAG_ROULETTE` and a push constant.
- The roulette runs after the BSDF sample and before next-event estimation in `crt_surface.glsl`, on the throughput including this bounce's attenuation. Next-event estimation at the vertex happens whether the path survives or not.
- The legacy `dielectric` material does not scale radiance by 1/η² on refraction, so its throughput carries no factor to remove.
- Shadow rays are always traced.

## Design

### Rules

A **Roulette** setting replaces the checkbox:

| Rule | Survival probability | Applies from | Notes |
|---|---|---|---|
| Off | 1 | | |
| Throughput | `clamp(max(β), 0.05, 1)` | **Minimum bounces** | today's; older files keep it |
| pbrt | `min(1, max(β · etaScale))` | **Minimum bounces**, 1 for new scenes | PBR 4ed §13.4; new scenes |
| Adjoint-driven | a weight window around `Ĩ / L̂ᵢ(x)` | 1 | later, needs [pbrt-path-guiding.md](pbrt-path-guiding.md) |

pbrt's rule has no floor, and after survival the largest component of `β · etaScale` becomes exactly 1: every surviving path carries about the same weight, which is what makes roulette pay. PBR 4ed Table 13.1 measures efficiency gains of 1.25× to 4.51× over no roulette on four scenes. The rule takes the maximum component rather than the mean, which pbrt explains keeps any spectral component from growing above 1 through roulette on saturated surfaces. In spectral modes the maximum runs over the live lanes (`laneMask`, [pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md)).

### Removing refraction's radiance scaling

`PathState.etaScale` starts at 1 and multiplies by `η²` whenever a BxDF samples transmission with relative index `η`, which pbrt's `BSDFSample::eta` reports. The new dielectric, thin dielectric and rough dielectric BxDFs of [pbrt-bsdf-library.md](pbrt-bsdf-library.md) scale radiance by `1/η²` as pbrt does, and report `η`. The legacy `dielectric` material reports 1, since it never scaled radiance, so the pbrt rule sees the same throughput through it as the throughput rule does.

### Shadow-ray roulette

PBR 4ed exercise 13.1: before appending a shadow ray, kernel 06 estimates its contribution `c` (the throughput times the unshadowed light sample). If the luminance of `c` falls below a threshold `t`, the ray is traced with probability `lum(c) / t` and its contribution divided by that probability. Unbiased, and it skips shadow rays toward dim lights, which the 980-light scene has hundreds of.

The threshold has to scale with the scene's brightness. It is a fraction (the **Shadow roulette** setting, default off, 0.01 when on) of the pixel's running mean, which kernel 06 reads from the accumulation image. A pixel with fewer than 16 samples has no reliable mean and traces every shadow ray. A roulette probability that depends only on earlier samples keeps each sample's expectation correct, so the estimate stays unbiased.

### Efficiency-optimized and adjoint-driven roulette

Veach (1997, §10.4.1; PBR 4ed exercise 13.2) derives the roulette threshold that maximizes efficiency from how much a path is expected to contribute relative to the pixel's value. The expected contribution needs an estimate of the radiance still to be gathered at the vertex. Vorba and Křivánek 2016 (ADRRS) take it from a learned radiance cache and define a weight window around `Ĩ / L̂ᵢ(x)`, the pixel estimate over the incident-radiance estimate: below the window the path plays roulette, above it the path splits. This plan reserves the **Adjoint-driven** rule for when [pbrt-path-guiding.md](pbrt-path-guiding.md) supplies `L̂ᵢ`. Roulette comes first.

Splitting needs free path slots to fork into, which exist only once [pbrt-wavefront-performance.md](pbrt-wavefront-performance.md) regenerates paths into a shared pool.

### Where the random numbers come from

The roulette number is `RaySamples.indirect.w` ([pbrt-sample-generation.md](completed/pbrt-sample-generation.md)). The shadow-roulette number comes from the path's PCG stream, since the record has no dimension for it and adding one would change every other sampler's layout.

## Registry, bindings and data

- `PathState.etaScale` (roadmap).
- Kernel 06 declares the accumulation image read-only for shadow roulette.
- Settings: **Roulette**, **Minimum bounces**, **Shadow roulette**. The scene file maps the old `russianRoulette` boolean to **Throughput** or **Off**.
- No new binding or variant.

## Verification

- With **Throughput** and shadow roulette off, every scene renders bit-identical.
- Every rule converges to the reference rendered with roulette off (**Compare to reference**, within noise) on the Cornell box, Sponza and a glass scene.
- The glass scene reproduces PBR Figure 13.11: with `etaScale` forced to 1, the pbrt rule shows extra noise inside the glass, and with it the noise goes away.
- PBR Table 13.1's measurement, redone for the four NEE scenes: relative MSE, frame time and efficiency for each rule against no roulette.
- Shadow roulette on the 980-light scene: shadow rays traced per frame (from the queue-header readback) and efficiency.

## Order of work

1. The **Roulette** setting with **Off**, **Throughput** and **pbrt**, and `etaScale` fixed at 1.
2. `etaScale` updates, when the new dielectric lands.
3. Shadow roulette.
4. Adjoint-driven roulette after path guiding, then splitting after path regeneration.

## Sources

PBR 4ed §2.2.4 (Russian roulette), §13.4 (`PathIntegrator`: the roulette rule, `etaScale`, Figure 13.11, Table 13.1), chapter 13 "Further Reading" and exercises 13.1 and 13.2; Arvo and Kirk 1990, *Particle transport and image synthesis*; Szécsi, Szirmay-Kalos and Kelemen 2003, *Variance reduction for Russian roulette* (the albedo-driven rule pbrt uses); Veach 1997 §10.4.1; Vorba and Křivánek 2016, *Adjoint-driven Russian roulette and splitting in light transport simulation*; pbrt-v4 `src/pbrt/cpu/integrators.cpp` and `wavefront/surfscatter.cpp`.
