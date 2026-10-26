# Measured BSDFs

Analytic BSDFs cannot reproduce iridescent paint, woven fabric, or the real angular falloff of a particular brushed aluminum. pbrt-v4's `MeasuredBxDF` renders reflectance measured on a goniophotometer, in the reparameterized form of Dupuy and Jakob (2018), which stores the data where the material's own visible-normal distribution concentrates it. The RGL database publishes dozens of isotropic and anisotropic materials in that format, spectrally. This plan adds a `measured` material that loads them.

*Not implemented. Depends on [pbrt-bsdf-library.md](pbrt-bsdf-library.md). Spectral measurements need [pbrt-spectral-rendering.md](completed/pbrt-spectral-rendering.md) to be seen as measured; exact RGB projects them. Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

No material is data-driven. Every appearance comes from a closed-form model.

## Design

### The data

An RGL `.bsdf` file holds, for one material (PBR 4ed §9.8.1):

- The microfacet normal distribution and the projected-area function, as 2D tables.
- A 2D warp that samples visible normals from them, marginal and conditional.
- The reflectance itself over (incident elevation, incident azimuth, a 2D sample-space position), per wavelength or per RGB channel, with the wavelengths it was measured at.
- Whether it is isotropic, which drops the incident azimuth dimension.

The loader (`src/measured_bsdf.h/.cpp`, part of `rt_bsdf`) parses the tensor file format, and the renderer concatenates every loaded material's tables into `MeasuredBsdfs` (binding 41). Isotropic materials run to a few MB; anisotropic ones to tens of MB. The material record stores offsets into that buffer.

### Evaluation and sampling

`crt_bxdf_measured.glsl` follows pbrt-v4:

- `f(wo, wi)`: the half vector, inverted through the visible-normal warp to a position in the sample space, then linear interpolation of the reflectance table in 3 or 4 dimensions, scaled by the Jacobian of the reparameterization.
- `sampleF()`: a visible normal drawn through the warp, the reflected direction, and the same interpolation for the value.
- `pdf()`: the warp's density at the half vector, times the Jacobian.

In spectral modes the table is interpolated in wavelength at each lane. In exact RGB it holds RGB: the loader projects spectral measurements to linear Rec. 709 once, at load.

The material is complex: large tables and irregular memory access belong in the complex queue ([pbrt-material-system.md](completed/pbrt-material-system.md)).

### Fourier BSDFs, optionally

PBR 3ed §8.6 renders isotropic BSDFs stored as Fourier series in azimuth over a grid of elevations, as produced by Jakob's `layerlab` for layered materials. pbrt-v3's `.bsdf` Fourier files are an existing library of layered-material data. A second type, `fourier`, is a later, optional step; the RGL format covers measured data better, and [pbrt-layered-materials.md](pbrt-layered-materials.md) covers layering procedurally.

### The UI and scene file

The Materials window's `measured` type has one parameter, a file picker for a `.bsdf` file. The scene file stores the path relative to the scene, like model paths.

## Importing pbrt scenes

[completed/pbrt-scene-import.md](completed/pbrt-scene-import.md) imports pbrt-v4 scenes today and substitutes for this plan's features. Each substitution is a line in the import report, so `./bin/pbrt_test --report <file.pbrt>` lists the scenes that need this plan. When this plan lands, change the importer as follows, then run `tools/verify/pbrt_compare.py` on the scenes named below.

- **Measured materials** (14 uses: `barcelona-pavilion`, `contemporary-bathroom`, `lte-orb`, `sportscar`) are gray Lambertian, and their `filename` is read and ignored (`mapMaterial()` in `src/pbrt_import.cpp`). Map `measured` to this plan's RGL BSDF, resolving `filename` against the scene's directory (`PbrtScene::directory`), as every other path is.

## Verification

- `bin/bsdf_test` on three RGL materials (one isotropic metal, one anisotropic, one paint): the χ² test against the warp's own density, and energy at or below 1.
- A sphere of each, rendered next to the RGL database's reference images under a matching environment.
- Every other scene renders bit-identical.

## Order of work

1. The `.bsdf` loader and the CPU `MeasuredBxDF` in `rt_bsdf`, tested.
2. Upload and the GLSL port, exact RGB first.
3. Spectral evaluation.
4. `fourier`, if wanted.

## Sources

PBR 4ed §9.8 (measured BSDFs, basic data structures, evaluation); PBR 3ed §8.6 (Fourier basis BSDFs), §14.1.4 (sampling them); Dupuy and Jakob 2018, *An adaptive parameterization for efficient material acquisition and rendering*; pbrt-v4 `src/pbrt/bxdfs.h` (`MeasuredBxDF`); the RGL material database at rgl.epfl.ch/materials.
