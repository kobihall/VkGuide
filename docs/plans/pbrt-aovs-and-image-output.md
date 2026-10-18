# Auxiliary outputs and image export

A render exists only on the GPU: nothing saves it, so every comparison with pbrt, every reference image and every figure starts with a screenshot of an 8-bit tone-mapped window. The tracer also produces nothing beside its radiance. pbrt's `GBufferFilm` writes position, normals, depth derivatives, uv, albedo and variance next to the image in one OpenEXR file, and its `SpectralFilm` writes per-wavelength bands. This plan adds image export, those auxiliary outputs (AOVs), light groups for relighting after the render, and an ambient occlusion view.

*Not implemented. Depends on [pbrt-film-and-reconstruction.md](pbrt-film-and-reconstruction.md) (`PixelState`, weight sums, variance) and [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) (the interaction the outputs read). PNG export uses [pbrt-display-and-color.md](pbrt-display-and-color.md)'s encoding. Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- Seven debug views (`CrtDebugView` in `src/rt_gpu.h`: primary direction, hit or miss, normal, bounce heat, sample-count heat, traversal cost, MIS weights) replace shading: a path ends at its first hit and writes the view's value as its radiance.
- **Compare to reference** copies the accumulation into a second GPU image. It lasts until the application quits.
- `stb_image.h` in `../CPPLibraries` reads PNG, JPEG and Radiance HDR. Nothing writes an image, and nothing reads OpenEXR.
- The device reports `shaderBufferFloat32AtomicAdd` but not `shaderImageFloat32AtomicAdd` (`vulkaninfo`), and the engine does not enable `VK_EXT_shader_atomic_float` yet.

## Design

### Image export

**File** → **Export Render...** opens a save dialog (`src/file_dialog.mm`), and the extension picks the format:

| Format | Content | Writer |
|---|---|---|
| `.exr` | linear, half or float, with every enabled output as named channels in one file | `tinyexr.h`, new in `../CPPLibraries` |
| `.pfm` | linear float RGB | 30 lines in `src/image_io.cpp` |
| `.hdr` | linear RGBE | `stb_image_write.h`, new in `../CPPLibraries` |
| `.png` | the display image as shown, 8-bit sRGB | `stb_image_write.h` |

Export copies the accumulation (and the AOV images) into a host-visible staging buffer with `immediateSubmit()`, then calls `vmaInvalidateAllocation()` before reading it. That is the read-side counterpart of the project's MoltenVK flush rule.

EXR metadata follows pbrt's `ImageMetadata` where it applies (render time, samples per pixel, `cameraFromWorld`, `NDCFromWorld`, color space, MSE when a reference is loaded), plus the scene path, the kernel selection and the render settings as a JSON string. Channel names follow `GBufferFilm` (`R`, `Albedo.R`, `P.X`, `dzdx`, `N.X`, `Ns.X`, `u`, `Variance.R`, `RelativeVariance.R`), so pbrt's `imgtool` reads the files.

`tinyexr` also reads EXR, so two loaders gain it: environment maps (`loadEnvironmentMap()`), and the **Compare to reference** section, which gains **Save reference** and **Load reference**. [pbrt-verification-harness.md](pbrt-verification-harness.md) relies on the second.

**Save when finished** writes the image to a chosen path when the render reaches its sample target. A command-line batch mode builds on it:

```sh
./bin/vulkan_guide --render assets/scenes/cornell_reference.gltf --spp 4096 --out cornell.exr
```

It opens the window, since the swapchain and ImGui assume one, renders, saves and quits. The verification scripts use it in place of the smoke-test hook.

### Auxiliary outputs

pbrt records the first visible surface's `VisibleSurface` at depth 0 of each camera path. This plan records the same fields:

| Output | Channels | Notes |
|---|---|---|
| Position | P.X, P.Y, P.Z | world space |
| Depth | Z, dzdx, dzdy | camera-space depth and its screen derivatives, from the ray-cone footprint ([pbrt-textures.md](pbrt-textures.md)) or zero |
| Normal | N.X, N.Y, N.Z | geometric |
| Shading normal | Ns.X, Ns.Y, Ns.Z | after normal and bump mapping |
| UV | u, v | |
| Albedo | Albedo.R, .G, .B | the BSDF's directional albedo `ρ(ωo)`, estimated with 16 fixed samples as pbrt's `surfscatter.cpp` does; the legacy materials report their color |
| Instance and material ids | id.instance, id.material | not averaged: each pixel keeps its first sample's ids |
| Variance | Variance.R, .G, .B, RelativeVariance | from `0902_compensated_mean` |
| Sample count | n | the existing `sampleCount` image |

A **Capture through specular** option records the first non-specular hit instead, so a mirror's albedo and normal show what it reflects. Denoisers expect that, and pbrt does not do it. Off by default.

Kernels 03, 04 and 06 write the fields into `PixelState` at bounce 0, 72 bytes a slot, allocated only while any output is on. Kernel 09 folds them into the AOV images with the sample's filter weight, the estimator pbrt's `gBufferWeightSum` applies. The AOV images are one rgba32f 2D array image (binding 32, `AovImages`) with a layer per four channels, allocated for the enabled outputs only.

An **Auxiliary outputs** section in the render panel has a checkbox per output. Turning one on restarts the render, since it has to accumulate from the first sample. Each enabled output registers with the `DisplayRegistry` as its own window under **Windows**, through a small visualization pass: normals as `0.5 + 0.5n`, depth normalized to its range, ids as hashed colors.

The existing debug views stay. They are cheaper, they need no extra memory, and older scene files select them.

### Ambient occlusion

A new debug view, after pbrt's `AOIntegrator`. At the first hit, kernel 06 emits one shadow ray in a cosine-distributed direction about the shading normal, capped at an **AO distance** setting, and kernel 08 adds 1 when it is unoccluded. It fits the existing debug-view path in `crt_debug.glsl`, and it is the fastest way to judge the geometry of an imported model before its materials are right.

### Light groups

Each punctual light, each emissive material and the environment carry a group, 0 to 7, set in the Scene panel. Group 0 is the default, so an untouched scene has one group.

- The kernels that add radiance (03, 04 and 08) also add it, times the filter weight, to their group's sums at the path's pixel.
- Several pool slots can land on one pixel in a frame, so the adds are atomic. Image float atomics are missing on this GPU, so the sums live in a storage buffer (binding 45, `ChannelSums`, 12 bytes per pixel per group) and the device enables `VK_EXT_shader_atomic_float`.
- A shadow ray carries its light's group in the high 8 bits of the word beside its origin, which it shares with the starting medium (the roadmap's shadow-ray table).
- A **Relight** toggle makes the tonemap pass read the group sums instead of the accumulation, with a scale and a tint per group. Both are display settings, so dimming a lamp or tinting the sky needs no new samples.

The atomics add in no fixed order, so the sum of the groups matches the accumulation to float rounding, not bit for bit. With four groups at 1920 × 1080 the buffer takes 100 MB; the panel shows the size before the toggle is set.

Light groups are not in pbrt. They come from production renderers (Cycles and Arnold call them light groups, RenderMan light path expressions), and they are cheap here because the tracer already knows which light each contribution comes from.

### Detector films

The readback, the file dialogs and the batch mode here are also how [physoptics-data-export.md](physoptics-data-export.md) writes detector films (EXR, NPY, ZBF), and `--export-detectors` joins the batch mode's flags. Detector films are their own buffers (binding 60), not auxiliary outputs of the camera.

### Spectral band images

With the hero-wavelength representation ([pbrt-spectral-rendering.md](pbrt-spectral-rendering.md)), an optional output keeps per-band sums as pbrt's `SpectralFilm` does: `nBuckets` bands (16 by default) over [λmin, λmax] (360 to 830 nm by default), each lane's radiance added to its wavelength's band with its sampling weight. The sums use the same `ChannelSums` buffer after the light groups, again by atomics. Export writes them in Fichet et al.'s OpenEXR spectral layout, which pbrt uses too. Outside the visible range this is the only way to see the result, and the display shows a chosen band in false color ([pbrt-display-and-color.md](pbrt-display-and-color.md)).

## Registry, bindings and data

- Binding 32 `AovImages`: rgba32f 2D array image.
- Binding 45 `ChannelSums`: a float storage buffer for light groups and spectral bands. This renames the roadmap's `LightGroups`.
- `PixelState`: 72 more bytes a slot while outputs are on.
- `ShadowRay`: the light group in the high 8 bits of `mediumAndGroup`.
- `CrtDebugView::AmbientOcclusion`.
- Scene file: each light's, emissive material's and the environment's group, the group names, the enabled outputs, and the export options. Relight scales and tints are display settings and are saved with the camera, like exposure.
- New dependencies in `../CPPLibraries`: `tinyexr.h` with its `miniz` (or the system zlib), and `stb_image_write.h`.

## Verification

- With every output, light groups and bands off, every scene renders bit-identical, and no extra buffer is allocated.
- An exported EXR, loaded as the reference, gives **Compare to reference** an MSE of exactly 0.
- A PNG export equals the display image byte for byte.
- pbrt's `imgtool info` lists an exported EXR's channels and metadata.
- The albedo of a white Lambertian plane is 1, and a smooth dielectric sphere's matches `bsdf_test`'s directional albedo at the same angle ([pbrt-verification-harness.md](pbrt-verification-harness.md)).
- On the Veach MIS scene with one group per light, the groups sum to the accumulation within 1e-4 relative at 256 spp.

## Order of work

1. Readback, PFM and PNG export.
2. `tinyexr`, EXR export with metadata, and EXR reference and environment loading.
3. Save when finished, and the batch mode.
4. The ambient occlusion view.
5. Auxiliary outputs.
6. Light groups and **Relight**.
7. Spectral band images, once the hero-wavelength representation exists.

## Sources

PBR 4ed §5.4.4 (the film interface, which names `SpectralFilm` without describing it), §5.4.6 (`RGBFilm`), §5.4.7 (`GBufferFilm` and `VisibleSurface`), chapter 5 "Further Reading" (film and denoising inputs); pbrt-v4 `src/pbrt/film.cpp` (`GBufferFilm`, `SpectralFilm`), `util/image.cpp` (EXR, PFM, PNG writers and `ImageMetadata`), `wavefront/surfscatter.cpp` (albedo from 16 samples), `cpu/integrators.cpp` (`AOIntegrator`); Fichet et al. 2021, *An OpenEXR layout for spectral images*; Cycles manual, "Light Groups".
