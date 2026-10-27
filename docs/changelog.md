# Changelog

## 2026-10-26

### New

- Four material types from pbrt-v4. `diffuse` is a Lambertian surface, or Oren and Nayar's rough diffuse surface when **roughness sigma** is above 0 degrees. `thin_glass` is a pane that transmits straight through and reflects what two parallel interfaces and their inter-reflections do, 2R / (1 + R). `diffuse_transmission` reflects and transmits diffusely, with a **scale**. `mix` takes one of two other materials at each hit, the second with the chance **amount**.
- A `mix` picks its child from a hash of the hit point and the direction, so a hit chooses the same child each time it is evaluated. A child can be any material that scatters, including another mix. Its emission and alpha cutoff do not show through the mix.
- `pbr` has a **dielectric F0** parameter, the reflectance of its non-metal part at normal incidence, 0.04 by default.
- A glTF material with `KHR_materials_transmission` at 1, no transmission texture and no emission imports as `glass` with the file's `KHR_materials_ior` and roughness. `KHR_materials_ior` and the factor of `KHR_materials_specular` set the F0 of every other material. `pavilion-day.gltf`'s water and glass are now glass. A material the importer reads but cannot carry (anisotropy, volume, a partial or textured transmission, a specular color) prints a line on the console and stays `pbr`.
- `tools/verify/bsdf_check.py` renders furnaces for the new types and a camera inside a glass sphere, which sees n² = 2.25. `run_all.py` runs it.
- `bin/bsdf_test` covers the new BxDFs, including a thin pane against 2R / (1 + R) and the choice of a mix against its amount. **Windows** → **GPU self-tests** → **BxDF parity** covers them in the shaders and compares the mix hash bit for bit.

### Changed

- A pbrt scene's `thindielectric` imports as `thin_glass`, no longer as solid glass. A `diffusetransmission` with a constant reflectance and transmittance imports as `diffuse_transmission`; a textured one is still Lambertian with its texture. A `mix` of two named materials imports as `mix`, no longer as whichever child pbrt picks more often. These scenes render differently from before.
- Scene files are payload version 19: the four types, a mix's `"materialA"` and `"materialB"` as material references, and `"f0"` on `pbr`. An older file opens as before.
- The glTF loader reads `KHR_materials_ior`, `_specular`, `_transmission`, `_anisotropy` and `_volume`, which it skipped before.

### Internal

- `rt_material.h` gains `ParamKind::Material`, `MaterialLink` and `PackContext::materialIndex`, which a mix's pack function calls for its children. `buildSceneAccel()` reserves a material's slot before packing it, so a mix's children follow it in the table. `CrtMixMaterial` is the mix's 64-byte record, laid out so a `ClassicMaterial` reader finds no texture and no alpha test.
- `BxdfMaterial` in `crt_bxdf.glsl` gains `reflectance`, `transmittance`, `orenA` and `orenB`. Kernel 06's complex build resolves a mix before it reads the material, and `CRT_MIX_DEPTH` bounds the chain at 8.
- `pbr` keeps its dielectric F0 in the `param` float of its record, 0 for 0.04. Every existing record renders bit-identical: seven scenes and an auxiliary-output render matched the build before.
- `docs/plans/pbrt-bsdf-library.md` moved to `docs/plans/completed/`, with an "As built" and a "Not built" section.

## 2026-10-23

### New

- **Raytrace Render** → **Spectral** → **Representation** chooses what a path carries: **Exact RGB**, **Monochromatic**, **RGB Gaussians**, **Spectral bands** or **Hero wavelengths**. Exact RGB is the default for a new scene and renders bit-identical to the build before: 12 fixed-seed renders of the test scenes, with and without auxiliary outputs, light groups and a debug view, matched channel by channel.
- **Monochromatic** traces one wavelength from 1 to 20,000 nm, set by **Wavelength**. **Display** shows the spectral radiance in **Grayscale**, in the **Wavelength color** (with chosen colors for **Infrared**, **Ultraviolet** and **XUV (below 100 nm)**), or through a viridis, inferno or turbo **Colormap**, linear or logarithmic. The display changes only the shown image, and an EXR export holds the radiance.
- **Hero wavelengths** draws 4, 8, 16 or 32 wavelengths per path, as pbrt-v4 does, over a **Range** that can be any part of 1 to 20,000 nm. **Sampling** is pbrt's visible-importance density, uniform, or **Emitters' spectra**, which mixes in the scene's light spectra and laser lines so a 0.01 nm line is not missed. **Spectral bands** carries 8, 16 or 32 box or Gaussian bands, and **RGB Gaussians** three Gaussian bands fitted to the CIE functions, centered at 616, 536 and 449 nm and editable.
- Every color of a material or light has a **Spectral** button. Its menu makes the parameter a constant, a **Color temperature**, one of pbrt's 88 named spectra (metals, glasses, illuminants), an `.spd` file, a **Dispersion formula** (the AGF catalog's 13 formulas and Cauchy) or an **Emission line** with a center, a width and a Gaussian or Lorentzian profile. A line's scale is its integral, so a laser's radiance is its power.
- **Spectral** → **RGB color space** says what space the scene's RGB values are in: sRGB, DCI-P3, Rec. 2020 or ACES2065-1. The spectral modes uplift RGB colors with pbrt-v4's sigmoid tables for that space, which the build generates. Outside 360 to 830 nm an RGB color holds its 360 or 830 nm value; `docs/plans/spectral-out-of-range.md` records the open question.
- **Spectral** → **Design wavelength**, 587.6 nm by default, is where exact RGB reads a dispersive glass's index.
- The `glass` and `conductor` material types are pbrt-v4's dielectric and conductor: exact Fresnel, smooth or rough with separate u and v roughness, an index that is a spectrum, and for the conductor a complex index per wavelength (pbrt's copper by default) or a reflectance. A glass whose index varies with wavelength disperses in the spectral modes: through a dense flint prism, hero wavelengths spread a white slit into a spectrum and 16 bands into 16 slits.
- **Spectral** → **Spectral images** keeps each pixel's spectral radiance in wavelength buckets (the bands themselves, or 1 to 256 buckets for hero wavelengths). An EXR export writes them as `S0.<wavelength>nm` channels in the Spectral OpenEXR layout, and a monochromatic render writes its wavelength's channel.
- The batch mode takes `--spectral`, `--lanes`, `--wavelength`, `--range`, `--sampling`, `--color-space`, `--design-wavelength` and `--spectral-images`.
- `bin/spectrum_test` checks the uplift in four color spaces, blackbodies, dispersion formulas, lines, wavelength sampling, bands, the packing of the spectral data and the display colors; `--fit-gaussians` refits the RGB Gaussians. `bin/bsdf_test` covers `glass` and `conductor`, and **Windows** → **GPU self-tests** gains a BxDF parity test and a spectrum parity test.
- `tools/verify/spectral_check.py` renders a white furnace in every representation with nine materials, the monochromatic furnace at 450, 632.8 and 1064 nm, `assets/scenes/prism_dispersion.gltf` and the Cornell box against exact RGB. `run_all.py` runs it, `spectrum_test` and `pbrt_test`. `pbrt_compare.py --spectral hero` compares a pbrt scene in hero mode.

### Changed

- A pbrt scene's conductors import as `conductor` and its dielectrics as `glass`, rough ones included, with their spectra. Blackbody, named and `.spd` emission keeps its spectrum for the spectral modes, and a `Film "spectral"` imports as hero wavelengths with spectral images. In exact RGB these scenes render differently from before: `killeroo-gold`'s gold was `pbr` with Schlick's Fresnel and is now the conductor. In hero mode its channel means are each 0.958 of pbrt's, the 4 percent left being the faceted loop-subdivision surface it was before.
- Scene files are payload version 18: `"spectral"` in `"render"`, `"spectralDisplay"`, and spectra written as objects such as `{"blackbody": 2700, "scale": 1}`. An RGB value is still an `[r, g, b]` array, and an older file opens in exact RGB.
- The first render in a spectral mode builds that lane count's pipelines, which takes up to 15 s for the 16- and 32-lane complex kernel 06 before Metal's shader cache holds them.

### Fixed

- Fixed the `synchronization2` device feature being off since the image outputs change (2026-10-21). The feature struct hung off the dynamic-rendering struct's `pNext`, and vk-bootstrap rewrites each struct's `pNext` when it joins the chain, so adding the float-atomics struct dropped it. Every frame calls `vkCmdPipelineBarrier2KHR` and `vkQueueSubmit2KHR`, which are invalid without the feature, and validation reported about 780 errors in a short run. Each feature struct now has its own `add_pNext`.

### Internal

- Kernels 00, 03, 04, 06 and 08 include `crt_spectrum.glsl` and are built once without `CRT_LANES` (the exact RGB build, today's `.spv` names) and once per entry of `VKGUIDE_SPECTRAL_FAMILIES` (`1;4;8;16;32`), as `<name>.comp[.<traversal>][.<class>].lanes<N>.spv`. `KernelVariant::laneDependent` marks them, and the pipeline cache key carries the family. Specialization constants 3, 4 and 5 are the representation, the wavelength sampling and the spectral images.
- Binding 24 is `SpectralData` and 25 `SpectralUplift`; binding 63, `PathSpectra`, holds a spectral build's throughputs, `CRT_LANES` floats per path. `PathState` stays 64 bytes, with `lambdaU` and `laneMask` in the exact RGB throughput's place. A spectral `ShadowRay` is 64 bytes at 1 or 4 lanes and 176 at 32, `CrtParams` is 240 bytes, and `GpuLight`'s two pad words are `spectrum` and `spectrumScale`. `ChannelSums` holds `3 g + 1 + buckets` floats a pixel.
- `rt_spectrum` gains `src/color_space.*`, `src/spectrum_ref.*` and `src/spectral.*`, the C++ mirror of `crt_spectrum.glsl`. `rt_bsdf` gains `src/bxdf.*`, mirrored by `crt_fresnel.glsl`, `crt_microfacet.glsl` and `crt_bxdf.glsl`. `rt_material` links both. `tools/rgb2spec/rgb2spec_opt.cpp` writes `assets/spectra/uplift_*.bin` at build time, which git ignores.
- `SpectrumRef` replaces the RGB placeholder in `rt_material.h`; `MaterialParam` carries a `SpectrumRole`, `PackContext` the working space and design wavelength, and `PackedMaterial` four spectrum slots. `SceneLight::color` is a `SpectrumRef`. Every spectrum parameter in the panels goes through `spectrumEdit()` (`src/spectrum_edit.h`).
- `PathSchedule` lowers the samples per frame until the pool fits in `CRT_MAX_POOL_BYTES`, 1.5 GB.

## 2026-10-22

### New

- `./bin/pbrt_to_gltf <scene.pbrt> <out.gltf>` converts a pbrt-v4 scene to glTF 2.0 through the renderer's importer, to open in Blender, a three.js or Babylon.js viewer, or Unity as a raster baseline. A mesh placed many times is written once. `--instancing` groups each mesh's placements into one `EXT_mesh_gpu_instancing` node, and `--weld` merges vertices identical in position, normal and uv: `sanmiguel-courtyard`'s 7.5 million vertices become 1.8 million. The environment map is written beside it as `.hdr`.
- **Stats** → **Occlusion culling**, on by default, skips a surface of 4,096 triangles or more whose box was hidden behind the rest of the scene two frames earlier. In `pavilion-day` the GPU time of the raster preview falls from 35 ms to 8 ms from the scene's camera, and from 63 ms to 24 ms looking across the pavilion. A surface that comes out from behind another while the camera moves appears two frames late. With a still camera the image is unchanged.

- **Stats** → **Pause raster preview** keeps the last raster picture instead of drawing an identical one, once the camera, the scene, the render scale and the background have been still for three frames. **Always** pauses whenever that holds, **While rendering** only while the raytracer renders, and **Never** draws every frame. **Always** is the default. The raster preview of `pavilion-day` costs 26 ms a frame while drawn and 4 ms while paused.

### Changed

- A pbrt scene imports standing on its up axis, which the importer takes from the scene's camera and turns to +y: the axis nearest the camera's up when that is pbrt's +y or +z, and for a plan view such as `watercolor/camera-12.pbrt` the axis it looks down. `sanmiguel`, `killeroos`, `villa`, `sportscar` and `sssdragon` stood on their side in the viewport and now stand upright; the import report names any up axis other than +y. A render from the scene's camera converges to the same image. A scene file saved earlier with a z-up pbrt model reopens with its objects where they were saved and its environment turned upright, so import the model again.

### Fixed

- Fixed the raster preview taking up to 2 seconds a frame in scenes of many meshes, where MoltenVK made every mesh's vertex buffer resident again on every draw. In a Debug build, `sanmiguel-courtyard` looking into the courtyard took 1,864 ms a frame and takes 23 ms, `zero-day` took 1,511 ms and takes 15 ms, and `bistro_cafe` took 514 ms and takes 15 ms.
- Fixed a pbrt environment map gaining or losing light in the import, depending on how its texels lined up with the renderer's map. `sanmiguel`'s sky, whose sun is two texels holding half its light, was 15 percent too bright; each texel now keeps exactly its own light.
- Fixed the app aborting with `VK_TIMEOUT` when one frame took the GPU more than a second, as a raster frame of pbrt's `landscape` or a 1600 by 1100 path-traced frame of `sanmiguel-courtyard` does. The frame's fence wait allows 30 seconds.
- Fixed the app aborting when a render's geometry did not fit in GPU memory. pbrt's `landscape` needs 4,217 MB of BVH geometry, more than this GPU allocates at once (3,072 MB) or holds (4,096 MB); its render is now refused with those numbers in **Raytrace Render**.
- Fixed a scene of many objects stalling after its import, while the raytracer looked up each object's mesh through every mesh node. pbrt's `landscape` (407,566 objects) had not opened after 25 minutes; it opens in 2.5 minutes.

### Internal

- `mesh.vert` reads its vertices through the pipeline's vertex input (binding 0, locations 0 to 4) and the world matrix as a per-instance attribute (binding 1, locations 5 to 8), not through a buffer device address in push constants. `GPUMeshBuffers` has no `vertexBufferAddress`, `RenderObject` carries `vertexBuffer`, vertex buffers are created with `VK_BUFFER_USAGE_VERTEX_BUFFER_BIT`, and `PipelineBuilder::setVertexInput()` is new. CLAUDE.md's "No buffer device addresses in raster shaders" says why.
- `drawGeometry()` draws each run of one surface with one material as one instanced draw, with the world matrices in draw order in `FrameData::instanceBuffer`. `MeshDrawList` sorts by material, mesh and first index. `sanmiguel-courtyard`'s 50,781 draws in view become 764.
- `RaytraceMeshData::findNode()` is a binary search, which relies on `buildRaytraceMeshData()` leaving the nodes sorted by model key and node index.

## 2026-10-21

### New

- **Raytrace Render** → **Film** chooses how a pixel is built. **Pixel sampling** places each sample in its pixel either uniformly or by the **Pixel filter**: box, triangle, Gaussian, Mitchell-Netravali or Lanczos, each with its radius and parameters. A pixel becomes the weighted average of the light around it, which is pbrt-v4's filter importance sampling, so each sample still belongs to one pixel.
- **Film** → **Compensated mean** keeps weighted float sums with Neumaier compensation, weighing a sample by the sign of a filter with negative lobes. Against the running mean, which sees the same samples, the Cornell box's accumulation differs by 5e-6 relative RMS at 16384 samples per pixel and 1e-4 at 262144, and the difference grows in proportion to the sample count.
- **Film** → **Clamp samples** scales a sample whose brightest channel exceeds a limit down to it. It is off by default and removes fireflies at the cost of their energy.
- **Film** → **Outlier reweighted** resolves each pixel from a cascade of buffers by brightness and weighs rare bright samples down, after Zirr, Hanika and Dachsbacher 2018. **Outlier kappa** sets how rare, 4 by default. On a glass caustic made of fireflies at 256 samples it lowers the relative MSE from 0.069 to 0.0045 and keeps 96 percent of the energy, where a clamp of 10 reaches 0.0055 and keeps 90 percent. It costs 21 percent more GPU time, and on scenes without fireflies it lowers no error.
- **Raytrace Render** → **Analysis view** → **Standard error** shows the standard error of each pixel's mean over the mean, from the running variance of the samples' luminance. It needs no reference.
- `bin/film_test` checks the filters, their sampling tables, the weighted mean, the compensated sum, Welford's variance, the clamp and the outlier reweighting on the CPU. **--selfcheck** plants a sampler 2 percent off and a sum without compensation and fails unless both are caught.
- **Windows** → **GPU self-tests** gains three tests: the film's shaders against `src/film.cpp`, kernel 09's accounting over synthetic frames (a dropped sample, a constant fed for 65536 frames, signed weights, the clamp), and the outlier kernel against its mirror.
- `tools/verify/filter_check.py` compares box filtering through the table with the unfiltered jitter and renders a checkerboard to the horizon under three filters. `film_check.py` measures the compensated film's precision and the outlier film against a clamp. `furnace_check.py` renders every pixel filter, where a mirror or glass sphere must stay exactly 1 in every pixel. `run_all.py` runs all three.
- `assets/scenes/caustic_glass.gltf` is a glass sphere on a floor under a small lamp, a caustic made of fireflies.
- **File** → **Export Render...** writes the render as the file's extension says: `.exr` with the picture and every output the render kept as named channels (pbrt's `GBufferFilm` names, and pbrt's metadata in the header), `.pfm`, `.hdr`, or `.png`, the display image as the window shows it. **Raytrace Render** → **Export** sets **Half floats in EXR** and **Save when finished**, which writes the render to a chosen path when it reaches its sample target. The scene file saves both.
- `./bin/vulkan_guide --render <scene> --spp N --out <file>` renders a scene headless, writes the result and quits, with exit code 0, 2 (refused) or 3 (timeout). `--seed`, `--size`, `--timeout`, `--aov`, `--capture-through-specular`, `--light-groups`, `--half`, `--debug-view`, `--ao-distance` and `--relight` set the rest, and `--help` lists them.
- **Raytrace Render** → **Outputs** keeps auxiliary outputs beside the picture: position, depth, normal, shading normal, uv, albedo, ids, variance and the sample count. Each shows in its own window under **Windows** and exports as channels. Albedo is the BSDF's directional albedo from pbrt's 16 fixed samples, and **Capture through specular** records the first surface that is not a mirror or glass. A render that keeps none runs as before and allocates nothing for them: the four older scenes, a scene with a fuzzy metal, glass and `pbr` under each of four samplers, and Sponza matched the build before byte for byte.
- **Light groups** keeps the light of each group apart. A light's group is in the **Scene** panel, an emissive material's in the **Materials** window, and the background's under **Outputs**. Each group exports as `LightGroup.<name>.R`, and **Relight** shows the picture rebuilt from the groups with a scale and a tint for each, without new samples. The gains are saved with the camera. Four groups sum to the picture within 1.6e-6 relative.
- **Raytrace Render** → **Debug view** → **ambient occlusion** shows the fraction of cosine-distributed rays from the first hit that travel the **AO distance** unoccluded, after pbrt's `AOIntegrator`. Inside a sphere of radius 2 it gives 0.9375, 0.7500 and 0.4375 for lengths 1, 2 and 3, the values of the closed form.
- **Set Environment Map...** reads OpenEXR files as well as Radiance `.hdr`, and a multi-channel EXR loads as a reference.
- `bin/aov_test` checks the auxiliary outputs on the CPU, and **--selfcheck** plants an albedo over the wrong weight and a variance over `n` and fails unless both are caught. `bin/exr_dump` lists the channels and attributes of an EXR file and writes each channel as `.npy`, and `bin/image_convert` converts between EXR, PFM and HDR.
- **Windows** → **GPU self-tests** gains three tests: kernel 09's fold of the outputs against `src/aov.cpp`, the 16-sample albedo of `crt_scatter.glsl` against `bsdfAlbedoEstimate()` in `src/bsdf.cpp` (0 of 6144 cases past 0.002), and the output windows' shader against their definitions.
- `tools/verify/aov_check.py` renders a quad, a glass sphere, a multi-light scene, the ambient occlusion sphere and the file formats through the batch mode, and compares them with what they must give. `run_all.py` runs it and `aov_test`.
- **File** → **Import pbrt Scene...** imports a pbrt-v4 scene file (`.pbrt` or `.pbrt.gz`) as a model: its meshes and PLY files, materials, image textures, normal maps, lights, spheres, camera and environment map. All 98 scene files in `assets/models/pbrt-v4-scenes` import. What the renderer lacks is substituted: coated diffuse is `pbr`, a disk is triangles with its area kept, a blackbody is its RGB.
- **Windows** → **Import Report** lists every substitution and every parameter the import did not read, once each, with how often it happened, where in the file it first did, and the plan in `docs/plans/` that adds what is missing. **Use pbrt settings** renders from the file's camera with its resolution, samples, sampler, pixel filter, ray depth and sample clamp.
- `./bin/vulkan_guide --render <file.pbrt>` renders a pbrt file in a new scene with the file's own settings and prints its import report.
- `tools/verify/pbrt_compare.py` renders a pbrt file with pbrt and with this renderer, twice each, and reports each channel's mean ratio and the relative MSE beside both images' own noise. The Cornell box written as a `.pbrt` file agrees with pbrt to within noise, and within 0.3 percent in each channel's mean.
- `bin/pbrt_test` checks the import on the CPU: pbrt-v4's tokenizer cases, every transform directive, `Import` and instancing, cameras against pbrt's projection, and the mapping of settings, materials, shapes and lights. `--scan <dir>` imports every scene file in a folder and `--report <file>` prints one file's import report.
- `bin/spectrum_test` checks the spectra behind the import's colors: pbrt-v4's CIE tables and named spectra, its normalizations, and ten spectra against pbrt's own renders, which agree within 3e-3.

### Changed

- A new scene renders with the Gaussian filter (radius 1.5, sigma 0.5) and the compensated film. Its converged image is slightly softer than before. A scene saved earlier opens with box pixels and the running mean and renders bit-identical to before.
- Scene files are payload version 15, which adds `"pixelFilter"`, `"clampSamples"`, `"clampValue"` and `"outlierKappa"` to `"render"`. A file without `"pixelFilter"` reads a box.
- Kernel 00's existing variant is named **Thin lens (box pixels)**, and kernel 09's **Running mean** ignores the sign of a filter's negative lobes. A render that needs the signs or the clamp runs the compensated mean instead, and the **Film** section says so.
- **Analysis view** → **Dropped samples** compares the samples taken with the samples accepted under the compensated film, and the render's target under the running mean.
- Scene files are payload version 16: `"aov"` and `"lightGroups"` in `"render"`, `"lightGroupNames"`, `"environmentGroup"` and `"export"` at the top, `"lightGroup"` on a light, a material and a material override, and `"relight"` on a camera. An older file opens with no outputs and one light group.
- The device requires `VK_EXT_shader_atomic_float` (buffer float add), which kernels 03, 04 and 08 use to add to the light groups' sums.
- `VKGUIDE_VERIFY`, `VKGUIDE_VERIFY_SEED`, `_SIZE` and `_TIMEOUT` are gone, replaced by `--render`, `--seed`, `--size` and `--timeout`. `tools/verify/vkscene.py` runs the batch mode, and its log lines start with `render:` instead of `verify:`.
- The PNG export is the display image with today's gamma 2 curve. Its bytes change when the sRGB display curve lands.
- Scene files are payload version 17. A model may be a `.pbrt` file, re-imported when the scene opens. A shape the model brought records its `"model"`, and `"environmentModel"` names the model whose environment the scene shows. An older file opens as before.
- A `lambertian` material shows a base color texture and a normal map when an imported model's material has them; pbrt's textured `diffuse` is one. Every other material renders as before: the ten scenes in `assets/scenes` match the previous build byte for byte.
- The **Materials** window calls a model's material "material N of <model>" rather than "glTF material", since a model may be a pbrt file.

### Fixed

- Fixed a sample dropped as non-finite being drawn again every frame at one sample per frame, which left its pixel short of samples. The compensated film counts every sample taken apart from the ones it accepts, so a pixel that drops one carries on. The running mean keeps the old behavior, so a scene that selects it renders bit-identical.
- Fixed the viewport slowing to over 700 ms a frame on scenes of tens of thousands of objects, such as pbrt's `sanmiguel-courtyard` with 69,408, before any render had started. Every frame rebuilt and re-sorted the raster draw list, compared every object against the last TLAS build, and laid out a Scene panel row per object. Each now happens only when the scene changes, and the panel builds only the rows in view: the scene draws in about 4 ms a frame.

### Internal

- The film is two kernel stages' worth of variants: kernel 00 `filter_importance` (`0002_generate_camera_rays_filtered.comp`) beside `thin_lens`, and kernel 09 `compensated_mean` (`0902_update_film_compensated.comp`) and `outlier_reweighted` (`0903_update_film_outliers.comp`) beside `running_mean`. Kernel 00's body is `include/crt_camera.glsl` and the compensated films' is `include/crt_film_update.glsl`; with the old variants selected, kernel 00's instructions are the same as before.
- `src/film.h/.cpp` is the static library `rt_film`, the line-for-line mirror of `shaders/rt/include/crt_filter.glsl` and `crt_film.glsl`.
- A sample's filter weight is the `w` of its radiance slot, +1 or -1, and kernel 09 reads it. Binding 29, `PixelState`, is not allocated.
- Binding 9 is `CrtBinding::SamplesTaken` (the `r32ui` image that was `sampleCount`). Binding 30 is `FilterTable`, binding 31 `AccumulationError` (`CrtFilmPixel`, 48 bytes a pixel) and binding 62 `FilmCascade` (96 bytes a pixel, only for the outlier film). The film record is a one-pixel stub under the running mean, and a film record past 1 GiB falls back to it.
- `CrtParams` stays 208 bytes: `filmClamp` and `outlierKappa` take two padding words.
- `KernelVariant` gains `keepsFilmRecord`, `keepsFilmCascade` and `passes`. A stage with `passes` above 1 is dispatched that many times per frame with a barrier between, and `params.bounce` is the pass. `KernelSettings` gains `PixelFilter` and `Film`.
- `analysis_view.comp` reads the film record through binding 5 and a `hasFilmRecord` push constant.
- `src/aov.h/.cpp` is the static library `rt_aov`, the line-for-line mirror of `shaders/rt/include/crt_aov.glsl` (the layers of `AovImages`, one pixel's accumulation and its resolve), with `src/render_export.h/.cpp`, which builds an export's channels.
- Bindings 29 `PixelState`, 32 `AovImages` and 45 `ChannelSums`, each a stub while its feature is off. A kernel with `KernelVariant::usesOutputs` is built with the specialization constants `CRT_AOV` (id 1) and `CRT_LIGHT_GROUPS` (id 2), and the pipeline cache key carries them. A kernel that declares the three bindings must be given them wherever it is built: the film self-test builds kernel 09 from raw bindings and binds stubs.
- `CrtParams` is 224 bytes. `GpuLight::group` takes a padding word, and the high 8 bits of `ShadowRay::mediumAndGroup` hold the light group.
- `src/vk_readback.h` copies GPU images and buffers to the host and invalidates the staging memory, for every export. `VulkanEngine::createImageArray()` makes a 2D array image.
- `sampleDimension()` in `crt_samples.glsl` answers from pbrt's 16 albedo numbers while `g_albedoSample` is set, and the fuzzy metal's direction in `crt_scatter.glsl` reads the same table. Both are behind `CRT_AOV`, and `src/bsdf.cpp` mirrors the metal case.
- `src/command_line.h/.cpp` parses the command line with no Vulkan. `VulkanEngine::beginBatch()` and `stepBatch()` replace `beginVerify()` and `stepVerify()`.
- `src/pbrt_parser.h/.cpp` (pbrt's scanner and directive switch, with the graphics state), `src/ply_mesh.h/.cpp` (rply, vendored in `../CPPLibraries/rply` from pbrt-v4) and `src/pbrt_import.h/.cpp` (the mapping and the report) are the static library `rt_pbrt`. They produce an `ImportedModel` (`src/imported_model.h`), which `loadImportedModel()` in `src/vk_loader.cpp` turns into a `LoadedModel` like a glTF file's. The design and as-built record is `docs/plans/completed/pbrt-scene-import.md`.
- `src/spectrum.h/.cpp` is the static library `rt_spectrum`: pbrt-v4's CIE tables and 88 named spectra in `src/spectrum_tables.cpp`, generated by `tools/spectrum_tables/extract_pbrt_spectra.py`, and their projection to linear sRGB. `tools/spectrum_tables/pbrt_reference.py` renders the reference values `bin/spectrum_test` reads from `tests/spectrum_pbrt_reference.inc`. `docs/plans/completed/pbrt-spectral-rendering.md` extends the library.
- The material model is the static library `rt_material`, and its GPU records (`CrtMaterialHeader`, `CrtClassicMaterial`, `CRT_MATERIAL_FLAG_*`) moved from `rt_gpu_types.h` to the Vulkan-free `src/rt_material_record.h`.
- Names that were glTF-only and now cover any imported model: `LoadedGLTF` is `LoadedModel`, `GltfMaterialOrigin` is `ModelMaterialOrigin`, `MeshMaterialMode::Gltf` is `MeshMaterialMode::Model`, `findGltfMaterial()` is `findModelMaterial()`, `refreshGltfMaterials()` is `refreshModelMaterials()`, `GltfMaterialIds` is `ModelMaterialIds`, `removeGltf()` is `removeModel()`, and `SceneMaterialRef::gltfIndex` is `modelMaterial`. The scene file's keys did not change.
- `VulkanEngine::importModel()` picks the importer by the file's name, and `addModel()` is what every import shares. `modelMaterialDef()` gives a model's material as its file defines it. `installEnvironmentMap()` takes float pixels, so a model's environment loads without a file. `SceneShape::modelKey` ties a shape to its model, and `MeshAsset::cpuNormalsMissing` makes the path tracer shade a mesh without vertex normals flat.
- Every plan for a feature pbrt scenes use has an "Importing pbrt scenes" section saying what to change in `src/pbrt_import.cpp` when it lands.
- `MeshDrawList` (`src/raster_draw_list.cpp`, compiled `-O2` in every configuration) holds the raster pass's draw list of mesh objects, rebuilt when `RaytraceSceneEditor::contentRevision()` or `m_sceneRevision` moves and culled each frame by its world-space boxes against the frustum's planes. `contentRevision()` moves with every edit but a camera's: camera edits call `markCameraChanged()`, and `RaytraceRenderer::ensureSceneAccel()` compares the scene by value only after the content revision moves.

## 2026-10-20

### New

- **Raytrace Render** → **Analysis view** shows measurements of the film in place of the picture, switchable at any time and on a finished render: **Error against reference**, **Difference from reference** (red brighter, blue darker), **Noise (convergence)**, which estimates each pixel's remaining error from how much it changed since the film held half the samples and needs no reference, **Energy above 1** and **Dropped samples**. **Debug view** is unchanged and still replaces shading inside the path tracer.
- `bin/bsdf_test` checks the six original materials on the CPU through `src/bsdf.cpp`, the line-for-line mirror of `crt_scatter.glsl`. For 130 configurations of material, parameters and incident angle it runs a chi-square test of one million sampled directions against the density, compares the weights with `f cos` cell by cell, compares the directional albedo from the sampler with the one from quadrature, checks reciprocity, and runs every material at corner samples. **--selfcheck** plants three errors (a density 5% high, an evaluation 5% high, a sampling roughness 10% off) and fails unless the tests catch them.
- **Windows** → **GPU self-tests** runs `shaders/test/bsdf_parity.comp` on 38912 random inputs, with the sample record off and on, and compares the shader with the C++ mirror. `VKGUIDE_SELFTEST=1` runs the tests at start-up and exits with 0 or 1.
- **Raytrace Render** → **Compare to reference** has **Save...** and **Load...** (`.exr` or `.pfm`), MAPE and the mean difference beside RMSE and relative MSE, and a log-log plot of relative MSE against samples per pixel. The plot keeps up to eight earlier renders measured against the same reference, labeled by sampler and direct-lighting strategy, and reports each curve's slope. An unbiased estimator falls with slope -1.
- `VKGUIDE_VERIFY=<scene>:<samples>:<output.exr|pfm>` opens a scene, renders it to that many samples with a fixed seed, writes the accumulation and quits. `VKGUIDE_VERIFY_SEED`, `_SIZE` and `_TIMEOUT` adjust it.
- `bin/image_diff` compares two EXR or PFM files with RMSE, relative MSE, MAPE and the mean difference.
- `tools/verify/` holds the scene-level checks. `furnace_check.py` renders a sphere of each material in a uniform white environment under every direct-lighting strategy, sampler, traversal and intersection flag. `cornell_check.py` compares the Cornell box with an independent radiosity solve (`radiosity.py`) and with pbrt-v4. `run_all.py` runs every check.
- `assets/scenes/white_furnace.gltf`, `cornell_reference.gltf` and `cornell_empty.gltf`, and `assets/environment_maps/uniform_white.hdr` (radiance 1 in every direction), written by `tools/verify/make_scenes.py`. The repository ignores `.hdr` files, so the script writes the map again where it is missing.

### Fixed

- Fixed fully metallic `pbr` materials dropping samples as non-finite. With metallic 1 the diffuse lobe has no weight, and a lobe draw that reached it divided 0 by 0. A white metallic sphere dropped one sample 1403 times in a 64 x 64 render of 4096 samples at one sample per frame. The material now always takes the specular lobe, and the random stream is unchanged.
- Fixed `randomFloat()` returning exactly 1.0 for 128 of its 2^32 PCG words, which its documentation excludes. It now returns at most 0.99999994 and every other draw is as it was.
- Fixed the comment at the top of `crt_scatter.glsl`, which said the weight a `pbr` sample returns is exactly `evalSurface() / pdfSurface()`. It is the chosen lobe's weight, equal to the ratio in expectation.

### Changed

- The error pass behind **Compare to reference** writes four sums per workgroup instead of two.

### Internal

- `tests/test_stats.h` holds the statistics the CPU tests share: Pearson's chi-square test with pooled cells and its p-value, the Sidak correction, a grid over directions with adaptive quadrature of a density over each cell, and a running mean with its standard error.
- `src/image_io.h/.cpp` is the static library `rt_image`: EXR through tinyexr (`../CPPLibraries/tinyexr/`, version 1.0.13, with the system zlib) and PFM, and `compareImages()`.
- `src/gpu_selftest.h/.cpp` lists the GPU self-tests in `gpuSelfTests()`. A test is a function that runs a shader from `shaders/test/` and compares it with a C++ mirror.
- `shaders/analysis_view.comp` runs after the tonemap when an analysis view is selected (`GpuRenderer::analysisView`, `recordAnalysis()`). The film keeps a copy of itself, `m_noiseBase`, taken at the start of the frame after its sample count has doubled, which the noise view compares it with.
- `GpuRenderer` keeps a convergence history per render (`convergence()`), can read the accumulation or the reference back to the host, and can replace the reference with a loaded image.

## 2026-10-19

### New

- **Windows** → **Materials** shows the material library: a **User** folder and one folder per imported model. Selecting a material shows an editor built from its type's parameters. **+ New**, **Duplicate**, **Delete** (refused while an object uses the material) and **Remove unused** manage the list.
- An edited parameter of a glTF material is marked, with a **revert** button beside it, and **Revert all** restores the whole material as the file defines it.
- **Raytrace Render** shows **Surface hits per bounce (basic)**, and a complex line beside it once the scene has a complex material.
- **Raytrace Render** → **Intersection precision** has **Error-bounded ray offsets** and **Watertight triangles**. Both are on by default and saved with the scene.
- `assets/scenes/thin_glass.gltf` is a pane of glass modeled as two quads 1e-4 apart, in front of a tiled wall. With error-bounded offsets the wall looks the same as without the pane, apart from the Fresnel loss.
- `bin/bvh_bench` runs the robust-intersection checks first: nextafter parity, a crack test, error bounds against a double-precision reference, and spawn offsets that must not re-hit their surface. **--robust N** sets the rays per check.
- Kernel 01 has seven samplers: Independent, Stratified, Halton, Sobol, Padded Sobol, ZSobol and PMJ02BN. **Raytrace Render** → **Sampler** chooses one, and **Randomization** chooses how Sobol and Halton points are scrambled. At equal samples the low-discrepancy samplers bring the relative MSE to between 0.6 and 0.8 of PCG's on the Cornell box, and between 0.2 and 0.45 on the Veach scene.
- `bin/sampler_test` checks the samplers on the CPU: determinism, (0, 2)-nets, strata, discrepancy, and ZSobol's blue-noise error.
- `assets/scenes/all_materials.gltf` is the Cornell box with one surface of each material type: Lambertian, Phong, fuzzy metal, glass, and two `pbr` finishes. It names no sampler, so it renders with **PCG per bounce**, the regression scene for bit-identical renders.
- The `VKGUIDE_WINDOW` environment variable opens the window without taking focus: `background` opens it behind the active app, and `minimized` minimizes it to the Dock, where it keeps rendering. Unset, the window opens in front and focused, as before.

### Changed

- Materials are a named library. A shape or a mesh object's override picks one from a **material** combo in the **Scene** panel, with **Edit** to open it in the **Materials** window and **New from this** to give the object its own copy. Editing a material changes every object that uses it.
- A material's parameters are edited in the **Materials** window. The **Scene** panel no longer shows them.
- A new shape gets its own material, named after the shape, and **Duplicate** copies the material along with the object, so objects stay independent until you pick a shared material.
- An imported model's glTF materials are library entries you can edit in place; a mesh object shaded with its glTF materials lists them with an **Edit** button each. The edits change the raytracer's render only, not the raster viewport.
- A material on a shape leaves out its textures, which shapes cannot carry yet: a glTF material on a sphere shows its factors.
- Removing a model removes its materials. One a shape or an override still uses becomes a user material without its textures, and the status line names it.
- Scene files are payload version 12, which saves the user materials in full, each glTF material only as the parameters you changed, and every object's material by reference. An older file opens with one user material per object, named after the object. A mesh object in glTF mode drops the override material older files stored for it.
- A ray leaving a surface starts just past the rounding error of its hit point, not at the hit point with the first 0.001 of its length skipped. A shadow ray to an area light ends just past the light point's error instead of at 0.9999 of the distance. Renders differ from before by float rounding in the noise, and the converged image changes only where the epsilon was wrong. **Error-bounded ray offsets** off restores the old behavior bit for bit.
- Triangles are tested with the watertight algorithm, and bounding box tests are widened so rounding cannot cull a grazing ray. It costs about 8 to 12 percent GPU time on Sponza, and the nodes and primitives tested per ray move by under 1.5 percent. **Watertight triangles** off restores Möller–Trumbore and the plain box tests bit for bit.
- Scene files are payload version 13, which adds `"errorBoundedOffsets"` and `"watertightTriangles"` to `"render"`. An older file opens with both on.
- A new scene renders with the ZSobol sampler, so its noise differs from before and the image converges faster; the converged image is the same. ZSobol costs about 40 percent more GPU time per frame on the all-shapes Cornell box. A scene file that names no sampler opens with **PCG per bounce** and renders bit-identical to before.
- Stratified and PMJ02BN need a known number of samples. With **Unlimited samples** on, a render runs the independent sampler in their place, and the **Sampler** section says so.
- Scene files are payload version 14, which adds `"samplerRandomization"` to `"render"`.

### Fixed

- Fixed orientations changing on every save. Opening a scene re-normalized each saved orientation, which moved it to a neighboring float and back on alternate saves, so a scene rendered slightly differently after each save and reopen. An orientation already unit length to float precision now loads exactly as saved. A scene whose stored orientations are not exact, such as `veach_mis.gltf`, renders differently by float rounding the first time it opens after this fix.
- Fixed importing a glTF file that has no materials: a primitive without a material read past the end of the file's empty material list, and the descriptor pool and uniform buffer were sized for zero materials. Such a file now gets glTF's default material, white with metallic and roughness 1, as its only material, and it appears in the **Materials** window as `default`.
- Fixed geometry thinner than 0.001 losing its second surface. A ray skipped every hit in the first 0.001 of its length, so a 1e-4 glass pane acted as a solid half-space and shrank the image behind it.
- Fixed surfaces far from the origin shadowing themselves. At 10 km the float spacing equals the 0.001 epsilon, and a Cornell box moved there rendered 3.2 percent darker and speckled.
- Fixed rays slipping through the edge or vertex that two triangles share. With Möller–Trumbore about 1 in 9 rays aimed at an edge of a tessellated sphere went through it, and 578 of 4096 rays through the vertices of a flat grid did.

### Internal

- `src/rt_material.h/.cpp` holds the material model: `MaterialDef` with its parameters in a `ParamSet` by name, and `MATERIAL_TRAITS`, the only per-type knowledge outside the shaders. A new type is a traits row, its parameters, its pack function, and its record reader in `shaders/rt/include/crt_material.glsl`.
- Binding 7 is a 16-byte `GpuMaterial` header (type, flags, parameter offset and count), and binding 26, `MaterialParams`, holds each material's parameter record. The six existing types pack their former 64-byte record unchanged, as `CrtClassicMaterial`.
- Kernel 02 classifies a hit from its material's flags, and the traversal's alpha test reads a material's parameters only when it has `CRT_MATERIAL_FLAG_CUTOUT`.
- Binding 23, `RaySamples`, holds a 48-byte record per path that kernel 01 writes and kernels 00 and 06 read, allocated only while a sampler that writes one is selected (192 MB at the 4M-path cap). Binding 46, `SamplerTables`, holds pbrt-v4's Sobol matrices, PMJ02BN sets and blue-noise textures from `assets/sampler/`, which `tools/sampler_tables/convert_pbrt_tables.py` makes.
- A record-writing sampler runs a camera pass over the whole pool before kernel 00, with `CRT_CAMERA_PASS` as its bounce (`KernelVariant::cameraPass`).
- Kernels 00 and 06 draw their random numbers through `sampleDimension()` in `shaders/rt/include/crt_samples.glsl` and are built with the specialization constant `CRT_SAMPLE_RECORD` (`ComputePassBuilder::addSpecialization()`, `KernelVariant::readsRecord`). Code that only runs with a record goes behind it, not behind a run-time flag, which changed `pcg_hash`'s rounding.
- `CrtParams` is 208 bytes: it carries the sampler's packed parameters and three reciprocals, since no sampler shader divides floats.
- `src/sampler.h/.cpp` is the static library `rt_sampler`, the line-for-line mirror of `shaders/rt/include/crt_sampler.glsl`. `tools/sampler_tables/generate_halton_tables.py` writes the Halton tables both sides read.
- Kernel 02 sends hits on complex materials to queue 6, `CRT_QUEUE_SURFACE_COMPLEX`. Every kernel 06 variant includes `include/crt_material_class.glsl`, sets `KernelVariant::perMaterialClass`, and is built and dispatched once per class over `KernelDomain::SurfaceQueues`. The path pool takes 180 bytes per path.
- `LoadedGLTF::materialsByIndex` and `materialNames` keep a file's materials in its own order, which a library entry's origin names them by.
- `BvhTriangle` holds three vertices, `v0`, `v1` and `v2`, instead of a vertex and two edges, so triangles that share a vertex hold identical copies of it. The surface index moves from `e1.w` to `v1.w`. The struct stays 48 bytes, and Möller–Trumbore takes its edges as `v1 - v0` and `v2 - v0`, the same floats as before.
- `src/robust.h` holds `gammaBound()`, `nextFloatUp()`, `nextFloatDown()`, `offsetRayOrigin()` and `differenceOfProducts()`. The robust section of `crt_common.glsl` mirrors it, and now holds the shared `differenceOfProducts()` and `sumOfProducts()` that `crt_light.glsl` had.
- `CrtParams.flags` gains `CRT_FLAG_ERROR_BOUNDS` (16) and `CRT_FLAG_WATERTIGHT` (32). Every trace takes its `tMin` from `traceTMin()`, which is 1e-30 under the first flag and `CRT_T_MIN` otherwise.
- `surfaceInteraction()` fills `p` and `pError` under `CRT_FLAG_ERROR_BOUNDS`: a triangle's point from its barycentrics, a shape's from the object-space ray reprojected onto the primitive (`shapeReproject()`). `toWorldPoint()` adds the round trip through the `toObject` and `toWorld` rows, since a rotated 40:1 scale puts the two a few thousand ulps from inverting each other.
- `instanceRay()` computes the watertight test's axis permutation and shear once per object-space ray. A CWBVH node box test pads each axis's near and far distance by its own gamma(4) bound, because Ize's `1 + 2 gamma(3)` factor assumes a relative error and the node's `offset + q * scaled` cancels.
- `sampleLight()` and `sampleDeltaLight()` return the sampled point's normal. They return no point: exposing `s.point` changed how the compiler fused the sampling arithmetic and broke the bit-identical comparison, so kernel 06 rebuilds the point from the direction and distance.
- `RenderSettings` gains `errorBoundedOffsets` and `watertightTriangles`. They are part of the render key, so a change restarts the render, and they stay out of `AccelSettings`, so a change does not rebuild any BLAS.

## 2026-10-18

### New

- **Raytrace Render** → **Debug view** has **shading vs geometric normal**: red where the normal a material shades with and the surface's own normal, both facing the ray, are more than 90 degrees apart, and grey with their cosine elsewhere.

### Internal

- The hit record is 24 bytes: `t`, `instance`, `barycentrics`, `primitive` and `traversalCost`. Kernels 04 and 06 rebuild the point, normals, uv, derivatives, material and light record with `surfaceInteraction()` in `shaders/rt/include/crt_interaction.glsl`. The design and as-built record are `docs/plans/completed/pbrt-thin-hit-record.md`.
- Normal maps are applied in kernel 06, by `materialShadingNormal()` in `crt_scatter.glsl`, instead of in kernel 02. Kernel 02 finds a triangle's material through its surface index in `BvhTriangle.e1.w`.
- `ShadowRay` is 48 bytes and carries its origin, so kernel 08 no longer reads `hits[]`. `GpuInstance` is 128 bytes, with the world-to-object rows renamed `toObject[3]` and the object-to-world rows added as `toWorld[3]`. The path pool takes 176 bytes per path, down from 184.
- A kernel that forms a hit point from `t` writes `fma(vec3(t), direction, origin)`. The shader compiler fused that expression in kernel 02 but not in kernel 04, and the two disagreed by an ulp.
- Analytic shapes have uv coordinates and derivatives (`shapeUv()`, `shapeDerivatives()` in `crt_shape.glsl` and `shape.cpp`). Seen from outside, u runs right and v runs down, so an image will appear upright as it does on a glTF mesh. No material samples a texture on a shape yet.
- `bin/bvh_bench` first checks the shapes' uvs and derivatives against finite differences, and their continuity across the sphere's seam and the box's edges.
- The plans for features from the pbrt books are renamed `docs/plans/pbrt-*.md`, and their roadmap is `docs/plans/pbrt-roadmap.md`. The laser optics simulator is planned in `docs/plans/physoptics-*.md`, with `docs/plans/physoptics-laser-optics.md` as its overview and the build order for every plan.
- The descriptions of the stub kernels 05 and 07 in **Windows** → **Raytracer Shaders** name the plan by its new path, `docs/plans/pbrt-participating-media.md`.

## 2026-09-29

### New

- **Windows** → **Raytracer Shaders** has an **Integrator** combo and a **Traversal** combo above the kernel rows, and **Raytrace Render** has the **Integrator** combo at the top of its settings. Path tracing is the only integrator so far.

### Changed

- The traversal (BVH binary, BVH compressed 8-wide, or linear scan) is its own choice rather than kernel 02's variant. Kernels 02 and 08 each show one variant, built for the selected traversal, and the render guard's message names the traversal instead of kernel 02.
- Scene files are payload version 11, which adds `"integrator"` and `"traversal"`. An older file opens with its kernel 02 choice as its traversal and renders exactly as before.

### Internal

- `GpuPathTracer` is split into `GpuRenderer` (`src/rt_gpu.h`: what every integrator shares), `WavefrontPool` (`src/rt_wavefront.h`: the per-path buffers), and `PathSchedule` (`src/rt_path_schedule.h`), behind the `IntegratorSchedule` interface in `src/rt_schedule.h`. The design is `docs/plans/completed/integrator-layer.md`.
- `KernelSlot` is a stage library shared by every integrator, and a variant of a stage must work in every integrator that runs it. `docs/plans/pbrt-roadmap.md` reserves stages 10 to 25 for the plans that add them.
- A kernel that traces includes `shaders/rt/include/crt_trace.glsl` and is compiled once per traversal, to `<name>.comp.<id>.spv`. `0201_intersect_closest.comp` and `0801_trace_shadow_rays.comp` replace the six per-traversal files, and a new traversal is a strategy file, a `crt_trace.glsl` branch, and a `TraversalVariant` row. `KernelVariant::followsSlot` is gone.
- Each owner publishes its resources into `CrtResourceTable`, and descriptor sets are written from it. A kernel that declares a binding nobody published aborts at its first dispatch instead of reading a stale descriptor.
- Pipelines are built on first use, keyed by variant and traversal; the default selection's are built at startup. `CrtParams` and the other C++ mirrors of `crt_common.glsl` moved to `src/rt_gpu_types.h`.

## 2026-09-26

### New

- **Raytrace Render** reports the samples kernel 09 dropped as non-finite, in orange, with their share of the samples taken. A dropped sample biases its pixel, so any count means a NaN or an infinity got into a path.

### Changed

- The **Environment Map** window shows a map brighter than 65504 at its stored scale, darker by the factor the load message gives.

### Fixed

- Fixed environment maps brighter than 65504 rendering their brightest texels black and dropping every sample that light sampling sent towards them, 35% of all samples under MIS in `moon_lab_4k.hdr`. The half-float GPU copy stored those texels as infinity; such a map is now stored divided by a power of two, 8 for `moon_lab_4k.hdr`.

### Internal

- `environmentPeak()`, `environmentStorageScale()` and `packEnvironmentTexels()` in `src/light_sampling.h` build the environment map's GPU copy. `VulkanEngine::m_environmentMapScale` reaches the shaders multiplied into the environment intensity, and `bin/light_test` checks the scale.
- `traversalStats[CRT_STAT_DROPPED_SAMPLES]` holds kernel 09's dropped-sample count. Kernel 09 binds `TraversalStats`, and the frame's counters are copied back after kernel 09 instead of before it.

## 2026-09-25

### New

- **Raytracer Shaders** window, under **Windows**. Pick which shader runs in each kernel of the wavefront path tracer. The choice saves with the scene.
- Kernel 02, intersect closest, has three shaders: BVH (binary), BVH (compressed 8-wide), and a linear scan with no acceleration structure. All three produce identical images from the same seed.
- Raytracing shaders live in `shaders/rt/`, named `NNVV_<name>.comp` for kernel `NN`, variant `VV`. The numbering follows figure 15.2 of *Physically Based Rendering*, 4th edition.
- Kernels 05 and 07 (participating media) exist as stubs and show greyed out in the shader window. The plan is in `docs/plans/pbrt-participating-media.md`.
- `docs/raytracing-overview.md` describes the tracer end to end.
- A sixth material type, **pbr**, is glTF 2.0's metallic-roughness model: a GGX specular lobe over a diffuse base, with metallic, roughness and an emission that does not end the path. Shapes and mesh overrides can pick it in the **Scene** panel.
- The path tracer applies glTF normal maps, metallic-roughness textures and emissive textures. Tangents come from the file's `TANGENT` attribute, or are generated from the uvs when it has none.
- glTF alpha cutouts (`alphaMode` `MASK`) render in the path tracer. Sponza's foliage and chains are cut out instead of drawn as solid quads.
- A glTF material with an `emissiveFactor` glows and still reflects light. `KHR_materials_emissive_strength` scales it.
- `KHR_lights_punctual` point, spot and directional lights load with their model and light the scene. The import divides candela and lux by 683 into the tracer's radiometric units, and removing the model removes its lights.
- Next-event estimation in the path tracer. At each hit on a surface that is not glass or a perfect mirror, kernel 06 samples one light and sends a shadow ray towards it, and kernel 08 traces the shadow rays with kernel 02's traversal.
- **Raytrace Render** → **Direct lighting** → **Strategy** picks how the path tracer finds direct light: BSDF sampling, light sampling, or multiple importance sampling with the balance or the power heuristic. The four are kernel 06 shaders, converge to the same image, and save with the scene.
- The path tracer samples emissive shapes, emissive glTF triangles, point, spot and directional lights, and the environment map as lights, each picked in proportion to its power. The infinite plane, the sky gradient and a solid background colour are not sampled; BSDF samples still find them.
- The environment map is importance-sampled by luminance, with pbrt-v4's piecewise-constant tables. Under `nowhere_road_4k.hdr`, whose sun holds half its light in 0.0004% of the sphere, MIS reaches a relative MSE of 3.4e-5 at 4096 samples where BSDF sampling stays at 4.4.
- Point, spot and directional lights are scene objects under **Scene** → **Lights**, with a colour, intensity, range and cone angles, and **Edit Transform** moves them. **+ Add** → **Light** places one at the viewport camera, and the viewport draws each as a star with its cone or direction.
- The **MIS weights** debug view shows which technique found the direct light at the first hit, after its MIS weight: red for BSDF samples, green for light samples, as in Veach's figure 9.8(d).
- **Raytrace Render** → **Compare to reference** keeps a converged render and shows every later render's RMSE and relative MSE against it, frame by frame.
- `assets/scenes/veach_mis.gltf` reproduces the scene of Veach's multiple importance sampling figures: four glossy plates, four coloured spheres of equal power, and an overhead spotlight. `docs/images/veach-mis-strategies.jpg` compares the four strategies on it.

### Changed

- Shading runs as three kernels instead of one. Kernel 02 sorts each path onto an escaped, emissive or surface queue, and kernels 03, 04 and 06 drain them.
- The BVH node layout follows the selected kernel 02 shader, so the Node layout combo is gone. Picking a shader rebuilds the BLASes in the layout that shader reads.
- The Acceleration structure section appears only while a kernel 02 shader that uses one is selected.
- The render guard prices a ray by the selected traversal. A linear scan over a large model trips the frame budget where a BVH does not.
- Scene files are payload version 10. Version 8 carries the kernel selection and the BVH build settings, version 9 the **pbr** material, and version 10 the scene's lights. Older files still open. An unknown shader name falls back to the slot's default and reports a load warning.
- Kernel 01 reseeds each path's random stream per bounce. The noise pattern differs from before; the converged image does not.
- New scenes, and scene files from before version 8, use MIS with the power heuristic. The noise differs from BSDF sampling; the converged image does not.
- The **Raytracer Shaders** window shows kernel 08 greyed out. It follows the kernel 02 shader, since a shadow ray walks the same acceleration structure.
- The render guard prices a bounce as two traversals when the direct-lighting strategy has a light to send shadow rays to.
- The render panel reports the BVH work per shadow ray and the shadow rays per bounce, beside the camera rays' figures.
- glTF materials render as **pbr** instead of lambertian. A model whose material leaves `metallicFactor` at its glTF default of 1 with no metal/rough texture now renders as metal, which is what the file specifies.
- The raytracer's texture array holds up to 256 textures instead of 64, across base colour, normal, metal/rough and emissive maps. Sponza uses 69.
- Alpha-blended glTF materials (`alphaMode` `BLEND`) render opaque in the path tracer, and the import prints a line naming each one.

### Fixed

- Fixed the raytracer's texture array staying in an undefined image layout until the first model import. Any scene without models raised a validation error.
- Fixed the raytracer ignoring the textures of a glTF file whose materials have no names, such as Sponza. The loader kept one material per name, so all 25 of Sponza's materials collapsed to one texture layer. Unloading the model also leaked 68 of its 69 images.
- Fixed a crash importing a glTF texture that names no sampler or no image. The loader threw `bad_optional_access`.

### Internal

- `src/rt_kernels.h` holds the kernel registry. A new traversal strategy is one `.comp` file and one table entry.
- Each kernel declares the descriptor bindings it uses and gets a set layout with only those. Binding numbers stay global and fixed in `shaders/rt/include/crt_common.glsl`.
- `shaders/rt/include/crt_traverse.glsl` states the traversal contract. A strategy supplies `traceScene()` and `occluded()`, and nothing else.
- `GpuMaterial` is 64 bytes and `GpuTriangleAttributes` is 80 bytes; the second gains three packed vertex tangents. `MeshAsset::cpuTangents` holds the tangents on the CPU side and never reaches the raster vertex buffer.
- The top bit of `BvhTriangle::v0.w` flags an alpha-tested triangle. Mask the index with `BVH_TRIANGLE_INDEX_MASK` before using it. The CPU traversals treat every triangle as opaque.
- Binding 12 is `materialTextures` (`CrtBinding::MaterialTextures`), and kernels 02, 04 and 06 declare it.
- Kernels 03 and 04 add to a path's radiance slot instead of writing it, and a barrier now separates kernel 04 from 06. A glowing **pbr** hit sits on both the emissive and the surface queue.
- `LoadedGLTF` keys its meshes, nodes, images and materials by a unique name, generated as `material3` and so on for an unnamed object.
- `GpuInstance` is 80 bytes and carries `triangleCount`, which the linear scan reads.
- CMake compiles shaders in subdirectories, writes each `.spv` beside its source, and passes `-I shaders/` to `glslangValidator`.
- Bindings 18 to 21 are `Lights`, `ShadowRays`, `TriangleLights` and `EnvironmentSampling`.
- `PathState` is 64 bytes. It carries the MIS record, `misPdf` and `misExponent`, that kernel 06 leaves for kernels 03 and 04 to weigh emission by.
- `HitRecord`'s spare pair holds `traversalCost` and `lightIndex`, and `GpuInstance` carries `lightBase`.
- `KernelVariant` gains `isDefault`, `nextEvent`, and `followsSlot`/`followsVariant`. `reconcileKernelSelection()` sets a following slot from its leader, and a scene file's entry for one is not read.
- Kernel 06's four shaders share one body, `shaders/rt/include/crt_surface.glsl`, and kernel 08's three share `crt_shadow.glsl`.
- `src/rt_lights.h/.cpp` builds the light list inside `buildSceneAccel()`, on every scene edit.
- `src/light_sampling.h/.cpp`, static library `rt_light`, mirrors `shaders/rt/include/crt_light.glsl` line for line. Change both together and run `bin/light_test`, which checks every sampler's pdf and solid angle on the CPU.
- `bvh_bench` checks `occludedScene()`, the CPU mirror of `occluded()`, against brute force.
- `docs/plans/shadow-rays-nee.md` moved to `docs/plans/completed/`, with an as-built section.
