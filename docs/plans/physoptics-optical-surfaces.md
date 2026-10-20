# Optical surfaces and bodies

A lens is two spherical caps cut to a diameter and joined by a ground edge. A cube beamsplitter is two prisms cemented along a diagonal that reflects. An off-axis parabola is an eccentric patch of a paraboloid. None of these is a unit primitive under a scale, which is all `ShapeKind` can express, and none survives tessellation: facets a few microns off the true sag smear a focus that should be diffraction limited. Optical design software describes such elements as lists of surfaces, each with a sag function, a semi-diameter and the media on either side. This plan adds that description as a new compound primitive, the optical body, with an exact intersector in GLSL and a line-for-line C++ mirror in a new library, `rt_optics`.

*Not implemented. Depends on [pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) (the hit's primitive and surface coordinates) and [pbrt-robust-intersections.md](completed/pbrt-robust-intersections.md) (spawn offsets). Part of the [laser optics plan](physoptics-laser-optics.md).*

## Decisions already made

On 2026-10-18 the user chose surface-list bodies over CSG ([pbrt-csg.md](pbrt-csg.md)) and over tessellated meshes. Every catalog element ([physoptics-optical-elements.md](physoptics-optical-elements.md)), every imported `.zmx` lens and every pbrt lens prescription ([physoptics-prescription-import.md](physoptics-prescription-import.md)) becomes the same kind of body, intersected by the same code.

## What the renderer does today

- `ShapeKind` (`src/shape.h`) has five unit primitives. A placed shape is a `GpuInstance` with `nodeBase == CRT_INSTANCE_SHAPE` and its kind in `triangleBase`. Both traversal strategies call `testShapeInstance()` in `crt_traverse.glsl` for such an instance (`crt_bvh.glsl` line 222, `crt_linear.glsl` line 34), which calls `hitShape()` by kind and returns one `t`.
- `shapeObjectBounds(kind)` gives the TLAS a fixed unit box per kind (`src/bvh_scene.cpp` line 127). Nothing per instance.
- `hitShape()` solves the sphere with the closest-approach discriminant of *Ray Tracing Gems* chapter 7, centered at the origin. For a 1 m radius surface seen over a 25 mm aperture, the two roots differ by about 2 m and the useful one is near zero, which is the configuration where a center-based quadratic loses digits.
- The CPU mirrors (`src/shape.cpp`, `src/bvh_scene.cpp`) are `float`.

## Design

### The prescription

An optical body is described in its own frame, the body frame: origin at the element's datum (the front vertex for a lens, the reflecting vertex for a mirror, the cube's center for a cube), +z along the axis light is designed to travel. `rt_optics` holds it as plain data, templated on the scalar type:

```cpp
template <typename Real>
struct OpticSurfaceT {
    SurfaceShape shape;            // Plane, Conic, Cylinder; later Toroid, EvenAsphere (see below)
    Frame<Real> placement;         // surface frame -> body frame: a rotation and the vertex's position
    Real curvature { 0 };          // c = 1 / R; 0 is a plane, so a Conic with c = 0 IS the plane
    Real conic { 0 };              // k: 0 sphere, -1 paraboloid, < -1 hyperboloid, (-1, 0) and > 0 ellipsoids
    Aperture<Real> aperture;       // in the surface frame's xy, relative to an offset center
    uint8_t mediumMinusZ, mediumPlusZ;   // indices into the body's media; 0 is the surrounding medium
    uint16_t materialSlot;         // which of the element's materials this surface is
    uint16_t flags;                // Optical, Edge (ground or blackened), Internal (a cemented or coated interface)
};

template <typename Real>
struct OpticPrescriptionT {
    std::vector<OpticSurfaceT<Real>> surfaces;
    std::vector<OpticMedium> media;      // glass ids from physoptics-optical-materials.md; [0] is "surrounding"
    Aabb bounds;                         // body frame, conservative, computed by the builder
};
```

Each surface lives in its own frame so that faces need not share an axis: the three faces of a right-angle prism, the tilted back of a wedge, the diagonal of a cube beamsplitter and an off-axis parabola's parent vertex are all just placements. A centered lens is the common case where every placement is a translation along z.

### Surface shapes

| Shape | Surface in its own frame | Used by |
|---|---|---|
| Conic | `c (x² + y² + (1 + k) z²) − 2z = 0`, the branch through the vertex. A plane at `c = 0`, a sphere at `k = 0`, a paraboloid at `k = −1` | every flat and spherical face, off-axis parabolas |
| Cylinder | `x² + y² = r²` between two bounding planes, which are `z = z0` and `z = z1` unless the payload gives tilted ones | lens and mirror edges, round filter rims, the rim of a wedged plate |
| Even asphere | conic sag plus `Σ αᵢ ρ²ⁱ`, `i` = 2 to 8 | `.zmx` `EVENASPH` surfaces |
| Toroid | from [physoptics-gratings-and-xuv.md](physoptics-gratings-and-xuv.md) | toroidal XUV mirrors |

The conic is written in vertex form on purpose. Its quadratic in `t`, with `o` and `d` the ray in the surface frame and `kz = 1 + k`,

```text
a = c (dx² + dy² + kz dz²)
b = 2 (c (ox dx + oy dy + kz oz dz) − dz)
e = c (ox² + oy² + kz oz²) − 2 oz
```

stays well conditioned as `c → 0`, where it becomes the plane `z = 0` without a special case, and it puts the useful root near the vertex where the error is smallest. The root is taken as `t = e / q` or `q / a` with `q = −(b + sign(b) √(b² − 4ae)) / 2`, the form that never subtracts nearly equal numbers. The branch test keeps only hits with `kz c z < 1`, the half of the quadric that contains the vertex, which is the half an optical surface uses.

The even asphere starts from the conic's root and refines it with Newton's method on `z − sag(ρ) = 0`, four iterations at most, which converges to a float's precision for every asphere in Thorlabs' and Edmund's catalogs.

The surface normal is the normalized gradient of the implicit function, and `rt_optics` also returns the two principal curvatures and their directions at a hit, which the beam solver's differential ray transfer needs ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)). For a conic both have closed forms.

### Apertures

A hit counts only inside the surface's aperture, measured in the surface frame's xy plane from an offset center:

| Aperture | Parameters | Used by |
|---|---|---|
| Circle | radius | round optics |
| Annulus | inner and outer radius | an iris's blades, a mirror's chamfer ring |
| Rectangle | half width, half height | square filters, prism faces, cube faces |
| Polygon | up to 6 vertices, convex | prism end caps, the cube's diagonal |

The offset center is how an off-axis parabola works: its surface is the parent paraboloid with its vertex at the parent's, and its circular aperture is centered at the off-axis distance. An aperture test widens its bound by a relative 1e-6, so a ray through the rim where a face meets its edge cylinder is caught by one of the two rather than slipping between them.

### Media, sides and closed bodies

Each surface names the medium on its −z and +z sides. The side a ray arrives from is the sign of `d · n` in the surface frame, so the hit itself says which medium the ray leaves and which it enters: kernel 06 reads both from the surface record and needs no stack of nested media. A lens in air has media `{surrounding, N-BK7}`; a cemented doublet has three, and its cemented surface is `Internal`. A body is closed when every ray that enters it leaves it, which the builders guarantee by making each edge cylinder's `z0` and `z1` the sags of its neighboring faces at the edge radius.

The surrounding medium is the medium the body sits in: air by default ([physoptics-optical-materials.md](physoptics-optical-materials.md) gives air its Ciddor index), or the outside medium of the instance's medium interface once [pbrt-participating-media.md](pbrt-participating-media.md) exists. A body inside another body, a lens immersed in a water cell, needs that medium interface; until then the outer body's glass must be set as the inner body's surrounding medium by hand, and the panel says so.

### The GPU record

Binding 55, `OpticSurfaces`, is one array of `vec4`. Each body's block holds its surface records, six `vec4` each, followed by a payload area for the body's media table ([physoptics-optical-materials.md](physoptics-optical-materials.md)), the even asphere coefficients and the polygon vertices:

```glsl
// 96 bytes, six vec4 of opticData[], at instance.attributeBase + 6 * surface
//   [0..2]  rows of the body -> surface transform
//   [3]     curvature, conic, cylinder z0, cylinder z1
//   [4]     aperture: half extents or radii (x, y), offset center (z, w)
//   [5]     uint bits: shape | aperture kind << 8 | flags << 16, material slot,
//           media (-z | +z << 16), payload offset in vec4s from the block start (0: none)
layout (std430, set = 0, binding = 55) readonly buffer OpticSurfaceBuffer { vec4 opticData[]; };
```

A body is a shape instance (`nodeBase == CRT_INSTANCE_SHAPE`) whose `triangleBase` holds `CRT_SHAPE_OPTIC`, tag 255. The tag sits outside the `ShapeKind` range that `SHAPE_TRAITS` covers, so the Geometry menu never offers it, and [pbrt-analytic-shapes.md](pbrt-analytic-shapes.md) and [pbrt-csg.md](pbrt-csg.md) can keep appending kinds below it. `attributeBase` is the body's block and `triangleCount` its surface count, two fields a shape leaves unused today; `materialBase` indexes `instanceMaterials[]` by material slot, as a mesh's does by surface. `GpuInstance` does not grow.

`testShapeInstance()` branches on the tag and calls `testOpticBody()` from the new `crt_optic.glsl`, which loops over the body's surfaces in the instance's object space and keeps the closest valid hit. Because the branch sits inside the function every strategy already calls, both BVH layouts, the linear scan, the kernel 08 any-hit walk and every future traversal get optics without a change to a strategy file. The hit records the surface index as the thin hit record's `primitive` and the hit's surface-frame `(x, y)` as its `barycentrics`, so `crt_interaction.glsl` rebuilds the point, the normal and the shading frame from them ([pbrt-thin-hit-record.md](completed/pbrt-thin-hit-record.md) gains this row in its parameterization table).

Every traversal declares binding 55, through `traversalBindings()` in `src/rt_kernels.cpp`, since any tracing kernel can reach an optic.

### Bounds and the TLAS

`TlasInstance` (`src/bvh_scene.h`) gains an optional per-instance object-space box, which `buildTlas()` uses in place of `shapeObjectBounds(kind)` when it is set. An optic's box is its prescription's `bounds`. Optics are always bounded.

### The CPU library

`rt_optics` is a new Vulkan-free static library, `-O2` in every build type like `rt_bvh` and `rt_light`:

- `src/optic_surface.h/.cpp`: the surface shapes, apertures, `intersectOpticBody<Real>()`, normals and principal curvatures, and the closed-body helpers the builders use. The `float` instantiation is the line-for-line mirror of `crt_optic.glsl`; the `double` one is what the beam solver uses.
- `src/optic_mesh.h/.cpp`: tessellation of a prescription for the raster viewport, a ring grid per face sized so the sag error stays under 1 µm, and the edge cylinders. It feeds the glass preview of [physoptics-optical-elements.md](physoptics-optical-elements.md); it is never traced.
- `bvh_scene.cpp`'s CPU traversals call the `float` intersector for optic instances, so `bin/bvh_bench` covers optics like every other primitive.

### Precision

A float carries about 7 digits. At 0.3 m from the origin its spacing is 30 nm, which locates a surface well enough for intensity transport. The solver, which differentiates rays and accumulates path lengths over meters of beam line, uses `double` on the CPU. The future wave backend needs optical path length to a fraction of a wavelength over a whole setup, which float cannot hold; the solver's double-precision path lengths per segment are kept for it ([physoptics-gaussian-beam-solver.md](physoptics-gaussian-beam-solver.md)).

[pbrt-robust-intersections.md](completed/pbrt-robust-intersections.md) gains the optic surfaces in its error-bound table: a conic hit is reprojected onto its surface along the normal and bounded like a quadric, a plane's error is zero along its normal.

## Registry, bindings and data

- Binding 55 `OpticSurfaces`; `CrtBinding::OpticSurfaces = 55`, added to `traversalBindings()`.
- `CRT_SHAPE_OPTIC` (255) in `crt_shape.glsl` and `GPU_SHAPE_OPTIC` in `src/shape.h`, outside `ShapeKind`.
- `crt_optic.glsl`; `rt_optics` with `src/optic_surface.*` and `src/optic_mesh.*`; `tests/optics_test.cpp` → `bin/optics_test`.
- `TlasInstance` gains an optional object-space box.
- No kernel, variant or scene-file change: the elements plan decides what is saved.

## Verification

- A scene without optics renders bit-identical, and no kernel binds anything new for it.
- `bin/optics_test`:
    - every shape's `float` hit matches a `double` reference (Newton on the implicit function in `long double`) to 1e-6 of the surface's size, over random rays, including grazing ones and curvatures from 1e-6 to 1e3 m⁻¹;
    - normals match a finite-difference gradient, and principal curvatures match finite differences of the normal;
    - closed bodies: for 10⁶ random rays through a biconvex lens, a prism and a cube, every ray that enters leaves, and the parity of the hit count is even;
    - a plano-convex lens of N-BK7 at 587.6 nm has the paraxial focal length of the lensmaker's equation to 1e-9 relative in `double`, traced with real rays at 1e-4 of the aperture.
- `bin/bvh_bench` with optic bodies: 0 mismatches against brute force, with `--perturb`.
- GPU parity ([pbrt-verification-harness.md](completed/pbrt-verification-harness.md)): a batch of fixed rays through each body gives the same surface indices and the same `t` to 1e-6 relative on the GPU and in the `float` mirror.

## Order of work

1. Conic and plane surfaces, circle and rectangle apertures, in `rt_optics`, with `bin/optics_test`.
2. Cylinders, annuli and polygons; closed bodies and their bounds.
3. The CPU traversal and `bvh_bench`.
4. `crt_optic.glsl`, binding 55 and instance packing in `buildSceneAccel()`. Until [physoptics-optical-elements.md](physoptics-optical-elements.md) exists, a `/*SMOKETEST*/` hook places a hard-coded lens (memory note, headless smoke testing).
5. The preview tessellation.
6. Even aspheres, for the importer.

## Open questions

- Freeform surfaces (Zernike sag, XY polynomials, Forbes Q-polynomials) would follow the even asphere's pattern: a payload of coefficients and a Newton refinement. None of the request's elements needs one, so this plan stops at the even asphere.
- Whether the preview should render the glass through the raster pass's transparent pipeline, or as an outline only, is a look-and-feel choice for the elements plan.

## Sources

Spencer and Murty 1962, *General ray-tracing procedure*; Welford 1986, *Aberrations of optical systems*, chapter 4 (ray tracing through centered and non-centered systems); Kingslake and Johnson 2010, *Lens design fundamentals*, chapter 2; the Zemax OpticStudio manual, "Standard", "Even Asphere" and "Toroidal" surface types and the sag of a conic; PBR 3ed §6.4 (the realistic camera's spherical elements); *Ray Tracing Gems* chapter 7 (precision of ray–quadric tests); Press et al. 2007, *Numerical recipes*, §5.6 (the stable quadratic).
