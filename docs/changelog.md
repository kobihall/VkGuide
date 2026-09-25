# Changelog

## 2026-09-25

### New

- **Raytracer Shaders** window, under **Windows**. Pick which shader runs in each kernel of the wavefront path tracer. The choice saves with the scene.
- Kernel 02, intersect closest, has three shaders: BVH (binary), BVH (compressed 8-wide), and a linear scan with no acceleration structure. All three produce identical images from the same seed.
- Raytracing shaders live in `shaders/rt/`, named `NNVV_<name>.comp` for kernel `NN`, variant `VV`. The numbering follows figure 15.2 of *Physically Based Rendering*, 4th edition.
- Kernels 05, 07 and 08 (participating media, shadow rays) exist as stubs and show greyed out in the shader window. Plans are in `docs/plans/participating-media.md` and `docs/plans/shadow-rays-nee.md`.
- `docs/raytracing-overview.md` describes the tracer end to end.

### Changed

- Shading runs as three kernels instead of one. Kernel 02 sorts each path onto an escaped, emissive or surface queue, and kernels 03, 04 and 06 drain them.
- The BVH node layout follows the selected kernel 02 shader, so the Node layout combo is gone. Picking a shader rebuilds the BLASes in the layout that shader reads.
- The Acceleration structure section appears only while a kernel 02 shader that uses one is selected.
- The render guard prices a ray by the selected traversal. A linear scan over a large model trips the frame budget where a BVH does not.
- Scene files are payload version 8 and carry the kernel selection and the BVH build settings. Older files still open. An unknown shader name falls back to the slot's default and reports a load warning.
- Kernel 01 reseeds each path's random stream per bounce. The noise pattern differs from before; the converged image does not.

### Fixed

- Fixed the raytracer's texture array staying in an undefined image layout until the first model import. Any scene without models raised a validation error.

### Internal

- `src/rt_kernels.h` holds the kernel registry. A new traversal strategy is one `.comp` file and one table entry.
- Each kernel declares the descriptor bindings it uses and gets a set layout with only those. Binding numbers stay global and fixed in `shaders/rt/include/crt_common.glsl`.
- `shaders/rt/include/crt_traverse.glsl` states the traversal contract. A strategy supplies `traceScene()` and nothing else.
- `GpuInstance` is 80 bytes and carries `triangleCount`, which the linear scan reads.
- CMake compiles shaders in subdirectories, writes each `.spv` beside its source, and passes `-I shaders/` to `glslangValidator`.
