# Bump mapping and shading normals

The tracer applies glTF normal maps and nothing else: no material can perturb its shading normal from a height or displacement texture, which is what pbrt's `BumpMap()` does. Shading normals that differ from the geometry also cause artifacts the tracer does not guard against: black or faceted terminators on low-polygon meshes, and light leaking through surfaces whose shading normal faces a light the geometry hides. This plan adds displacement-based bump mapping and the two known fixes for those artifacts.

*Not implemented. Depends on [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md) (geometric normal, dp/du, dn/du; normal mapping moved to the material kernel) and [pbrt-textures.md](pbrt-textures.md) (scalar texture references, footprints). Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- Normal maps are applied in kernel 02 (`normalMapped()` in `crt_intersect.glsl`), and [pbrt-thin-hit-record.md](pbrt-thin-hit-record.md) moves them into material evaluation unchanged.
- A mapped normal that faces away from the incoming ray is dropped.
- Nothing compares the shading normal with the geometric normal, since the geometric normal is not kept.

## Design

### Bump mapping

A material may carry a scalar `displacement` texture reference ([pbrt-material-system.md](pbrt-material-system.md)). The displaced surface is `p' = p + d(p) n(p)`, and its normal follows from finite differences of the displacement in u and v (PBR 4ed §10.5.4):

```text
du = 0.5 × (|∂u/∂x| + |∂u/∂y|), or 0.0005 when the footprint gives nothing
∂p'/∂u ≈ ∂p/∂u + (d(u + du, v) − d(u, v)) / du × n + d(u, v) × ∂n/∂u
```

and the same in v. The shading normal becomes their cross product, flipped to the side of the original. Three texture evaluations per hit, so a material with a displacement texture is complex ([pbrt-material-system.md](pbrt-material-system.md)).

The step `du` comes from the ray-cone footprint ([pbrt-textures.md](pbrt-textures.md)) when there is one, so bump detail filters away with distance like texture detail does.

Displacement and normal maps are exclusive per material, as in pbrt: a material has at most one of them.

### Shadow terminators

A smooth shading normal on a coarse mesh lights points whose geometry faces away from the light, and the shadow ray from such a point hits the mesh itself: the terminator comes out jagged and black. Two families of fix, either selectable:

- A shadowing term (Chiang, Li and Burley 2019; Conty Estevez, Lecocq and Stein 2019, the two PBR 4ed cites): multiply each light sample's contribution by a microfacet-style shadowing function of the angle between the geometric and shading normals, which fades the terminator smoothly. Chiang's form is `G = min(1, cos θ_g / (cos θ_s cos θ_gs))`, clamped and smoothed.
- Offsetting the shadow ray's origin (Hanika 2021): move the origin toward where the smooth surface interpolated from the vertex normals would be, so the shadow ray starts above the facet that would otherwise block it.

The first is a per-light-sample weight in kernel 06; the second changes the spawn point of shadow rays. A **Shadow terminator** setting chooses off (default, bit-identical), shadowing term or offset.

### Shading normals that point into the surface

When the shading and geometric normals disagree about which side a direction is on, a BSDF sample can leave on the wrong side, and a light sample can light a surface from behind. pbrt handles this per BxDF by checking the geometric normal for reflection versus transmission. The new BxDFs of [pbrt-bsdf-library.md](pbrt-bsdf-library.md) do the same: a sampled direction is rejected when the geometric normal says it crosses the surface and the lobe is reflective, or does not cross and the lobe is transmissive. The legacy materials keep today's behavior.

## Verification

- Every scene with neither feature renders bit-identical.
- A sphere with a wrinkled (turbulence) displacement matches PBR 3ed Figure 10.31's look, once [pbrt-procedural-textures.md](pbrt-procedural-textures.md) supplies turbulence; until then an image height map is the test.
- A 100-triangle sphere lit from the side: jagged black terminator with the setting off, smooth with either fix.
- A thin wall with a normal map that tilts toward a light behind it: no leaked light with the geometric-normal check on.

## Order of work

1. The geometric-normal check in the new BxDFs.
2. Bump mapping with image displacement textures.
3. The two terminator fixes.

## Sources

PBR 4ed §10.5.3 (normal mapping), §10.5.4 (bump mapping), chapter 10 "Further Reading" (shadow terminators, bump map filtering); PBR 3ed §9.3; Chiang, Li and Burley 2019, *Taming the shadow terminator*; Conty Estevez, Lecocq and Stein 2019, *A microfacet-based shadowing function to solve the bump terminator problem* (Ray Tracing Gems ch. 12); Hanika 2021, *Hacking the shadow terminator* (Ray Tracing Gems II ch. 4); pbrt-v4 `src/pbrt/materials.h` (`BumpMap`, `NormalMap`).
