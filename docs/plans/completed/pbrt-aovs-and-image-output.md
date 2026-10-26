# Auxiliary outputs and image export

A render existed only on the GPU. Nothing saved it, so every comparison with pbrt, every reference image and every figure started with a screenshot of an 8-bit tone-mapped window, and the tracer produced nothing beside its radiance. pbrt's `GBufferFilm` writes position, normals, depth, uv, albedo and variance next to the image in one OpenEXR file. This plan adds image export, those auxiliary outputs (AOVs), light groups for relighting after the render, an ambient occlusion view and a command-line batch mode.

*Implemented 2026-10-21, except the spectral band images. Depends on [pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md) and [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md). Part of the [roadmap](../pbrt-roadmap.md).*

## Decisions

On 2026-10-21, when implementation started, the user answered four questions:

- Build steps 1 to 6 of the plan. The spectral band images (step 7) wait for [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md).
- Add `stb_image_write.h` to `../CPPLibraries` (the user downloaded it) and write both `.hdr` and `.png` now. A PNG is the display image as it is today, with the gamma 2 curve of `shaders/tonemap.comp`. Its bytes change when [pbrt-display-and-color.md](../pbrt-display-and-color.md) replaces that curve with sRGB.
- Replace the `VKGUIDE_VERIFY` environment variables with command-line flags, and move `tools/verify/` and the docs to them.
- Report the albedo as the 16-sample estimate of the BSDF's directional albedo for all six materials, not as the material's color.

## Image export

**File** → **Export Render...** and the **Export** section of the **Raytrace Render** panel open a save dialog, and the extension picks the format:

| Format | Content | Writer |
|---|---|---|
| `.exr` | the picture and every output the render kept, as named channels | tinyexr, in `src/image_io.cpp` |
| `.pfm` | the picture, linear float RGB | `src/image_io.cpp` |
| `.hdr` | the picture, linear RGBE | `stb_image_write.h` |
| `.png` | the display image as the window shows it, 8-bit | `stb_image_write.h` |

`src/vk_readback.h` is the one place that copies GPU memory to the host: `readbackImage()` and `readbackBuffer()` wait for the device, copy into a `GPU_TO_CPU` buffer and call `vmaInvalidateAllocation()` before the CPU reads it. That is the read side of the project's MoltenVK flush rule. The detector films of [physoptics-data-export.md](../physoptics-data-export.md) use the same two functions.

The EXR channels follow pbrt's `GBufferFilm`: `R`, `G`, `B`, `Albedo.R`, `P.X`, `N.X`, `Ns.X`, `u`, `v`, `Variance.R`, `RelativeVariance.R`. The depth is `Z`, the sample count `n`, the ids `id.instance` and `id.material`, and each light group `LightGroup.<name>.R`. The header carries pbrt's `ImageMetadata` (`renderTimeSeconds`, `samplesPerPixel`, `cameraFromWorld`, `NDCFromWorld`, `MSE` when a reference is loaded, `chromaticities`) and three strings: `vkguide:scene`, `vkguide:kernels` and `vkguide:settings`. **Half floats in EXR** stores 16-bit channels.

`src/render_export.h` builds the channel list from a `RenderOutputsData` (`src/aov.h`) with no Vulkan, and `bin/exr_dump` lists the channels and attributes of a file and writes each channel as a NumPy `.npy` file.

**Save when finished** writes the render to a chosen path when it reaches its sample target, not when you stop it. The scene file saves the path and the half-float option.

EXR files also load as environment maps (**Set Environment Map...** takes `.hdr` and `.exr`), and an EXR with extra channels loads as a reference.

## Batch mode

```sh
./bin/vulkan_guide --render cornell_reference.gltf --spp 4096 --out cornell.exr
```

The window opens behind the active app (`VKGUIDE_WINDOW` still picks its placement), the scene renders to its sample target with a fixed seed, the result is written and the program quits. The exit code is 0 when the file was written, 2 when the scene or the options were refused and 3 on a timeout. The flags are `--render`, `--out`, `--spp`, `--seed`, `--size WxH`, `--timeout`, `--aov <list|all>`, `--capture-through-specular`, `--light-groups`, `--half`, `--debug-view`, `--ao-distance` and `--relight`. `./bin/vulkan_guide --help` lists them. `src/command_line.h` parses them with no Vulkan, and `VulkanEngine::beginBatch()` applies them to the scene's own settings.

`--export-detectors` joins the flags when [physoptics-data-export.md](../physoptics-data-export.md) lands.

## Auxiliary outputs

Kernels 03, 04 and 06 record the first surface a camera path meets into `PixelState` (binding 29, 72 bytes a pool slot, scalars only so that std430 does not pad it to 80). The path's slot is cleared by kernel 00 each frame. Kernel 09 folds the slots of a pixel into the AOV image with the sample's filter weight, the sign in its radiance slot's `w`, as pbrt's `gBufferWeightSum` does.

| Output | Channels | Notes |
|---|---|---|
| Position | `P.X` `P.Y` `P.Z` | world space |
| Depth | `Z` | distance along the camera's forward axis |
| Normal | `N.X` `N.Y` `N.Z` | geometric, as authored |
| Shading normal | `Ns.X` `Ns.Y` `Ns.Z` | after the normal map, on the geometric normal's side |
| UV | `u` `v` | |
| Albedo | `Albedo.R` `.G` `.B` | the directional albedo, 16 fixed samples |
| Ids | `id.instance` `id.material` | indices into the uploaded instance and material tables, the first surface sample's, never averaged |
| Variance | `Variance.R` `RelativeVariance.R` and the other channels | the samples' own variance, Welford's, as pbrt's `rgbVariance` |
| Sample count | `n` | the film's `samplesTaken` image |

Position, depth, normals and uv average over the samples that met a surface. Albedo averages over every sample, and a miss has albedo 0. The estimators are in `src/aov.h`, the line-for-line mirror of `shaders/rt/include/crt_aov.glsl`.

The AOV image (binding 32) is a `rgba32f` 2D array. A layer holds four floats: the weights and the first sample's ids, position and depth, the normal, the shading normal, uv, albedo, the variance's mean and count, and its M2. The render allocates the layers its outputs need and nothing else, and the panel shows the size before you turn an output on. An image past 1 GiB of layers is left off with a note.

The albedo estimate runs in kernel 06. `sampleDimension()` answers from pbrt's 16 fixed numbers (`ucRho` and `uRho` of `surfscatter.cpp`) while `g_albedoSample` is set, so the estimate uses `scatterSurface()` unchanged and never touches the path's stream or its sample record. The fuzzy metal's direction reads the same table.

**Capture through specular** records the first surface that is not a mirror or glass, so a window shows what is behind it. It is off by default, and pbrt does not do it.

Each enabled output shows in its own window under **Windows**, through `shaders/aov_view.comp`: normals as `0.5 + 0.5 n`, depth as near-bright, ids as hashed colors, and the others in their natural range.

Turning an output on restarts the render. The kernels write the outputs behind the specialization constant `CRT_AOV`, so a render that keeps none runs the code it ran before.

## Ambient occlusion

`CrtDebugView::AmbientOcclusion`, after pbrt's `AOIntegrator`. At the first hit, kernel 06 sends one shadow ray in a cosine-distributed direction about the shading normal, as long as the **AO distance**, and kernel 08 adds 1 to the radiance when it is unoccluded. A miss is black and an emitter seen directly is white. A distance of 0 is a tenth of the scene's size.

## Light groups

Each punctual light, each emissive material (`MaterialDef::lightGroup`) and the background carries a group, 0 to 7, from a list of names the scene holds (`RaytraceSceneEditor::lightGroupNames()`). Group 0 is the default, so an untouched scene has one group. **Raytrace Render** → **Outputs** → **Light groups** turns the sums on and edits the names and the background's group. A light's group is in the **Scene** panel and a material's in the **Materials** window.

- Kernels 03, 04 and 08 add each contribution, times the filter sign, to its group's sums at the path's pixel with `atomicAdd()`. The device now requires `VK_EXT_shader_atomic_float` and its buffer float add. Image float atomics are missing, so the sums are a buffer (binding 45, `ChannelSums`), `3 g + 1` floats a pixel: the `g` groups' sums, then `sum(w)`, which kernel 09 adds.
- A shadow ray carries its light's group in the high 8 bits of `mediumAndGroup`, which kernel 06 sets from the light record's `group` (a former padding word of `GpuLight`).
- **Relight** shows `sum over groups of gain * sums / sum(w)` in place of the film, with a scale and a tint per group. The gains are display settings, saved with the camera like the exposure, and changing them needs no new samples. The picture is exported as the film holds it, and a PNG export shows the relit picture.

The atomics add in no fixed order, so the groups sum to the picture to float rounding, not bit for bit. At 1920 by 1080 with four groups the sums take 108 MB.

## Registry, bindings and data

- Binding 29 `PixelState`, 32 `AovImages` and 45 `ChannelSums`, each a stub while its feature is off. Kernels 00, 03, 04, 06, 08 and 09 declare the ones they use, and set `KernelVariant::usesOutputs`, which builds them with the specialization constants `CRT_AOV` (id 1) and `CRT_LIGHT_GROUPS` (id 2).
- `CrtParams` is 224 bytes: `aovOutputs`, `aovLayers`, `lightGroupCount`, `environmentGroup` and `aoDistance`.
- `PathSchedule` allocates `PixelState` only while an output accumulates (`WavefrontPool::ensure()` takes a flag).
- Scene files are payload version 16: `"aov"` and `"lightGroups"` in `"render"`, `"lightGroupNames"`, `"environmentGroup"` and `"export"` at the top, `"lightGroup"` on a light, a material, a material override and a camera's `"relight"` gains.

## Verification

- With every output, the groups and the debug view off, renders are byte for byte what the build before made: the Cornell box, the all-materials scene, the caustic scene and the sphere scene at 120 by 90 and 32 samples, a scene with a fuzzy metal, glass, `pbr` and a lamp under each of four samplers, and Sponza. `tools/verify/aov_check.py --baseline DIR` repeats it against a copy of the old `bin/` and `shaders/`.
- A render with every output and the groups on has the same R, G and B as one with none. An exported EXR loaded as the reference gives **Compare to reference** an RMSE of exactly 0, and the EXR's channels equal the PFM's bit for bit.
- A Lambertian quad facing the camera reads its albedo to 5e-7, its normal to 6e-8, its depth exactly, its position to 0.008 of a pixel, and has zero variance in a uniform light. A pixel that missed has no position, normal, albedo or id (`id.instance` is -1).
- A glass sphere's albedo is exactly 1. Capturing through specular records the quad behind it at depth 4.000 with the quad's albedo times 0.914, the share of samples that met it.
- Four light groups (a sky, two lamps and a point light) sum to the picture within 1.6e-6 relative in every pixel above 0.05, and each group is the lamp's color.
- The ambient occlusion view inside a sphere of radius 2 gives 0.9375, 0.7500, 0.4375 and 0.0000 at ray lengths 1, 2, 3 and 4.5, the values of `1 - (L / 2R)^2`.
- A PNG equals the display image of the same render in every byte, an HDR file matches the PFM to RGBE's precision (0.4 percent of the brightest channel), the half-float EXR to half's precision and 38 percent of the size, and an EXR environment map renders what the Radiance map it was made from does. A relit render with a group at scale 0 or 2 equals the picture without or with that group twice.
- `bin/aov_test` checks the layout, the resolve, the weights, the ids and the variance on the CPU, and `--selfcheck` plants an albedo over the wrong weight and a variance over `n` and fails unless both are caught. Four GPU self-tests run kernel 09's fold through the real shader against `src/aov.cpp` (worst difference 1.2e-7), the 16-sample albedo through the real `crt_scatter.glsl` against `bsdfAlbedoEstimate()` (0 of 6144 cases past 0.002, worst 7e-7, with and without a sample record), and the views against their definitions (0.5 of 255).
- OpenEXR's `exrinfo` reads the exported files. pbrt's `imgtool` is not installed here, so that check is not run.

## Not built

- The spectral band images (`nBuckets` bands over [360, 830] nm, in Fichet et al.'s layout). They need the hero-wavelength representation of [pbrt-spectral-rendering.md](pbrt-spectral-rendering.md), which writes them into `ChannelSums` after the light groups.
- `dzdx` and `dzdy`. pbrt takes them from ray differentials. The channels are left out of the file until [pbrt-textures.md](../pbrt-textures.md) adds ray-cone footprints, so no channel holds a value nothing computed.

## Where it departs from the plan and from pbrt

- The variance comes from per-channel Welford sums kept in two AOV layers, not from the film record, which holds only the luminance's variance. It takes every accepted sample, where pbrt takes the samples that met a surface.
- The AOV image holds a layer per output instead of packing channels four to a layer, and position and depth share one. All eight layers at 1920 by 1080 take 265 MB.
- The group sums carry `sum(w)` so that a group image and Relight need no other source for the weight. They leave out a non-finite term where the film drops the whole sample, and they hold the unclamped sums where the film clamps or reweights outliers.
- A path's position and normals are world space, where pbrt's default `coordinatesystem` is camera space.
- A light group check on the Veach scene was replaced by a purpose-built scene, because that file's inline materials cannot name a group.
- `crt_scatter.glsl`'s fuzzy metal case gained a branch for the albedo table, behind `CRT_AOV`, and `src/bsdf.cpp` mirrors it.

## Sources

PBR 4ed §5.4.4 (the film interface), §5.4.6 (`RGBFilm`), §5.4.7 (`GBufferFilm` and `VisibleSurface`); pbrt-v4 `src/pbrt/film.cpp` (`GBufferFilm::AddSample()` and `GetImage()`), `util/image.cpp` (EXR writer and `ImageMetadata`), `wavefront/surfscatter.cpp` (the 16 albedo samples), `cpu/integrators.cpp` (`AOIntegrator`); Fichet et al. 2021, *An OpenEXR layout for spectral images*; the Cycles manual, "Light Groups".
