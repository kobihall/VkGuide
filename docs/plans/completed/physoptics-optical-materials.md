# Optical materials and coatings

An optic is defined as much by what it is made of as by its shape. A lens focuses because N-BK7's index is 1.5168 at 587.6 nm and changes with wavelength; a 50:50 beamsplitter splits because a dielectric stack on its front reflects half of the s and p light at 45°; a neutral density filter dims because its glass absorbs. The renderer has a scalar index of refraction on the legacy dielectric and, after [pbrt-bsdf-library.md](pbrt-bsdf-library.md), exact Fresnel terms for a dielectric or a conductor. This plan adds what an optical bench needs on top of that: glass catalogs with dispersion formulas and internal transmittance, the surrounding air, coatings from ideal splitters to thin-film stacks and protected metals, and the material that every surface of an optical body uses.

*Implemented 2026-10-29, together with [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md) and [physoptics-optical-elements.md](physoptics-optical-elements.md); see "As built" at the end for what changed from the design below. Depends on [pbrt-material-system.md](pbrt-material-system.md) (material types and the library), [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) (`SpectrumRef`, dispersion, lane termination), [pbrt-bsdf-library.md](pbrt-bsdf-library.md) (exact Fresnel, the dielectric BxDF, transport modes) and [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md) (surfaces and their media). Polarizing coatings are fully used only with [pbrt-polarization.md](../pbrt-polarization.md). Part of the [laser optics plan](../physoptics-laser-optics.md).*

## What the renderer does today

- `dielectric` has one scalar `ir` and Schlick's approximation. `glass` ([pbrt-bsdf-library.md](pbrt-bsdf-library.md), built 2026-10-23) has exact Fresnel (`frDielectric()` and `frComplex()` in `crt_fresnel.glsl`, mirrored in `src/bxdf.cpp`), a rough form, transport modes and an index that is a `SpectrumRef`.
- Schlick's approximation is wrong inside glass, which is where a lens's internal reflections happen ([pbrt-verification-harness.md](pbrt-verification-harness.md) measured it). Leaving glass of index 1.5 it gives a reflectance of 0.040 at 30 degrees and 0.041 at 40, where the exact value is 0.055 and 0.245 (0.380 at 41 degrees). Light that should reflect inside a prism or a thick lens would transmit. The optical interface material must use the exact term, and its test must compare internal reflectance with the Fresnel equations at angles up to the critical angle, from both sides.
- [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) built `SpectrumRef` with the kinds `Constant`, `Named` (pbrt's 88 named spectra, compiled into `src/spectrum_tables.cpp`: metal η and k, BK7 and a few glasses), `Tabulated`, `File` (`.spd`), `Dispersion` and `Line`, and dispersion by terminating secondary lanes. `Dispersion` is the `DispersionFormula` kind this plan asked for: the AGF formulas 1 to 13 and Cauchy (`evalDispersion()` in `src/spectrum_ref.cpp`, mirrored in `crt_spectrum.glsl`), with coefficients typed in the **Spectral** menu. In exact RGB `glass` reads its index at the **Design wavelength** (**Render Settings** → **Spectral**, 587.6 nm by default); the legacy `dielectric` keeps its scalar `ir`.
- Nothing models absorption inside glass, air's index, or any coating.

## Design

### Glass catalogs

A glass is a library entry, `OpticGlass`, with:

- A dispersion formula and its coefficients, in the formula set of the Zemax AGF catalog format, which Schott, Ohara, CDGM, Hoya and Sumita publish their catalogs in: Schott (1), Sellmeier 1 to 5 (2, 4, 6, 9, 11), Herzberger (3), Conrady (5), Handbook of Optics 1 and 2 (7, 8), and Extended 1 to 3 (10, 12, 13), numbered as the OpticStudio manual numbers them. `SpectrumRef`'s `Dispersion` kind holds it: a formula id and up to ten coefficients (built with spectral rendering).
- Its valid wavelength range. Outside it the formula is still evaluated and the panel warns, since optical glass formulas extrapolate badly into the UV and the IR.
- Its internal transmittance, the AGF file's `IT` lines: (λ, transmittance, thickness) triples, turned into an absorption coefficient `α(λ) = −ln(T) / d` and stored as a `Tabulated` `SpectrumRef`.
- A thermal block (`TD` line) is read and kept but not used: no plan models temperature.

The AGF reader is `src/optic_glass.h/.cpp` in `rt_optics`. [physoptics-prescription-import.md](../physoptics-prescription-import.md) reuses it for the catalogs a `.zmx` file names.

The renderer bundles `assets/glass/bundled.agf`, the glasses the request's elements are sold in, with coefficients from the manufacturers' data sheets as collected by refractiveindex.info (CC0): N-BK7, N-SF11, N-SF10, N-F2 and F2, N-SF6, UV fused silica (Corning 7980), CaF₂, MgF₂, sapphire (ordinary ray), and the coating oxides SiO₂, Ta₂O₅, TiO₂, HfO₂ and Al₂O₃. A vendor's full catalog is loaded from its own AGF file through **Optics** → **Load glass catalog...**, and saved with the scene by path.

### The surrounding medium and the design wavelength

- Air is a glass entry with Ciddor's 1996 formula at 15 °C, 101 325 Pa and 450 ppm CO₂ (n = 1.000 276 at 633 nm). A scene setting, **Surrounding medium**, is air (default) or vacuum, which XUV setups need ([physoptics-gratings-and-xuv.md](../physoptics-gratings-and-xuv.md)). It is medium 0 of every optical body ([physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)).
- In exact RGB there are no wavelengths, so dispersion is off and every glass's index is read at one **Design wavelength**: 587.6 nm (the helium d line, the catalogs' reference) by default, or the laser's wavelength when the scene's beam axes all start at lasers of one wavelength. A 1064 nm setup in exact RGB then focuses where it would in the lab. The setting exists since spectral rendering (2026-10-23), and pbrt's `glass` reads it too; the legacy `dielectric` keeps its scalar index, so existing scenes do not change. This plan makes it follow the lasers.
- Absorption in exact RGB is evaluated at the spectral plan's representative wavelengths for red, green and blue (630, 532 and 465 nm), so a colored glass filter looks colored in an RGB render even though its index uses the design wavelength. The panel notes this.

### Coatings

A coating is a library entry, `OpticCoating`, that replaces a surface's bare Fresnel reflection and transmission. It answers one question for a wavelength λ, an angle of incidence θ in the incident medium, and a polarization (s or p): the complex amplitudes `r` and `t`, from which `R = |r|²`, `T = (n_t cos θ_t / n_i cos θ_i) |t|²` and the absorptance `A = 1 − R − T`. The amplitudes are kept as well as the powers, for [pbrt-polarization.md](../pbrt-polarization.md), whose Mueller matrices need the phase difference between s and p, and for the future wave backend.

| Kind | Parameters | Use |
|---|---|---|
| Uncoated | none: exact Fresnel between the surface's two media | a bare glass face, the back of a mirror blank before grinding |
| Ideal | R and T, optionally separate for s and p, independent of λ and θ | an idealized 50:50 splitter, a perfect mirror, a perfect polarizing cube |
| Tabulated | R(λ) and T(λ), per polarization or averaged, at one or more angles | a vendor's measured curve, a dichroic's data sheet |
| Thin-film stack | incident medium, layers (glass, physical thickness or quarter waves at a design λ), substrate | AR coatings, dielectric mirrors, dichroics, long- and short-pass edges, polarizing splitters |
| Metal | a metal's η and k (pbrt's named spectra: Ag, Al, Au, Cu), with an optional protective stack over it | protected silver, gold and aluminum mirrors |

The thin-film stack is the transfer-matrix method (Macleod, *Thin-film optical filters*, chapter 2): one 2 × 2 complex characteristic matrix per layer, multiplied through the stack per wavelength, angle and polarization, with complex indices so absorbing layers and metals work. It runs in `rt_optics` on the CPU in `double`. Its GLSL mirror, `crt_tmm.glsl`, evaluates stacks of up to 32 layers per lane for the **Exact coatings** option. [pbrt-appearance-extras.md](../pbrt-appearance-extras.md)'s thin-film iridescence is a one-layer case of the same code and reuses it.

A tabulated curve is interpolated linearly in λ and in θ between the angles given; outside the given angles it holds the nearest one, and the panel shows the coverage. The importer reads CSV files of wavelength in nm and percent or fraction, which is what Thorlabs' coating data exports to, with one column per polarization or one averaged column.

### Coating tables on the GPU

Evaluating a stack per hit per lane is exact but slow. By default the CPU tabulates every coating in the scene into binding 56, `OpticCoatings`: R and T for s and p, and the phase of r and t for each, over a grid of wavelengths (the scene's spectral range at 1 nm, or the active band centers) and 91 angles from 0° to 90°. `crt_coating.glsl` interpolates the grid bilinearly. A coating's table is rebuilt when the coating, the spectral range or the representation changes, which a stack of 30 layers does in a few milliseconds on the CPU. In exact RGB the table has three wavelength columns, the coating's response projected onto red, green and blue as the spectral plan projects any reflectance spectrum, so a dichroic mirror reflects the colors it should.

Without the polarized family, a surface uses the average of s and p, the unpolarized assumption pbrt makes (PBR 4ed §9.3.5). A laser is usually polarized, so the panel shows a warning on a polarizing coating while polarization is off: its split then reflects the average, not what a polarized laser would see ([physoptics-polarization-optics.md](../physoptics-polarization-optics.md)).

### The optical interface material

Every surface of an optical body has a material, and most have the new `optical_interface` type, a `MATERIAL_TRAITS` row in the basic class:

| Parameter | Meaning |
|---|---|
| Coating | a coating library entry, or none (uncoated) |
| Surface finish | polished (default) or ground |
| Scatter | none (default), or a total integrated scatter and an ABg lobe (see below) |

It has no index parameter: the two indices come from the surface's two media, read from the surface record, and the side the ray arrived from picks which is which. Sampling chooses a delta reflection with probability `R / (R + T)` and a delta refraction (Snell, with total internal reflection) otherwise, and returns the weight `(R + T)`. An absorbing coating's `A` is lost there. In radiance transport a refraction divides by η², as pbrt's dielectric does, and in importance transport (light paths from a laser) it does not ([pbrt-bsdf-library.md](pbrt-bsdf-library.md) §"The BxDF interface in GLSL"). With a non-constant index in a spectral mode, the refraction terminates the secondary lanes ([pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)). [physoptics-guided-tracing.md](../physoptics-guided-tracing.md) can bias the reflect-or-refract choice and corrects the weight for it.

A ground surface is pbrt's rough `glass` BxDF at roughness 1 between the same two media: lens edges, the backs of some mirror blanks, the frosted face of a diffuser. Other surfaces that are not optical use ordinary materials: anodized aluminum for mounts and iris blades (a `diffuse` reflectance of 0.04, set from the mount's model, [physoptics-bench-and-mounts.md](physoptics-bench-and-mounts.md)), black flock for beam dumps.

### Absorption inside a body

A ray inside an optical body travels in a known medium, the one on the side of the surface it last crossed. When it reaches the next surface, the material at that hit multiplies the throughput by `exp(−α(λ) t)` before scattering, with `t` the hit distance, since the previous vertex was where the ray entered. That is Beer–Lambert for the segment, applied locally, with no medium tracking, and it is exact for the homogeneous glass of an optic. A neutral density filter is a glass with a flat `α`; an absorbing colored filter (Schott's OG, RG and BG glasses) is a glass with that catalog's `IT` data. Once [pbrt-participating-media.md](../pbrt-participating-media.md) exists, a body can instead name a medium for its glass, for scattering glass or a cell of milky water.

### Surface scatter

A polished optic scatters about 0.1 % of what hits it, enough to matter for stray light in a laser lab and invisible otherwise. The **Scatter** parameter adds a lobe around the specular directions with the ABg model that stray-light software uses (FRED, Zemax's non-sequential mode): `BSDF(β − β₀) = A / (B + |β − β₀|^g)`, with `β` the projection of the direction on the surface, and A set so the lobe integrates to the chosen total integrated scatter. TIS defaults to Davies' `(4π σ cos θ / λ)²` from an rms roughness σ of 1 nm. The lobe is sampled by its own inverse CDF, and it makes the surface non-specular for next-event estimation. It is off by default and comes last in this plan.

### The scene model

- Glasses and coatings are entries of the material library ([pbrt-material-system.md](pbrt-material-system.md)), in two new folders of the **Materials** window, **Glasses** and **Coatings**, with the bundled entries read-only and duplicable.
- An optical element owns one material per material slot, created from its catalog kind's defaults ([physoptics-optical-elements.md](physoptics-optical-elements.md)). The library's `origin` field gains an optic origin, the element's id and the slot, the way a glTF material's records its model and index, so editing an element's coating is an edit to its own entry and the scene file saves only what was changed.
- Presets for Thorlabs' common coatings: `-E02` broadband dielectric (400 to 750 nm), `-P01` protected silver, `-G01` protected gold, `-F01` protected aluminum, `-A`, `-B` and `-C` broadband AR (350 to 700, 650 to 1050 and 1050 to 1700 nm), and the BSW and BSN beamsplitter coatings. Each preset is a thin-film or metal design that approximates the published curve. Loading Thorlabs' own data file for the part, as a tabulated coating, replaces the approximation with the measurement, and the panel names which one is in use.

## Registry, bindings and data

- Material type `optical_interface` (basic class); library entry kinds `OpticGlass` and `OpticCoating`; the optic origin.
- `SpectrumRef` kind `Dispersion`, which [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) built; this plan adds the AGF reader that fills it.
- Binding 56 `OpticCoatings`: the coating tables.
- The body block in binding 55 gains a media table in its payload: each medium's index and absorption as `SpectrumRef`s, and its index at the design wavelength for exact RGB.
- `src/optic_glass.*` (AGF reader, formulas, air), `src/optic_coating.*` (coatings, the transfer matrix, tabulation), `crt_coating.glsl`, `crt_tmm.glsl`, all in `rt_optics` with tests in `bin/optics_test`.
- Scene settings: **Surrounding medium**, **Design wavelength** (the setting spectral rendering added, shown here too), **Exact coatings**, in the **Optics** section of **Render Settings**, and the **Glasses** and **Coatings** folders of the Materials window, all registered under the **Optical bench** capability ([physoptics-scene-capabilities.md](physoptics-scene-capabilities.md)). Scene file: loaded catalog paths, user glasses and coatings, element-owned entries as overrides.
- Assets: `assets/glass/bundled.agf`, `assets/coatings/` for the preset designs, each with its source in a header comment.

## Verification

- A scene without optical interfaces renders bit-identical.
- `bin/optics_test`, glass: N-BK7's n_d is 1.51680 and its Abbe number 64.17 from the bundled coefficients (Schott data sheet) to 1e-5; every bundled glass reproduces its data sheet's indices at the F, d and C lines to 1e-5; air at 633 nm is 1.000276 ± 2e-6.
- `bin/optics_test`, coatings:
    - an uncoated interface equals `frDielectric()` from [pbrt-bsdf-library.md](pbrt-bsdf-library.md) to 1e-6 at every angle, with `R_p = 0` at Brewster's angle and total internal reflection past the critical angle;
    - a single quarter-wave MgF₂ layer on N-BK7 reflects `((n₀ n_s − n₁²) / (n₀ n_s + n₁²))²` at its design wavelength at normal incidence;
    - a quarter-wave stack of N pairs reaches the closed-form peak reflectance `((n_s − n₀ (n_H/n_L)^{2N}) / (n_s + n₀ (n_H/n_L)^{2N}))²`;
    - every lossless stack has `R + T = 1` to 1e-9, and swapping the incident and substrate media leaves T unchanged (reciprocity);
    - bare silver reflects 0.95 to 0.97 at 633 nm, from pbrt's η and k;
    - the GPU table interpolation matches the exact transfer matrix to 1e-3 in R at any (λ, θ) between grid points.
- Rendering, with the light integrator and a detector ([physoptics-detectors.md](physoptics-detectors.md)): an ideal 50:50 splitter divides the power on two detectors 50/50 within Monte Carlo noise; a neutral density filter of optical density 1 transmits 10 % ± noise; a long-pass dichroic at 45° splits a two-line source by wavelength.

## Order of work

1. The AGF reader, the formulas, air and the design wavelength, with their tests.
2. `optical_interface` with uncoated surfaces and absorption inside a body.
3. Ideal and tabulated coatings, and the CSV importer.
4. The transfer matrix, stacks and metals, and the Thorlabs presets.
5. The GPU coating tables, then **Exact coatings**.
6. Surface scatter.

## Open questions

- Thorlabs' coating curves are published per product as spreadsheets. The presets above approximate them by design; for any part the user's lab relies on, the user downloads the curve and it is loaded as a tabulated coating. Which parts' curves to bundle is the user's choice.
- Whether a glass's temperature (the AGF `TD` line, and thermal lensing in a high-power beam) is ever wanted. Nothing in the request needs it.

## Sources

Macleod 2010, *Thin-film optical filters*, chapters 2 to 5 (characteristic matrices, quarter-wave stacks, AR coatings, edge filters); Born and Wolf 1999, *Principles of optics*, §1.5 (Fresnel's equations) and §1.6 (stratified media); Ciddor 1996, *Refractive index of air: new equations for the visible and near infrared*, Applied Optics 35(9); Schott Technical Information TIE-29, *Refractive index and dispersion*; the Zemax OpticStudio manual, "Glass catalogs" (the AGF format and its dispersion formulas); Polyanskiy 2024, *Refractiveindex.info database of optical constants*, Scientific Data 11, 94; Davies 1954 and Stover 2012, *Optical scattering: measurement and analysis* (total integrated scatter, the ABg model); PBR 4ed §9.3 (Fresnel for dielectrics and conductors); Thorlabs coating pages for the `-E02`, `-P01`, `-G01`, `-F01`, `-A`, `-B`, `-C`, BSW and BSN coatings.

## As built

Implemented on 2026-10-29, all six steps of the order of work, in the same change as the surfaces and elements plans.
The CPU side is `src/optic_glass.{h,cpp}`, `src/optic_coating.{h,cpp}` and `src/optic_library.{h,cpp}` in `rt_optics`;
the GPU side is `shaders/rt/include/crt_coating.glsl` and `crt_optic_material.glsl`; the panels are `src/optics_ui.cpp`.
`docs/physical-optics-overview.md` §6 is the readable account.

What differs from the design above:

- **`optical_interface` is a complex-class material**, `MaterialType` 12, with the record `CrtOpticMaterial` (finish,
  total integrated scatter, ABg B and g), not a basic one. Exact Fresnel, dispersion and the BxDF frame exist only in
  the complex build of kernel 06.
- **Glasses and coatings live in an `OpticalLibrary`**, beside the material library rather than in it: a coating's layer
  stack does not fit a `ParamSet`. The library holds the built-in AIR and VACUUM, the bundled catalog, catalogs the
  user loads, and user glasses and coatings. A material names its coating by key (`ParamKind::Coating`, `CoatingRef`),
  and an element names its glasses by name. The Materials window lists both under **Glasses** and **Coatings**, and
  shows a glass's index and internal transmittance and a coating's R and T against wavelength and angle.
- **The presets are designs in code** (`presetCoatings()`), not files in `assets/coatings/`, like the scene templates:
  E02, P01, G01, F01, A, B, C, single-layer MgF2, BSW, BSN, a 567 nm long-pass dichroic, the ideal 50:50, polarizing,
  mirror, AR and absorber coatings, and bare (uncoated). Each names the Thorlabs curve it approximates in its `source`.
  The layer materials (SiO2, Ta2O5, TiO2, HfO2, Al2O3, MgF2) are glasses of `assets/glass/bundled.agf`, which also
  holds N-BK7 (with its internal transmittance), N-SF11, N-SF10, N-F2, F2, N-SF6, N-SF1, fused silica, CaF2, sapphire
  and ND-OD1. **Optics** → **Import Coating Curve** reads a reflectance or transmittance CSV.
- **No `crt_tmm.glsl`.** `crt_coating.glsl` holds both the table lookup (`coatingLookup()`) and the per-hit transfer
  matrix (`coatingExact()`), in float, against `transferMatrix()` in double.
- **The tables sample the cosine, not the angle**, in the lower-index medium of the pair: 91 columns at
  u = 1 - cos(pi j / 180). From inside glass, the reflectance rises with an infinite slope in the angle at the critical
  angle, which an angle grid cannot follow. Past the critical angle the lookup returns total reflection. A coating's
  table depends on both media, so binding 56 holds one per (coating, media pair), and `instanceMaterials[]` gives each
  surface of an optic its own after the slot materials.
- **The tables hold power only**: Rs, Rp, Ts and Tp for each direction. The phases wait for
  [pbrt-polarization.md](../pbrt-polarization.md).
- **Exact RGB** reads indices at the design wavelength and absorption at 630, 532 and 465 nm, and builds a coating's RGB
  table by projecting its spectral R and T onto each channel. **Design wavelength** in the **Optics** section of
  **Render Settings** is the spectral setting, shown again.
- **Air** is Ciddor 1996 at 15 C, 101,325 Pa and 450 ppm CO2, tabulated as a `SpectrumRef` from 200 to 2000 nm.
- **Surface scatter** follows the plan's ABg lobe around the specular direction, with two changes. The lobe is divided
  by the fraction of it that lies above the surface (16 tabulated values against the angle of incidence), since without
  that a grazing beam lost what fell below the horizon; and it is sampled by inverting its radial CDF (32 radii) with
  rejection of directions below the surface, up to 16 tries (`CRT_ABG_TRIES`). It is evaluated for light samples, and
  `g_opticSampledDelta` tells kernel 06 whether a sample was the delta or the lobe, for the MIS record.
- **Absorption** over the segment a ray has just crossed is applied in kernel 06 at the hit that ends it
  (`crt_surface.glsl`), from the medium the ray was in. A refraction through a dispersive medium terminates the
  secondary hero lanes, as the legacy glass does.
- **Element-owned materials** carry an `OpticSlotOrigin` (optic id and slot). They are hidden from material combos,
  **Delete unused** and the User folder, and a scene file saves a slot's material only when it differs from the
  kind's default.
- **The AGF reader** accepts UTF-16 files, which Zemax writes.

Verification (the plan's list, with what each gave):

- A scene without optical interfaces renders bit-identical: the 10 renders listed in the surfaces plan's record.
- `bin/optics_test`, glasses: N-BK7's n_d is 1.51680 and its Abbe number 64.17 to 0.005. Every bundled glass gives its
  catalog n_d to 1e-5 and v_d to 0.01. Air at 633 nm is 1.000276 within 2e-6. ND-OD1 transmits 0.1 through 2 mm to
  1e-6. N-BK7 absorbs ten times more at 310 nm than at 550 nm.
- `bin/optics_test`, coatings:
    - uncoated against `frDielectric()`: within 1e-6 at every angle, from both sides; Rp below 1e-12 at Brewster's
      angle; total reflection past the critical angle;
    - a quarter-wave MgF2 layer on N-BK7 and quarter-wave stacks of 1 to 8 pairs: within 1e-9 of their closed forms;
    - every preset stack, with its films' absorption off: R + T = 1 and forward T equal to backward T, each within
      1e-9, at 200 random wavelengths and angles;
    - bare silver at 633 nm reflects between 0.95 and 0.97;
    - the GPU table's R within 1e-3 of the transfer matrix between grid points, for BSW between air and N-BK7 from
      both sides, up to 85 degrees;
    - a CSV curve in percent reads as its fraction.
- **Optic parity** (GPU self-tests): the table lookup within 3.6e-7 of the C++ lookup, and `coatingExact()` within 1.3e-6
  of the transfer matrix.
- The detector renders wait for [physoptics-detectors.md](physoptics-detectors.md), which does not exist yet. Since 2026-11-04 it does: `tools/verify/detector_check.py` measures an ideal 50:50 cube's split on two power meters. The neutral density filter and the dichroic on detectors remain.
  `tools/verify/optics_check.py` checks the same things in images instead. With `--quick`:
    - a lossless polished lens in a white furnace reads 1 against the background in exact RGB, monochromatic and
      exact coatings, and within 0.6% in hero 4 and bands 8;
    - an OD 1 filter through ideal AR faces transmits 0.0996 in exact RGB and 0.099 in hero 4;
    - an ideal 50:50 cube passes (0.497, 0.498, 0) of a red and a green emitter, where (0.5, 0.5, 0) is exact;
    - an N-BK7 window transmits 0.9208 bare (Fresnel at its two faces, with interreflections, gives 0.92), and 0.9953
      `-A` coated by the table against 0.9956 by the transfer matrix;
    - a lens with an ABg lobe (TIS 0.05, B 0.01) reads 0.9999 in the furnace, and the lobe's light by BSDF sampling and
      by MIS agree within 1.4%.

Not built, and left open: the Thorlabs curves themselves (the presets are designs; a downloaded curve loads through
**Import Coating Curve**) and a glass's temperature.

### Indices relative to air (2026-11-02)

The laser sources' checks ([physoptics-laser-sources.md](physoptics-laser-sources.md)) found that a catalog glass's formula was read as an absolute index and divided by Ciddor's air at each surface, so the air came out twice: a catalog's index is already relative to air, as Schott's data and the AGF format give it. In air every lens refracted by 2.8e-4 too little and focused 0.05 % long. A catalog glass is now `relativeToAir`: `glassIndex()` multiplies its formula by Ciddor's air, so indices are absolute everywhere, a lens in air refracts by the catalog's own ratio, and in vacuum by the absolute index. `indexRelativeToAir()` gives the catalog's value, which nd, the Abbe number and the panel show, and `absoluteIndexRef()` tabulates the absolute index for the GPU's media and coating layers every nanometer from 200 to 2500 nm. `bin/optics_test` checks N-BK7's ratio in air against Schott's 1.5150892 at 632.8 nm, and the table against the formula.
