# Layered materials

Car paint, varnished wood, plastic and tarnished metal are a smooth or rough dielectric over something else. The glTF pbr material approximates that with a GGX lobe over a Lambertian lobe weighted by the Fresnel term at the viewing angle, which does not account for light that reflects back down from the underside of the coating. pbrt-v4 solves the one-dimensional equation of transfer between the two interfaces by a random walk inside the BSDF itself (Guo et al. 2018). This plan adds that layered BSDF and the two materials built from it, and the 3rd edition's cheaper Fresnel blend beside them.

*Not implemented. Depends on [pbrt-bsdf-library.md](pbrt-bsdf-library.md). Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

The `pbr` material is the only layered-looking material, and it is not a layered BSDF: its lobe choice is `luminance(F)` against `luminance(diffuse × (1 − F))`, and nothing models the coating's internal reflection or the saturation it causes (PBR 4ed Figure 14.22(b)).

## Design

### The layered BSDF

`crt_bxdf_layered.glsl` implements pbrt-v4's `LayeredBxDF` (PBR 4ed §14.3.2):

- A top interface (a smooth or rough dielectric), a bottom interface (diffuse or conductor), and between them an optional homogeneous slab with thickness, albedo and a Henyey–Greenstein asymmetry g.
- `f()` estimates the BSDF value by following `nSamples` random walks of at most `maxDepth` bounces between the interfaces, with next-event connections through the top interface.
- `sampleF()` returns a direction whose distribution is the true BSDF, with `f` and `pdf` only proportional to their true values, and sets `pdfIsProportional`.
- `pdf()` estimates the density stochastically, mixed with a uniform term so it is never zero where the BSDF is not.

The walks need more random numbers than the per-bounce sample record holds, so they draw from the path's PCG stream after the record's dimensions, as pbrt-v4 does ([pbrt-sample-generation.md](pbrt-sample-generation.md)).

### MIS with a stochastic density

A stochastic `pdf()` is still a valid MIS weight in expectation, but kernel 06 must not use the density returned by `sampleF()` when `pdfIsProportional` is set. It calls `bxdfPdf()` for the MIS record instead, as pbrt-v4's `PathIntegrator` does. `crt_surface.glsl` gains that one branch; every other BxDF leaves the flag clear.

### Materials

| Type | Top | Bottom | Queue |
|---|---|---|---|
| `coated_diffuse` | dielectric (η, roughness) | diffuse (reflectance) | complex |
| `coated_conductor` | dielectric (η, roughness) | conductor (η and k or reflectance, roughness) | complex |
| `fresnel_blend` | analytic: Ashikhmin–Shirley glossy lobe over a Fresnel-attenuated diffuse lobe | | basic |

Shared parameters: thickness (default 0.01), slab albedo (default black, no scattering) and g, `maxDepth` (default 10), `nSamples` (default 1).

`fresnel_blend` is PBR 3ed's `SubstrateMaterial` (§8.5): a closed-form alternative with deterministic `f` and `pdf`, for scenes where the layered walk's cost or noise is not worth it.

### glTF clearcoat

`KHR_materials_clearcoat` maps to `coated_conductor` or `coated_diffuse` depending on the base's metallic factor, with the clearcoat factor as the top interface's weight and the clearcoat roughness as its roughness. A base that is neither fully metallic nor fully dielectric uses a mix of the two, which the mix material resolves stochastically ([pbrt-bsdf-library.md](pbrt-bsdf-library.md)).

## Verification

- `bin/bsdf_test` runs its tests on the stochastic BSDFs by averaging `f()` and `pdf()` over 256 evaluations per cell before comparing. The χ² test is against the averaged density.
- A white furnace with a white diffuse base under a clear coat must stay at or below 1.
- The dragon or a sphere rendered as blue diffuse, then coated smooth, then coated rough, reproduces the saturation shift of PBR 4ed Figure 14.22.
- Every scene without the new types renders bit-identical.

## Order of work

1. `LayeredBxDF` in C++ first, tested with `bin/bsdf_test`.
2. The GLSL port and `coated_diffuse`, in the complex queue.
3. `coated_conductor`, then `fresnel_blend`.
4. The clearcoat import.

## Open questions

- A bottom interface that is itself the glTF pbr BxDF would let `KHR_materials_clearcoat` sit over any glTF base exactly. It is one more case in the bottom switch, and more register pressure in the complex build; worth doing only if clearcoated glTF models turn out to be common.

## Sources

PBR 4ed §14.3 (the one-dimensional equation of transfer, `LayeredBxDF`, coated diffuse and coated conductor materials), §9.1.2 (`pdfIsProportional`); PBR 3ed §8.5 (Fresnel incidence effects, the Fresnel blend BRDF); Guo, Hašan and Zhao 2018, *Position-free Monte Carlo simulation for arbitrary layered BSDFs*; Ashikhmin and Shirley 2000; pbrt-v4 `src/pbrt/bxdfs.h` (`LayeredBxDF`, `CoatedDiffuseBxDF`, `CoatedConductorBxDF`).
