# Thin hit record and surface interactions

Kernel 02 resolves every hit into a 48-byte record: position, the shading normal after normal mapping, uv, material, face and light index. Materials, bump mapping, anisotropy, ray cones, error-bounded offsets and media all need more than that record holds: the geometric normal, a tangent frame, and position derivatives. This plan makes the hit record thin, as pbrt-v4 does, and adds one shared include that rebuilds the full surface interaction wherever it is needed.

*Implemented 2026-10-18; the as-built record is the last section before the sources. No prerequisites. Part of the [roadmap](../pbrt-roadmap.md).*

## Decision already made

On 2026-09-26 the user chose the thin record over growing today's record to 80 to 96 bytes.

## What the renderer does today

- `HitRecord` (`shaders/rt/include/crt_common.glsl`) is 48 bytes: position, t, normal, material and face, uv, traversal cost and light index.
- `crt_intersect.glsl` interpolates the shading normal and applies the normal map in kernel 02, then flips the normal to face the ray. The geometric normal is not stored anywhere, so no later kernel can detect a shading normal that points into the surface.
- Shapes have no parametric (u, v) coordinates and no tangent, so they cannot be textured and have no frame for anisotropic reflection.
- Kernel 08 reads a shadow ray's origin from `hits[]`, which is why nothing may write `hits[]` between 06 and 08 (codebase map §8.15).

## Design

### The record

```glsl
// 24 bytes, by ray queue position. t < 0 is a miss
struct HitRecord {
    float t;
    uint instance;          // into instances[]
    uint primitive;         // a mesh: BvhTriangle index; a shape: its part (box face, cylinder cap)
    vec2 barycentrics;      // a triangle's (u, v); a shape's object-space parameters, by kind
    float traversalCost;    // the debug view's input
};
```

The light index and the material no longer live in the record. Kernel 04 finds the light from `instance` and `primitive` as kernel 02 does today; the material comes from the instance and, for a triangle, its surface.

### Classification in kernel 02

Kernel 02 still sorts hits onto the escaped, emissive and surface queues, and after [pbrt-material-system.md](../pbrt-material-system.md) onto the basic or complex surface queue. It needs the material for that, but not the attribute record:

- A triangle's surface index moves into `BvhTriangle.e1.w`, which is unused today, so the material is `instanceMaterials[instance.materialBase + surface]` with no attribute fetch.
- A shape's material is `instance.materialBase`, as now.
- Each material record carries flag bits (emits, scatters, complex), so classification reads one word.

The alpha test stays inside the traversal, unchanged: it already fetches the uv only for triangles flagged as cutouts.

### `crt_interaction.glsl`

One include rebuilds what a kernel needs from a hit and the ray that made it:

```glsl
struct SurfaceInteraction {
    vec3 p;             // world space
    vec3 pError;        // absolute error bound (pbrt-robust-intersections.md fills it; zero until then)
    vec3 n;             // geometric normal, unit, pointing out of the surface as authored
    vec3 ns;            // shading normal, interpolated, unit, same side as n
    vec3 dpdu, dpdv;    // position derivatives; dpdu is the tangent of the shading frame
    vec3 dndu, dndv;    // normal derivatives, for ray cones and bump mapping
    vec2 uv;
    uint material;
    uint lightIndex;    // CRT_NO_LIGHT when the surface is not in the light list
    bool frontFace;     // the ray arrived against n
};

SurfaceInteraction surfaceInteraction(HitRecord hit, vec3 rayOrigin, vec3 rayDirection);
```

Kernels 04, 06, the medium kernels and the auxiliary-output writes call it. Kernel 03 needs only the direction and does not.

Normals keep pointing outward as authored, as in pbrt (PBR 4ed §9.1.1). The existing materials expect a normal that faces the incoming ray, so `crt_scatter.glsl` flips `ns` itself at entry, exactly as kernel 02 does today; the BxDFs of [pbrt-bsdf-library.md](../pbrt-bsdf-library.md) take the outward frame and handle the side in their local frame.

### Normal mapping moves to the material kernel

Normal maps are a property of the material, so the mapping moves from `crt_intersect.glsl` into material evaluation (`resolveMaterial()` in `crt_scatter.glsl`, later `crt_material.glsl`). The arithmetic moves unchanged, including the rule that a mapped normal tilting past the viewer is dropped. [pbrt-bump-mapping.md](../pbrt-bump-mapping.md) adds displacement-based bump mapping in the same place.

### Shape parameterization

Each shape kind gets PBR's parametric coordinates and derivatives, in `crt_shape.glsl` and `shape.cpp` together:

| Shape | (u, v) | dp/du |
|---|---|---|
| Sphere | φ / 2π, θ / π | along increasing φ |
| Cylinder | φ / 2π on the side, polar on the caps | along φ |
| Quad | object x and z mapped to [0, 1] | object +x |
| Box | per face, that face's two axes mapped to [0, 1] | the face's first axis |
| Plane | world x and z, unscaled, so a texture tiles once per unit | +x |
| Optical body ([physoptics-optical-surfaces.md](../physoptics-optical-surfaces.md)) | the hit's x and y in its surface's own frame, with `primitive` the surface's index in the body | the surface frame's +x |

`shapeParameters(kind, objectPoint)` returns the uv, dp/du and dp/dv. Kernel 02 stores what it needs in `barycentrics` (a sphere's φ and θ, for example), so the interaction does not solve for them again.

### Shadow rays carry their origin

`ShadowRay` grows from 32 to 48 bytes with an explicit origin. Kernel 06 writes the spawn point from the interaction, and kernel 08 stops reading `hits[]`. This removes the trap in codebase map §8.15 and lets [pbrt-robust-intersections.md](../pbrt-robust-intersections.md) offset both ends of a shadow ray.

### Instances

Rebuilding a triangle's position from barycentrics, which [pbrt-robust-intersections.md](../pbrt-robust-intersections.md) needs for its error bounds, requires the object-to-world transform as well as today's world-to-object rows. `GpuInstance` takes three more `vec4` rows and grows from 80 to 128 bytes. Until that plan lands, `p` is `rayOrigin + t · rayDirection`, exactly as today, so this plan does not change any image.

## Data and bindings

| Struct | Before | After |
|---|---|---|
| `HitRecord` | 48 bytes | 24 bytes |
| `ShadowRay` | 32 bytes | 48 bytes |
| `GpuInstance` | 80 bytes | 128 bytes |
| `BvhTriangle` | 48 bytes, `e1.w` unused | 48 bytes, `e1.w` holds the surface index |

The C++ mirrors (`rt_gpu.h`, `rt_accel.h`, `bvh_layout.h`) change with them, each under its `static_assert`. No binding number changes.

Pool memory at the 4M-path cap drops by 32 MB net: 96 MB saved on hits, 64 MB spent on shadow rays.

## Order of work

1. Move normal mapping from kernel 02 to material evaluation, with the hit record unchanged. Bit-identical.
2. Store the surface index in `e1.w` and read materials in kernel 02 from it.
3. Add `crt_interaction.glsl` and move kernels 04 and 06 onto it while kernel 02 still writes the full record, so both paths can be compared field by field in a debug build.
4. Shrink `HitRecord`, add the shadow-ray origin, and delete the old fields.
5. Shape parameterization, mirrored in `shape.cpp`, checked by `bvh_bench` (uv continuity across the sphere seam and the box edges).
6. Object-to-world rows in `GpuInstance`.

## Verification

- Steps 1 to 4 render bit-identical accumulation buffers for the Cornell box, the sphere scene, the Veach scene and Sponza with a fixed seed, under every kernel 02 and 06 variant.
- The normal debug view shows the same image.
- A debug view of the new geometric normal against the shading normal shows where the two disagree by more than 90 degrees; the count should be zero on the Cornell box and small on Sponza.

## Open questions

- Settled on 2026-10-18: `surfaceInteraction()` computes a triangle's `dndu` and `dndv` from its vertex normals for every hit, as pbrt does, with no opt-in parameter. A kernel that never reads them leaves the arithmetic to the compiler, which drops it.

## As built

Implemented 2026-10-18. The user settled four questions the design left open:

- Kernel 02 classifies from the material's `type` and `emission`, as before. The flag bits the design gives each material record arrive with the 16-byte material header of [pbrt-material-system.md](../pbrt-material-system.md), and `GpuMaterial` stays 64 bytes.
- `dndu` and `dndv` are always computed (see "Open questions").
- `dpdu` and `dpdv` are the geometric derivatives of the parameterization; a triangle's come from its uvs. `shadingFrame()` builds the material's tangent by making `dpdu` perpendicular to the shading normal. glTF vertex tangents stay with the normal map, and there is no separate shading block as in pbrt-v4.
- The infinite plane's uv is object x and z rather than world x and z, so a texture turns with a rotated plane. The plane has no size, so it still tiles once per unit.

What landed:

- `HitRecord` (`crt_common.glsl`) is 24 bytes in the order `t`, `instance`, `barycentrics`, `primitive`, `traversalCost`. The order in the design leaves the `vec2` at offset 12, which std430 pads to 16, and the struct to 32 bytes.
- `shaders/rt/include/crt_interaction.glsl` holds `SurfaceInteraction`, `surfaceInteraction()`, `facingShadingNormal()` and `shadingFrame()`. A triangle's geometric normal is turned to the side of its vertex normals, pbrt-v4's `orientationIsAuthoritative`, so `n` and `ns` are on the same side.
- Kernel 02 (`crt_intersect.glsl`) writes the thin record and reads a triangle's material through `BvhTriangle.e1.w`, which `buildMeshBlas()` in `rt_accel.cpp` fills. For a shape it stores `shapePart()` and `shapeUv()`.
- The normal map moved to `materialShadingNormal()` in `crt_scatter.glsl` with its arithmetic unchanged. Kernel 04 uses `facingShadingNormal()` for its debug views, since only `pbr` materials carry maps and kernel 06 writes their debug views.
- `ShadowRay` is 48 bytes, with `origin` and a reserved `mediumAndGroup` word first. Kernel 08 no longer declares `Hits`.
- `GpuInstance` is 128 bytes: the world-to-object rows are now `toObject[3]`, and `toWorld[3]` holds the object-to-world rows.
- `crt_shape.glsl` and `shape.cpp` gained `shapePart()`, `shapeUv()` and `shapeDerivatives()`, and `shapeNormal()` is written on top of `shapePart()`. Every shape follows glTF's orientation: seen from outside, u runs right and v runs down, so an image appears upright with the existing texture sampling, and `cross(dpdu, dpdv)` points into the surface, the opposite of pbrt. A cylinder's caps are polar with v the radius, and its bottom cap's angle runs the other way so both caps keep that orientation.
- `rt_kernels.cpp` gained `withInteraction()`, the bindings `surfaceInteraction()` reads, which kernels 04 and 06 declare.
- A new debug view, "shading vs geometric normal", is red where the shading normal the materials use and the geometric normal, both turned to face the ray, are more than 90 degrees apart, and grey with their cosine elsewhere.
- `bin/bvh_bench` checks the shape parameterization before its BVH tests.

One change was needed for the bit-identical renders. The point is written `fma(vec3(t), direction, origin)` in `surfaceInteraction()` and in kernel 02. The old kernel 02 compiled `origin + t * direction` to a fused multiply-add, and kernel 04 compiled the same expression without one, so kernel 04 rebuilt points an ulp away from kernel 02's. A cylinder light's pdf depends on the exact point, and the MIS renders of a scene with one differed in about 100 floats out of 120,000 until both kernels spelled the fusion out.

Verification, with a temporary hook that opened a scene, rendered with seed 12345 and dumped the rgba32f accumulation:

- Bit-identical to the commit before: the Cornell box at 200 × 150 × 16 under the four kernel 06 variants and the three traversals, and its normal view; the sphere scene and its normal view; the Veach scene under MIS and light sampling, and its MIS-weights view; Sponza at 160 × 90 × 4 under both BVH layouts, and its normal view. `assets/scenes/Sponza.gltf` still names its model at `../Sponza/Sponza.gltf`, from before the assets moved, so the runs used a copy pointing at `assets/models/Sponza/`.
- Bit-identical to a build of the old sources: a scratch scene with a tilted plane, an emissive box and an emissive cylinder (rotated, non-uniformly scaled), glass sphere, box and cylinder, a fuzzy metal cylinder, a quad light, a Phong sphere and a glowing `pbr` box. It rendered under MIS with the power heuristic, BSDF sampling and light sampling, one traversal each, and in the normal view and the MIS-weights view, the latter under the balance heuristic.
- The traversal-cost view of Sponza is not deterministic on the old code either: two runs of the same build differ in 49 of 57,600 floats. The new build matched one of the two outcomes.
- The new debug view at 320 × 180 shows 0 red pixels on the Cornell box and on the shape scene, and 264 of 57,446 hit pixels on Sponza, along grazing walls and normal-mapped detail.
- Zero validation messages across five configurations with the validation layer on. With `TriangleLights` deliberately left out of `withInteraction()`, the layer reported `VUID-VkComputePipelineCreateInfo-layout-07988`, so it was active.
- `bin/bvh_bench assets/models/structure.glb --rays 2000`, with and without `--perturb`: PASS. Its shape check found 0 failures over about 84,000 hit points and 0 seam discontinuities. Flipping the sign of the sphere's `dpdv` made it report 20,000 failures. `bin/light_test`: PASS.

Not measured: frame time. The thin record moves 24 fewer bytes per hit through kernel 02's writes and 03, 04 and 06's reads, and kernels 04 and 06 fetch the triangle and its attributes again.

## Sources

PBR 4ed §3.11 (interactions), §6.2 to §6.5 (parametric shapes, triangle derivatives), §9.1.1 (the shading frame), §15.3.6 (the GPU returning barycentrics and building the interaction later); pbrt-v4 `src/pbrt/interaction.h`, `Triangle::InteractionFromIntersection()`.
