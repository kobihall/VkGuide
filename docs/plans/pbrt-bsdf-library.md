# BSDF library

The six materials the tracer has are Ray Tracing in One Weekend's lambertian, fuzzed metal, phong and Schlick dielectric, an emitter, and glTF's metallic-roughness model. None of them has an exact Fresnel term, a complex index of refraction, a rough dielectric, anisotropy, a thin window pane or diffuse transmission, and the dielectric does not scale radiance by η² when it refracts. This plan adds pbrt-v4's BxDFs as new material types beside the existing ones, which all stay.

*Not implemented. Depends on [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md), [pbrt-material-system.md](completed/pbrt-material-system.md) and [pbrt-verification-harness.md](completed/pbrt-verification-harness.md). Dispersion also needs [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md). Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- `crt_scatter.glsl` implements each material three times (sample, evaluate, density) in world space, with a normal flipped to face the incoming ray.
- The dielectric uses Schlick's approximation and a scalar IOR, and treats every refraction as radiance-preserving.
- The metal is a mirror direction plus a random offset in a ball of radius `fuzz`. Its density was derived by hand (`fuzzBallPdf()`).
- The pbr material samples GGX visible normals (Heitz 2018) with a colored Schlick Fresnel, and layers a Lambertian lobe under it without conserving energy at grazing angles (its own comment says so).
- [pbrt-verification-harness.md](completed/pbrt-verification-harness.md) measured two flaws this plan must not carry over. Schlick's approximation is called with the incident cosine in both directions, so inside glass of index 1.5 it gives 0.040 at 30 degrees and 0.041 at 40 where the exact Fresnel reflectance is 0.055 and 0.245 (0.380 at 41 degrees, just under the critical angle); from outside it is within 0.013 up to 70 degrees. `glass` and `thin_glass` use `frDielectric()`, which has no such error. And phong, the fuzzy metal and the Lambertian lobe of `pbr` are not reciprocal (the relative asymmetry of `f(wo, wi)` against `f(wi, wo)` is 0.5 to 0.95): the Lambertian lobe's `1 - F(wo)` factor depends on one direction only. A new BxDF should be reciprocal, and `bin/bsdf_test` fails a type listed as reciprocal that is not. The three old types stay as they are, to keep every scene bit-identical.
- `isSpecularMaterial()` decides which surfaces skip next-event estimation: a dielectric, or a metal whose fuzz is below 1e-4.

## Design

### New material types

Each is a `MATERIAL_TRAITS` entry ([pbrt-material-system.md](completed/pbrt-material-system.md)). Names avoid the ones scene files already use: `dielectric` keeps meaning the Ray Tracing in One Weekend glass.

| Type | BxDF | Parameters |
|---|---|---|
| `diffuse` | Lambertian, or Oren–Nayar when σ > 0 | reflectance, σ (degrees) |
| `conductor` | smooth or rough Torrance–Sparrow with complex Fresnel | η and k, or reflectance; roughness u and v; remap roughness |
| `glass` | smooth or rough dielectric (Walter et al. 2007) with exact Fresnel | η; roughness u and v; remap roughness |
| `thin_glass` | thin dielectric, R' = R + T²R / (1 − R²) | η |
| `diffuse_transmission` | Lambertian reflection and transmission | reflectance, transmittance |
| `mix` | chooses one of two materials | two material ids, amount |

The ones the other plans add through the same table: `coated_diffuse` and `coated_conductor` ([pbrt-layered-materials.md](pbrt-layered-materials.md)), `measured` ([pbrt-measured-bsdfs.md](pbrt-measured-bsdfs.md)), `hair` ([pbrt-curves-and-hair.md](pbrt-curves-and-hair.md)), `subsurface` ([pbrt-subsurface-scattering.md](pbrt-subsurface-scattering.md)), `interface` ([pbrt-participating-media.md](pbrt-participating-media.md)). The laser optics plans add `optical_interface` ([physoptics-optical-materials.md](physoptics-optical-materials.md)), which reuses this plan's `frDielectric()`, `frComplex()` and dielectric BxDF with a coating in place of the bare Fresnel term, and its rough `glass` for ground lens edges.

### The BxDF interface in GLSL

New BxDFs work in pbrt's local shading frame (PBR 4ed §9.1.1): z is the outward shading normal, x is dp/du, directions point away from the surface, and `wo.z < 0` means the ray arrived from inside.

```glsl
struct BSDFSample {
    Spectrum f;          // the BSDF value, not yet times |cos θ|
    vec3 wi;             // local frame
    float pdf;           // solid angle; 1 for a delta lobe
    uint flags;          // CRT_BXDF_REFLECTION | TRANSMISSION | DIFFUSE | GLOSSY | SPECULAR
    float eta;           // relative IOR crossed by a transmission, else 1
    bool pdfIsProportional;   // layered BSDFs (pbrt-layered-materials.md)
};

Spectrum bxdfF(BxDFParams b, vec3 wo, vec3 wi, uint mode);
bool bxdfSampleF(BxDFParams b, vec3 wo, float uc, vec2 u, uint mode, out BSDFSample s);
float bxdfPdf(BxDFParams b, vec3 wo, vec3 wi, uint mode);
uint bxdfFlags(BxDFParams b);
```

`mode` is radiance or importance transport (PBR 4ed §9.5.2), which matters only for the dielectric's η² factor and for the bidirectional integrators. Light paths from lasers to detectors are importance transport, so a detector reads power with no η² factor, as [physoptics-detectors.md](physoptics-detectors.md) requires.

`crt_material.glsl` evaluates a material's parameters at the interaction into `BxDFParams` and switches on the type. The existing six types keep their world-space code in `crt_scatter.glsl`, called exactly as today, so they stay bit-identical. Kernel 06's body branches once, on whether the material is one of the original six.

### Files

| File | Holds |
|---|---|
| `crt_microfacet.glsl` | Trowbridge–Reitz with anisotropic α: D, Λ, G1, G, the visible-normal density and its sampling (PBR 4ed §9.6), `effectivelySmooth()` (α < 1e-3), `regularize()` |
| `crt_fresnel.glsl` | `frDielectric()` (exact, with total internal reflection), `frComplex()` (complex η per lane, §9.3.6) |
| `crt_bxdf_diffuse.glsl` | diffuse, Oren–Nayar, diffuse transmission |
| `crt_bxdf_conductor.glsl` | conductor |
| `crt_bxdf_dielectric.glsl` | glass, thin glass |
| `crt_material.glsl` | parameter evaluation, the type switch, mix resolution |

Every one has its line-for-line C++ counterpart in `rt_bsdf`.

### What each BxDF gets right

- `conductor` takes its reflectance from the complex Fresnel equations with per-lane η and k (gold, copper and the rest come from named spectra, [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)). A reflectance-only mode sets η = 1 and derives k from the color, as pbrt-v4 does. In exact RGB, η and k are evaluated at representative wavelengths for red, green and blue (630, 532 and 465 nm).
- `glass` refracts with the generalized half vector (§9.7), chooses reflection with probability R, divides radiance by η² on transmission in radiance mode, and returns the crossed η so that [pbrt-russian-roulette.md](pbrt-russian-roulette.md) can track `etaScale`. A non-constant η terminates the secondary wavelengths ([pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)).
- `thin_glass` sums the geometric series of inter-reflections between two parallel interfaces (§9.5.1) and transmits along −wo, for window panes and bubbles that should not bend light.
- Oren–Nayar, the `diffuse` type with σ above 0, is Oren and Nayar's approximation with A and B precomputed from σ (PBR 3ed §8.4.1). pbrt-v4 dropped it; it stays useful for rough clay and the moon.
- `mix` resolves to one of its two materials with probability equal to the amount, as pbrt-v4 does, so a 50 % mix costs the evaluation of one material (PBR 4ed §10.5.1). The choice must come out the same whenever the same hit is evaluated again (next-event estimation and the continuation both evaluate it), so it uses a hash of the hit point and the outgoing direction, as pbrt's `ChooseMaterial()` does, rather than a sample dimension.

`bxdfFlags()` replaces `isSpecularMaterial()` for the new types: a surface whose every lobe is specular skips next-event estimation and leaves the MIS record at full weight, as today.

### Anisotropy

Rough conductors and glass take separate u and v roughness, applied along dp/du and dp/dv from the interaction. glTF's `KHR_materials_anisotropy` maps onto them.

### glTF extensions

fastgltf parses the material extensions; the importer maps them onto these types:

| Extension | Becomes |
|---|---|
| `KHR_materials_ior` | the pbr material's specular F0, and `glass` η |
| `KHR_materials_transmission` | `glass` with the base color as transmission tint, roughness carried over |
| `KHR_materials_volume` | an absorbing interior medium ([pbrt-participating-media.md](pbrt-participating-media.md)), or thin-walled `thin_glass` when the thickness is 0 |
| `KHR_materials_specular` | the pbr material's specular strength and color |
| `KHR_materials_anisotropy` | u and v roughness |

A glTF material with none of these keeps becoming `pbr`, so no existing import changes.

## Verification

- Every existing scene renders bit-identical.
- `bin/bsdf_test` passes the χ², weight and energy tests for every new type across roughness 0.01 to 1, IOR 1.1 to 2.5 and incident angles to 89° ([pbrt-verification-harness.md](completed/pbrt-verification-harness.md)), and the reciprocity test for glass in both transport modes.
- The white furnace shows glass and a smooth conductor with η = 1, k = 0 as invisible, and rough conductors losing the energy single scattering is known to lose (compensated later by [pbrt-appearance-extras.md](pbrt-appearance-extras.md)).
- A glass sphere seen from inside a water-filled box (camera inside a closed glass mesh) renders with η² scaling on; the same scene with the scaling removed renders too bright, which is the bug the scaling fixes.

## Order of work

1. `crt_microfacet.glsl` and `crt_fresnel.glsl`, mirrored and tested on the CPU first.
2. `diffuse` and `conductor`, and the kernel 06 branch between old and new types.
3. `glass`, then `thin_glass`, `diffuse_transmission`, `mix`.
4. Oren–Nayar and anisotropy.
5. The glTF extensions.

## Sources

PBR 4ed chapter 9 (§9.1 BSDF representation and delta conventions, §9.2 diffuse, §9.3 Fresnel for dielectrics and conductors, §9.4 conductor BRDF, §9.5 dielectric and thin dielectric BSDFs and non-symmetric scattering, §9.6 microfacet theory and visible-normal sampling, §9.7 rough dielectric), §10.5.1 (diffuse, dielectric and mix materials); PBR 3ed §8.4.1 (Oren–Nayar), §9.2.5 (the other materials); Walter et al. 2007; Heitz 2018; pbrt-v4 `src/pbrt/bxdfs.h`, `util/scattering.h`, `materials.h`; the Khronos glTF extension specifications named above.
