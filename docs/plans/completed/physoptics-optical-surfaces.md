# Optical surfaces and bodies

A lens is two spherical caps cut to a diameter and joined by a ground edge. A cube beamsplitter is two prisms cemented along a diagonal that reflects. An off-axis parabola is an eccentric patch of a paraboloid. None of these is a unit primitive under a scale, which is all `ShapeKind` can express, and none survives tessellation: facets a few microns off the true sag smear a focus that should be diffraction limited. Optical design software describes such elements as lists of surfaces, each with a sag function, a semi-diameter and the media on either side. This plan adds that description as a new compound primitive, the optical body, with an exact intersector in GLSL and a line-for-line C++ mirror in a new library, `rt_optics`.

*Implemented 2026-10-29, together with [physoptics-optical-materials.md](physoptics-optical-materials.md) and [physoptics-optical-elements.md](physoptics-optical-elements.md); see "As built" at the end for what changed from the design below. Depends on [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md) (the hit's primitive and surface coordinates) and [pbrt-robust-intersections.md](pbrt-robust-intersections.md) (spawn offsets). Part of the [laser optics plan](../physoptics-laser-optics.md).*

## Decisions already made

On 2026-10-18 the user chose surface-list bodies over CSG ([pbrt-csg.md](../pbrt-csg.md)) and over tessellated meshes. Every catalog element ([physoptics-optical-elements.md](physoptics-optical-elements.md)), every imported `.zmx` lens and every pbrt lens prescription ([physoptics-prescription-import.md](../physoptics-prescription-import.md)) becomes the same kind of body, intersected by the same code.

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
| Toroid | from [physoptics-gratings-and-xuv.md](../physoptics-gratings-and-xuv.md) | toroidal XUV mirrors |

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

The surrounding medium is the medium the body sits in: air by default ([physoptics-optical-materials.md](physoptics-optical-materials.md) gives air its Ciddor index), or the outside medium of the instance's medium interface once [pbrt-participating-media.md](../pbrt-participating-media.md) exists. A body inside another body, a lens immersed in a water cell, needs that medium interface; until then the outer body's glass must be set as the inner body's surrounding medium by hand, and the panel says so.

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

A body is a shape instance (`nodeBase == CRT_INSTANCE_SHAPE`) whose `triangleBase` holds `CRT_SHAPE_OPTIC`, tag 255. The tag sits outside the `ShapeKind` range that `SHAPE_TRAITS` covers, so the Geometry menu never offers it, and [pbrt-analytic-shapes.md](../pbrt-analytic-shapes.md) and [pbrt-csg.md](../pbrt-csg.md) can keep appending kinds below it. `attributeBase` is the body's block and `triangleCount` its surface count, two fields a shape leaves unused today; `materialBase` indexes `instanceMaterials[]` by material slot, as a mesh's does by surface. `GpuInstance` does not grow.

`testShapeInstance()` branches on the tag and calls `testOpticBody()` from the new `crt_optic.glsl`, which loops over the body's surfaces in the instance's object space and keeps the closest valid hit. Because the branch sits inside the function every strategy already calls, both BVH layouts, the linear scan, the kernel 08 any-hit walk and every future traversal get optics without a change to a strategy file. The hit records the surface index as the thin hit record's `primitive` and the hit's surface-frame `(x, y)` as its `barycentrics`, so `crt_interaction.glsl` rebuilds the point, the normal and the shading frame from them ([pbrt-thin-hit-record.md](pbrt-thin-hit-record.md) gains this row in its parameterization table).

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

[pbrt-robust-intersections.md](pbrt-robust-intersections.md) gains the optic surfaces in its error-bound table: a conic hit is reprojected onto its surface along the normal and bounded like a quadric, a plane's error is zero along its normal.

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
- GPU parity ([pbrt-verification-harness.md](pbrt-verification-harness.md)): a batch of fixed rays through each body gives the same surface indices and the same `t` to 1e-6 relative on the GPU and in the `float` mirror.

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

## As built

Implemented on 2026-10-29, all six steps of the order of work, in the same change as the materials and elements plans.
The CPU library is `src/optic_surface.{h,cpp}` and `src/optic_mesh.{h,cpp}` in the static library `rt_optics`; the GPU
side is `shaders/rt/include/crt_optic.glsl`. `docs/physical-optics-overview.md` §6 is the readable account.

What differs from the design above:

- **The intersector.** Four changes to the conic and cylinder roots, all mirrored in `crt_optic.glsl`, came from
  `bin/optics_test` measuring the float hits against a long double reference:
    - The ray is re-centered on its closest approach to the surface frame's origin before the quadratic is formed. From
      an origin 0.15 m from a 25 mm surface, b² and 4ae cancel in the discriminant, and a near-tangent ray's two roots
      merged into one in float.
    - Each root takes one Newton step on the implicit function, evaluated at the hit point. The step is kept only when it
      moves t by at most 1e-4 of t: near a tangent the derivative vanishes, and an unlimited step put "hits" 0.4 of the
      surface's size off it.
    - An even asphere takes up to 8 Newton steps, not 4, and a root that does not satisfy its sag to 1e-5 of the point's
      size is a miss. The test's asphere (alpha4 = 50, alpha6 = -2e4 at c = 10) did not converge in 4.
    - A root that would divide by zero is marked by a flag, not a NaN: Metal compiles with fast math, where a NaN test is
      not reliable.
- **Apertures.** `APERTURE_SLACK` is 4e-6, not 1e-6, and an `Ellipse` aperture exists for a tilted back face (a wedge's,
  a plate beamsplitter's), whose cut by the edge cylinder is an ellipse in its own frame.
- **The hit point and its error.** A hit is reprojected onto its surface before the spawn offset (`opticReproject()`,
  mirrored by `reprojectOntoSurface()`): one step along the implicit function's gradient for a conic, which is exact for
  a plane; radially for a cylinder; to the sag for an asphere. The bound is gamma(12) of the point's largest coordinate
  plus the surface origin's, on every axis. The re-centering rounds a spawned ray's t by about that much, and a bound
  that vanished along a plane's normal at its center let a ray inside a flat window find the face it had just crossed.
  A white furnace through that window read 185 at its center before the fix, and 1.0000 after.
- **The record.** Binding 55 is as designed, with the payload after all of a body's surfaces and then the media table
  (`packOpticBody()`). `GpuInstance`'s two pad words are now `opticMedia` and `opticSlots`. After an optic's slot
  materials, `instanceMaterials[]` holds one coating table per surface, since a coating's table depends on the two media
  it sits between.
- **`CRT_OPTICS`**, specialization constant 6, gates every optic branch: the traversal, kernel 02, `surfaceInteraction()`
  and kernel 06. `GpuRenderer::bindingsOf()` adds bindings 55 and 56 to every kernel that declares `Instances`, rather
  than `traversalBindings()`, since kernels 04 and 06 rebuild optic hits too. `pipelineFeatures()` bit 4 carries it.
- **Bounds.** `SceneInstance` (the plan's `TlasInstance`) gains `optic`, the body in float; `buildTlas()` takes the
  body's bounds as the instance's box, and prices an optic at its surface count in the scene's SAH cost.
- **Principal curvatures** exist for conics, aspheres and cylinders, in double and float.

Verification:

- A scene without optics renders bit-identical: 10 renders against a build of the sources from before this work (the
  Cornell box, the sphere and Veach scenes, the caustic, all-materials, thin-glass, white-furnace and prism scenes in exact
  RGB, all-materials in hero 8 and the prism in bands 16), byte for byte, after the last change to the kernels.
- `bin/optics_test` at 200,000 rays: every shape's float hit is within 1e-6 of its size of the surface, measured as
  |F| / |grad F| in long double at the float point, over random and grazing rays and curvatures from 1e-6 to 1e3 per
  meter. A grazing ray (within 3 degrees of tangent, or 1e-4 of an edge) may miss or take the other root, which is
  float's limit; its hits must still lie on the surface. Normals match a finite-difference gradient to 1e-6 and
  curvatures finite differences of the normal to 1e-4.
- Closed bodies: every kind at every size, at 1,000,000 lines each in `--rays 4000000`, leaves about 1 line in 10^6 inside
  the body in float, at edges, and none in double. The test counts an event at an edge (hits within 1e-5 of the size)
  once, ignores an isolated hit in the slack band, and does not ask float of a chord shorter than 1e-3 of the size, which
  float cannot resolve.
- The lensmaker's equation for a plano-convex N-BK7 lens at 587.6 nm matches a real ray at 1e-4 of the aperture to
  1e-9 relative.
- `bin/bvh_bench` adds a lens, a prism, a cube and an off-axis parabola to its scene: 0 mismatches against brute force
  for every builder and layout, with `--perturb` too.
- **Windows** → **GPU self-tests** → **Optic parity**: 14,336 rays at every kind's body and a bi-convex lens give the same
  surface on the GPU and in the float mirror, with t within 1.4e-6 relative and normals within 9.1e-6. No surface
  decision flips at a rim.
