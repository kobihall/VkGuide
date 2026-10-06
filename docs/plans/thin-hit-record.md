# Thin hit record and surface interactions

Kernel 02 resolves every hit into a 48-byte record: position, the shading normal after normal mapping, uv, material, face and light index. Materials, bump mapping, anisotropy, ray cones, error-bounded offsets and media all need more than that record holds: the geometric normal, a tangent frame, and position derivatives. This plan makes the hit record thin, as pbrt-v4 does, and adds one shared include that rebuilds the full surface interaction wherever it is needed.

*Not implemented. No prerequisites. Part of the [roadmap](roadmap.md).*

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

Kernel 02 still sorts hits onto the escaped, emissive and surface queues, and after [material-system.md](material-system.md) onto the basic or complex surface queue. It needs the material for that, but not the attribute record:

- A triangle's surface index moves into `BvhTriangle.e1.w`, which is unused today, so the material is `instanceMaterials[instance.materialBase + surface]` with no attribute fetch.
- A shape's material is `instance.materialBase`, as now.
- Each material record carries flag bits (emits, scatters, complex), so classification reads one word.

The alpha test stays inside the traversal, unchanged: it already fetches the uv only for triangles flagged as cutouts.

### `crt_interaction.glsl`

One include rebuilds what a kernel needs from a hit and the ray that made it:

```glsl
struct SurfaceInteraction {
    vec3 p;             // world space
    vec3 pError;        // absolute error bound (robust-intersections.md fills it; zero until then)
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

Normals keep pointing outward as authored, as in pbrt (PBR 4ed §9.1.1). The existing materials expect a normal that faces the incoming ray, so `crt_scatter.glsl` flips `ns` itself at entry, exactly as kernel 02 does today; the BxDFs of [bsdf-library.md](bsdf-library.md) take the outward frame and handle the side in their local frame.

### Normal mapping moves to the material kernel

Normal maps are a property of the material, so the mapping moves from `crt_intersect.glsl` into material evaluation (`resolveMaterial()` in `crt_scatter.glsl`, later `crt_material.glsl`). The arithmetic moves unchanged, including the rule that a mapped normal tilting past the viewer is dropped. [bump-mapping.md](bump-mapping.md) adds displacement-based bump mapping in the same place.

### Shape parameterisation

Each shape kind gets PBR's parametric coordinates and derivatives, in `crt_shape.glsl` and `shape.cpp` together:

| Shape | (u, v) | dp/du |
|---|---|---|
| Sphere | φ / 2π, θ / π | along increasing φ |
| Cylinder | φ / 2π on the side, polar on the caps | along φ |
| Quad | object x and z mapped to [0, 1] | object +x |
| Box | per face, that face's two axes mapped to [0, 1] | the face's first axis |
| Plane | world x and z, unscaled, so a texture tiles once per unit | +x |

`shapeParameters(kind, objectPoint)` returns the uv, dp/du and dp/dv. Kernel 02 stores what it needs in `barycentrics` (a sphere's φ and θ, for example), so the interaction does not solve for them again.

### Shadow rays carry their origin

`ShadowRay` grows from 32 to 48 bytes with an explicit origin. Kernel 06 writes the spawn point from the interaction, and kernel 08 stops reading `hits[]`. This removes the trap in codebase map §8.15 and lets [robust-intersections.md](robust-intersections.md) offset both ends of a shadow ray.

### Instances

Rebuilding a triangle's position from barycentrics, which [robust-intersections.md](robust-intersections.md) needs for its error bounds, requires the object-to-world transform as well as today's world-to-object rows. `GpuInstance` takes three more `vec4` rows and grows from 80 to 128 bytes. Until that plan lands, `p` is `rayOrigin + t · rayDirection`, exactly as today, so this plan does not change any image.

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
5. Shape parameterisation, mirrored in `shape.cpp`, checked by `bvh_bench` (uv continuity across the sphere seam and the box edges).
6. Object-to-world rows in `GpuInstance`.

## Verification

- Steps 1 to 4 render bit-identical accumulation buffers for the Cornell box, the sphere scene, the Veach scene and Sponza with a fixed seed, under every kernel 02 and 06 variant.
- The normal debug view shows the same image.
- A debug view of the new geometric normal against the shading normal shows where the two disagree by more than 90 degrees; the count should be zero on the Cornell box and small on Sponza.

## Open questions

- Whether `dndu` and `dndv` are computed for triangles at all before a feature needs them. pbrt computes them from vertex normals for every hit; ray cones ([textures.md](textures.md)) and bump mapping are the only consumers, so they can be computed lazily behind a function parameter.

## Sources

PBR 4ed §3.11 (interactions), §6.2 to §6.5 (parametric shapes, triangle derivatives), §9.1.1 (the shading frame), §15.3.6 (the GPU returning barycentrics and building the interaction later); pbrt-v4 `src/pbrt/interaction.h`, `Triangle::InteractionFromIntersection()`.
