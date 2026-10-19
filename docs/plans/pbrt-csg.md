# Constructive solid geometry

Constructive solid geometry (CSG) builds a solid from others by union, intersection and difference: a cylinder with spherical pits, a lens as the intersection of two spheres, a box with a hole bored through it. PBR 4ed exercise 6.10 asks for it, pointing at Roth 1982 for ray casting CSG models and at Amanatides and Mitchell 1990 for its accuracy problems. This plan adds CSG over the analytic shapes as one more shape kind, so the traversal does not change.

*Not implemented. Depends on [pbrt-analytic-shapes.md](pbrt-analytic-shapes.md) (shape values and more closed kinds) and [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md). Part of the [roadmap](pbrt-roadmap.md).*

## Relation to the optics plans

On 2026-10-18 the user chose surface-list optical bodies ([physoptics-optical-surfaces.md](physoptics-optical-surfaces.md)) over CSG for lenses and prisms, because CSG cannot express an internal interface (a cube beamsplitter's diagonal, a cemented doublet) and a lens prescription is already a list of surfaces. This plan's lens made from two spheres stays as a test: rendered beside the same lens as an optical body, it must give the same image within noise.

## What the renderer does today

- Each analytic shape is an instance whose `intersectShape()` returns the closest hit only.
- Every shape is two-sided, and nothing records which side of a closed shape is inside.

## Design

### Operands and intervals

CSG needs, for each operand, where the ray is inside it. For a convex closed shape that is one interval `[t_in, t_out]`, from the two roots its intersection test already computes before discarding the far one. The operands are the convex closed kinds:

- sphere, box, capped cylinder, and the cone with its base, from [pbrt-analytic-shapes.md](pbrt-analytic-shapes.md);
- the infinite plane, as a half-space, whose interval runs from its crossing to infinity on one side.

Meshes, partial shapes and open shapes are not operands: a mesh would need every hit along the ray from its BLAS, and an open shape has no inside.

`crt_shape.glsl` gains `shapeInterval(kind, values, ray)`, returning the interval and the normal at each end, mirrored in `shape.cpp`.

### The CSG shape

A new kind, `Csg`, whose instance holds a small tree: at most 8 operand shapes, each with its own transform relative to the CSG object and its own material, combined by union, intersection and difference nodes. The tree is stored flattened in binding 51, `CsgNodes` (new; the roadmap's binding table gains it), and the instance's `ShapeParams` entry points at its first node.

Intersecting it follows Roth 1982: compute each operand's interval in its own object space, then combine them bottom-up as sorted interval lists (at most 8 intervals, fixed-size arrays in registers), and return the first boundary of the result's first interval that lies beyond the ray's minimum distance. Because the whole evaluation sits behind `intersectShape()`, every traversal strategy, both CPU mirrors and `bvh_bench` get CSG without other changes.

The hit records which operand the boundary came from:

- The normal is that operand's, flipped when the operand is subtracted, so a hole bored through a box has normals facing into the hole.
- The material is that operand's. A sphere cut out of a marble box shows the sphere's material on the cut surface, unless the user sets the CSG object's material to override every operand.
- [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md)'s record holds the operand index in `primitive` and the operand's own parameters in `barycentrics`, so the interaction rebuilds from the operand like any shape.

The CSG object's bounds for the TLAS: for a union, the union of the operands' boxes; for an intersection, their intersection; for a difference, the first operand's box.

### Accuracy

Where two operands' surfaces coincide, such as two boxes unioned face to face, their interval ends are equal up to rounding, and the combination can leave a sliver of wrong inside or outside along the seam. Amanatides and Mitchell 1990 discuss this regularization problem. The interval combination treats ends within the intersection's error bound ([pbrt-robust-intersections.md](completed/pbrt-robust-intersections.md)) as equal, which removes the seams.

### Editing and lights

- The Scene panel gains a **CSG** folder. A CSG object lists its operands and operations as a small tree, with **Add operand**, an operation combo per node, and **Edit Transform** per operand, relative to the object.
- The raster preview draws each operand's preview mesh translucently, since a triangle preview of the combined solid would need mesh booleans.
- An emissive CSG object stays out of the light list, and BSDF samples find it at full weight, as for the infinite plane.

## Registry, bindings and data

- `ShapeKind::Csg`; binding 51 `CsgNodes`.
- `shapeInterval()` in `crt_shape.glsl` and `shape.cpp`.
- Scene file: a CSG record with its tree of operands (kind, transform, size, values, material) and operations.

## Verification

- A scene without CSG renders bit-identical.
- `bin/bvh_bench` with CSG objects: the traversal matches brute force with 0 mismatches over random rays, with `--perturb`.
- The union of two disjoint spheres renders the same image as the two spheres placed alone, within noise.
- A sphere intersected with a half-space renders the same image as the partial sphere with the matching `zmax` from [pbrt-analytic-shapes.md](pbrt-analytic-shapes.md).
- A box unioned with an identical box face to face shows no seam.
- The lens formed by intersecting two spheres, in glass, focuses light as a thick lens should.

## Order of work

1. `shapeInterval()` for the sphere and box, with tests against the existing closest-hit code.
2. The `Csg` kind with union, intersection and difference over those two, and `bvh_bench`.
3. The remaining operand kinds and the half-space.
4. The Scene panel's tree editor and the preview.
5. Seam handling with error bounds.

## Sources

PBR 4ed exercise 6.10; Roth 1982, *Ray casting for modeling solids*; Amanatides and Mitchell 1990, *Some regularization problems in ray tracing*; Hoffmann 1989, *Geometric and solid modeling: an introduction*.
