# Textures and filtering

Every texture the path tracer reads is resampled to 512 × 512, stored as one layer of a 2D array image with no mip levels, and read with `textureLod(..., 0)`. Distant textured surfaces alias into noise that only more samples can average out, high-resolution textures lose their detail, textures cannot repeat or clamp as their glTF sampler says, and shapes cannot be textured at all. This plan stores textures at their own size with mip chains, estimates each hit's footprint with ray cones, introduces the texture reference that material parameters use, and lets any material on any shape or mesh use an image.

*Not implemented. Depends on [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) (shape uvs, derivatives) and [pbrt-material-system.md](completed/pbrt-material-system.md) (parameters). Part of the [roadmap](pbrt-roadmap.md).*

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

A material parameter that can be textured holds a `TexRef` ([pbrt-material-system.md](completed/pbrt-material-system.md)):

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

`crt_texture.glsl` evaluates one against a `SurfaceInteraction`. The procedural kinds are left to [pbrt-procedural-textures.md](pbrt-procedural-textures.md), whose representation is undecided; `kind` is where they will plug in.

### Coordinates

The mappings PBR 4ed §10.2 describes, chosen per reference:

- UV mapping uses the mesh's first uv set, or the shape's own parameterization ([pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md)), with the glTF texture transform applied.
- Spherical and cylindrical mappings project about a point and axis in the object's space, for objects without useful uvs.
- Planar mapping projects onto two axes, for decals and tiled floors.

A glTF texture's second uv set (`TEXCOORD_1`) is not loaded today; supporting `texCoord = 1` is part of the importer work here.

### Footprints with ray cones

pbrt's wavefront integrator carries no ray differentials and approximates the footprint from the camera alone (PBR 4ed §15.3.9), which is wrong after a curved reflection. Ray cones (Akenine-Möller et al. 2019, 2021) track a width and a spread angle per path, two floats:

- Kernel 00 sets the spread to the angle one pixel subtends and the width to zero.
- At a hit the width grows by `spread × t`, and the spread gains a term from the surface curvature for a reflection or refraction, which the interaction's normal derivatives give ([pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md)).
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

## Sources

PBR 4ed §10.1 (texture sampling rates, ray differentials for specular reflection and transmission, filtering), §10.2 (texture coordinate generation), §10.4 (image textures, MIP maps, EWA), §15.3.9 (approximate differentials in the wavefront integrator); PBR 3ed §10.4; Akenine-Möller et al. 2019, *Texture level of detail strategies for real-time ray tracing* (Ray Tracing Gems ch. 20), and 2021, *Improved shader and texture level of detail using ray cones*; Shirley, *Ray Tracing: The Next Week*, "Texture Coordinates for Spheres" and "Accessing Texture Image Data"; Khronos `KHR_texture_transform`.
