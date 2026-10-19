# Error-bounded intersections

Every ray the tracer spawned started at a hit point computed in float, and every trace ignored hits closer than `CRT_T_MIN = 0.001`. That fixed epsilon is too large for small objects (a leaf 1 mm thick loses its back face) and too small for large or distant ones (a surface at 10 km self-shadows). Möller–Trumbore can also miss a ray that passes exactly through the edge shared by two triangles. This plan adopts pbrt's approach: bound the error of every intersection point, offset spawned rays just past that bound, and use a watertight triangle test.

*Implemented 2026-10-19, apart from camera-relative rendering space, which moved to [pbrt-camera-relative-space.md](../pbrt-camera-relative-space.md). Depends on [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md). Part of the [roadmap](../pbrt-roadmap.md).*

## Decisions

On 2026-09-26 the user accepted these defaults: error-bounded offsets replace `CRT_T_MIN`, with the old epsilon kept behind a flag, and the watertight triangle test sits behind a runtime flag with Möller–Trumbore kept.

On 2026-10-19 the user answered three questions when implementation started:

- Triangles are stored as three vertices (`v0`, `v1`, `v2`) instead of a vertex and two edges. Rebuilding `v1` as `v0 + e1` rounds differently for every triangle that shares the vertex, which loses the watertight guarantee.
- A shadow ray to an area light is offset at the light end along the sampled point's normal. The light samplers return that normal, and the `1 - 1e-4` shortening is gone.
- Steps 1 to 4 first, then a decision on step 5. On the same day the user chose to skip step 5 and plan it separately.

## What was built

### Flags and settings

`RenderSettings` has `errorBoundedOffsets` and `watertightTriangles`, both on by default. They are in the render key, so a change restarts the render, and not in `AccelSettings`, so a change does not rebuild any BLAS. **Raytrace Render** → **Intersection precision** is a section of its own beside **Acceleration structure**, because the linear traversal hides that section and takes both flags. Scene files are payload version 13, and an older file opens with both flags on.

With both flags off a render is bit-identical to one from before this plan. The widened box tests belong to **Watertight triangles**: both are guarantees that rounding cannot lose a hit.

### Error bounds

`src/robust.h` and the robust section of `crt_common.glsl` hold `gammaBound()`, `nextFloatUp()`, `nextFloatDown()` and `offsetRayOrigin()`, line for line. `surfaceInteraction()` fills `p` and `pError` under the first flag:

- A triangle's point is `b0 p0 + b1 p1 + b2 p2` in object space with error `γ7 (|b0 p0| + |b1 p1| + |b2 p2|) + γ2 (|p0| + |p1| + |p2|)`. The second term covers `b0 = 1 - b1 - b2` not being exact.
- A shape's point comes from the object-space ray and is reprojected onto the primitive (`shapeReproject()`, `shapeHitPoint()` on the CPU): a sphere's onto its radius, a cylinder's side onto its rim, and a coordinate the primitive fixes (a quad's y, a box face's axis, a cap's y) is set to it with zero error. The other coordinates have error `γ5 |p|`.
- `toWorldPoint()` carries the error through the instance transform as PBR 4ed 6.8.5 does, and adds twice the round trip `toWorld (toObject p) - p`. The `toObject` rows are the inverse of the `toWorld` rows only to rounding, and for a rotated 40:1 scale they are a few thousand ulps apart. The first version of the bound, without this term, let 0.4 percent of the spawns from such a cylinder re-hit it.

Kernels 02 and 04 and `surfaceInteraction()` call `shapeObjectHit()`, so they agree on the point and on the part and uv derived from it.

### Spawning rays

- Kernel 06 spawns the continuation at `offsetRayOrigin(p, pError, n, outDir)`.
- A shadow ray starts at the offset point. To an area light it ends at the sampled point offset along the light's normal by `γ16 (|p_light| + |origin|)` toward the origin. To a point or spot light it ends at the light, and a directional light or the environment has no end. The ray's direction and length are recomputed from the two offset points.
- Every trace takes its `tMin` from `traceTMin()`: `CRT_T_MIN` without the flag, and 1e-30 with it. The value is strictly above zero on purpose. A ray spawned exactly on an infinite plane at y = 0 under an identity transform has no error along its normal and computes `t = ±0` for it, and `t >= 0` would hit it. pbrt's analytic shapes use a strict `t > 0` for the same reason.
- Kernels 05 and 07 ([pbrt-participating-media.md](../pbrt-participating-media.md)) spawn from medium points, which have no normal and no offset.

### Watertight triangles

`hitTriangleWatertight()` and `intersectBvhTriangleWatertight()` follow PBR 4ed 6.8.4: translate to the ray origin, permute so the largest component is z, shear, evaluate three edge functions with `differenceOfProducts()`, and require `t` above its own error bound. The Radeon Pro 560X has no `shaderFloat64`, so there is no double-precision fallback at an edge function of exactly zero. A ray exactly through an edge or vertex is accepted by every triangle that owns it, which can count a hit twice and never loses one. The shear is applied with `fma()` so a vertex gets the same arithmetic in whichever of the three slots it sits. `instanceRay()` computes the permutation and shear once per object-space ray.

### Conservative box tests

The binary layout, the TLAS and the analytic box widen each slab's far distance by `1 + 2γ3` (Ize 2013). That factor assumes a relative error in `t`, which holds for `(bmin - o) * r` and fails for the CWBVH node test: its child distance is `offset + q * scaled`, which cancels, so its error is relative to the node's extent. A first attempt that padded only the far distance with the node's absolute error still lost 8 of 3969 rays through the vertices of a flat grid, because the comparison pairs one axis's near distance with another axis's far distance. The CWBVH test now lowers each axis's near distance and raises its far distance by that axis's own `γ4 (|offset| + 255 |scaled|)`, and a quarter of that bound is already enough on the grid.

## Verification

All of these ran on 2026-10-19.

- Both flags off, eight renders bit-identical to the commit before: the Cornell box under the three traversals, the sphere scene, the Veach scene, and Sponza under both layouts (160 × 90 × 4), plus the traversal-cost view, with the traversal counters equal. Exposing the sampled light point from `sampleLight()` broke this at the 1e-6 level without changing a hit, because it changed how the compiler fused the sampling arithmetic. The sampler now returns only the normal.
- `bin/bvh_bench`, from `tests/bvh_robust.cpp`:
    - `nextFloatUp()` and `nextFloatDown()` match `std::nextafter` on 996,094 values.
    - Crack test, 6 million rays aimed at the edges and vertices of a sphere of 9,000 triangles: Möller–Trumbore leaks through 11 percent, watertight through none, on both layouts.
    - Vertex grid, one ray through each interior vertex of a 64 × 64 quad grid: Möller–Trumbore loses 563 of 3969 (binary) and 906 (CWBVH), watertight none.
    - Error bounds against a double-precision reference, over every shape kind and mesh sphere and box at three scales (0.005 to 0.04 units near the origin, 0.5 to 20 at ±100, 100 to 800 at ±10,000): the largest distance from the exact surface is 0.44 of the bound, and no sample exceeds it.
    - Spawn offsets: with them no spawn re-hits its surface in about 48 million, under both triangle tests. Without them 2 to 50 percent do, depending on the kind and the scale, which shows the test can fail.
- The GPU version of the vertex grid, with the camera aligned to the vertices: Möller–Trumbore leaks 578, 908 and 114 interior rays under the binary, CWBVH and linear traversals, and watertight leaks none. The mesh's own boundary pixels are left out, since a ray there may pass either side.
- `assets/scenes/thin_glass.gltf`, a pane of glass as two quads 1e-4 apart: with the epsilon the wall behind it shrinks, with error-bounded offsets it matches a render without the pane, apart from the Fresnel loss (mean 0.937 of the no-pane render).
- The Cornell box shifted by 10,000 units: the epsilon renders 3.2 percent darker with 8,000 changed pixels, and error-bounded offsets match the unshifted render within 0.005 percent. Shifted by 100,000 units neither does, since the coordinates themselves no longer fit (float spacing 7.8e-3 against a box 55 units wide).
- Flags on against flags off, same seeds: mean brightness differs by at most 0.065 percent (Cornell +0.005, sphere scene +0.0005, Veach -0.0004, Sponza -0.06 percent) with no consistent sign.
- Cost on Sponza at 640 × 360 and 8 samples, interleaved runs: error-bounded offsets cost nothing measurable, and the watertight test with the widened boxes costs 8 to 12 percent GPU time on both layouts. Nodes per ray rise 0.2 percent and primitives 1.4 percent, far under the 20 percent PBR 4ed 6.8.8 reports, so the render guard's estimate still covers it.
- With the Vulkan validation layers on, five renders under every traversal gave no messages.
- An emissive triangle-mesh light under a rotated, scaled transform renders the same with and without the flags.
- Not run: the Cornell box at 1,000,000 units, which belongs to camera-relative space.

## Step 5: camera-relative rendering space

Not built. Nothing planned needs it: the laser optics scenes sit within a meter of the origin, and the error-bounded offsets already cover them. [pbrt-camera-relative-space.md](../pbrt-camera-relative-space.md) holds the design and the condition that would start it. The one measurement here: a Cornell box 100,000 units from the origin renders wrongly with or without offsets, because its coordinates no longer fit in float.

## For later plans

- [physoptics-optical-surfaces.md](../physoptics-optical-surfaces.md) adds its surfaces to the error-bound table: a conic hit reprojected along its normal and bounded like a quadric, and a plane's error zero along its normal. The `toWorldPoint()` round trip applies to a surface frame's transform the same way. A cemented interface a few microns thick is the case the offsets decide.
- `src/robust.h` is `float` only. The beam solver's double-precision geometry needs `gammaBound()` and the offsets templated on the scalar.
- [pbrt-analytic-shapes.md](../pbrt-analytic-shapes.md) adds a case to `shapeReproject()` and `shapeHitPoint()` for each kind. A kind that function does not know gets `γ5 |p|` and is not reprojected.
- [pbrt-csg.md](../pbrt-csg.md) can read `pError` to treat interval ends within the error bound as coincident.
- A change to a light sampler must keep what it exposes to kernel 06 to the normal only, or the flags-off comparison needs a new baseline.

## Sources

PBR 4ed §6.8 (floating-point error analysis, conservative ray–bounds intersections, robust triangle intersections, bounding intersection point error, robust spawned ray origins, avoiding intersections behind ray origins), §5.1.1 (rendering space); Woop, Benthin and Wald 2013, *Watertight ray/triangle intersection*; Ize 2013, *Robust BVH ray traversal*; pbrt-v4 `src/pbrt/util/float.h`, `interaction.h` (`OffsetRayOrigin`), `transform.h` (transforming a ray with an error bound).
