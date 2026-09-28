# Subdivision surfaces and displacement

A coarse control mesh can describe a smooth surface: Loop subdivision refines it towards a limit surface with continuous normals (PBR 3ed §3.8; pbrt-v4's `loopsubdiv` shape). A displacement texture can move a mesh's surface for real, where bump mapping only changes its shading. pbrt-v4 does both on the CPU at load time: it subdivides, or splits triangles until every edge is shorter than an `edgelength`, displaces the vertices along their normals, and hands the resulting triangles to the ordinary mesh path. This plan does the same, so the GPU traversal does not change.

*Not implemented. Displacement textures come from [textures.md](textures.md) (CPU-side images) and share the material parameter [bump-mapping.md](bump-mapping.md) introduces. Part of the [roadmap](roadmap.md).*

## What the renderer does today

- Meshes are the glTF triangles as authored. `RaytraceRenderer::prepareGeometry()` builds one BLAS per `MeshAsset` at import and caches it (`rt_accel.h`).
- No material displaces geometry, and [bump-mapping.md](bump-mapping.md) plans displacement as shading only.

## Design

### Where refinement happens

Refinement produces new triangles for the BLAS builder, on the CPU, before the BLAS build. The BLAS cache, keyed today by `MeshAsset`, becomes keyed by the mesh and its refinement settings, so two objects that share a mesh and settings share one refined BLAS. The kernels, the traversal and `bvh_bench` see ordinary triangles.

The raster preview keeps drawing the original mesh. A **Preview refined mesh** option uploads the refined triangles to the preview too, for checking.

### Loop subdivision

pbrt's `LoopSubdivide()` (PBR 3ed §3.8.1 and §3.8.2; pbrt-v4 `util/loopsubdiv.cpp`):

- Each level splits every triangle into four, and moves each vertex by Loop's weights: the even (existing) vertices by their valence, the odd (new) vertices from the two triangles sharing their edge. Boundary edges use the boundary rules, so open meshes keep their borders.
- After the last level, vertices move to their limit positions and get limit-surface normals from the tangent masks, so the shading is smooth even at few levels.
- The mesh must be manifold. A non-manifold edge stops subdivision of that object with a load warning.

A **Subdivision levels** setting per mesh object, 0 (off) by default, up to 5. Each level multiplies the triangle count by 4, and the panel shows the count before it is applied: a 10,000-triangle mesh at level 4 is 2.56 million triangles.

glTF files have no subdivision flag, so the user sets it per object. Scenes imported from pbrt ([pbrt-scene-import.md](pbrt-scene-import.md)) bring their `loopsubdiv` shapes with their `levels`.

### Displacement

pbrt-v4's `TriQuadMesh::Displace()`, applied to a mesh whose material has a displacement texture and whose **Displacement** mode is set to geometry:

1. Refine: split each triangle's longest edge at its midpoint until every edge is shorter than the **Edge length** (world units), recording split edges in a map so that neighbouring triangles split shared edges identically, which prevents cracks. Positions, normals and uvs interpolate at each split.
2. Displace: move each vertex along its interpolated normal by the texture's value at its uv, times the displacement scale.
3. Recompute vertex normals from the displaced triangles.

The texture is evaluated on the CPU from the decoded image [textures.md](textures.md) keeps for textured emission. Procedural displacement waits for [procedural-textures.md](procedural-textures.md); its noise functions would need CPU mirrors, which the shared `crt_noise.glsl` plan already includes.

The **Displacement** mode per object is **Bump** (shading only, [bump-mapping.md](bump-mapping.md), the default) or **Geometry** (this plan). With geometry displacement the material's bump mapping is skipped for that object, since the geometry already carries the detail. pbrt's `displacementedgescale` option, which scales every edge length for quick previews, becomes a global **Displacement detail** setting.

Subdivision and displacement compose: subdivide first, then displace the subdivided mesh.

### Cost

Refinement runs on a worker thread when the settings change, and the object keeps its previous BLAS until the new one is ready, so the editor does not freeze. The render guard prices frames from the refined BLAS, since a displaced mesh can hold far more triangles than its source.

## Registry, bindings and data

- No new kernel, variant or binding.
- `src/mesh_refine.cpp`: Loop subdivision and edge-length displacement, pure CPU and unit-tested.
- Per mesh object: **Subdivision levels**, **Displacement** mode, **Edge length**; the global **Displacement detail**. All in the scene file.
- The BLAS cache key gains the refinement settings.

## Verification

- With every object at 0 levels and bump displacement, every scene renders bit-identical, and no BLAS is rebuilt.
- `bin/mesh_refine_test`: a subdivided tetrahedron and cube match pbrt's `loopsubdiv` output for the same control mesh and level count, vertex for vertex, to 1e-5; refinement leaves no edge longer than the edge length; a displaced closed mesh stays closed (every edge still shared by two triangles).
- A plane displaced by a checker texture shows real silhouettes and self-shadowing that the bump-mapped version lacks, at the cost the panel predicted.
- `bin/bvh_bench` on a displaced mesh: 0 mismatches against brute force.

## Order of work

1. `mesh_refine.cpp` with the edge-length refinement and its tests.
2. Displacement from image textures, the per-object mode, and the BLAS cache key.
3. Loop subdivision with limit positions and normals.
4. The worker thread and the refined preview.

## Sources

PBR 3ed §3.8 (subdivision surfaces: mesh representation, Loop's rules, boundaries, limit positions and tangents); PBR 4ed chapter 6 "Further Reading" (subdivision and displacement) and chapter 10 "Further Reading" (displacement mapping); pbrt-v4 `src/pbrt/util/loopsubdiv.cpp`, `util/mesh.h` (`TriQuadMesh::Displace()`), `shapes.cpp` (the `plymesh` displacement and `loopsubdiv` shapes), `cmd/plytool.cpp` (`displace`); Loop 1987, *Smooth subdivision surfaces based on triangles*.
