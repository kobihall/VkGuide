# Light shape sampling

Once the light selection has picked an emitter, `crt_light.glsl` picks a point on it: spheres by the cone they subtend, rectangles and triangles uniformly in solid angle, cylinders by area. None of these knows the cosine at the receiving point, so a diffuse surface next to a large light spends samples on directions that graze it. pbrt-v4 warps its triangle and rectangle samples by that cosine. The next-event estimation work dropped the warp because kernel 04 would need to invert the sampling map to price a BSDF-sampled hit. This plan brings the warp back with the inverses pbrt provides, adds exact projected-solid-angle sampling where it exists, and samples only the visible part of a cylinder.

*Not implemented. No plan dependencies. [pbrt-analytic-shapes.md](pbrt-analytic-shapes.md) adds the disk, whose solid-angle sampler is here. Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

| Shape | Sampler | Source |
|---|---|---|
| Sphere | uniform in the subtended cone, Taylor series for small cones; by area from inside | PBR 4ed §6.2.4 |
| Rectangle, box face | uniform spherical rectangle; by area below 1e-3 sr and above 6.22 sr | Ureña et al. 2013 |
| Triangle | uniform spherical triangle, same fallbacks | Arvo 1995b |
| Cylinder | uniform by area over the whole side | PBR 4ed §6.3.3 |

Kernel 06 samples the light and records the MIS pdf of the BSDF sample. Kernel 04, on a BSDF-sampled hit, recomputes the light pdf from `PathState.origin` and the hit point alone (`areaLightPdf()`), which is all a cosine-free density needs.

## Design

A **Shape sampling** setting in the render panel's direct-lighting section chooses a family, and each shape uses the best sampler it has in that family. Like the light selection, it is a uniform switch in `crt_light.glsl`, not a kernel variant.

| Family | Sphere | Rectangle | Triangle | Cylinder | Disk |
|---|---|---|---|---|---|
| Solid angle (today, default) | cone | spherical rectangle | spherical triangle (Arvo) | area | area |
| Cosine-warped | cone | spherical rectangle, bilinear warp | spherical triangle, bilinear warp | visible area | solid angle (Guillén) |
| Projected solid angle | projected cap (Peters and Dachsbacher) | projected polygon (Peters) | projected polygon (Peters) | visible area | solid angle (Guillén) |

Every family keeps pbrt's fallbacks: area sampling for a tiny or huge solid angle, and the plain solid-angle sampler when the receiving point has no normal, which is the case in a medium ([pbrt-participating-media.md](pbrt-participating-media.md)).

### The previous vertex's normal

A cosine-aware density depends on the receiving point's shading normal, so kernel 04 needs it to price a hit, as pbrt's `prevIntrCtx` provides. `PathState` gains `prevNormal`, the octahedral encoding of the normal kernel 06 used, 16 bits per component in one `uint`. Kernel 06 warps with the decoded value, not the exact one, so both kernels compute the same density bit for bit. It fills the last word of the 96-byte layout the roadmap reserves.

### Cosine-warped samples

pbrt-v4's triangle and bilinear-patch `Sample()` methods (PBR 4ed §6.5.4) evaluate the cosine at the four corners of the unit square that the spherical sampler maps onto the light, clamped below at 0.01. They draw the square's point from the bilinear density over those four values and multiply the pdf by it. The warp follows the cosine approximately, is cheap, and keeps stratification.

Pricing a hit needs the square's point for a given direction: `InvertSphericalTriangleSample()` (after Arvo's `SphTri` code, 2001b) and `InvertSphericalRectangleSample()` from pbrt-v4's `util/sampling.cpp`. Where the inverse lands slightly off the forward map, the two kernels' pdfs differ, and the MIS weights of the two strategies no longer sum exactly to 1. That is a small bias, which pbrt accepts. `bin/light_test` measures it: over 10⁶ random configurations it reports the worst and the 99th-percentile round-trip error `|u − inverse(forward(u))|` and the resulting pdf mismatch. The verification below bounds its effect on an image.

### Projected solid angle

A density proportional to the cosine exactly, `pdf(ω) = cos θ / Ω⊥`, where `Ω⊥` is the light's projected solid angle. The pdf needs no inverse map, only `Ω⊥`, which has a closed form for spheres and for polygons (Lambert's formula), so kernel 04's side is cheaper than the warp's.

- Spheres: Peters and Dachsbacher 2019, with Peters 2019's pdf. It improves on Ureña and Georgiev 2018; PBR 4ed exercise 6.7 suggests either.
- Triangles and rectangles: Peters 2021b, which clips the polygon to the upper hemisphere, splits it into parts and inverts each part's distribution with two iterations of a specialized solver. PBR 4ed cites the same paper for its faster solid-angle triangle sampler.

For a diffuse receiver this cancels the cosine from the estimator entirely. For a glossy one it helps less; Peters 2021b combines it with sampling linearly transformed cosines for the specular lobe, which is a later step here.

### Cylinders by visible area

A point on the far side of a cylinder cannot light the receiver. Sampling only the part of the side facing the receiving point (Gardner et al. 1987, Zimmerman 1995; PBR 4ed exercise 6.6) halves the wasted samples at least. The caps are separate light records already and keep their own samplers.

### Disks by solid angle

Disks arrive with [pbrt-analytic-shapes.md](pbrt-analytic-shapes.md) and pbrt samples them by area. Guillén et al. 2017 sample the spherical ellipse a disk subtends uniformly, with no rejection, and its pdf is the reciprocal of the ellipse's solid angle.

## Registry, bindings and data

- No new kernel, variant or binding.
- `PathState.prevNormal`.
- The **Shape sampling** setting, saved with the render settings.
- C++ mirrors of every new sampler and inverse in `light_sampling.cpp`, as the existing ones are.

## Verification

- With **Solid angle** selected, every scene renders bit-identical.
- `bin/light_test`, per shape and family: the pdf integrates to 1 over the light's solid angle; samples pass a χ² test against the pdf; the inverses' round-trip error as above.
- On the Cornell box and on a diffuse floor under a large rectangle, each family converges to the same image (**Compare to reference** against a 64k-spp render, within noise). This bounds the warp's inverse bias.
- Equal-spp MSE for each family on those two scenes and the Veach scene, as convergence plots from [pbrt-verification-harness.md](pbrt-verification-harness.md), next to the kernel 06 timings from the render panel.

## Order of work

1. `prevNormal`. Bit-identical.
2. Cosine-warped triangles and rectangles, with the inverses and their tests.
3. Cylinders by visible area.
4. Projected solid angle for spheres, then for polygons.
5. Disks by solid angle, when disks exist.

## Sources

PBR 4ed §2.4.2 (sampling the bilinear function), §6.2.4 (sphere sampling), §6.3.3 (cylinder sampling), §6.5.4 (triangle sampling and the bilinear warp), §6.6.2 (bilinear patch sampling), chapter 6 "Further Reading" (sampling shapes) and exercises 6.6 and 6.7; Arvo 1995b, *Stratified sampling of spherical triangles*, and 2001b, the `SphTri.C` code (the inverse); Ureña et al. 2013, *An area-preserving parametrization for spherical rectangles*; Ureña and Georgiev 2018, *Stratified sampling of projected spherical caps*; Peters and Dachsbacher 2019, *Sampling projected spherical caps in real time*; Peters 2019, *Sampling projected spherical caps with multiple importance sampling*; Peters 2021b, *BRDF importance sampling for polygonal lights*; Guillén et al. 2017, *Area-preserving parameterizations for spherical ellipses*; Gardner et al. 1987; Zimmerman 1995; pbrt-v4 `src/pbrt/shapes.h`, `util/sampling.cpp`.
