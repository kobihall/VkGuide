# pbrt scene import

The strongest test of a physically based renderer is the same scene rendered by a trusted one. pbrt-v4 is that renderer here: its source is in `../pbr-book-website/pbrt-v4`, it builds on this Mac, and the scenes published with it (`assets/models/pbrt-v4-scenes`) cover every feature the other plans add. This plan adds an importer for pbrt-v4's scene format, a report of everything it substitutes, and a script that renders one file with both renderers and compares them.

*Implemented 2026-10-21. Depends on [pbrt-aovs-and-image-output.md](pbrt-aovs-and-image-output.md) (EXR export and the batch mode). What it imports exactly grows with every other plan; each of those plans has an "Importing pbrt scenes" section that says what to change. Part of the [roadmap](../pbrt-roadmap.md).*

## Decisions

On 2026-10-21, when implementation started, the user answered four questions:

- A `.pbrt` file builds the same in-engine model a glTF file does. Names that were glTF-only and now cover both became neutral: `LoadedGLTF` is `LoadedModel`, `GltfMaterialOrigin` is `ModelMaterialOrigin`, `MeshMaterialMode::Gltf` is `MeshMaterialMode::Model`, and `findGltfMaterial()` is `findModelMaterial()`. Scene file keys did not change.
- Area lights import as two-sided. pbrt's are one-sided unless `twosided` is set, and [pbrt-light-types.md](../pbrt-light-types.md) records what restores that.
- The RGB projection of spectra starts the `rt_spectrum` library now, which [pbrt-spectral-rendering.md](../pbrt-spectral-rendering.md) extends.
- An infinite light's map is converted in memory to the renderer's equirectangular layout, with its rotation baked in, and owned by the model.

The user also asked that the tokenizer and parser stay as simple as pbrt's. The scanner is pbrt's `Tokenizer::Next()`, about 80 lines that split text into words, numbers, quoted strings and brackets, and the parser is pbrt's `parse()` directive switch. There is no grammar and no virtual target interface: the parser keeps the graphics state itself and fills plain structs.

## Reading the file

`src/pbrt_parser.h/.cpp` reads a file and everything it includes into a `PbrtScene`:

- The scanner reads `.gz` files through zlib, as pbrt reads them through libdeflate.
- Every directive pbrt-v4 parses is handled: the transforms (`Translate`, `Rotate`, `Scale`, `LookAt`, `Transform`, `ConcatTransform`, `Identity`, `CoordinateSystem`, `CoordSysTransform`, `ActiveTransform`, `TransformTimes`), `AttributeBegin` and `AttributeEnd` (and the deprecated `TransformBegin` and `TransformEnd`), `Attribute`, `ReverseOrientation`, the scene-wide directives, the content directives, `ObjectBegin`, `ObjectEnd` and `ObjectInstance`, `Include` and `Import`, `ColorSpace` and `Option`. pbrt-v3's `WorldEnd` is accepted and ignored.
- A parameter list follows pbrt's `parseParameters()`. `color`, `point`, `vector` and `normal3` become `rgb`, `point3`, `vector3` and `normal`, and pbrt-v3's quoted bools read as bools.
- A shape records the transform at both ends of the shutter, the material in effect (by index, or by name: a `NamedMaterial` may come before its `MakeNamedMaterial`, as pbrt allows), the area light, the media and `ReverseOrientation`.
- Paths resolve against the directory of the first file read, which is pbrt's rule (`SetSearchDirectory()`), not against the including file.
- `Import` restores the graphics state when its file ends, as pbrt's separate builder does. The restore waits until the next directive starts, because a parameter list looks one token past the end of the file.
- A syntax error stops the read with `file:line: message`, where pbrt would exit. What pbrt only warns about goes into `PbrtScene::warnings`.
- `PbrtParseOptions::skipShapes` counts shapes of a type without keeping them. The importer skips `curve`, since `bunny-fur` and the `hair` scenes hold 5.9 million curves whose parameters would take gigabytes.

`src/ply_mesh.h/.cpp` reads PLY meshes through rply, the copy pbrt vendors, which reads `.ply.gz` through zlib. It is in `../CPPLibraries/rply`. The reader is pbrt's `TriQuadMesh::ReadPLY()`: positions, normals, uvs under any of pbrt's four names, triangles, quads in pbrt's bilinear patch order, and face indices.

## Mapping onto the scene model

`src/pbrt_import.h/.cpp` maps a `PbrtScene` to an `ImportedModel` (`src/imported_model.h`). An `ImportedModel` is the model as plain data, with no Vulkan in it: meshes, material definitions, image files, nodes, analytic shapes, lights, a camera, an environment, the file's render settings and the import report. `loadImportedModel()` in `src/vk_loader.cpp` turns it into a `LoadedModel`, and from there a pbrt model is a glTF model to everything downstream: mesh objects, the material library and its overrides, the BLAS cache, the texture array and the raster preview.

### Coordinates

pbrt's world is left-handed and its camera looks down +z; the renderer's world is right-handed and its camera looks down -z. A camera point maps by `M = diag(1, 1, -1)`. A world point also maps by `M`, unless the camera's own transform has a negative determinant. Many pbrt-v4 scenes start with `Scale -1 1 1` before `LookAt` for this reason. Then world points map unchanged (`Importer::chooseWorldMirror()`). Either way the camera stays a rotation, and a point lands at the same place on the screen in both renderers, which `bin/pbrt_test` checks for both cases.

The renderer orients a triangle by its object-space winding transformed by the inverse transpose, and so does pbrt after its `transformSwapsHandedness` correction, so the mirror needs no winding change. `ReverseOrientation` flips a mesh's vertex normals when it has them and its winding when it does not, as pbrt does.

pbrt looks up image textures at `1 - v`, so its v runs up the image, and glTF's runs down: the importer writes `1 - v`.

### Cameras and settings

- pbrt's `fov` spans the shorter image axis. `camera()` converts it to a vertical field of view with the film's aspect ratio, or `frameaspectratio`. `lensradius` becomes the aperture, `2 * lensradius`, and `focaldistance` the focus distance.
- pbrt multiplies its image by the imaging ratio, shutter times ISO over 100. That becomes the camera's exposure, which is display only, so the window looks like pbrt's image and an EXR export holds the radiance.
- The file's render settings are offered, not forced. **Windows** → **Import Report** → **Use pbrt settings** (`VulkanEngine::usePbrtSettings()`) applies the resolution, `pixelsamples`, the sampler and its randomization (pbrt's defaults: permuted digits for Halton, fast Owen for the rest), the pixel filter, `maxdepth + 1` as the ray depth, and `maxcomponentvalue` as the sample clamp. It also selects the filtered camera kernel and the compensated film, and renders from the file's camera. A `gbuffer` film turns on the auxiliary outputs, and the `ambientocclusion` integrator turns on the ambient occlusion view.
- The renderer's ray depth counts segments. pbrt's `maxdepth` counts bounces and adds emission one bounce further, so `maxdepth 5` is a ray depth of 6.
- A scene with no infinite light gets a black background, since the renderer would otherwise show its sky.

### Materials

| pbrt | Here |
|---|---|
| `diffuse` | `lambertian` |
| `coateddiffuse` | `pbr`, metallic 0: a GGX coat with F0 0.04 over Lambertian |
| `conductor` | `pbr`, metallic 1, the metal's Fresnel reflectance at normal incidence as its color |
| `coatedconductor` | `pbr`, metallic 1, the conductor's color; the coat is dropped |
| `dielectric` | `dielectric`, smooth, eta averaged over wavelength |
| `thindielectric` | `dielectric` |
| `diffusetransmission` | `lambertian` with `reflectance` times `scale` |
| `mix` | whichever of its two materials pbrt picks more often |
| `measured`, `hair`, `subsurface` | `lambertian` |
| `interface`, `""`, `"none"` | the shape is not drawn |

Roughness becomes GGX alpha through pbrt's `RoughnessToAlpha()` (`sqrt(r)` when `remaproughness` is on), and `pbr`'s perceptual roughness is the square root of alpha.

A material's color may be a texture. `textureValue()` resolves it to a constant factor and at most one image, which becomes the material's base color texture: an `imagemap` with its `scale`, `uscale`, `vscale`, `udelta` and `vdelta`, or a `scale`, `mix`, `constant` or `checkerboard` texture built from those. Float textures, such as `roughness`, are read as their constant. A `normalmap` becomes the material's normal texture, with tangents in pbrt's frame: x along dp/du, and y the normal crossed with x.

`lambertian` now reads a base color texture and a normal texture when an imported model's material sets them. Every other material packs as it did, which the bit-identity check below confirms.

An `AreaLightSource "diffuse"` makes the shape's material an `Emissive` one with the light's radiance: `L` normalized as pbrt normalizes it, then times `scale`, or scaled to `power` over the shape's area.

### Shapes

- `trianglemesh`, `plymesh` and `loopsubdiv`'s control mesh become meshes, one per shape, each placed by a node at the shape's transform. A mesh without vertex normals is shaded flat by the path tracer (`MeshAsset::cpuNormalsMissing`), as pbrt shades it with the geometric normal.
- `ObjectBegin` and `ObjectEnd` define meshes once, and each `ObjectInstance` adds nodes that place them, so instances share their BLASes.
- `bilinearmesh` and a PLY file's quads are two triangles a patch.
- `sphere` is the analytic sphere when it is whole and its transform scales it along its axes, and triangles otherwise. It belongs to the model, `SceneShape::modelKey`, as a glTF file's lights do.
- `disk` and `cylinder` are triangles with 128 segments a circle, their radii scaled so the area, and an area light's power, stays the disk's or the cylinder's.
- `curve` is counted and skipped.

### Lights

- `point`, `spot` and `distant` become scene lights. Their spectrum is normalized to luminance 1 and multiplied by `scale`, with `power` and `illuminance` converted by pbrt's formulas.
- `goniometric` and `projection` become point lights without their image.
- `infinite` becomes the environment. A square map in pbrt's equal-area octahedral layout is resampled to 2N by N equirectangular texels, each the mean of four nearest lookups, through the light's transform and the world mirror (`resampleEqualArea()`, with pbrt's `EqualAreaSphereToSquare()` ported). A map without a file, `"rgb L"`, is a constant map. `illuminance` is converted as pbrt measures it. The scene file records the model that owns the environment (`"environmentModel"`) and no path, and removing the model removes the map.

### Spectra

`src/spectrum.h/.cpp` (static library `rt_spectrum`) holds pbrt-v4's CIE 1931 tables and all 88 of its named spectra in `src/spectrum_tables.cpp`, generated from pbrt's `src/pbrt/util/spectrum.cpp` by `tools/spectrum_tables/extract_pbrt_spectra.py`. A spectrum is dense at every nanometer from 360 to 830, the samples pbrt's `InnerProduct()` sums.

- A light's spectrum becomes `emissionToRgb()`: its XYZ through pbrt's sRGB matrix, scaled to luminance 1, which is pbrt's `scale /= SpectrumToPhotometric(L)`. An `"rgb L"` keeps its RGB, which is what pbrt's normalization of the illuminant alone gives. A color outside sRGB (a 1850 K blackbody's blue is -0.0097) is clamped to it.
- A reflectance spectrum becomes `reflectanceToRgb()`, its color under D65.
- An index of refraction becomes `luminanceWeightedMean()`. A conductor's color is its spectral Fresnel reflectance at normal incidence, computed from eta and k at every nanometer, under D65.
- `.spd` files read as pbrt reads them.

### The import report

Every substitution, every directive or parameter the importer does not read, and every warning of the parser goes into `ImportReport`. Each distinct message appears once, with how often it happened, the `file:line` it first happened at, and the plan in `docs/plans/` that adds what is missing. The parameters a mapping never looked up are found automatically: `PbrtParams` marks a parameter as it is read, and `reportUnused()` lists the rest. **Windows** → **Import Report** shows it, the batch mode prints it, and `./bin/pbrt_test --report <file.pbrt>` prints it without a GPU.

## In the scene

- **File** → **Import pbrt Scene...** takes `.pbrt` and `.pbrt.gz` files. `VulkanEngine::importModel()` picks the importer by the file's name, so a scene file's model list can name either, and opening the scene re-imports it.
- The camera the file defines is added to the scene's cameras. **Use pbrt settings** renders from it.
- Its lights and spheres belong to the model and go when it is removed, as do its environment and materials.
- `./bin/vulkan_guide --render <file.pbrt>` renders a pbrt file in a new scene with **Use pbrt settings** applied, and prints the import report and `render: pbrt imaging ratio <r>`.

## Comparing with pbrt

`tools/verify/pbrt_compare.py SCENE.pbrt` renders a file with pbrt and with this renderer, twice each with different seeds, and prints:

- Each channel's mean radiance here over pbrt's.
- The relative MSE between the two seeds' averages.
- The noise floor: the same measure between each renderer's two seeds. Two converged renders of the same scene differ by their noise alone.
- The import report.

pbrt's image is divided by its imaging ratio. `--out DIR` keeps PNGs of both renders and of their difference. `--cornell` compares `tools/verify/cornell.py`'s Cornell box written as a `.pbrt` file by `pbrt_cornell.py`, which needs no substitution. The script is Python beside the other checks in `tools/verify/`, not the shell script the plan named, since it reuses `vkscene.py`'s batch-mode runner, and the repository ignores `*.sh`.

## Registry, bindings and data

- Static libraries `rt_pbrt` (`pbrt_parser`, `ply_mesh`, `pbrt_import`, rply), `rt_spectrum` and `rt_material`. The material model was compiled into the app and included `rt_gpu_types.h`, which pulled Vulkan into it for one 64-byte struct: its GPU records now live in the Vulkan-free `src/rt_material_record.h`, so the importer and its test can build materials.
- `LoadedModel::imported` holds what the importer brought beyond the geometry. `modelMaterialDef()` gives a model's material as its file defines it, for import and for **Revert** in the **Materials** window, either glTF's or the importer's definition.
- Scene file payload version 17: a model may be a `.pbrt` file, a shape records its `"model"`, and `"environmentModel"` names the model whose environment the scene shows.
- No kernel, variant or binding.

## Verification

- `bin/spectrum_test` checks the tables (Y sums to pbrt's `CIE_Y_integral`, all 88 named spectra, the illuminants normalized), the normalizations, `.spd` files, and ten spectra against pbrt-v4's own renders made by `tools/spectrum_tables/pbrt_reference.py`. The ten are three blackbodies, two standard illuminants, an RGB light, a spectral reflectance, and gold, copper and silver at normal incidence. All agree with pbrt within 3e-3, about pbrt's noise.
- `bin/pbrt_test` checks the scanner with pbrt-v4's own tokenizer cases from `parser_test.cpp`. A hand-written scene uses every transform directive, the attribute stack, named coordinate systems, `Import`'s scoping, an `Include` from a subdirectory, an instance and a forward-referenced named material, each transform to 1e-9. It checks four cameras against pbrt's projection (landscape and portrait, with and without `Scale -1 1 1`), and the mapping of settings, lights, a disk's area, a sphere, a dielectric's uvs, a gold conductor and the report.
- `./bin/pbrt_test --scan assets/models/pbrt-v4-scenes` imports all 98 scene files of the repository on the CPU. The largest, `landscape`, holds 407,566 instances, and `kroken` 40 million triangles.
- `tools/verify/pbrt_compare.py --cornell --spp 1024`: relative MSE 4.98e-4 against a noise floor of 4.86e-4, and channel means within 0.3 percent of pbrt's.
- `killeroo-simple` at 256 spp: means within 0.45 percent. The killeroos are their faceted control meshes where pbrt subdivides them.
- `ganesha` at 64 spp, with its equal-area sky, an image texture and two area lights: means within 0.7 percent. The statue is slightly lighter, where pbrt's coated diffuse darkens its base.
- `killeroo-gold` at 256 spp: the grid texture and the gold match, the mean is 4 percent dark, and the per-pixel error is large where the faceted gold reflects different parts of the room.
- The ten scenes in `assets/scenes`, Sponza among them, render byte for byte as the build before this one did (16 spp, 160 by 120, seed 7).
- `bin/light_test`, `sampler_test`, `film_test --selfcheck`, `aov_test --selfcheck`, `bsdf_test --quick` and the GPU self-tests pass.

## Not built

- A scene of hundreds of thousands of instances (`landscape`, `sanmiguel`) imports, but each instance is a mesh object in the **Scene** panel and the TLAS, and each mesh is uploaded with its own immediate submit. Scenes of that size need batched uploads and a scene panel that does not list every object.
- The texture array holds 256 layers at 512 by 512, so `sanmiguel`'s 231 images fit and lose resolution, and a larger set loses images. [pbrt-textures.md](../pbrt-textures.md) replaces it.
- The import report is not saved with the scene; reopening the scene imports the file again and rebuilds it.

## Sources

The pbrt-v4 file format documentation (pbrt.org, "pbrt-v4 file format"); the `pbrt-v4-scenes` repository; PBR 4ed appendix C (processing the scene description); pbrt-v4 `src/pbrt/parser.cpp` and `parser_test.cpp` (the tokenizer and parser), `scene.cpp` (`BasicSceneBuilder`: the graphics state, instancing, `Import`), `paramdict.cpp`, `materials.cpp` (each material's parameters and defaults), `lights.cpp` (light normalization, `power`, `illuminance`), `cameras.cpp` (the screen window), `film.cpp` (`PixelSensor`, the imaging ratio), `util/spectrum.cpp` (the CIE tables and named spectra), `util/colorspace.cpp`, `util/math.cpp` (`EqualAreaSphereToSquare()`), `util/mesh.cpp` (`TriQuadMesh::ReadPLY()`) and `util/file.cpp` (`ResolveFilename()`); rply (Diego Nehab), the PLY reader pbrt vendors.
