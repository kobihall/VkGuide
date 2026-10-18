# Error-bounded intersections

Every ray the tracer spawns starts at a hit point computed in float, and every trace ignores hits closer than `CRT_T_MIN = 0.001`. That fixed epsilon is too large for small objects (a leaf 1 mm thick loses its back face) and too small for large or distant ones (a surface at 10 km self-shadows). Möller–Trumbore can also miss a ray that passes exactly through the edge shared by two triangles. This plan adopts pbrt's approach: bound the error of every intersection point, offset spawned rays just past that bound, and use a watertight triangle test.

*Not implemented. Depends on [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md). Part of the [roadmap](pbrt-roadmap.md).*

## Decisions already made

On 2026-09-26 the user accepted these defaults: error-bounded offsets replace `CRT_T_MIN`, with the old epsilon kept behind a flag, and the watertight triangle test sits behind a runtime flag with Möller–Trumbore kept.

## What the renderer does today

- `CRT_T_MIN = 0.001` is the minimum t of every closest-hit and shadow trace (`crt_common.glsl`).
- `hitTriangle()` is Möller–Trumbore, two-sided, with a `1e-30` determinant test (`crt_common.glsl`), mirrored by `intersectBvhTriangle()` in `bvh_layout.cpp`.
- The slab test replaces near-zero reciprocal components with a signed `1e-12` and uses no rounding margin on `tMax`.
- Shadow rays stop at `distance · (1 − 1e-4)` short of the light (`crt_surface.glsl`).
- Rendering happens in world space. A scene far from the origin loses precision everywhere.

## Design

### Intersection points with error bounds

Each primitive reports a conservative absolute error `pError` for the point it returns, following PBR 4ed §6.8.5:

- Triangles: the point is rebuilt from barycentrics as `b0 p0 + b1 p1 + b2 p2` in object space, with error `γ7 (|b0 p0| + |b1 p1| + |b2 p2|)`, then transformed to world space with the transform's own error added (§6.8.5, "Transformations").
- Shapes: each unit primitive's reprojected point and error, per §6.8.5's quadric cases. A sphere's point is reprojected onto the radius, a quad's and a box face's error is zero along their plane normal.
- The instance transform adds `γ3 (|m00 x| + |m01 y| + |m02 z| + |m03|)` per row.
- Optical bodies ([physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)): a conic or toroidal hit is reprojected onto its surface along the normal and bounded like a quadric, a plane's error is zero along its normal, and the surface frame's transform adds its own term like an instance's. A light path through a long train of optics spawns a ray at every surface, so these offsets decide whether thin cemented interfaces are found.

These need the object-to-world rows that [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md) adds to `GpuInstance`. `surfaceInteraction()` fills `SurfaceInteraction.pError`.

### Spawning rays

`offsetRayOrigin(p, pError, n, w)` moves the origin along the geometric normal by `d = dot(abs(n), pError)`, onto the side `w` leaves through, then rounds each component away from the surface with `nextFloatUp` and `nextFloatDown` (PBR 4ed §6.8.6). Every trace then uses `tMin = 0`:

- Kernel 06 spawns the continuation and the shadow ray with it.
- A shadow ray to an area light is offset at both ends (`SpawnRayTo`), and to a point light at the start only; the `1 − 1e-4` shortening goes away.
- Kernels 05 and 07 ([pbrt-participating-media.md](pbrt-participating-media.md)) spawn from medium points, which have no normal and no offset.

GLSL has no `nextafter`; `uintBitsToFloat(floatBitsToUint(x) + 1)` with sign handling is the standard substitute, and `crt_common.glsl` gains `nextFloatUp()` and `nextFloatDown()`.

### Watertight triangles

The watertight test (Woop, Benthin and Wald 2013; PBR 4ed §6.5.3 and §6.8.4) translates the triangle to the ray origin, permutes the axes so the ray's largest direction component is z, shears the ray onto +z, and evaluates three edge functions whose signs decide the hit. It never reports a miss for a ray through a shared edge.

pbrt falls back to double precision when an edge function is exactly zero. The Radeon Pro 560X has no `shaderFloat64`, so the edge functions use the FMA-based difference of products (`differenceOfProducts()`, already in `crt_light.glsl`), which makes a spurious zero far rarer than a plain product. The residual cases are rays exactly through a vertex, which a two-sided test then counts for both triangles. That can double-count a hit but never loses one, which is the failure that matters.

Both tests stay available. `hitTriangle()` branches on a push-constant flag, which is uniform per dispatch and costs nothing measurable. The shear constants are computed once per ray and passed down the traversal, so the watertight path adds no per-triangle setup.

### Conservative box tests

Ize (2013), as PBR 4ed §6.8.2: multiply the slab test's far distance by `1 + 2γ3`, so a ray grazing a box edge is never culled by rounding. Both BVH layouts' box tests and their C++ mirrors change together.

### Camera-relative rendering space

PBR 4ed §5.1.1 shows a scene 1,000,000 units from the origin breaking up in world space and rendering cleanly in camera-world space, where the camera is at the origin but the axes keep the world's orientation. `buildSceneAccel()` can apply that translation to every instance, light and TLAS box when the render starts; the BLASes stay in object space and do not change. This is the last step, since it only matters for scenes far from the origin, and it restarts the render on every camera move, which restart-on-change already does.

### Settings and defaults

The **Acceleration structure** section gains an **Intersection precision** group:

- **Error-bounded ray offsets**, on by default for new scenes and for older files. Off restores `CRT_T_MIN` exactly.
- **Watertight triangles**, on by default. Off restores Möller–Trumbore exactly.
- **Camera-relative space**, off until the last step is verified.

All three are saved with the scene in the payload version this lands in.

## Verification

- With every flag off, the Cornell box, sphere scene, Veach scene and Sponza render bit-identical to before.
- `bvh_bench` gains the watertight test and a crack test: rays aimed exactly at the shared edges and vertices of a finely tessellated sphere (`assets/structure.glb` works too) must report a hit every time. Möller–Trumbore is expected to miss some; the watertight test must miss none.
- A thin-geometry scene (two quads 1e-4 apart, lit from one side) shows light leaking through with the epsilon and none with the offsets.
- A copy of the Cornell box moved to (1e6, 1e6, 1e6) matches the original with camera-relative space on.
- The traversal-cost counters are compared before and after. PBR 4ed §6.8.8 reports about 20 % more intersection work from origins sitting closer to surfaces; the render guard's estimate must still cover it.

## Order of work

1. `nextFloatUp`/`nextFloatDown`, the offset function and the flag, with `pError = 0`: spawn points move by one ulp. Compare against the epsilon on the thin-geometry scene.
2. Error bounds for triangles and instance transforms, then for each shape kind, each checked on the CPU against a double-precision reference in `bvh_bench`.
3. The watertight test in `crt_common.glsl` and `bvh_layout.cpp`, and the crack test.
4. Conservative box tests.
5. Camera-relative space.

## Sources

PBR 4ed §6.8 (floating-point error analysis, conservative ray–bounds intersections, robust triangle intersections, bounding intersection point error, robust spawned ray origins, avoiding intersections behind ray origins), §5.1.1 (rendering space); Woop, Benthin and Wald 2013, *Watertight ray/triangle intersection*; Ize 2013, *Robust BVH ray traversal*; pbrt-v4 `src/pbrt/util/float.h`, `interaction.h` (`OffsetRayOrigin`).
