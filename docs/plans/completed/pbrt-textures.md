# Textures and filtering

Every texture the path tracer reads is resampled to 512 × 512, stored as one layer of a 2D array image with no mip levels, and read with `textureLod(..., 0)`. Distant textured surfaces alias into noise that only more samples can average out, high-resolution textures lose their detail, textures cannot repeat or clamp as their glTF sampler says, and shapes cannot be textured at all. This plan stores textures at their own size with mip chains, estimates each hit's footprint with ray cones, introduces the texture reference that material parameters use, and lets any material on any shape or mesh use an image.

*Implemented 2026-10-31, together with [pbrt-light-types.md](pbrt-light-types.md). "Spike results" records what decided the storage, and "As built" what differs from the design below. Depends on [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md) (shape uvs, derivatives) and [pbrt-material-system.md](pbrt-material-system.md) (parameters). Part of the [roadmap](../pbrt-roadmap.md).*

## Decision already made

On 2026-09-26 the user chose a descriptor array of individually sized, mipmapped textures, each with its own sampler, over adding mips to the fixed-size array or building an atlas. The plan starts with a MoltenVK spike, and the 2D array with mips is the fallback.

## What the renderer does today

- `RaytraceTextureArray` (`src/rt_textures.h`) blits every glTF image into a 512² layer of one `VK_IMAGE_VIEW_TYPE_2D_ARRAY` image, up to 256 layers, and binds it at 12 as `sampler2DArray materialTextures`. Its header explains why: indexing an array of descriptors by a per-invocation value needs descriptor indexing, which it says the project does not enable.
- The device is created with `descriptorIndexing = true` in the 1.2 features (`vk_engine.cpp`), and `vulkaninfo` on the Radeon Pro 560X reports `runtimeDescriptorArray`, `descriptorBindingPartiallyBound` and `shaderSampledImageArrayNonUniformIndexingNative`.
- Base color and emissive layers are decoded with `srgbToLinear()` after bilinear filtering, which filters in sRGB space.
- Shapes have no uvs, so their materials never carry textures.

## Design

### The spike

Before anything else, a throwaway compute shader samples 300 textures of different sizes through `uniform sampler2D textures[]` with `nonuniformEXT` indices, on this machine, under MoltenVK with Metal argument buffers enabled (`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS`). It checks for validation errors, compares the results against a CPU reference, and times it against the array path. If it fails, the rest of this plan uses a 2D array with mip levels at a configurable layer size instead, and every section below still applies except per-texture sizes and samplers.

### Storage

- Binding 27, `Textures`: `layout (set = 0, binding = 27) uniform sampler2D textures[];`, a variable-count, partially bound descriptor array.
- Each texture is its own `VkImage` at its file's resolution, capped by a **Maximum texture size** setting (default 4096), with a full mip chain generated on the GPU by successive blits.
- Color textures use `R8G8B8A8_SRGB`, so the hardware decodes to linear before filtering, which is correct; normal, metal/rough and other data textures use `UNORM`.
- Each texture has its own `VkSampler` built from its glTF sampler (wrap modes, min and mag filters), cached by parameters.
- `TextureLibrary`, beside the material library, owns every texture: those glTF files bring, and images the user loads for their own materials (PNG and JPEG through `stb_image`, HDR through the existing loader), with the color space the user declares.

The array path (binding 12) stays as a **Texture storage** setting, legacy array or native, because only it reproduces today's images bit for bit. New scenes use native.

### Texture references

A material parameter that can be textured holds a `TexRef` ([pbrt-material-system.md](pbrt-material-system.md)):

```glsl
struct TexRef {          // 32 bytes in the material's parameter block
    uint kind;           // CRT_TEX_NONE, CRT_TEX_IMAGE, later procedural kinds
    uint index;          // into textures[]
    uint mapping;        // CRT_MAP_UV0, _SPHERICAL, _CYLINDRICAL, _PLANAR
    uint channel;        // which channel a scalar parameter reads
    vec4 transform;      // KHR_texture_transform: offset.xy, scale.xy
    // rotation and the mapping's own parameters follow in the next vec4 when used
};
```

`crt_texture.glsl` evaluates one against a `SurfaceInteraction`. The procedural kinds are left to [pbrt-procedural-textures.md](../pbrt-procedural-textures.md), whose representation is undecided; `kind` is where they will plug in.

### Coordinates

The mappings PBR 4ed §10.2 describes, chosen per reference:

- UV mapping uses the mesh's first uv set, or the shape's own parameterization ([pbrt-thin-hit-record.md](pbrt-thin-hit-record.md)), with the glTF texture transform applied.
- Spherical and cylindrical mappings project about a point and axis in the object's space, for objects without useful uvs.
- Planar mapping projects onto two axes, for decals and tiled floors.

A glTF texture's second uv set (`TEXCOORD_1`) is not loaded today; supporting `texCoord = 1` is part of the importer work here.

### Footprints with ray cones

pbrt's wavefront integrator carries no ray differentials and approximates the footprint from the camera alone (PBR 4ed §15.3.9), which is wrong after a curved reflection. Ray cones (Akenine-Möller et al. 2019, 2021) track a width and a spread angle per path, two floats:

- Kernel 00 sets the spread to the angle one pixel subtends and the width to zero.
- At a hit the width grows by `spread × t`, and the spread gains a term from the surface curvature for a reflection or refraction, which the interaction's normal derivatives give ([pbrt-thin-hit-record.md](pbrt-thin-hit-record.md)).
- The mip level is `log2(width × sqrt(texelsPerUvArea × uvAreaPerWorldArea) / |cos θ|)`, with the triangle's uv-to-world area ratio precomputed per triangle and stored in `BvhTriangle.e2.w`, which is unused, and each shape's computed analytically.
- Glossy reflections widen the spread by an amount tied to the roughness (Akenine-Möller 2021 §4), so blurry reflections fetch coarse mips.

`PathState` gains `coneWidth` and `coneSpread` (roadmap). A **Texture filtering** setting offers point, bilinear at the finest level (today's behavior), trilinear by cone, and anisotropic, which passes cone-derived gradients to `textureGrad` with the sampler's anisotropy at 16.

The traversal's alpha test keeps reading the finest level, so cut-outs do not change with distance.

### Textured emission

`RaytraceTextureArray::meanLuminance()` reads back emissive layers to price emitting triangles. With native storage the CPU keeps the decoded image from load and computes the same mean from it, with no readback.

## Registry, bindings and data

- Binding 27 `Textures`. Binding 12 stays, legacy.
- `BvhTriangle.e2.w` holds the triangle's uv-to-world area ratio.
- `PathState.coneWidth`, `coneSpread`.
- Settings: **Texture storage**, **Maximum texture size**, **Texture filtering**. Scene file: those, plus each user texture's path, color space and sampler.

## Importing pbrt scenes

[completed/pbrt-scene-import.md](pbrt-scene-import.md) imports pbrt-v4 scenes today and substitutes for this plan's features. Each substitution is a line in the import report, so `./bin/pbrt_test --report <file.pbrt>` lists the scenes that need this plan. When this plan lands, change the importer as follows, then run `tools/verify/pbrt_compare.py` on the scenes named below.

- **One image per material.** `textureValue()` in `src/pbrt_import.cpp` resolves a material's `reflectance` (or `color`) texture to a constant factor times at most one image, which becomes the material's base color texture through the texture array. `scale` textures multiply into the factor; a `scale` of two images keeps the first. Float textures, such as `roughness` and `amount`, are read as their constant: an `imagemap` float texture is read as its `scale` (`floatParam()` reports each). With this plan, give `TexRef`s to every texturable parameter.
- **Mappings.** pbrt's `uv` mapping (`uscale`, `vscale`, `udelta`, `vdelta`) is baked into each mesh's uvs, with v flipped to run down the image (`buildMesh()`). `spherical`, `cylindrical` and `planar` mappings (`villa`) are read as `uv` and reported. `wrap` other than `repeat` is reported. Map both to this plan's mappings and wrap modes, and stop baking the uv transform.
- **Encodings.** 8-bit images are decoded to sRGB bytes for the texture array (`loadImportedImage()` in `src/vk_loader.cpp`): `encoding "linear"` and `"gamma g"` are converted, and float images (EXR, PFM) are clamped to 1, losing HDR texels. Native-size images with their own formats remove the conversion. The texture array's 256-layer cap drops textures past it: `sanmiguel` has 231 images, `landscape` 125.
- **Shape alpha.** A shape's `alpha` (a float or a texture) is ignored and reported: 707 shapes in `barcelona-pavilion`, `bistro`, `landscape`, `sanmiguel` and `watercolor`, so leaves render as solid quads. The renderer's alpha test reads the base color's alpha (glTF's cutout). Map pbrt's per-shape alpha texture to this plan's texture references with the traversal's cutout test.
- **Shapes without uvs.** pbrt gives each triangle of a mesh without uvs the coordinates (0, 0), (1, 0), (1, 1); the importer gives zeros. A textured material on such a mesh shows one texel.

## Verification

- The spike's results decide the storage path, and are recorded in this file.
- With legacy storage and bilinear filtering, every scene renders bit-identical.
- A textured checker plane receding to the horizon at 1 spp: aliasing with bilinear-at-the-finest-level, none with trilinear, sharper with anisotropic (PBR 4ed Figure 10.1's comparison).
- A mirror sphere reflecting the checker plane: cones keep the reflected texture filtered, where camera-only footprints alias.
- Sponza: memory reported per texture, no validation errors, and the render panel's frame time compared with the array path.

## Order of work

1. The spike.
2. `TextureLibrary`, native images with mips and samplers, and binding 27, reading LOD 0.
3. `TexRef` in material parameters, and textures on shapes.
4. Ray cones and the filtering setting.
5. Mappings, the texture transform, and `TEXCOORD_1`.

## Spike results

The spike ran on 2026-10-31 on the Radeon Pro 560X: a compute shader sampled N textures of 4 to 1024 texels a side, 4.2 million lookups at texel centers, each checked against the CPU's copy.

On Vulkan SDK 1.3.290 (MoltenVK 1.2.9), with Metal argument buffers off by default:

- A `texture2D[128]` array with a separate `sampler[4]` sampled correctly, at 3.2 ns a lookup.
- A `sampler2D[]` array (a sampler per texture) failed to compile above 16, Metal's per-stage sampler limit.
- 129 or more textures failed to compile ("cannot reserve 'texture' resource locations").
- With `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1`, 300, 1000 and 4000 textures sampled correctly, at 3.6 to 3.9 ns a lookup. The 512 × 512 array path took 1.8 ns over 256 layers. But turned on for the application, every kernel failed to build: MoltenVK could not pad the argument buffers of the per-variant set layouts ("Argument buffer resource base type could not be determined").

The user chose to install Vulkan SDK 1.4.363 (MoltenVK 1.4.2), which uses argument buffers by default. On it:

- The spike's 1000 textures sample correctly, a sampler each, with update-after-bind layouts and no validation messages.
- Every existing scene renders bit-identical to SDK 1.3.290, with the old SPIR-V and with the SPIR-V glslang 16.6 compiles (128 of the 130 `.spv` files changed).
- `vkGetDescriptorSetLayoutSupport` accepts a set of 1024 textures and 128 samplers and refuses 2048 textures, so `TextureLibrary::MAX_TEXTURES` is 1024.

The rest of this plan therefore uses per-texture images.

## As built

- **Set 1, not set 0.** Binding 27 is in a descriptor set of its own, written once when the library changes rather than at every dispatch, since a set 0 is written per launch and some 80 launches a frame would rewrite every texture's descriptor. Binding 0 of set 1 holds the samplers, separate from the images, so a texture is a pair of indices: 64 sampler states, each twice (its filters, then nearest for **Point**). `TextureLibrary` (`src/texture_library.h`) owns the images, the samplers, the set and its update-after-bind pool. `ComputePassBuilder::setSecondSetLayout()` adds the layout, and `GpuRenderer::bindingsOf()` gives `CrtBinding::Textures` to every kernel that reads materials.
- **Storage.** A color texture is stored `R8G8B8A8_SRGB` and its level 0 is a copy of its sRGB bytes, so the blits that build its mip chain filter linear values. A float file (EXR, PFM, HDR) is `R16G16B16A16_SFLOAT`. An image over **Maximum texture size** keeps the levels within it. The library reads back a level of at most 256 texels a side as linear luminance, which prices textured emitters and measures light images.
- **Texture storage** decides only where an imported model's own textures come from. Under **Legacy array**, its four glTF textures are read from binding 12 as before, and every scene renders bit for bit as before. Everything else always reads the library: textures given to a material, a shape's textures, light images. A file without the setting opens as legacy; a new scene is native.
- **References.** A material's references are `MaterialDef::textures`, a `TextureBinding` by parameter name. `packTextureTable()` writes them as 96-byte entries at the end of the record, after the emission block of [pbrt-light-types.md](pbrt-light-types.md), found by an extension header in the record's last vec4 (`CRT_MATERIAL_FLAG_EXTENDED`). The header stays 16 bytes. A reference's value multiplies its parameter. `textureTarget()` names the record field each parameter modulates. A remapped roughness multiplies alpha by the texel's square root, and Oren-Nayar's sigma is recomputed on the GPU. Not every float is texturable after all: an enum, a link, and the ABg lobe's B and g, whose table the CPU integrates.
- **`CRT_TEXTURES`** (specialization constant 8) is set when the scene has a library texture, so a scene without one runs the code it ran before.
- **Footprints.** The uv-to-world ratio comes from the interaction's dp/du and dp/dv, not from `BvhTriangle`, which has no spare word since [pbrt-robust-intersections.md](pbrt-robust-intersections.md). The cone's footprint becomes two vectors on the surface, carried into uv through the inverse of [dp/du dp/dv]. Trilinear picks the level of the footprint's longer axis, as Akenine-Möller et al. do; anisotropic hands both axes to `textureGrad`. A bounce widens the spread by twice the curvature times the width, and by half a radian times the lobe's roughness (1 for a diffuse surface). `CRT_RAY_CONES` (constant 9) is on only for trilinear and anisotropic filtering in a textured scene. The cone fields are `PathState`'s two former pad words.
- **`TEXCOORD_1`** is binding 64, `TriangleUv1`, three uvs per attribute record, a stub until a mesh has a second set (`CRT_FLAG_UV1`), so `GpuTriangleAttributes` stays 80 bytes.
- **Mappings** take the object-space point, or with `TextureBinding::worldSpace` the world point, through a 4 × 3 transform. A pbrt texture's is pbrt's world.
- **Emission prices** read the library's mean over the triangle (`TextureLibrary::meanLuminance()`). A shape uses the image's mean.
- **UI.** **Render Settings** → **Textures**, and the **Textures** folder of a material in the **Materials** window.

### Importing pbrt scenes as built

Each item the "Importing pbrt scenes" section lists is done. The meshes keep the base color's uv mapping baked in, which the legacy array reads. Each reference's transform is relative to it, so every texture keeps its own. A mesh without uvs gets pbrt's corners only when its material has a reference, since that splits its vertices. A shape's float alpha of 0 drops the shape, and a constant between 0 and 1 is reported and drawn opaque. The `octahedralsphere` wrap clamps.

### Verification as built

- **Bit identity.** With legacy storage and bilinear filtering, seven scenes render byte for byte as a build of the commit before: the Cornell box, Veach's, every material, the caustic, thin glass, the sphere scene and Sponza. `optics_bench` cannot take part: its SBVH build differs from run to run on either build.
- **The checker plane** (`assets/scenes/tx_checker_plane.gltf`) at 1 sample a pixel, the far plane's noise between two seeds: point 0.116, bilinear 0.115, trilinear 0.018, anisotropic 0.026. Against the converged image, anisotropic is the nearest (0.058 against trilinear's 0.114), since trilinear blurs along the plane.
- **The mirror** (`tx_mirror_checker.gltf`): the reflection's noise is 0.062 with bilinear and 0.031 with trilinear filtering.
- **Sponza**: 69 textures, 362.7 MB, against 69 MB for the array's 69 layers. 64 samples at 640 × 360 took 6.8 s of GPU time against the array's 5.5 s. No validation messages. Its mean is 2 percent above the array's, from filtering in linear rather than sRGB values.
- **pbrt** (`tools/verify/pbrt_light_scenes.py`, `assets/models/pbrt_lights/textured.pbrt`): uv scale and offset, a planar mapping and an imaged area light match pbrt to 0.2 percent in red and blue. The relative MSE (3.5e-3 against a noise floor of 1e-4) is the checks' edges, which the two renderers filter differently.

## Sources

PBR 4ed §10.1 (texture sampling rates, ray differentials for specular reflection and transmission, filtering), §10.2 (texture coordinate generation), §10.4 (image textures, MIP maps, EWA), §15.3.9 (approximate differentials in the wavefront integrator); PBR 3ed §10.4; Akenine-Möller et al. 2019, *Texture level of detail strategies for real-time ray tracing* (Ray Tracing Gems ch. 20), and 2021, *Improved shader and texture level of detail using ray cones*; Shirley, *Ray Tracing: The Next Week*, "Texture Coordinates for Spheres" and "Accessing Texture Image Data"; Khronos `KHR_texture_transform`.
