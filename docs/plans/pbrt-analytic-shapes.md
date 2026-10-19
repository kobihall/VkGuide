# Analytic shapes

The tracer has five analytic shapes: sphere, infinite plane, quad, box and capped cylinder, each a unit primitive placed by an instance transform. pbrt has more. Its disks and annuli make round area lights, and its partial spheres, cylinders and disks cut away a range of angle or height. The 3rd edition adds cones, paraboloids and hyperboloids, and the 4th edition adds bilinear patches, which are non-planar quads with a fast intersection test. *The Next Week*'s "Additional 2D Primitives" section derives disks, annuli and triangles from the quad's own plane coordinates. This plan adds all of them through the existing `SHAPE_TRAITS` machinery.

*Not implemented. Depends on [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) (each shape's uv and derivatives). Disk sampling by solid angle is in [pbrt-light-shape-sampling.md](pbrt-light-shape-sampling.md). Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- `ShapeKind` (`src/shape.h`) and `SHAPE_TRAITS` (`src/rt_scene_types.h`) define each kind once: its name, its editable `ShapeParam`s and which size axes each drives, whether it rotates, and its defaults. The editor, gizmo, scene file and preview all read the table.
- A placed shape is `SceneShape`: kind, position, orientation and a `size` scaling the unit primitive. Nothing else describes a shape.
- `intersectShape()` and `shapeNormal()` exist in C++ and in `crt_shape.glsl`, mirrored line for line. `shape_mesh.cpp` makes the raster preview.
- Emissive shapes join the light list per shape (six records per box), sampled by `crt_light.glsl`: spheres by cone, quads and box faces as spherical rectangles, cylinders by area.

## Design

### Parameters that are not scales

Partial shapes need an angle or a height range, which no `size` axis expresses. `ShapeParam` gains a second kind, a *value*, and `SceneShape` gains `values`, four floats whose meaning each kind's traits define, with defaults that give the complete shape. On the GPU a shape's values are one `vec4` in a new buffer, binding 50 (`ShapeParams`), indexed by the shape instance's `attributeBase`, a field that only meshes use today.

### New kinds

| Kind | Unit primitive | Values | Source |
|---|---|---|---|
| Disk | radius 1 in the xz plane, facing +y | inner radius (an annulus when above 0), φmax | PBR 4ed §6.4; *The Next Week*, "Additional 2D Primitives" |
| Triangle | the corners (0, 0, 0), (1, 0, 0), (0, 0, 1) | none | *The Next Week*, "Additional 2D Primitives" |
| Sphere (partial) | the existing sphere | zmin, zmax, φmax | PBR 4ed §6.2 |
| Cylinder (partial, open) | the existing cylinder | φmax, and whether it has caps | PBR 4ed §6.3 |
| Cone | radius 1 at y = 0, apex at y = 1 | φmax | PBR 3ed §3.5.1 |
| Paraboloid | radius 1 at the top, y from 0 to 1 | zmin, φmax | PBR 3ed §3.5.2 |
| Hyperboloid | the surface swept by the segment from p1 to p2 around the y axis | p1 and p2 as values, φmax | PBR 3ed §3.5.3 |
| Bilinear patch | the unit square's corners, each lifted by a height | four corner heights | PBR 4ed §6.6 |

Optical bodies are not among these kinds. [physoptics-optical-surfaces.md](physoptics-optical-surfaces.md) adds them as a compound shape with tag `CRT_SHAPE_OPTIC` (255), above the `ShapeKind` range, so the kinds here keep appending below it. Its conic surfaces are a lens designer's vertex-form conics with clear apertures, not this plan's paraboloid and hyperboloid, which stay for scene geometry.

The partial sphere and cylinder are the existing kinds with new values, not new kinds, so an existing shape keeps its record and loads unchanged. An ellipse is a disk with unequal sizes; an elliptical cylinder, a cylinder with unequal x and z sizes.

The bilinear patch uses Reshetov's intersection (2019), which pbrt-v4 adopted. Its four corner heights make any twisted quadrilateral. A planar patch is a rectangle, and is sampled as one.

Each kind gets:

- `intersectShape()` and `shapeNormal()` cases in C++ and GLSL, with the rounding-error bounds of [pbrt-robust-intersections.md](completed/pbrt-robust-intersections.md) once that plan lands (PBR 4ed §6.8.5 gives them for every quadric).
- A row in [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md)'s parameterization table: pbrt's (u, v), which for partial shapes span only the kept range, and dp/du, dp/dv, dn/du and dn/dv.
- A bounding box for the TLAS, tightened to the kept range for partial shapes.
- A preview mesh in `shape_mesh.cpp`.
- A `SHAPE_TRAITS` row with its params and values.

### As lights

| Kind | Sampling | Source |
|---|---|---|
| Disk, annulus | by area over the kept range; a full disk by solid angle in [pbrt-light-shape-sampling.md](pbrt-light-shape-sampling.md) | PBR 4ed §6.4.3 |
| Triangle | spherical triangle, like a mesh triangle | PBR 4ed §6.5.4 |
| Partial sphere | by area, uniform in z and φ over the kept range | PBR 4ed §6.2.4 and exercise 6.5 |
| Partial or open cylinder | by area over the kept range | PBR 4ed §6.3.3 |
| Cone | by area | closed form |
| Bilinear patch | rectangles as spherical rectangles; otherwise by area through the bilinear parameterization, warped by the emission texture when there is one | PBR 4ed §6.6.2 |
| Paraboloid, hyperboloid | not sampled | |

A partial sphere cannot use the cone sampler, which assumes the whole sphere is there. PBR 4ed exercise 6.5 notes that pbrt's own `Sphere::Sample()` makes this mistake. Area sampling over the kept z and φ range is exact.

Paraboloids and hyperboloids have no closed-form area sampling, and pbrt 3 never implemented it. Like the infinite plane today, an emissive one stays out of the light list, and kernel 04 counts whatever a BSDF sample finds on it at full weight.

## Registry, bindings and data

- `ShapeKind` gains `Disk`, `Triangle`, `Cone`, `Paraboloid`, `Hyperboloid` and `BilinearPatch`; `CRT_SHAPE_*` mirrors them.
- Binding 50 `ShapeParams` (new; the roadmap's binding table gains it).
- `SceneShape::values`; `ShapeParam` value kinds. The scene file names each value by its traits, so a shape record gains fields such as `"phiMax"` and `"innerRadius"` only when they differ from the defaults.
- No new kernel or variant.

## Verification

- A scene without the new kinds or values renders bit-identical.
- `bin/bvh_bench` gains the new kinds in its brute-force comparison, with `--perturb`: 0 mismatches between the CPU traversal and brute force over random rays.
- `bin/light_test`: for each sampled kind, the area pdf integrates to 1 over the kept surface, and samples lie on it.
- A disk light and a sphere light of equal power and area converge to the same far-field illumination.
- A partial sphere with φmax = π shows a clean cut, and its interior is visible through it (two-sided surfaces).
- A flat bilinear patch renders the same image as a quad in the same place, to within one ulp of `t`.

## Order of work

1. `ShapeParam` values, `SceneShape::values`, binding 50.
2. Disk and annulus, then the triangle.
3. Partial spheres and cylinders, and open cylinders.
4. Bilinear patches.
5. Cones, paraboloids and hyperboloids.

## Sources

PBR 4ed §6.1 to §6.4 (spheres, cylinders and disks, partial by zmin, zmax and φmax), §6.5.4 (triangle sampling), §6.6 (bilinear patches), §6.8.5 (error bounds for quadrics), exercise 6.5; PBR 3ed §3.5 (cones, paraboloids and hyperboloids); Reshetov 2019, *Cool patches: a geometric approach to ray/bilinear patch intersections* (Ray Tracing Gems ch. 8); Shirley, *Ray Tracing: The Next Week*, "Additional 2D Primitives"; pbrt-v4 `src/pbrt/shapes.h`.
