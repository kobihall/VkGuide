# Changelog

## 2026-09-25

### New

- **Raytracer Shaders** window, under **Windows**. Pick which shader runs in each kernel of the wavefront path tracer. The choice saves with the scene.
- Kernel 02, intersect closest, has three shaders: BVH (binary), BVH (compressed 8-wide), and a linear scan with no acceleration structure. All three produce identical images from the same seed.
- Raytracing shaders live in `shaders/rt/`, named `NNVV_<name>.comp` for kernel `NN`, variant `VV`. The numbering follows figure 15.2 of *Physically Based Rendering*, 4th edition.
- Kernels 05, 07 and 08 (participating media, shadow rays) exist as stubs and show greyed out in the shader window. Plans are in `docs/plans/participating-media.md` and `docs/plans/shadow-rays-nee.md`.
- `docs/raytracing-overview.md` describes the tracer end to end.
- A sixth material type, **pbr**, is glTF 2.0's metallic-roughness model: a GGX specular lobe over a diffuse base, with metallic, roughness and an emission that does not end the path. Shapes and mesh overrides can pick it in the **Scene** panel.
- The path tracer applies glTF normal maps, metallic-roughness textures and emissive textures. Tangents come from the file's `TANGENT` attribute, or are generated from the uvs when it has none.
- glTF alpha cutouts (`alphaMode` `MASK`) render in the path tracer. Sponza's foliage and chains are cut out instead of drawn as solid quads.
- A glTF material with an `emissiveFactor` glows and still reflects light. `KHR_materials_emissive_strength` scales it.
- `KHR_lights_punctual` point, spot and directional lights load with their model. The path tracer does not render them yet; `docs/plans/shadow-rays-nee.md` §2.8 lists what rendering them needs.

### Changed

- Shading runs as three kernels instead of one. Kernel 02 sorts each path onto an escaped, emissive or surface queue, and kernels 03, 04 and 06 drain them.
- The BVH node layout follows the selected kernel 02 shader, so the Node layout combo is gone. Picking a shader rebuilds the BLASes in the layout that shader reads.
- The Acceleration structure section appears only while a kernel 02 shader that uses one is selected.
- The render guard prices a ray by the selected traversal. A linear scan over a large model trips the frame budget where a BVH does not.
- Scene files are payload version 9. Version 8 carries the kernel selection and the BVH build settings, and version 9 the **pbr** material. Older files still open. An unknown shader name falls back to the slot's default and reports a load warning.
- Kernel 01 reseeds each path's random stream per bounce. The noise pattern differs from before; the converged image does not.
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
- `shaders/rt/include/crt_traverse.glsl` states the traversal contract. A strategy supplies `traceScene()` and nothing else.
- `GpuMaterial` is 64 bytes and `GpuTriangleAttributes` is 80 bytes; the second gains three packed vertex tangents. `MeshAsset::cpuTangents` holds the tangents on the CPU side and never reaches the raster vertex buffer.
- The top bit of `BvhTriangle::v0.w` flags an alpha-tested triangle. Mask the index with `BVH_TRIANGLE_INDEX_MASK` before using it. The CPU traversals treat every triangle as opaque.
- Binding 12 is `materialTextures` (`CrtBinding::MaterialTextures`), and kernels 02, 04 and 06 declare it.
- Kernels 03 and 04 add to a path's radiance slot instead of writing it, and a barrier now separates kernel 04 from 06. A glowing **pbr** hit sits on both the emissive and the surface queue.
- `LoadedGLTF` keys its meshes, nodes, images and materials by a unique name, generated as `material3` and so on for an unnamed object.
- `GpuInstance` is 80 bytes and carries `triangleCount`, which the linear scan reads.
- CMake compiles shaders in subdirectories, writes each `.spv` beside its source, and passes `-I shaders/` to `glslangValidator`.
