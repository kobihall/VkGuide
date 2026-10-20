# Curves and hair

Hair, fur and grass have millions of strands far thinner than a triangle mesh can represent economically. pbrt models them as cubic Bézier curves with a varying width, intersected by recursive subdivision, and shades them with a hair BSDF that follows how light scatters in a dielectric cylinder with pigment. This plan adds the curve primitive to the BVH, the hair BSDF, and a loader for Cem Yuksel's cyHair files, the format pbrt's `cyhair2pbrt` converts.

*Not implemented. Depends on [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md), [pbrt-material-system.md](completed/pbrt-material-system.md) and [pbrt-bsdf-library.md](pbrt-bsdf-library.md) (the BxDF interface and `rt_bsdf`). Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- The BVH library (`rt_bvh`) builds BLASes over triangles only, stored as `BvhTriangle`. Analytic shapes are single-primitive instances tested directly.
- The traversal's leaf test is `testTriangles()` in `crt_traverse.glsl`, shared by every strategy.
- No material models fibers.

## Design

### The curve primitive

pbrt-v4's `Curve` (PBR 4ed §6.7): a cubic Bézier segment of four control points, a width at each end interpolated linearly, and one of three types:

| Type | Surface | Use |
|---|---|---|
| Flat | a ribbon that always faces the ray | distant hair and fur |
| Cylinder | a flat ribbon whose shading normal bends across its width as a cylinder's would | hair seen up close |
| Ribbon | a ribbon oriented by normals given at its two ends | grass blades, leaves |

The intersection follows pbrt: in a coordinate system where the ray runs along +z, subdivide the curve recursively until each piece is nearly straight, and test the ray against the resulting segments' widened bounds. pbrt derives the maximum depth from the curve's flatness and its width, capped at 10; on the GPU the recursion becomes a loop over a small explicit stack. PBR 4ed exercise 6.13 points at faster intersectors (Reshetov 2017; Reshetov and Luebke 2018, the phantom ray-hair intersector), which are later variants of the same test.

A hit records the curve parameter `u` along the strand and `v` across it. The hair BSDF needs `h`, the offset from the fiber's axis in [−1, 1], which is `2v − 1`. [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md)'s barycentrics field carries `u` and `v`.

### In the BVH

A curve set is an object like a mesh: one BLAS over its segments, instanced by the TLAS.

- `rt_bvh` generalizes the primitive a BLAS holds. A leaf stays a range of primitive indices, and the BLAS records whether they are triangles or curve segments. The builders only need bounds and centroids, so they do not change; a curve segment's bounds are its control points' box widened by the larger end width.
- Segments go into binding 34, `Curves`: four control points, two widths, and for ribbons two normals, 64 bytes per segment.
- The traversal's leaf test dispatches on the BLAS's primitive type: `testTriangles()` or a new `testCurves()`, both in `crt_traverse.glsl`. Every strategy therefore gets curves without its own code, and the CPU traversals in `bvh_scene` mirror the test for `bvh_bench`.
- `GpuInstance`'s existing `triangleBase` points at the first segment for a curve instance.

### The hair BSDF

pbrt's `HairBxDF` (PBR 4ed §9.9), after Chiang et al. 2016, d'Eon et al. 2011 and Marschner et al. 2003:

- Light either reflects off the fiber's surface (R), passes through it (TT), reflects once inside (TRT), or takes longer paths (TRRT and beyond, lumped together).
- Each path's longitudinal scattering uses d'Eon's energy-conserving Mp, its azimuthal scattering a trimmed logistic Np, and its attenuation Ap, from the Fresnel terms and the pigment's absorption along the path through the cylinder.
- Parameters: absorption σa, given directly, as an RGB reflectance inverted by Chiang's fit, or as eumelanin and pheomelanin concentrations (pbrt's three options); longitudinal and azimuthal roughness βm and βn; the cuticle scale tilt α; η = 1.55.
- Sampling picks a scattering path by its attenuation, then samples Mp and Np, as pbrt does.

It is a `hair` material type in the material system, evaluated in the complex material queue. It is the first BSDF whose frame is the fiber's tangent rather than a surface normal: the interaction's `dpdu` gives the fiber direction.

The C++ mirror joins `rt_bsdf`, and `bin/bsdf_test` ports pbrt's own hair tests from `bsdfs_test.cpp`: reciprocity, the white furnace with no absorption (with and without sampling), `h` at the fiber's edge, sampling weights, and sampling consistency.

### Loading hair

- cyHair (`.hair`, Yuksel's format): a header, then per strand a point count and points, with optional per-point widths and colors. `src/hair_loader.cpp` turns each strand's polyline into Bézier segments, as `cyhair2pbrt` does, and makes one curve set per file. The Scene panel gains a **Hair** folder beside **Models**, placed with the gizmo.
- pbrt scene files' `curve` shapes load through [pbrt-scene-import.md](pbrt-scene-import.md).
- glTF has no curve primitive, so imported models bring none.

### As lights

pbrt-v4 does not sample curves as lights. An emissive curve set stays out of the light list, and BSDF samples find it at full weight.

## Registry, bindings and data

- Binding 34 `Curves`.
- `testCurves()` in `crt_traverse.glsl`; a primitive-type field per BLAS in `bvh_layout`.
- The `hair` material type; `src/hair_loader.cpp`; the **Hair** folder and its scene-file records (path, pose, curve type, material).

## Verification

- A scene without curves renders bit-identical.
- `bin/bvh_bench` with a curve set: the traversal matches brute force with 0 mismatches over random rays, with `--perturb`, for all three curve types.
- `bin/bsdf_test`: pbrt's hair tests pass against the mirror, and the GPU parity pass ([pbrt-verification-harness.md](completed/pbrt-verification-harness.md)) matches the mirror.
- A white furnace ([pbrt-verification-harness.md](completed/pbrt-verification-harness.md)): a hair ball with zero absorption under a uniform white environment is invisible.
- A hair model rendered with eumelanin concentrations of 0.3, 1.3 and 8, the values PBR 4ed §9.9.8 gives for blond, brown and black hair, matches the look of Figure 9.47. Blond hair at a ray depth of 3 and of 50 differs as Figure 9.55 shows, since light-colored hair depends on light scattered between many fibers.

## Order of work

1. Generalize `rt_bvh` leaves to typed primitives, with triangles only. Bit-identical.
2. Curve segments, `testCurves()` and the CPU mirror, with `bvh_bench`.
3. The cyHair loader and the **Hair** folder.
4. The hair BSDF and its tests.
5. A faster intersector as a later variant.

## Sources

PBR 4ed §6.7 (curves: bounding and intersection by subdivision), §9.9 (scattering from hair), exercise 6.13; Nakamaru and Ohno 2002, *Ray tracing for curves primitive*; Marschner et al. 2003, *Light scattering from human hair fibers*; d'Eon et al. 2011, *An energy-conserving hair reflectance model*; Chiang et al. 2016, *A practical and controllable hair and fur model for production path tracing*; Reshetov 2017 (Budan–Fourier curve intersection, HPG); Reshetov and Luebke 2018, *Phantom ray-hair intersector*; pbrt-v4 `src/pbrt/shapes.h` (`Curve`), `bxdfs.h` (`HairBxDF`), `bsdfs_test.cpp`, `cmd/cyhair2pbrt.cpp`; Yuksel, cyHair file format.
