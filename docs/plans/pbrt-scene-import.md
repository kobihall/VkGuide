# pbrt scene import

The strongest test of a physically based renderer is the same scene rendered by a trusted one. pbrt-v4 is that renderer here: its source is in `../pbr-book-website/pbrt-v4`, it builds on this Mac, and the scenes published with it (the separate `pbrt-v4-scenes` repository) cover every feature the other plans add. Comparing the two renderers needs a scene both can read. This plan adds an importer for a subset of pbrt-v4's scene format that grows with the features, and a comparison workflow built on OpenEXR export.

*Not implemented. Depends on [pbrt-aovs-and-image-output.md](completed/pbrt-aovs-and-image-output.md) (EXR reference loading). What it can import grows with every other plan. Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- **File** → **Import glTF Model...** is the only way to bring in geometry. A scene file (`scene_io.h`) records model paths, which are re-imported on load, plus shapes, cameras, lights and settings.
- The world is right-handed with the camera looking down −z, as in glTF. pbrt's world is left-handed with the camera looking down +z.
- Nothing reads PLY meshes, which most pbrt scenes use.

## Design

### Parsing

A tokenizer and parser for pbrt-v4's format in `src/pbrt_import.cpp`: quoted strings, numbers, brackets, comments, and typed parameter lists (`"float fov" [45]`, `"rgb reflectance" [0.5 0.5 0.5]`, `"spectrum eta" "metal-Au-eta"`). The directives:

- Transforms: `Translate`, `Rotate`, `Scale`, `LookAt`, `Transform`, `ConcatTransform`, `CoordinateSystem`, `CoordSysTransform`, `ReverseOrientation`, and the graphics-state stack `AttributeBegin`/`AttributeEnd`.
- Scene-wide: `Camera`, `Film`, `Sampler`, `PixelFilter`, `Integrator`, `ColorSpace`, `Option`, `WorldBegin`.
- Content: `Shape`, `Material`, `MakeNamedMaterial`, `NamedMaterial`, `Texture`, `LightSource`, `AreaLightSource`, `MakeNamedMedium`, `MediumInterface`, `ObjectBegin`/`ObjectEnd`/`ObjectInstance`.
- Files: `Include` and `Import`, resolved relative to the including file.

pbrt-v4's `parser_test.cpp` gives tokenizer cases worth porting as tests. The parser does not reuse pbrt's code, which depends on pbrt's own utility libraries.

### Coordinates

pbrt's left-handed world and camera become right-handed by mirroring z in both: a world point maps by `M = diag(1, 1, −1)`, and a camera-to-world matrix `C` becomes `M C M`. Mirroring both keeps the image the right way round: pbrt's camera has +x right and +y up, as ours does. Triangle winding flips, which matters only for one-sided emitters and `ReverseOrientation`, and the importer flips those flags to match.

pbrt's perspective `fov` spans the shorter image axis (its screen window runs from −1 to 1 on that axis, `cameras.cpp`). The importer converts it to `SceneCamera`'s vertical field of view using the film's aspect ratio.

### What maps where

Each pbrt feature maps to the plan that adds it. Until that plan lands, the importer substitutes and says so:

| pbrt | Here | Until then |
|---|---|---|
| `perspective`, `orthographic`, `spherical`, `realistic` cameras | [pbrt-cameras.md](pbrt-cameras.md) | perspective with the same pose |
| `rgb`, `gbuffer`, `spectral` films; `iso`, `exposuretime`, `whitebalance`, `sensor` | [pbrt-film-and-reconstruction.md](completed/pbrt-film-and-reconstruction.md), [pbrt-display-and-color.md](pbrt-display-and-color.md), [pbrt-aovs-and-image-output.md](completed/pbrt-aovs-and-image-output.md) | resolution only |
| samplers, `pixelsamples` | [pbrt-sample-generation.md](completed/pbrt-sample-generation.md) | PCG, and the sample count |
| filters | [pbrt-film-and-reconstruction.md](completed/pbrt-film-and-reconstruction.md), implemented: `box`, `triangle`, `gaussian`, `mitchell` and `sinc` (as `lanczos`) map to `RenderSettings::pixelFilter` | the file's filter kind and its radius |
| `path`, `volpath`, `bdpt`, `lightpath`, `sppm`, `mlt`, `ambientocclusion` | the integrators of the other plans; `ambientocclusion` is [pbrt-aovs-and-image-output.md](completed/pbrt-aovs-and-image-output.md)'s debug view | `path` |
| `trianglemesh`, `plymesh` (with `displacement`), `loopsubdiv` | meshes; [pbrt-subdivision-and-displacement.md](pbrt-subdivision-and-displacement.md) | meshes without refinement |
| `sphere`, `cylinder`, `disk`, `bilinearmesh`, `curve` | [pbrt-analytic-shapes.md](pbrt-analytic-shapes.md), [pbrt-curves-and-hair.md](pbrt-curves-and-hair.md) | full spheres; others skipped |
| materials, `interface` | [pbrt-bsdf-library.md](pbrt-bsdf-library.md), [pbrt-layered-materials.md](pbrt-layered-materials.md), [pbrt-measured-bsdfs.md](pbrt-measured-bsdfs.md), [pbrt-curves-and-hair.md](pbrt-curves-and-hair.md), [pbrt-subsurface-scattering.md](pbrt-subsurface-scattering.md), [pbrt-material-system.md](completed/pbrt-material-system.md) | a `pbr` material with the closest parameters, or gray Lambertian |
| `imagemap` and the other textures | [pbrt-textures.md](pbrt-textures.md), [pbrt-procedural-textures.md](pbrt-procedural-textures.md) | image maps only; procedural textures become their mean |
| lights | [pbrt-light-types.md](pbrt-light-types.md), [pbrt-environment-lighting.md](pbrt-environment-lighting.md) | point, spot, distant, diffuse area, image infinite (converted to equirectangular) |
| media | [pbrt-participating-media.md](pbrt-participating-media.md) | skipped |
| spectra: named, `blackbody`, `.spd` files | [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md) | their RGB projection |

Every substitution and every unknown directive or parameter goes into an import report shown after loading, with file and line, so a mismatch in the comparison can be traced to what the importer dropped.

Units: with pbrt's default sensor and imaging ratio, an emitter `"rgb L" [1 1 1]` produces a pixel value of 1, so RGB emission and reflectance carry over unchanged in exact RGB mode. Lights given by `power` or `illuminance` go through [pbrt-light-types.md](pbrt-light-types.md)'s photometric conversions, and the comparison scenes test them.

### PLY and files

- A small PLY reader, vendored into `../CPPLibraries`: pbrt vendors `rply`, which reads ASCII and binary PLY with positions, normals, uvs, face indices and quads. Quads become two triangles until bilinear patches exist.
- gzip-compressed files (`.ply.gz`, `.pbrt.gz`), which pbrt-v4 reads through libdeflate, decompress through zlib, which macOS provides.

### In the scene model

A `.pbrt` file imports as a model, like a glTF file: **File** → **Import pbrt Scene...** reads it, its meshes become mesh objects, and its analytic shapes, cameras and lights become scene objects. The scene file records the `.pbrt` path and re-imports it on load, and edits to its materials are saved as overrides, as [pbrt-material-system.md](completed/pbrt-material-system.md) does for glTF materials. Its render settings (resolution, samples, integrator, filter) are offered, not forced: **Use pbrt settings** applies them.

### Comparing with pbrt

A script, `tools/compare_with_pbrt.sh`, renders a scene with a locally built pbrt-v4 to EXR, renders it here in batch mode ([pbrt-aovs-and-image-output.md](completed/pbrt-aovs-and-image-output.md)), and reports the relative MSE and the mean relative error between them. **Compare to reference** does the same interactively after **Load reference**. Both renderers need enough samples that their noise is below the difference being looked for; the script reports each image's own variance estimate beside the difference.

## Registry, bindings and data

- `src/pbrt_import.cpp` and its tests; a vendored PLY reader.
- **File** → **Import pbrt Scene...**; the import report window.
- Scene file: a model entry whose path is a `.pbrt` file.
- No kernel, variant or binding.

## Verification

- Scenes without a pbrt import render bit-identical.
- Parser tests: pbrt-v4's tokenizer cases; a hand-written scene using every supported directive round-trips its transforms to 1e-6.
- `assets/scenes/cornell_box.gltf` written out by hand as a `.pbrt` file, and the Cornell box from `pbrt-v4-scenes`, rendered by both renderers at 16384 spp: relative MSE below the two images' own noise, and the mean relative error under 0.5 %.
- A scene with `fov` on a portrait film frames the same view in both renderers.
- As each plan lands, its features gain a comparison scene: glass and metal for [pbrt-bsdf-library.md](pbrt-bsdf-library.md), a hair model for [pbrt-curves-and-hair.md](pbrt-curves-and-hair.md), a smoke volume for [pbrt-participating-media.md](pbrt-participating-media.md), and so on.

## Order of work

1. The tokenizer, parser and transform stack, with tests.
2. Cameras, `trianglemesh` and `plymesh`, `diffuse` and `dielectric`, point and area lights: enough for the Cornell box.
3. The comparison script.
4. Each further feature as its plan lands.

## Sources

The pbrt-v4 file format documentation (pbrt.org, "pbrt-v4 file format"); the `pbrt-v4-scenes` repository (github.com/mmp/pbrt-v4-scenes); PBR 4ed appendix C (processing the scene description: tokenizing, parsing, the graphics state, building the scene); pbrt-v4 `src/pbrt/parser.cpp`, `parser_test.cpp`, `cameras.cpp` (the perspective camera's screen window), `util/file.cpp` (gzip); rply (Diego Nehab), the PLY reader pbrt vendors.
