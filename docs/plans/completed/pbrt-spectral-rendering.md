# Spectral rendering

The tracer multiplied RGB triples. PBR 4ed §4.6.4 shows why that is not physics: the same sRGB green, expressed in ACES2065-1 and multiplied by itself, comes out darker, more saturated and outside the gamut of real colors, because a product of tristimulus values depends on the color space. Dispersion, blackbody emitters, measured metal indices of refraction, laser lines and chromatic media all need light carried per wavelength. This plan makes the quantity every path carries a spectrum whose size and meaning are chosen at runtime, while exact RGB stays available and bit-identical.

*Implemented 2026-10-23, with the glass and conductor of [pbrt-bsdf-library.md](../pbrt-bsdf-library.md) that dispersion and measured metals need. Depends on [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md) and [pbrt-material-system.md](pbrt-material-system.md). The [laser optics plan](../physoptics-laser-optics.md) builds its glasses, coatings, lasers and spectral goals on it. [spectral-out-of-range.md](../spectral-out-of-range.md) holds the open question of RGB data beyond 360 to 830 nm. Part of the [roadmap](../pbrt-roadmap.md).*

## Requirements and decisions

The user called this the change they care about most, and the one most likely to change (2026-09-26):

- A runtime option that keeps the RGB renderer exactly as it is today.
- A 3-component spectral mode whose components are weights of red, green and blue Gaussians in wavelength.
- Denser distributions over arbitrary wavelength ranges.
- Room for a far-future goal: an approximate electromagnetic simulation in frequency space (x-ray and infrared optics, broadband spectra, polarization, diffraction) through a Monte Carlo solution of a path integral over a dyadic Green's tensor. On 2026-10-18 the user decided to design and build that backend themselves, last, after every pbrt and physoptics plan; no plan describes it.

Decisions taken the same day: spectral is a runtime mode compiled in families by lane count; materials and lights are authored once, as `SpectrumRef`s, and each representation evaluates or projects them; new scenes default to exact RGB.

When implementation started on 2026-10-23, the user set the main goal as good spectral features for the laser optics simulator, with pbrt-style spectral rendering second, and answered twelve questions:

- A **monochromatic** representation traces exactly one fixed wavelength. It has its own one-lane build, its wavelength is a setting separate from the **Design wavelength** (a button copies one to the other), and it displays in grayscale, in the wavelength's own color (with chosen colors outside the visible range), or through an intensity colormap.
- RGB-authored spectra outside 360 to 830 nm hold their 360 and 830 nm values for now. Whether they should extrapolate physically, fall to zero or do something else is undecided; [spectral-out-of-range.md](../spectral-out-of-range.md) records the question without a design.
- Exact RGB is a separate build in which a spectrum is a `vec3`, so its SPIR-V is the shaders it always was.
- Dispersion comes with every part of [pbrt-bsdf-library.md](../pbrt-bsdf-library.md) it needs, and the rest of that plan waits: `crt_fresnel.glsl`, `crt_microfacet.glsl`, the local-frame BxDF interface with its transport mode, `glass` (smooth and rough, dispersive, η²) and `conductor` (spectral η and k), their C++ mirrors, `bin/bsdf_test` and a GPU parity test, and the importer's mapping of pbrt's conductors and dispersive dielectrics. `thin_glass`, `diffuse_transmission`, `mix`, Oren–Nayar and the glTF extensions wait.
- The RGB Gaussians are fitted to the CIE functions and editable.
- An RGB color in the RGB Gaussian mode is uplifted and projected onto the bands, like every other spectral mode, so it does not reproduce exact RGB's colors exactly.
- Three additions beyond the plan: wavelength sampling from the emitters' spectra, wide-gamut uplift tables (sRGB, DCI-P3, Rec. 2020, ACES2065-1) with a scene working space, and the **Design wavelength** setting now.
- Spectral images are an opt-in output in the hero and band representations. A monochromatic render needs none, since its picture is its one wavelength.

## Builds and lane families

A kernel that touches radiance or reflectance includes `shaders/rt/include/crt_spectrum.glsl`, and CMake compiles it once per build:

- The exact RGB build, without `CRT_LANES`: `Spectrum` is `vec3`, and every function in `crt_spectrum.glsl` is the `vec3` expression the kernels always had. Its `.spv` keeps today's name, `0601_surface_scatter_bsdf.comp.basic.spv`.
- One build per entry of `VKGUIDE_SPECTRAL_FAMILIES` (default `1;4;8;16;32`), with `-DCRT_LANES=N`, as `<name>.comp[.<traversal>][.<class>].lanes<N>.spv`. `Spectrum` is `float` for one lane, `vec4` for four, and `struct { vec4 c[N / 4]; }` above.

`KernelVariant::laneDependent` marks the kernels built this way (00, 03, 04, 06 and 08). Kernels 01, 02 and 09 are built once. `GpuRenderer`'s pipeline cache is keyed by variant, traversal and family, and builds a family's pipelines the first time a render needs them. A cold build of the complex kernel 06 at 16 or 32 lanes takes up to 15 s on the Radeon Pro 560X; once Metal's shader cache holds it, any family's pipelines build in about a second. `GpuRenderer` prints each pipeline that takes longer than 0.25 s.

Within a family, specialization constants choose the rest: 3 is the representation, 4 the wavelength sampling, and 5 whether the render keeps spectral images. Loops over lanes in the 8-, 16- and 32-lane builds run to `CRT_LANE_END`, a value read from `SpectralData`, so Metal's compiler does not unroll them. Unrolled, the 16-lane complex kernel 06 took 177 s to compile and the 32-lane one failed with "Compiler encountered an internal error". The same pressure shaped two other rules: each material evaluation does one uplift table lookup, and `sampleLight()` evaluates its emission once.

## Representations

`SpectralSettings` in `src/spectral.h` holds the choice. `src/spectral.cpp` is the line-for-line C++ mirror of `crt_spectrum.glsl` and packs `SpectralData` (binding 24).

| Representation | Build | Lanes mean | Evaluating data | To the film |
|---|---|---|---|---|
| Exact RGB | RGB | R, G, B | RGB passes through; other spectra are projected to RGB on the CPU | identity |
| Monochromatic | 1 lane | one fixed wavelength, 1 to 20,000 nm | evaluated at the wavelength | the spectral radiance in all three channels |
| RGB Gaussians | 4 lanes | weights of three Gaussian bands | projected onto the bands on the CPU; textures uplifted then projected on the GPU | a band-to-RGB matrix |
| Spectral bands | 8, 16 or 32 lanes | N fixed bands over the range, box or Gaussian basis | projected onto the bands | a band-to-RGB matrix |
| Hero wavelengths | 4, 8, 16 or 32 lanes | N wavelengths drawn per path | evaluated at each wavelength | Monte Carlo estimate against the color-matching functions |

The RGB Gaussians' defaults are centered at 616, 536 and 449 nm with full widths at half maximum of 39, 46 and 38 nm. `./bin/spectrum_test --fit-gaussians` fits them: the three bands that bring a grid of sRGB colors, uplifted as reflectances, projected onto the bands and through the film, back closest to themselves (RMS error 0.063). Band representations project a spectrum onto a band by its basis-weighted average. On the GPU, a texel is uplifted and evaluated at each band's four Gauss–Legendre (box) or Gauss–Hermite (Gaussian) nodes. The film matrix reconstructs `Σ L_b ψ_b` with `ψ_b = φ_b / Σ φ`, a partition of unity, against the CIE functions at every whole nanometer, so a constant spectrum comes back exactly.

### Wavelength sampling

Hero wavelengths follow pbrt-v4 (PBR 4ed §4.5.4): one uniform sample `u` chooses the first wavelength, and lane `i` uses `u + i / N`, wrapped. Three densities are available:

- **Visible importance**, pbrt's `p(λ) ∝ sech²(0.0072 (λ − 538))` (§5.4.2). It needs the range to be exactly 360 to 830 nm; any other range samples uniformly, and the panel says so.
- **Uniform** over the range.
- **Emitters' spectra**, the addition for narrow sources. A mixture of the base density (visible or uniform, the share set by **Emitters' share**, 0.5 by default) and the scene's emitters: a 512-bin table of every smooth emitter spectrum weighed by its power, and up to eight truncated Cauchy components, one per line emitter, at the line's center and width. A 0.01 nm laser line in a white scene is found by most paths instead of almost none.

Light paths that start at an emitter, which [pbrt-bidirectional-path-tracing.md](../pbrt-bidirectional-path-tracing.md)'s light integrator needs, are not built. The mixture above is what they draw from when that plan lands.

## Path state and memory

`PathState` stays 64 bytes, with every field at its exact RGB offset. A spectral build puts `lambdaU` (the sample the path's wavelengths come from) and `laneMask` (the lanes alive after a dispersive event) in the twelve bytes the exact RGB build's throughput takes, and moves the throughput to `PathSpectra` (binding 63), `CRT_LANES` floats per path. Kernels 01 and 02 read `PathState` and so keep one build, and `pad0` and `pad1` stay free for the plans that claimed them. The wavelengths and their densities are recomputed where needed.

A spectral shadow ray carries the radiance slot, `lambdaU` and `laneMask`, and a `Spectrum` contribution at byte 48: 64 bytes at 1 or 4 lanes, 80 at 8, 112 at 16 and 176 at 32 (`shadowRayBytes()` in `src/rt_wavefront.h`). The exact RGB shadow ray stays 48 bytes. `PathSchedule` lowers the samples per frame until the pool fits in `CRT_MAX_POOL_BYTES`, 1.5 GB, which a wide family reaches before the 4,194,304-path limit.

## `SpectrumRef`: authoring once

Every spectrally varying parameter of a material, a light or the background is a `SpectrumRef` (`src/spectrum_ref.h`):

| Kind | Holds | Example |
|---|---|---|
| `Rgb` | an RGB triple in the scene's working space | a diffuse albedo, an area light's radiance |
| `Constant` | one value | a non-dispersive index of 1.5 |
| `Blackbody` | a temperature in kelvin and a scale | a 2700 K lamp |
| `Named` | one of pbrt-v4's 88 named spectra and a scale | `metal-Au-eta`, `glass-BK7`, `stdillum-D65` |
| `Tabulated` | (λ, value) pairs and a scale | a measured filter |
| `File` | a `.spd` file's samples | a spectrometer reading |
| `Dispersion` | a formula and up to ten coefficients: the AGF catalog's formulas 1 to 13 (Schott, Sellmeier 1 to 5, Herzberger, Conrady, the Handbook of Optics forms, extended) and Cauchy | a catalog glass's index |
| `Line` | a center, a full width at half maximum, a Gaussian or Lorentzian profile, and a scale that is the line's integral | a 632.8 nm HeNe laser |

What a reference means is its parameter's `SpectrumRole`: `Albedo` (bounded to [0, 1], uplifted with pbrt's `RGBAlbedoSpectrum`), `Unbounded`, `Illuminant` (uplifted and multiplied by the working space's illuminant, pbrt's `RGBIlluminantSpectrum`), or `Scalar` (an index of refraction, read as a number). An `Illuminant` of any kind except `Rgb` and `Line` is normalized to luminance 1 before its scale, as pbrt normalizes its lights, so a 2700 K blackbody of scale 10 is as bright as an RGB light of (10, 10, 10). A `Line`'s scale is its radiometric integral, so a line is specified in watts per square meter per steradian whatever its width.

Exact RGB reads `exactRgb()`: the authored triple untouched for an sRGB scene, and other kinds projected through the CIE functions on the CPU. A glass's index reads at the **Design wavelength** and a conductor's η and k at 630, 532 and 465 nm.

### RGB uplift and color spaces

`tools/rgb2spec/rgb2spec_opt.cpp` is pbrt-v4's `rgb2spec_opt` with a thread pool and a binary output. CMake builds it and writes `assets/spectra/uplift_{srgb,dci_p3,rec2020,aces2065_1}.bin` (9.4 MB each, 64³ entries of three sigmoid coefficients, gitignored) once per build tree; the sRGB table is bit-identical to pbrt's `rgbspectrum_srgb.cpp`. `SpectralUplift` (binding 25) holds the scene's table, and a trilinear lookup gives the coefficients of `s(c0 λ² + c1 λ + c2)`, `s(x) = ½ + x / (2√(1 + x²))` (Jakob and Hanika 2019; PBR 4ed §4.6.6).

**RGB color space** sets the space the scene's RGB values are in. Exact RGB converts them to linear sRGB, and the spectral modes uplift them with that space's table. Textures and environment maps are sRGB in every case. An uplifted RGB value is defined from 360 to 830 nm and holds its end values beyond, which is what a monochromatic 1064 nm render of an RGB scene sees.

### On the GPU

`SpectralData` (binding 24) is one buffer of 32-bit words: a header (`SH_*` in `crt_spectrum.glsl`), the CIE tables at 1 nm, the working space's illuminant, the bands, the wavelength mixture, and a pool of packed references. Each reference starts with a tag (RGB, constant, blackbody, tabulated, dispersion, line or bands) and its role, then its payload: an RGB reference keeps its triple, its uplift coefficients and, in a band representation, its band values. `SceneSpectra` interns every distinct reference a scene uses, and the material records and lights name them by index. `PackedMaterial::spectra` holds a material's four slots (albedo, emission, η, k) and its emission scale. `GpuLight` keeps its `radiance` for exact RGB and gains `spectrum` and `spectrumScale` in what were pad words.

## From lanes to the film

Kernels 03, 04 and 08 convert a path's radiance to film color when they add it, with `filmAdd()`. In exact RGB it adds `radiance.rgb += c`, the line it replaced. The hero estimate is `Σ L_i x̄(λ_i) / p(λ_i) / N / ∫ȳ` over live lanes, then XYZ to linear sRGB, and the bands multiply by the film matrix. Converting at each add keeps the densities a contribution had when it was added, so a later dispersive event adds no color noise to earlier light (pbrt-v4's chapter 13 exercise). The radiance slot stays a `vec4`, and kernel 09, the accumulation image and everything after them are the same in every build.

Debug views ride in the first lane. The albedo output is the reflectance under the working space's white, converted to RGB the same way.

### The monochromatic display

A monochromatic render holds its spectral radiance in all three channels. **Display** shows it in one of three ways, in the tone-map pass only, so changing it restarts nothing:

- **Grayscale**, the radiance as it is.
- **Wavelength color**, the radiance times the wavelength's own color: the CIE color of the wavelength, brought into sRGB by adding white and scaled so its brightest channel is 1. Outside 380 to 780 nm it blends into the chosen colors for **Infrared**, **Ultraviolet** and **XUV (below 100 nm)**.
- **Colormap**, exposure times radiance through viridis, inferno or turbo, linear from 0 to 1 or **Logarithmic** over a number of decades below 1.

## Spectral images

**Spectral images** keeps each pixel's mean spectral radiance in wavelength buckets over the range, beside the picture, as pbrt's `SpectralFilm` does. The band representations use their bands as buckets; hero wavelengths fall into as many buckets as you choose (1 to 256). The sums follow the light groups and the weight sum in `ChannelSums` (binding 45), so `channelStride()` is `3 × groups + 1 + buckets`. An EXR export writes one emissive channel per bucket in the Spectral OpenEXR layout (Fichet, Pacanowski and Wilkie 2021), named `S0.<integer>,<six digits>nm` with a comma for the decimal point, with the attributes `spectralLayoutVersion` = `1.0` and `emissiveUnits` = `W.m^-2.sr^-1.nm^-1`. A monochromatic render's picture is written as that one channel too.

## Dispersion, glass and conductors

`src/bxdf.h/.cpp` (part of `rt_bsdf`) and `crt_fresnel.glsl`, `crt_microfacet.glsl` and `crt_bxdf.glsl` mirror pbrt-v4's `FrDielectric()`, `FrComplex()`, `TrowbridgeReitzDistribution` (anisotropic α, visible-normal sampling, `EffectivelySmooth()` below 1e-3), `DielectricBxDF` and `ConductorBxDF` line for line. They work in pbrt's local shading frame, with z the shading normal and x along dp/du, and take a transport mode, so the glass divides radiance by η² on transmission in radiance mode and not in importance mode.

Two material types use them, both in the complex material class, so only the complex build of kernel 06 evaluates them and the basic build and every existing scene are unchanged:

- `glass`: η (a `Scalar` spectrum, 1.5 by default), roughness u and v, and **remap roughness**. A smooth glass reflects with probability R and refracts otherwise; a rough one uses the generalized half vector (Walter et al. 2007).
- `conductor`: η and k (pbrt's copper by default), or **from reflectance**, which sets η = 1 and derives k from a normal-incidence reflectance as pbrt does; roughness u and v. An RGB η or k is read per channel in exact RGB, and in the spectral modes as its `Unbounded` uplift, tabulated every 5 nm (pbrt's `RGBUnboundedSpectrum`).

A glass whose η is not constant is dispersive. When a path refracts through one, `terminateSecondary()` keeps one lane and multiplies it by the number of lanes that were alive (PBR 4ed §4.5.4, §10.5.1): lane 0 for hero wavelengths, a live band chosen at random for the bands. Exact RGB reads the index at the **Design wavelength**, 587.6 nm (the helium d line) by default, so a 1064 nm design focuses in exact RGB where it would in the lab. The legacy `dielectric` keeps its scalar index.

## Lights and the light list

A light's color, an emissive material's color and the `pbr` emission are `Illuminant` references. The light list prices lights by power from each spectrum's photometric luminance, computed on the CPU, so light selection is the same in every representation. A light with no luminance, such as a 1064 nm laser line, is priced by its radiometric power over 1 to 20,000 nm divided by the luminance integral, so it is still chosen in proportion to the light it gives.

## The panel

**Raytrace Render** → **Spectral**:

- **Representation**: exact RGB, monochromatic, RGB Gaussians, spectral bands or hero wavelengths.
- **Wavelength** (monochromatic), with a button that sets it to the **Design wavelength**, and **Display**.
- **Lanes** (4, 8, 16 or 32, bands and hero), **Range** (360 to 830 nm by default, any part of 1 to 20,000 nm), **Band basis** (box or Gaussian) and the three **Fitted bands** (RGB Gaussians, each a center and a full width at half maximum).
- **Sampling** (hero): visible importance, uniform or emitters' spectra, and **Emitters' share**.
- **Spectral images** and its bucket count.
- **RGB color space** and **Design wavelength**.

Every spectrum parameter in the **Scene** panel and the **Materials** window has a **Spectral** button beside its color. Its menu turns the parameter into a **Color**, a **Constant**, a **Color temperature**, a **Named spectrum**, an **.spd file...**, a **Dispersion formula** or an **Emission line**, and its editor shows the value at the design wavelength (`src/spectrum_edit.cpp`).

## Scene file and command line

Payload version 18 adds to `"render"`:

```json
"spectral": {"representation": "hero", "lanes": 4, "range": [360, 830], "wavelength": 587.6,
             "designWavelength": 587.6, "bandBasis": "box",
             "gaussians": [[616, 39], [536, 46], [449, 38]], "sampling": "visible",
             "emittersShare": 0.5, "colorSpace": "srgb", "images": 0, "matching": "cie1931"}
```

and `"spectralDisplay"` at the top. A spectrum is today's `[r, g, b]` array when it is RGB, which keeps older files and RGB parameters unchanged, or an object such as `{"blackbody": 2700, "scale": 1.0}`, `{"named": "metal-Au-eta"}`, `{"line": 632.8, "fwhm": 0.002, "profile": "gaussian", "scale": 1.0}`, `{"dispersion": 2, "coefficients": [...], "name": "N-BK7", "range": [300, 2500]}` (the AGF formula number) or `{"spd": "filters/red.spd", "scale": 1.0}`. A file older than version 18 reads in exact RGB with sRGB colors.

The batch mode takes `--spectral rgb|monochromatic|gaussians|bands|hero`, `--lanes`, `--wavelength`, `--range <min>,<max>`, `--sampling`, `--color-space`, `--design-wavelength` and `--spectral-images <n>`, each in place of the scene's own setting.

## Importing pbrt scenes

The importer keeps pbrt's spectra for the spectral modes and their projection for exact RGB:

- A `blackbody`, named or `.spd` emission becomes the light's `SpectrumRef`. A deep red blackbody outside sRGB is clamped in exact RGB only, and the report says so.
- `diffuse` keeps a spectral reflectance.
- `conductor` becomes `conductor`, with its η and k spectra or its reflectance, and its anisotropic roughness. A textured reflectance still becomes `pbr`.
- `dielectric` becomes `glass`, smooth or rough, with its η spectrum, dispersive when the spectrum varies. `thindielectric` is still solid glass, reported against the BSDF library.
- `Film "spectral"` imports as hero wavelengths with its range and its bucket count as spectral images.

The report lines that remain point to the plans that add what is missing, chiefly the BSDF library for spectra on materials it does not have yet. All 98 scene files in `assets/models/pbrt-v4-scenes` still import.

## Verification

- Exact RGB renders bit-identical to the build before this plan after every step: the Cornell box, the sphere scene, the Veach scene, the material gallery with and without auxiliary outputs and light groups, the caustic, thin glass, the white furnace, the Cornell reference, a debug view and Sponza, at a fixed seed (12 renders, compared channel by channel).
- `bin/spectrum_test` adds uplift, blackbody, dispersion, line, sampling, band, packing and display checks. In each of the four color spaces, colors well inside sRGB (a 7³ grid from 0.1 to 0.9) come back from their uplift within 1e-3, the sRGB gamut's corners within 0.1, and uplifted reflectances stay in [0, 1]. Through the RGB Gaussians a lit sRGB color comes back 0.030 off on average and 0.054 at worst. A 6504 K blackbody lands within 0.005 of D65 in CIE 1960 uv. N-BK7 has n_d = 1.5168 and an Abbe number of 64.17. Each sampling density integrates to 1 and matches its histogram, and the hero estimate of a test scene's color converges to the integral in every sampling mode, a 0.01 nm line included.
- `bin/bsdf_test` runs the χ², weight, energy and reciprocity checks for `glass` at indices 1.33, 1.5 and 2.4 and α 0.1 to 0.8, from outside and inside, anisotropic and in importance mode, and for `conductor` with pbrt's copper and gold at α 0.05 to 0.7 and a reflectance of 1. Smooth glass weighs a transmission by 1 / η² in radiance mode, and its reflectance is zero for p-polarized light at Brewster's angle. `--selfcheck` plants a dielectric density 5 percent off and a conductor 5 percent too bright, and fails unless both are caught.
- **Windows** → **GPU self-tests** gains two tests: `crt_bxdf.glsl` against `src/bxdf.cpp`, and `crt_spectrum.glsl`'s wavelengths, densities, reference evaluation and film conversion against `src/spectral.cpp`.
- `tools/verify/spectral_check.py`, which `run_all.py` runs:
    - A white furnace in seven representations (exact RGB, RGB Gaussians, 8, 16 and 32 bands, and hero at 4, 8 and 32 lanes) with nine materials. A white Lambertian, a mirror, the classic dielectric, `glass` at 1.5 and dispersive glass-F11 vanish, a 0.5 Lambertian is half, and every background is neutral within the representation's resolution (0.0006 for hero at 4 lanes, 0.025 for the RGB Gaussians). Rough glass-F11 matches exact RGB (0.666 of the background, energy single scattering loses), and copper and an RGB-indexed conductor match the 32-lane hero render. Exact RGB's copper is 3.4 percent darker than the spectral one, since it reads the measured tables at three wavelengths.
    - The monochromatic furnace at 450, 632.8 and 1064 nm.
    - The Cornell box in every representation against exact RGB, by the largest difference in a channel's mean at 1024 spp: 4.3 percent for the RGB Gaussians, 3.2 for 8 box bands, 1.2 for 16 Gaussian bands, 0.5 for 32 bands, and 0.4 for hero at 4, 8 and 32 lanes. The check allows 6, 4, 2, 2 and 1 percent.
    - `assets/scenes/prism_dispersion.gltf`: a 60° glass-F11 prism against a white slit, at minimum deviation for 587.6 nm. Exact RGB shows one white slit. Hero spreads it into a spectrum and the 16 bands into 16 slits, with the red, green and blue centroids 5.8 and 7.9 pixels apart; the two agree on each channel's light within 3 percent.
- `tools/verify/pbrt_compare.py --spectral hero` renders a pbrt scene in hero mode in place of its own film. On `killeroo-gold` at 64 spp, exact RGB's channel means are 0.932, 0.952 and 0.975 of pbrt's, and hero's are 0.958 in all three: the gold's color matches pbrt and the 4 percent left is the faceted loop-subdivision mirror [pbrt-scene-import.md](pbrt-scene-import.md) measured.

## Not built

- Sensor response curves in place of the CIE functions (`"matching"` accepts only `cie1931`), and white balance: [pbrt-display-and-color.md](../pbrt-display-and-color.md).
- Light paths that draw their wavelengths from the emitter, for [pbrt-bidirectional-path-tracing.md](../pbrt-bidirectional-path-tracing.md).
- The **Design wavelength** following a scene's single laser: [physoptics-laser-sources.md](../physoptics-laser-sources.md) sets it when lasers exist.
- AGF catalogs, the Henke tables and coatings: [physoptics-optical-materials.md](../physoptics-optical-materials.md) and [physoptics-gratings-and-xuv.md](../physoptics-gratings-and-xuv.md). The `Dispersion` kind and the line kind are what they fill in.
- The rest of [pbrt-bsdf-library.md](../pbrt-bsdf-library.md).

## Where it departs from the plan and from pbrt

- Exact RGB is its own build, not lanes 0 to 2 of the four-lane build, so its SPIR-V does not change at all. The plan's four-lane exact RGB would have made every RGB render pay for a fourth lane.
- The throughput lives in `PathSpectra` (binding 63), not in `PathState`, and `lambdaU` and `laneMask` take the exact RGB throughput's place. `PathState` keeps its 64 bytes in every build, where the plan's would have grown to 96.
- There is a one-lane family, for the monochromatic representation the user added.
- A spectral shadow ray is 64 bytes at four lanes, as the plan said, and grows to 176 at 32.
- The representation, the sampling and the spectral images are specialization constants within a family, so a family is one set of SPIR-V and the choice inside it costs no branch.
- The `SpectrumRef` kinds are one `Rgb` kind with a role, not three RGB kinds; the role belongs to the parameter. `Named` and `File` are kinds of their own.
- pbrt normalizes a `Line`-like spectrum to luminance 1 like any light; here a line's scale is its radiometric integral, which a laser's power gives directly and which stays defined outside the visible range.
- Lights without luminance are priced radiometrically, where pbrt's power light sampler gives them probability 0.
- Loops over lanes run to a bound read at run time in the wide families, which Metal needs in order to compile them.

## Sources

PBR 4ed §4.4 (blackbody emitters, standard illuminants), §4.5 (spectral distributions, `SampledSpectrum`, `SampledWavelengths`, secondary termination), §4.6 (XYZ, RGB color spaces, why spectral rendering, number of wavelength samples, RGB to spectra), §5.4.2 (sensor response, visible wavelength sampling), chapter 9 (§9.3 Fresnel, §9.4 conductor BRDF, §9.5 dielectric BSDF and non-symmetric scattering, §9.6 microfacet theory, §9.7 rough dielectric), §10.5.1 (dispersion in `DielectricMaterial`), chapter 13 exercise on per-update wavelength division, chapter 15 exercise on compact wavelengths; Jakob and Hanika 2019, *A low-dimensional function space for efficient spectral upsampling*; Radziszewski et al. 2009 (visible wavelength sampling); Wilkie et al. 2014 (hero wavelength sampling); Walter et al. 2007; Heitz 2018; Fichet, Pacanowski and Wilkie 2021, *An OpenEXR layout for spectral images*; the Zemax AGF glass catalog format; pbrt-v4 `src/pbrt/util/spectrum.*`, `util/color.*`, `util/colorspace.*`, `bxdfs.*`, `util/scattering.h`, `film.cpp` (`SpectralFilm`), `cmd/rgb2spec_opt.cpp`.
