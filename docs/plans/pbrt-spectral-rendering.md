# Spectral rendering

The tracer multiplies RGB triples. PBR 4ed §4.6.4 shows why that is not physics: the same sRGB green, expressed in ACES2065-1 and multiplied by itself, comes out darker, more saturated and outside the gamut of real colors, because a product of tristimulus values depends on the color space. Dispersion, blackbody emitters, measured metal indices of refraction and chromatic media all need light carried per wavelength. This plan makes the quantity every path carries a spectrum whose size and meaning are chosen at runtime, while exact RGB stays available and bit-identical.

*Not implemented. Depends on [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) and [pbrt-material-system.md](completed/pbrt-material-system.md). The [laser optics plan](physoptics-laser-optics.md) builds its glasses, coatings, lasers and spectral goals on it; the additions marked 2026-10-18 below come from there. Part of the [roadmap](pbrt-roadmap.md).*

## Requirements and decisions

The user called this the change they care about most, and the one most likely to change (2026-09-26):

- A runtime option that keeps the RGB renderer exactly as it is today.
- A 3-component spectral mode whose components are weights of red, green and blue Gaussians in wavelength.
- Denser distributions over arbitrary wavelength ranges.
- Room for a far-future goal: an approximate electromagnetic simulation in frequency space (x-ray and infrared optics, broadband spectra, polarization, diffraction) through a Monte Carlo solution of a path integral over a dyadic Green's tensor. The details are not needed now; the design must not close that door. On 2026-10-18 the user decided to design and build that backend themselves, last, after every pbrt and physoptics plan; no plan describes it.

Decisions taken the same day:

- Spectral is a runtime mode, compiled in families by lane count (`CRT_LANES` 4, 8, 16, 32), selected at runtime.
- Materials and lights are authored once, as `SpectrumRef`s, and each representation evaluates or projects them. Exact RGB passes RGB inputs through untouched.
- New scenes default to exact RGB until the spectral modes are verified.

## What the renderer does today

- `PathState.throughput`, `ShadowRay.contribution` and every radiance computation are `vec3`.
- Material colors (`GpuMaterial.albedo`, `emission`), light radiance (`GpuLight.radiance`), the background color and the environment map are linear RGB with Rec. 709 primaries. Textures are sRGB-encoded RGB.
- The film accumulates RGB, and `tonemap.comp` shows it.
- The dielectric has one scalar IOR. The metal has an RGB albedo and no complex index of refraction.

## Design

Three independent axes describe a spectral configuration:

1. The lane count (compile time) is how many values a path carries: 4, 8, 16 or 32. It chooses the pipeline family.
2. The representation (runtime, within a family) says what those values mean: RGB channels, fixed basis bands, or stochastically sampled wavelengths.
3. The spectral data (scene) is what materials and lights specify, as `SpectrumRef`s, independent of the other two.

### Lane-count families

GLSL has no templates, so the lane count is a define:

```glsl
// crt_spectrum.glsl
#ifndef CRT_LANES
#define CRT_LANES 4
#endif
#define CRT_LANE_VECS (CRT_LANES / 4)
struct Spectrum { vec4 c[CRT_LANE_VECS]; };

Spectrum spectrumMul(Spectrum a, Spectrum b);      // lane by lane
Spectrum spectrumScale(Spectrum a, float s);
float spectrumMax(Spectrum a);                     // over live lanes
float spectrumAverage(Spectrum a);
bool spectrumIsBlack(Spectrum a);
```

With four lanes the struct is one `vec4`, and arithmetic on lanes 0 to 2 is exactly the `vec3` arithmetic of today, which is what makes exact RGB bit-identical.

Kernels whose code touches radiance or reflectance depend on the lane count: 00, 01 (if a sampler writes wavelength dimensions), 03, 04, 05, 06, 07, 08 and 09. Kernel 02 does not, since the thin hit record carries no radiance. `KernelVariant` gains `laneDependent`, and:

- CMake compiles each lane-dependent kernel once per family listed in `VKGUIDE_SPECTRAL_FAMILIES` (default `4;8;16;32`), as `0601_surface_scatter_bsdf.comp.lanes8.spv` and so on. Kernels that are not lane-dependent compile once. This is the second permutation axis after the traversal one of [integrator-layer.md](completed/integrator-layer.md): a kernel that traces and is lane-dependent, such as kernel 08, is compiled once per family and traversal, as `0801_trace_shadow_rays.comp.lanes8.bvh_binary.spv`.
- `GpuRenderer`'s pipeline cache, keyed today by variant and traversal, gains the family in its key. It builds a family's pipelines the first time a render needs that family and keeps them, so a render that stays in one family never pays for the others.

A future polarized family ([pbrt-polarization.md](pbrt-polarization.md)) turns each lane of the throughput into a four-component adjoint Stokes vector; the throughput's type is the only one that changes.

### Representations

A representation defines what each lane means, how `SpectrumRef`s turn into lane values, and how lanes turn into the film's color. The table of representations is data (`SPECTRAL_REPRESENTATIONS` in `src/spectrum.h`), so a new one is an entry plus, at most, one evaluation branch in `crt_spectrum.glsl`.

| Representation | Families | Lanes mean | Evaluating data | To the film |
|---|---|---|---|---|
| Exact RGB | 4 | R, G, B, unused | RGB passes through; spectral data is projected to RGB on the CPU | identity |
| RGB Gaussians | 4 | weights of three Gaussian bands | projected onto the bands on the CPU; textures uplifted then projected | a 3 × 3 band-to-XYZ matrix |
| Spectral bands | 8, 16, 32 | N fixed bands over [λmin, λmax], box or Gaussian basis | projected onto the bands | an N × 3 band-to-XYZ matrix |
| Hero wavelengths | 4, 8, 16, 32 | N wavelengths drawn per path | evaluated at each wavelength | Monte Carlo estimate against the color-matching functions |

Hero wavelengths follow pbrt-v4 (PBR 4ed §4.5.4): one uniform sample chooses the first wavelength, the rest follow at equal steps and wrap around, and over the visible range the sample is warped by pbrt's visual-importance density `p(λ) ∝ sech²(0.0072 (λ − 538))` (§5.4.2), which cut color noise sharply in pbrt's test for a 1 % cost. Outside the visible range, or for a user range, sampling is uniform.

Light paths, which the light integrator of [pbrt-bidirectional-path-tracing.md](pbrt-bidirectional-path-tracing.md) starts at an emitter, draw their wavelengths from that emitter's spectrum instead (2026-10-18): the lanes are stratified samples of the emitter's normalized spectral power, and each lane's density is that distribution's. For a laser whose linewidth is below the sampling resolution every lane lands on the line, with one density, so no lane is spent where the source emits nothing. A camera path keeps the visual-importance or uniform sampling above, and a scene lit only by lasers outside the visible range defaults its range to theirs ([physoptics-gratings-and-xuv.md](physoptics-gratings-and-xuv.md)).

Bands are deterministic: every path carries every band, so broadband quantities such as a transmission spectrum through a filter stack come out without wavelength noise. The RGB Gaussian mode is the three-band case the user asked for, with a Gaussian basis.

### Path state

Whatever the representation, a path stores only:

- `throughput`, a `Spectrum`.
- `lambdaU`, the one uniform sample its hero wavelengths derive from. The wavelengths and their densities are recomputed where needed, which is a handful of instructions and keeps `PathState` from growing by 8 floats per 4 lanes (pbrt-v4 chapter 15's exercise on compressing `SampledWavelengths`).
- `laneMask`, which lanes are still alive after a dispersive event. All ones otherwise.

A shadow ray is added to the film by kernel 08, after it knows the visibility and, in a medium, the per-lane transmittance ([pbrt-participating-media.md](pbrt-participating-media.md)). So `ShadowRay.contribution` becomes a `Spectrum` too, and the shadow ray carries the path's `lambdaU` and `laneMask` for the conversion. With four lanes the shadow ray is 64 bytes, up from [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md)'s 48; the roadmap's shadow-ray table gives the layout.

### `SpectrumRef`: authoring once

Every spectrally varying parameter of a material, light, medium or background is a `SpectrumRef`:

| Kind | Holds | Example |
|---|---|---|
| `RgbReflectance` | an RGB color in [0, 1] | a diffuse albedo |
| `RgbUnbounded` | an RGB color of any magnitude | a scattering coefficient |
| `RgbIlluminant` | an RGB color and a scale | an area light's radiance |
| `Tabulated` | (λ, value) pairs | gold's η and k, a measured filter |
| `Blackbody` | a temperature in kelvin and a scale | a 2700 K lamp |
| `DispersionFormula` | a formula id and up to ten coefficients, in the formula set of the AGF glass catalog format (Schott, Sellmeier 1 to 5, Conrady, Herzberger, the Handbook of Optics and extended forms; Cauchy is one of them) | a catalog glass's index, from [physoptics-optical-materials.md](physoptics-optical-materials.md) (2026-10-18; this replaces separate `Cauchy` and `Sellmeier` kinds) |
| `Line` | a center wavelength, a linewidth and a profile (Gaussian or Lorentzian), and a scale | a laser line ([physoptics-laser-sources.md](physoptics-laser-sources.md), 2026-10-18) |
| `Constant` | one value | a non-dispersive IOR of 1.5 |

On the GPU a `SpectrumRef` is 16 bytes (a kind and three payload words) in the material or light record, pointing into `SpectralData` (binding 24):

- For the RGB kinds, the RGB triple itself, which is all exact RGB reads, plus the three sigmoid-polynomial coefficients that spectral modes evaluate.
- For every other kind, an RGB projection computed on the CPU (for exact RGB), and the tabulated samples or coefficients.
- For band representations, each reference's band coefficients, projected on the CPU for the active band set and rebuilt when the representation changes.

RGB textures are uplifted per texel on the GPU. `SpectralUplift` (binding 25) holds pbrt-v4's sigmoid coefficient table for sRGB (3 × 64³ × 3 floats, 9.4 MB): a trilinear lookup gives three coefficients, and `s(c0 λ² + c1 λ + c2)` with `s(x) = ½ + x / (2√(1 + x²))` gives the reflectance at any wavelength (Jakob and Hanika 2019; PBR 4ed §4.6.6). The table is generated by a port of pbrt-v4's `rgb2spec_opt`, built as a CMake tool and run once per build tree, and cached.

The environment map stays an rgba16f image. Its texels are uplifted as illuminants: a sigmoid spectrum for the normalized color, times the scale, times the D65 illuminant (pbrt's `RGBIlluminantSpectrum`), so a white texel lights a white surface white.

### From lanes to the film

Kernels 03, 04 and 08 add radiance to the pool slot. They convert to film space at the moment they add:

```glsl
void filmAdd(uint slot, PathState path, Spectrum L);   // crt_spectrum.glsl
```

- Exact RGB: adds lanes 0 to 2, exactly as today.
- Bands: multiplies by the precomputed band-to-XYZ matrix, then XYZ to the working RGB space (linear Rec. 709, negative values kept).
- Hero: the Monte Carlo estimate of the CIE XYZ integrals (PBR 4ed Equation 4.23), `Σ L_i x̄(λ_i) / p(λ_i) / N` over live lanes, then to working RGB.

Converting at every add, rather than once at the film as pbrt does, also removes the extra color noise pbrt's chapter 13 exercise describes: contributions added before a dispersive event keep the wavelength densities they had when they were added. The radiance slot stays a `vec4`, so kernel 09, the accumulation image and everything after them are unchanged in every family.

The color-matching functions are the CIE 1931 2° tables at 1 nm, 360 to 830 nm, stored in `SpectralData`. The film plan can replace them with a camera's sensor response curves (pbrt's `PixelSensor`, PBR 4ed §5.4.2), which is a different table in the same place. Outside the visible range XYZ says nothing, so [pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md) writes band images directly (pbrt's `SpectralFilm`) and the display maps them to false color.

### Dispersion

A dielectric whose IOR is not constant sends different wavelengths in different directions, so after the refraction the path can follow only one:

- Hero wavelengths: pbrt's `TerminateSecondary()`. Lane 0 survives, the others are masked off, and lane 0's density is multiplied by `1 / N` (PBR 4ed §4.5.4, §10.5.1).
- Bands: choose one live band uniformly, mask the others, and multiply the surviving lane by the number of live bands.
- Exact RGB: no dispersion. The dielectric reads its IOR at 550 nm, which is what the scalar IOR means today. The optical glasses of [physoptics-optical-materials.md](physoptics-optical-materials.md) read theirs at the scene's **Design wavelength** instead (587.6 nm by default, or the scene's laser wavelength), so a 1064 nm setup focuses in exact RGB where it would in the lab; legacy materials keep 550 nm (2026-10-18).

The IOR is evaluated at the surviving lane's wavelength (hero) or band center (bands). [pbrt-bsdf-library.md](pbrt-bsdf-library.md) adds the dispersive dielectric; this plan supplies `laneMask` and the termination functions.

### Lights and the light list

`GpuLight.radiance` becomes a `SpectrumRef`. The light list still prices lights by power, from the photometric luminance (Y) of each light's spectrum, computed on the CPU, so light selection does not change between representations. Blackbody lights store a normalized Planck curve (`BlackbodySpectrum`, PBR 4ed §4.4.1) and a scale; the UI offers **Color temperature** beside **Color** on every light ([pbrt-light-types.md](pbrt-light-types.md)).

### Named spectra

pbrt-v4 embeds measured spectra that the scene file names (PBR 4ed §4.5.3), under the Apache 2.0 license: η and k for silver, aluminum, gold, copper, brass and others, dispersion data for BK7 and other glasses, the CIE standard illuminants A, D50, D65 and F1 to F12, and several camera sensors. They go into `assets/spectra/` as `.spd` files (λ, value per line), loaded on demand, with the license and the source named in a header comment. Optical glasses come from AGF catalogs instead, bundled and user-loaded, and the XUV complex indices from the Henke tables, both added by the laser optics plans ([physoptics-optical-materials.md](physoptics-optical-materials.md), [physoptics-gratings-and-xuv.md](physoptics-gratings-and-xuv.md)).

### The panel

**Raytrace Render** gains a **Spectral** section:

- **Representation** chooses exact RGB, RGB Gaussians, spectral bands or hero wavelengths.
- **Lanes** is 4, 8, 16 or 32, fixed at 4 for the two RGB modes.
- **Range** sets λmin and λmax, 360 to 830 nm by default.
- **Band basis** (bands): box or Gaussian. **Gaussian centers and widths** (RGB Gaussians).
- **Wavelength sampling** (hero): visible importance or uniform.
- **Color matching** uses the CIE 1931 2° functions, or a sensor once the film plan adds them.

Every material's color widget gains a **Spectral** menu: a named spectrum, a blackbody temperature, a `.spd` file, or a dispersion formula (a catalog glass, or typed Cauchy or Sellmeier coefficients). Picking one turns the parameter into that `SpectrumRef`; picking a color again turns it back.

### Scene file

The payload version this lands in adds:

```json
"spectral": {"representation": "hero", "lanes": 4, "range": [360, 830],
             "sampling": "visible", "matching": "cie1931"}
```

and saves each `SpectrumRef` as either today's `[r, g, b]` array, which keeps older files and RGB parameters unchanged, or an object such as `{"spd": "metals/Au_eta.spd"}` or `{"blackbody": 2700, "scale": 1.0}`.

### Room for the electromagnetic goal

Nothing here builds toward wave optics, but four choices keep the path open:

- The lane count is not fixed anywhere outside `crt_spectrum.glsl`, so families beyond 32 lanes are a CMake list entry.
- Representations are data. An infrared or x-ray range is a range and a sampling density, and a non-visible film output already exists (band images).
- `SpectrumRef` kinds can grow. Complex refractive indices are already tabulated kinds (conductor η and k); a dielectric tensor or a coherence parameter would be further kinds.
- The polarized family ([pbrt-polarization.md](pbrt-polarization.md)) changes the element type of a lane from a scalar to a Stokes vector, the first step toward carrying fields rather than intensities.

## Order of work

1. `Spectrum` with `CRT_LANES = 4` and the exact RGB representation, replacing `vec3` in every kernel. Bit-identical.
2. `SpectrumRef` in materials, lights and backgrounds, RGB kinds only. Bit-identical.
3. `rt_spectrum` (CIE tables, the sigmoid table generator and evaluator, blackbody, `.spd` loading) and `bin/spectrum_test`.
4. Hero wavelengths with four lanes, `filmAdd()`, and uplifting of materials, textures, lights and the environment.
5. RGB Gaussians.
6. Spectral bands and the 8-, 16- and 32-lane families, with lazy pipeline builds.
7. Named spectra, blackbody, tabulated and dispersion-model kinds, and the **Spectral** menu.
8. Dispersion, with [pbrt-bsdf-library.md](pbrt-bsdf-library.md)'s dielectric.

## Verification

- Exact RGB renders bit-identical to today after steps 1 and 2, and after every later step, for the Cornell box, sphere scene, Veach scene and Sponza with a fixed seed.
- `bin/spectrum_test`: uplifting any sRGB color and projecting back gives the input to within 1e-3 (the table's identity goal, PBR 4ed §4.6.6); uplifted reflectances stay within [0, 1]; a 6504 K blackbody projects to within 0.005 of D65's chromaticity; the CIE tables integrate to their published values.
- A white furnace ([pbrt-verification-harness.md](completed/pbrt-verification-harness.md)) with gray materials under a D65 environment renders neutral gray in every representation.
- Hero wavelengths, spectral bands and RGB Gaussians converge to the same image on scenes with smooth spectra, within the tolerance the band count implies, measured with **Compare to reference**.
- A prism scene (a triangular prism of dense flint under a narrow white spotlight) shows a spectrum on the floor in hero and band modes, and a white spot in exact RGB.

## Open questions

These are the user's to settle before steps 5 and 7:

- The three Gaussians' centers and widths. One option is the dominant wavelengths of the sRGB primaries (about 612, 549 and 465 nm) with widths chosen so that the three bands reproduce the sRGB white point; another is fitting them to the CIE functions.
- In the RGB Gaussian mode, whether an RGB material color maps directly onto the three weights, which behaves like RGB with a defined spectrum behind it, or goes through the sigmoid uplift and is projected, which agrees with the other spectral modes but no longer reproduces RGB mode's colors exactly.
- Whether glTF colors, which are defined in sRGB, should be uplifted with the sRGB table in every case, or whether a scene should be able to declare a wide-gamut working space (pbrt supports sRGB, DCI-P3, Rec. 2020 and ACES2065-1 tables).

## Sources

PBR 4ed §4.4 (blackbody emitters, standard illuminants), §4.5 (spectral distributions, `SampledSpectrum`, `SampledWavelengths`, secondary termination), §4.6 (XYZ, RGB color spaces, why spectral rendering, number of wavelength samples, RGB to spectra), §5.4.2 (sensor response, visible wavelength sampling), §9.3.2 (index of refraction), §10.5.1 (dispersion in `DielectricMaterial`), §14.2.2 (chromatic media and spectral MIS), chapter 13 exercise on per-update wavelength division, chapter 15 exercise on compact wavelengths; Jakob and Hanika 2019, *A low-dimensional function space for efficient spectral upsampling*; Radziszewski et al. 2009 (visible wavelength sampling); Wilkie et al. 2014 (hero wavelength sampling); Shirley, *Ray Tracing: The Rest of Your Life*, "The Rest of Your Life" (go spectral); pbrt-v4 `src/pbrt/util/spectrum.*`, `util/color.*`, `util/colorspace.*`, `cmd/rgb2spec_opt.cpp`.
