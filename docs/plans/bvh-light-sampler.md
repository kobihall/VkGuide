# Light selection and the light BVH

Next-event estimation picks one light from an alias table built over each light's power, the same table for every shading point. A dim lamp beside the receiver and a bright one across the building get the probabilities of their powers, not of what they deliver here. The NEE work recorded the result: in its 980-light test scene, where Suzanne's 968 emissive triangles dominate, light sampling was the weakest strategy, with a relative MSE of 2.0e-3 against BSDF sampling's 9.6e-4 at 4096 spp. pbrt-v4's default, the `BVHLightSampler` (Conty Estevez and Kulla 2018), bounds groups of lights by position, direction and power in a tree, and picks a light by walking the tree with an importance that depends on the receiving point. This plan adds it, with pbrt's uniform and exhaustive samplers beside the existing power sampler.

*Not implemented. Uses `PathState.prevNormal` from [light-shape-sampling.md](light-shape-sampling.md); whichever of the two plans lands first adds it. Part of the [roadmap](roadmap.md).*

## Decision already made

On 2026-09-26 the user chose to make light selection a setting, a uniform switch in `crt_light.glsl` like the BVH builder, instead of kernel 06 variants. Kernel 06's variants stay the direct-lighting strategies.

## What the renderer does today

- `LightListBuilder` (`src/rt_lights.h`) prices every light by power and builds two alias tables: over all lights, and over the delta lights alone for the BSDF strategy (`sampleLightIndex()`, `sampleDeltaLightIndex()`, `lightPmf()` in `crt_light.glsl`).
- The environment's power is its luminance integral times the scene's cross-section, so it competes with the local lights in the same table.
- Kernel 04, pricing a BSDF-sampled hit on an emitter, reads `lightPmf(index)`, which does not depend on where the path came from.

## Design

### Samplers

A **Light selection** setting in the direct-lighting section:

| Sampler | Probability of light *i* at point *p* | Cost per sample |
|---|---|---|
| Uniform | 1 / N | O(1) |
| Power (today, default) | Φᵢ / ΣΦ | O(1), alias table |
| Exhaustive | importanceᵢ(p, n) / Σ importance | O(N), every light's importance |
| BVH | the product of the child importances along the path to *i* | O(depth) |

Uniform and exhaustive are pbrt-v4's `UniformLightSampler` and `ExhaustiveLightSampler`. Uniform is the textbook baseline; exhaustive evaluates the same importance as the BVH over every light, which makes it the reference the BVH approximates, and the best choice for a scene with a handful of lights.

The BSDF strategy's delta-light table is unchanged under every sampler.

### Light bounds

Each light gets pbrt's `LightBounds` (PBR 4ed §12.6.3): a box, a principal direction `w`, the power `Φ`, a cone `θo` around `w` that contains every surface normal, a cone `θe` beyond it into which the light still emits (π/2 for a diffuse emitter), and whether it is two-sided.

| Light | Bounds |
|---|---|
| Sphere, cylinder | the shape's box; `θo` = π (normals everywhere) |
| Rectangle, triangle, disk | the shape's box; `w` = the normal, `θo` = 0, two-sided by the emitter's flag ([light-types.md](light-types.md)) |
| Point, goniometric, projection | a degenerate box at the point; `θo` = π |
| Spot | the point; `w` = the axis, `θo` = the outer cone, `θe` = 0 |
| Directional, environment, sun | none: infinite lights stay out of the tree |

The importance of a bounds at a receiving point `p` with normal `n` is pbrt's: `Φ cos θ' / d²` times the bound on the receiver's cosine, where `θ'` is the angle between `w` and the direction to `p`, shrunk by `θo` and by the angle the box subtends, zero past `θe`, and `d²` is clamped to half the box's diagonal so a point inside the box does not divide by zero. In a medium, where there is no normal, the receiver term is 1.

### The tree

Built on the CPU with the light list, whenever an emitter changes, by pbrt's method: split at the bucket boundary (12 buckets per axis) that minimises the Conty–Kulla cost, `Φ × M_Ω × surface area × Kr`, where `M_Ω` measures the bounds' cone of directions and `Kr` penalises thin boxes.

Nodes use pbrt's `CompactLightBounds`, 24 bytes: the box quantised to 16 bits per coordinate within the scene's bounds, `w` octahedral-encoded, `Φ` as a float, and both cosines in 15 bits with the two-sided flag. A node adds a child or light index and a leaf bit, 32 bytes aligned, the same as pbrt's `LightBVHNode`. The nodes go into binding 28, `LightBvh`.

### Sampling and the pmf

Sampling descends from the root: at each interior node, compute both children's importance, pick one with probability proportional to it, and rescale the random number. The pmf is the product of the choices.

Kernel 04 needs the same pmf for a light that a BSDF sample hit, so it can weigh the hit with MIS. pbrt stores each light's *bit trail*, its path from the root, and re-walks the tree along it, computing the same importances from the path's previous vertex. `GpuLight` holds the trail in a padding word (32 levels at most, which pbrt also assumes). Kernel 04 reads `PathState.origin` and `prevNormal` for the previous vertex, so both kernels see the same receiver and compute the same pmf.

### Infinite lights

pbrt gives the tree and each infinite light an equal share. This plan keeps today's rule instead: the infinite lights and the tree's root are priced by power, as the power sampler prices them. Both are unbiased; the power rule stays closer to today's behaviour in scenes with a dim sky and many lamps, and the exhaustive sampler measures what either costs.

### Later

- Adaptive splitting (Conty Estevez and Kulla 2018): where a node's importance bound is loose, sample both children and send two shadow rays. It shares the shadow-queue capacity question with light-sample splitting in [direct-lighting-strategies.md](direct-lighting-strategies.md).
- BSDF-aware importance (Liu, Xu and Yan 2019): replace the receiver's cosine bound with a bound on the BSDF lobe, which helps glossy receivers.
- Visibility-aware selection (Vévoda et al. 2018) learns which lights are occluded where. It is closer to [path-guiding.md](path-guiding.md) than to this plan.

## Registry, bindings and data

- Binding 28 `LightBvh`: the nodes.
- `GpuLight`: the bit trail in a padding word.
- The **Light selection** setting, saved with the render settings. The render panel shows the tree's node count, depth and build time.
- `rt_light` gains `LightBounds`, the importance function, the build, sampling and the pmf walk, mirrored line for line in `crt_light.glsl`.

## Verification

- With **Power** selected, every scene renders bit-identical.
- `bin/light_test`: for random points and normals, each sampler's pmf sums to 1 over the lights; sampled indices match the pmf by χ²; the BVH's pmf from the bit-trail walk equals the product recorded while sampling, exactly.
- The BVH and exhaustive samplers converge to the power sampler's image on the Veach scene and Sponza (**Compare to reference**, within noise).
- The NEE work's 980-light scene at 192 × 108 and 4096 spp: the relative MSE table of `shadow-rays-nee.md` §9 again, one row per sampler. With the BVH, light sampling should beat BSDF sampling's 9.6e-4, which the power sampler's 2.0e-3 does not.

## Order of work

1. `LightBounds`, the importance function, and the exhaustive sampler. The exhaustive sampler tests the importance before any tree exists.
2. The uniform sampler.
3. The BVH build, sampling and the bit-trail pmf.
4. Adaptive splitting and BSDF-aware importance, as later work.

## Sources

PBR 4ed §3.8.4 (bounding directions), §12.6 (light samplers: uniform, power, BVH, compact bounds), chapter 12 "Further Reading" (many-light sampling); Conty Estevez and Kulla 2018, *Importance sampling of many lights with adaptive tree splitting*; Liu, Xu and Yan 2019, *Adaptive BRDF-oriented multiple importance sampling of many lights*; Laine 2010 (bit trails); Vévoda et al. 2018, *Bayesian online regression for adaptive direct illumination sampling*; pbrt-v4 `src/pbrt/lightsamplers.h` and `lightsamplers.cpp`; `docs/plans/completed/shadow-rays-nee.md` §9 (the many-emitter result).
