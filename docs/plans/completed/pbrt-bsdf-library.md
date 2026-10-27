# BSDF library

The six materials the tracer has are Ray Tracing in One Weekend's lambertian, fuzzed metal, phong and Schlick dielectric, an emitter, and glTF's metallic-roughness model. None of them has an exact Fresnel term, a complex index of refraction, a rough dielectric, anisotropy, a thin window pane or diffuse transmission, and the dielectric does not scale radiance by η² when it refracts. This plan adds pbrt-v4's BxDFs as new material types beside the existing ones, which all stay.

*Implemented 2026-10-26. `glass`, `conductor`, the Fresnel and microfacet modules and the BxDF interface came first, on 2026-10-23, with [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md); "Built with spectral rendering" records them and "As built" records the rest. "Not built" lists what the user left out. Depends on [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md), [pbrt-material-system.md](pbrt-material-system.md) and [pbrt-verification-harness.md](pbrt-verification-harness.md). Part of the [roadmap](../pbrt-roadmap.md).*

## Built with spectral rendering

On 2026-10-23 the user asked for every part of this plan that dispersion needs, and nothing more ([pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)). As built:

- `src/bxdf.h/.cpp` in `rt_bsdf` holds `frDielectric()`, `frComplex()`, `refractDirection()`, `TrowbridgeReitz`, `DielectricBxdf` and `ConductorBxdf` (with `fromReflectance()`), pbrt-v4's code. Its line-for-line GLSL mirrors are `crt_fresnel.glsl`, `crt_microfacet.glsl` and one `crt_bxdf.glsl` for both BxDFs, in place of the per-family files the table under "Files" lists. Add the next BxDFs to `crt_bxdf.glsl` or split it when it grows.
- The interface is `bxdfSample()`, `bxdfF()`, `bxdfPdf()` and `bxdfFlags()` on a `BxdfMaterial`, with the transport mode. `BxdfSample` has no `pdfIsProportional` yet; [pbrt-layered-materials.md](../pbrt-layered-materials.md) adds it.
- `glass` (η as a spectrum, roughness u and v, remap roughness) and `conductor` (η and k, or from reflectance; roughness u and v) are `MATERIAL_TRAITS` rows in the complex material class, so only kernel 06's complex build evaluates them, and `ComplexMaterial` in `crt_scatter.glsl` dispatches to them beside the `pbr` path. The basic build and every existing scene are unchanged. Anisotropy follows dp/du from the interaction.
- In exact RGB, `glass` reads η at the scene's **Design wavelength** (587.6 nm by default) and `conductor` reads η and k at 630, 532 and 465 nm. An RGB η or k is read per channel, as pbrt's `RGBUnboundedSpectrum`.
- Neither type takes a texture or a normal map yet: `albedoLayer` and `normalLayer` are -1 in their records. [pbrt-textures.md](../pbrt-textures.md) and a later step of this plan add them.
- The importer maps pbrt's `conductor` (a textured reflectance still becomes `pbr`) and `dielectric`, smooth or rough, as "Importing pbrt scenes" asks. `thindielectric` is still solid glass.
- `bin/bsdf_test --material glass|conductor` runs the χ², weight, energy and reciprocity checks on them, in both transport modes for glass, and **Windows** → **GPU self-tests** → **BxDF parity** compares `crt_bxdf.glsl` with `src/bxdf.cpp`. `tools/verify/spectral_check.py`'s furnace shows glass vanishing and a rough glass losing what single scattering loses.


## What the renderer does today

- `crt_scatter.glsl` implements each material three times (sample, evaluate, density) in world space, with a normal flipped to face the incoming ray.
- The dielectric uses Schlick's approximation and a scalar IOR, and treats every refraction as radiance-preserving.
- The metal is a mirror direction plus a random offset in a ball of radius `fuzz`. Its density was derived by hand (`fuzzBallPdf()`).
- The pbr material samples GGX visible normals (Heitz 2018) with a colored Schlick Fresnel, and layers a Lambertian lobe under it without conserving energy at grazing angles (its own comment says so).
- [pbrt-verification-harness.md](pbrt-verification-harness.md) measured two flaws this plan must not carry over. Schlick's approximation is called with the incident cosine in both directions, so inside glass of index 1.5 it gives 0.040 at 30 degrees and 0.041 at 40 where the exact Fresnel reflectance is 0.055 and 0.245 (0.380 at 41 degrees, just under the critical angle); from outside it is within 0.013 up to 70 degrees. `glass` and `thin_glass` use `frDielectric()`, which has no such error. And phong, the fuzzy metal and the Lambertian lobe of `pbr` are not reciprocal (the relative asymmetry of `f(wo, wi)` against `f(wi, wo)` is 0.5 to 0.95): the Lambertian lobe's `1 - F(wo)` factor depends on one direction only. A new BxDF should be reciprocal, and `bin/bsdf_test` fails a type listed as reciprocal that is not. The three old types stay as they are, to keep every scene bit-identical.
- `isSpecularMaterial()` decides which surfaces skip next-event estimation: a dielectric, or a metal whose fuzz is below 1e-4.

## Design

### New material types

Each is a `MATERIAL_TRAITS` entry ([pbrt-material-system.md](pbrt-material-system.md)). Names avoid the ones scene files already use: `dielectric` keeps meaning the Ray Tracing in One Weekend glass.

| Type | BxDF | Parameters |
|---|---|---|
| `diffuse` | Lambertian, or Oren–Nayar when σ > 0 | reflectance, σ (degrees) |
| `conductor` | smooth or rough Torrance–Sparrow with complex Fresnel | η and k, or reflectance; roughness u and v; remap roughness |
| `glass` | smooth or rough dielectric (Walter et al. 2007) with exact Fresnel | η; roughness u and v; remap roughness |
| `thin_glass` | thin dielectric, R' = R + T²R / (1 − R²) | η |
| `diffuse_transmission` | Lambertian reflection and transmission | reflectance, transmittance |
| `mix` | chooses one of two materials | two material ids, amount |

The ones the other plans add through the same table: `coated_diffuse` and `coated_conductor` ([pbrt-layered-materials.md](../pbrt-layered-materials.md)), `measured` ([pbrt-measured-bsdfs.md](../pbrt-measured-bsdfs.md)), `hair` ([pbrt-curves-and-hair.md](../pbrt-curves-and-hair.md)), `subsurface` ([pbrt-subsurface-scattering.md](../pbrt-subsurface-scattering.md)), `interface` ([pbrt-participating-media.md](../pbrt-participating-media.md)). The laser optics plans add `optical_interface` ([physoptics-optical-materials.md](../physoptics-optical-materials.md)), which reuses this plan's `frDielectric()`, `frComplex()` and dielectric BxDF with a coating in place of the bare Fresnel term, and its rough `glass` for ground lens edges.

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

`mode` is radiance or importance transport (PBR 4ed §9.5.2), which matters only for the dielectric's η² factor and for the bidirectional integrators. Light paths from lasers to detectors are importance transport, so a detector reads power with no η² factor, as [physoptics-detectors.md](../physoptics-detectors.md) requires.

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
- `glass` refracts with the generalized half vector (§9.7), chooses reflection with probability R, divides radiance by η² on transmission in radiance mode, and returns the crossed η so that [pbrt-russian-roulette.md](../pbrt-russian-roulette.md) can track `etaScale`. A non-constant η terminates the secondary wavelengths ([pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)).
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
| `KHR_materials_volume` | an absorbing interior medium ([pbrt-participating-media.md](../pbrt-participating-media.md)), or thin-walled `thin_glass` when the thickness is 0 |
| `KHR_materials_specular` | the pbr material's specular strength and color |
| `KHR_materials_anisotropy` | u and v roughness |

A glTF material with none of these keeps becoming `pbr`, so no existing import changes.

## Importing pbrt scenes

[completed/pbrt-scene-import.md](pbrt-scene-import.md) imports pbrt-v4 scenes today and substitutes for this plan's features. Each substitution is a line in the import report, so `./bin/pbrt_test --report <file.pbrt>` lists the scenes that need this plan. When this plan lands, change the importer as follows, then run `tools/verify/pbrt_compare.py` on the scenes named below.

- **Conductors** become `pbr` with metallic 1, the metal's color at normal incidence and Schlick's Fresnel (`mapMaterial()` and `conductorColor()` in `src/pbrt_import.cpp`). Map `conductor` to this plan's conductor with eta and k (RGB until [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)) and its anisotropic roughness, which `roughnessOf()` averages today. 78 conductors, in 13 scenes; `killeroo-gold` is the comparison scene.
- **Rough dielectrics** are smooth (`setDielectric()` reports them; `lte-orb-rough-glass`). `thindielectric` is a solid dielectric (`villa`). Map both to this plan's dielectric and thin dielectric.
- **Diffuse transmission** is Lambertian with its transmittance dropped (326 materials, mostly foliage, in `barcelona-pavilion`, `bunny-fur`, `kroken`, `landscape`, `sanmiguel`, `villa` and `watercolor`). Map it to this plan's diffuse transmission with `reflectance`, `transmittance` and `scale`.
- **Mix** picks whichever of its two materials pbrt chooses more often, from a constant `amount`; an `amount` texture is read as its constant (`bmw-m6`, `crown`, `kroken`, `watercolor`). Map it to this plan's mix with the amount as a texture once [pbrt-textures.md](../pbrt-textures.md) can carry one.
- **Roughness.** pbrt's `roughness` becomes GGX alpha through `RoughnessToAlpha()` (`sqrt` when `remaproughness` is true), and `pbr` takes the square root of that. Give the new BxDFs alpha directly.

## Verification

- Every existing scene renders bit-identical.
- `bin/bsdf_test` passes the χ², weight and energy tests for every new type across roughness 0.01 to 1, IOR 1.1 to 2.5 and incident angles to 89° ([pbrt-verification-harness.md](pbrt-verification-harness.md)), and the reciprocity test for glass in both transport modes.
- The white furnace shows glass and a smooth conductor with η = 1, k = 0 as invisible, and rough conductors losing the energy single scattering is known to lose (compensated later by [pbrt-appearance-extras.md](../pbrt-appearance-extras.md)).
- A glass sphere seen from inside a water-filled box (camera inside a closed glass mesh) renders with η² scaling on; the same scene with the scaling removed renders too bright, which is the bug the scaling fixes.

## Order of work

1. `crt_microfacet.glsl` and `crt_fresnel.glsl`, mirrored and tested on the CPU first.
2. `diffuse` and `conductor`, and the kernel 06 branch between old and new types.
3. `glass`, then `thin_glass`, `diffuse_transmission`, `mix`.
4. Oren–Nayar and anisotropy.
5. The glTF extensions.

## As built

The user settled these on 2026-10-26.

- `diffuse` (reflectance, σ in degrees), `thin_glass` (η as a spectrum), `diffuse_transmission` (reflectance, transmittance, scale) and `mix` are `MATERIAL_TRAITS` rows in the complex class, so only kernel 06's complex build evaluates them. `src/bxdf.h` holds `DiffuseBxdf` (Lambertian at σ = 0, Oren and Nayar's model above), `DiffuseTransmissionBxdf`, `ThinDielectricBxdf` and `mixChoosesSecond()`; `crt_bxdf.glsl` mirrors them line for line. A `diffuse` has no texture. The `lambertian` type stays for textured surfaces.
- A `mix` takes two library materials by reference and an `amount`, the chance of the second. The reference is a new parameter kind, `ParamKind::Material`, holding a `MaterialLink`. A child may be any material that scatters, including another mix, to a depth of 8. Kernel 06 resolves the chain first, from a hash of the hit point, the outgoing direction and the mix's material index (`mixHash()`, PCG over the float bits), and then reads the child everywhere it read the mix. The choice comes out the same each time the same hit is evaluated, with no sample dimension. A mix does not show its children's emission or alpha cutoff, because kernels 02 and 04 read the mix's header; the child's normal map and the rest apply. `buildSceneAccel()` packs a mix's children when it packs the mix, so a child gets a slot in the material table even when nothing else references it.
- The scene file is payload version 19. A mix writes `"materialA"` and `"materialB"` as a shape writes its `"material"` (an index into `"materials"`, or a model and a material index), and `"f0"` joins the `pbr` parameters. An imported mix holds its children's indices in the model until `addModel()` gives them ids.
- `thin_glass` and `diffuse_transmission` follow pbrt-v4's code. `thin_glass` reflects 2R / (1 + R) and transmits straight on in both transport modes. A diffuse transmission chooses reflection or transmission by the larger component of each, as pbrt does.
- `pbr` reads its dielectric F0 from the `param` float of its record, 0 for glTF's 0.04, so every record that existed renders bit-identical. A new `f0` parameter sets it.
- The importer maps pbrt's `thindielectric` to `thin_glass`. It maps a `diffusetransmission` with a constant reflectance and transmittance to `diffuse_transmission`; a textured one stays Lambertian with its texture and keeps its report line, because the complex types take no textures until [pbrt-textures.md](../pbrt-textures.md). It maps a `mix` of two named materials to `mix`, with the children as materials of the model, and falls back to the child pbrt picks more often when one is missing or invisible. A pbrt scene's rough dielectric and conductor were already mapped.
- glTF imports read three extensions. `KHR_materials_ior` and the factor of `KHR_materials_specular` set `f0` to ((ior − 1) / (ior + 1))² times the factor. `KHR_materials_transmission` with a factor of 1, no texture and no emission becomes `glass` with the file's index and the roughness glTF's alpha gives (`remapRoughness` off). `pavilion-day.gltf`'s water and architectural glass take this path; `structure.glb`'s `light_shafts` (textured, emissive) stays `pbr`. A material the loader reads but cannot carry prints a line on the console.

Verification, all in exact RGB:

- `bin/bsdf_test` runs the χ², weight, energy and reciprocity checks on Lambertian, Oren-Nayar at σ of 20, 45 and 90, and diffuse transmission, from both sides. It checks a thin pane against the closed form 2R / (1 + R) at three indices, and a mix's choice against its amount over 200,000 hits. `--selfcheck` plants a diffuse evaluation 5 percent high and expects it caught. The hemisphere suite gains `pbr` rows with an F0 other than 0.04.
- **Windows** → **GPU self-tests** → **BxDF parity** covers the four new BxDFs and compares `mixHash()` bit for bit with the shader's: 90,112 cases, none different.
- `tools/verify/bsdf_check.py` renders furnaces. A white `diffuse`, a thin pane, and a diffuse transmission whose parts sum to 1 vanish to within 0.002, a 0.5 `diffuse` is 0.5, and mixes give their mean. A camera inside a closed glass sphere in the white environment sees 2.2500 = η², the case this plan's verification asked for.
- Seven scenes (`all_materials`, `caustic_glass`, `cornell_box`, `sphere_scene`, `thin_glass`, `veach_mis`, `white_furnace`) and an auxiliary-output render of `all_materials` render with the same pixels as the build before. A mix of diffuse and thin dielectric agrees with pbrt (1.0000 against 1.0).
- The same furnaces in the hero (4 and 32 lanes), band (8) and monochromatic representations give the same values to within 0.002, a dispersive thin pane included.
- `bin/pbrt_test --scan` imports all 98 scene files.

A closed cavity of Lambertian and refracting glass loses light to total internal reflection that the renderer's depth limit of 16 (`CRT_MAX_DEPTH`) cuts off. A camera inside a sphere that is half Lambertian and half glass of index 1.5 sees 2.08 where the exact value is 2.25, and the loss grows with the index. pbrt at `maxdepth` 100 gets the exact value. `bsdf_check.py` keeps its mixes out of such cavities.

## Not built

- The glTF extensions `KHR_materials_anisotropy` and `KHR_materials_volume`. The loader parses both and prints a line when a material uses them. `pbr` has one roughness, and the volume needs the interior medium of [pbrt-participating-media.md](../pbrt-participating-media.md). Anisotropy in `conductor` and `glass` is built.
- A partial transmission factor, a transmission texture and a non-white `KHR_materials_specular` color. A `mix` of `pbr` and `glass` would carry the first, but a model's library holds one entry per glTF material, so the two children have nowhere to live. The material stays `pbr`, and the console says so.
- The sampled-direction check against the geometric normal that [pbrt-bump-mapping.md](../pbrt-bump-mapping.md) describes for the new BxDFs. It waits for that plan.
- Textures on every type but `pbr` and `lambertian`, which [pbrt-textures.md](../pbrt-textures.md) adds.
- `pdfIsProportional` on `BxdfSample`, which [pbrt-layered-materials.md](../pbrt-layered-materials.md) adds.

## Sources

PBR 4ed chapter 9 (§9.1 BSDF representation and delta conventions, §9.2 diffuse, §9.3 Fresnel for dielectrics and conductors, §9.4 conductor BRDF, §9.5 dielectric and thin dielectric BSDFs and non-symmetric scattering, §9.6 microfacet theory and visible-normal sampling, §9.7 rough dielectric), §10.5.1 (diffuse, dielectric and mix materials); PBR 3ed §8.4.1 (Oren–Nayar), §9.2.5 (the other materials); Walter et al. 2007; Heitz 2018; pbrt-v4 `src/pbrt/bxdfs.h`, `util/scattering.h`, `materials.h`; the Khronos glTF extension specifications named above.
