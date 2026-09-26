# Changelog

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
- Kernels 05 and 07 (participating media) exist as stubs and show greyed out in the shader window. The plan is in `docs/plans/participating-media.md`.
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
