# BVH acceleration for the GPU path tracer

*Implemented 2026-09-18. Replaces the extend stage's linear scan over every world-space triangle.*

## 0. Scope

The path tracer (`docs/plans/completed/compute-pipeline-raytracing.md`) tested every ray against every triangle. A 1M-triangle model at 1080p was ~2e12 tests in one dispatch, which hung the GPU and panicked the kernel once. This adds a two-level bounding volume hierarchy:

- **BLAS**: one per unique glTF mesh, in the mesh's object space. Built once when the model is imported, never rebuilt by scene edits.
- **TLAS**: over the placed mesh objects and the analytic spheres. Rebuilt whenever an object or sphere changes. The ray is carried into each instance's object space, so translation, rotation and non-uniform scale all work on an unchanged BLAS.

Construction methods and node layouts are modular. Any builder feeds any layout, both can be switched at runtime from the panel, and the defaults are one function (`defaultAccelSettings()`).

Sources: Shirley, *Ray Tracing: The Next Week* §3 and §8 (BVH and instances); Bikker, *How to build a BVH*, parts 1–2 (node, midpoint split, SAH, ordered traversal); Sebastian Lague's *Ray-Tracing* (a per-model BVH with world-to-local ray transforms; it scans models linearly and transforms normals by local-to-world, which this implementation corrects: TLAS over models, inverse-transpose normals); karimsayedre's CUDA RTIOW (full-sweep SAH, FMA slab test, node reordering); GPSnoopy's *RayTracingInVulkan* (the hardware BLAS/TLAS model, and attribute fetch only for the closest hit). Beyond those: Wald 2007 (binned SAH), Stich et al. 2009 (SBVH), Aila & Laine 2009 (binary GPU layout), Ylitie et al. 2017 (CWBVH).

## 1. Modules

| File | What |
|---|---|
| `src/bvh.h`, `bvh_build.cpp` | `Aabb`, the generic binary `Bvh` (explicit children, leaf ranges into `primRefs`), `BvhBuildOptions`, `buildBvh()`, `computeBvhStats()` (SAH cost, depth, leaf sizes), `validateBvh()`, `limitLeafSize()`. The top-down framework: leaf/split decisions, median fallback, threading. |
| `src/bvh_split.h`, `bvh_split_object.cpp`, `bvh_split_spatial.cpp` | The split strategies, one function each: midpoint, binned SAH, sweep SAH, spatial splits. A new builder is one more function plus one enum value. |
| `src/bvh_layout.h/.cpp` | `packBvh()` into the GPU layouts (binary Aila–Laine, 64 B/node; CWBVH 8-wide, 80 B/node), `BvhTriangle` (v0 + two edges, 48 B), and CPU traversals that mirror the shader line for line. |
| `src/bvh_scene.h/.cpp` | Engine-independent two-level structure: `buildBlas()` (build, validate, pack), `buildTlas()`, `traceScene()` (CPU mirror of the GPU's two-level trace), the scene SAH cost. |
| `src/rt_accel.h/.cpp` | Engine glue: `AccelSettings`, `RaytraceBlasCache` (a BLAS per `MeshAsset`, reused across imports, built in parallel), `packGeometry()`, `buildSceneAccel()` (instances, materials, per-surface material tables, TLAS). |
| `shaders/crt_bvh.glsl` | TLAS + BLAS traversal. Binary by default; CWBVH when `CRT_BVH_CWBVH` is defined. |
| `shaders/crt_extend.glsl` + `crt_extend.comp` / `crt_extend_cwbvh.comp` | The extend stage body and its two builds, one per layout. `GpuPathTracer` picks the one matching the scene. |
| `tests/bvh_bench.cpp` → `bin/bvh_bench` | CPU-only verification and comparison on a real glTF file (§4). |

`rt_bvh` is a static library with no Vulkan in it, compiled `-O2` even in Debug builds (`RT_BVH_ALWAYS_OPTIMIZE`): unoptimised glm made BLAS builds 20× slower.

## 2. Builders (`BvhBuilder`)

| Builder | Split choice | Notes |
|---|---|---|
| `Midpoint` | middle of the centroid bounds, longest axis | Bikker part 1 / RTNW. No cost model; splits until `maxLeafSize`. |
| `BinnedSah` | SAH at `binCount` planes per axis | Wald 2007. Bins are capped at the node's reference count (small nodes are most of the tree). |
| `SweepSah` | SAH between every pair of adjacent centroids | Bikker part 2's exhaustive search in O(n log n) per node via sorting. |
| `SpatialSah` | binned object split, plus spatial splits where the object split's children overlap by more than `spatialAlpha` × the root area | Stich et al. 2009: triangle clipping, reference unsplitting, duplication capped by `spatialBudget`. **Default.** |

All use SAH leaf termination (`split cost ≥ leaf cost`, Bikker part 2), a hard `maxLeafSize`, a `maxDepth` that keeps every traversal stack in bounds, and a median fallback when a strategy finds no split. Subtrees above 8k references on each side are built on worker threads; the tree is identical either way.

## 3. GPU side

- Bindings (`crt_common.glsl`): 6 instances (TLAS slot order), 7 materials (spheres first), 11 BLAS triangles, 13 BLAS nodes, 14 per-triangle shading attributes, 15 TLAS nodes, 16 per-instance per-surface material indices, 17 traversal counters.
- BLAS nodes, triangles and attributes live in one **device-local** buffer, uploaded through staging once per BLAS set. Instances, materials and TLAS live in one small host-visible buffer, replaced whenever the scene changes. A gizmo drag re-uploads ~70 KB, not the geometry.
- The traversal reads positions only. The closest hit alone fetches its normals, uvs and surface (GPSnoopy's closest-hit pattern), and normals go to world space by the transpose of the stored world-to-object rows (the inverse transpose).
- Safety: every tree is validated before upload (`validateBvh`, `packBvh` depth checks). Stacks stop pushing rather than overflow. Every ray has a hard budget of 65536 node visits across both levels, so a malformed tree ends a dispatch instead of hanging the GPU.
- The render guard is now `pixels × samples/frame × scene SAH cost` (expected node visits + primitive tests per ray) against the same 5e8 budget. Structure.glb scores ~120/ray, so 1080p at 1–2 samples/frame passes and heavier settings need the explicit override.

## 4. Verification

- **CPU, every builder × layout** (`bin/bvh_bench`): validation, then traced against brute force. basicmesh: 20,000 random rays; structure.glb (476 instances, 1.06M placed triangles): 3,000 rays, plus 2,000 with `--perturb` (random rotation + non-uniform scale per instance). **0 mismatches** in every configuration.
- **GPU vs the old linear tracer:** a fixed-seed 320×180 render of basicmesh from two viewpoints, shaded and normal views. 98.7–100% of pixels bit-identical, max difference 0.0005. With every object rotated, non-uniformly scaled and translated, compared against the pre-BVH tracer rebuilt from backup (which baked transforms into world triangles): max difference 0.005 except one edge-graze pixel at 0.031.
- Validation layers: 0 messages over every GPU run.

## 8. Open / next

- **Traversal speed.** ~9 Mrays/s end to end inside the structure is low for this GPU class, and builder quality moves it little, which suggests a fixed per-step cost. First candidates: stacks in shared memory rather than private arrays (which likely spill to scratch on AMD), a short stack, and fewer TLAS visits (the TLAS is identical across builders and carries a large share of node visits in this instanced scene). Measure with the work counters plus alternating timings.
- **CWBVH collapse.** It is greedy (tinybvh's). Ylitie's SAH-optimal dynamic-programming collapse would slot into `CwbvhPacker::convert` alone.
- **BVH optimisation passes** (reinsertion, Meister & Bittner 2018) could run between `buildBvh` and `packBvh` for any builder.
- **Caching BLASes on disk** for very large models, if import time ever matters.

## 9. As built: measurements

Structure.glb: 108 meshes, 285k unique triangles, 476 instances.

**Build (at import, all 108 BLASes in parallel):** midpoint 60–100 ms, binned SAH 105–150 ms, sweep SAH 420–490 ms, SBVH 610–690 ms. SBVH duplicates 27% of triangle references. GPU geometry for SBVH + CWBVH is 38 MB.

**Traversal work per ray** (counted on the GPU by the extend stage, so independent of clocks; 320×180, 8 bounces, inside the structure):

| | midpoint | binned | sweep | SBVH |
|---|---|---|---|---|
| binary: nodes + prims | 51.8 + 14.3 | 49.9 + 10.0 | 49.7 + 10.0 | 49.3 + 9.3 |
| CWBVH: nodes + prims | 35.8 + 12.5 | 34.2 + 11.1 | 34.1 + 11.0 | 34.7 + 10.3 |

**Time** (re-measured 2026-09-19 on a cool machine: four identical runs agreed within 0.7%; each configuration run twice, forwards then reversed, averaged; 640×360, 32 samples, 8 bounces, raster preview off; whole frames, so the constant shading cost is included):

| ms/frame | midpoint | binned | sweep | SBVH |
|---|---|---|---|---|
| inside, binary | 92.8 | 75.8 | 73.0 | 74.5 |
| inside, CWBVH | 90.1 | 82.6 | 80.0 | 78.2 |
| outside, binary | 20.8 | 17.4 | 17.3 | 16.5 |
| outside, CWBVH | 18.4 | 17.1 | 16.6 | 15.8 |

Pass-to-pass variation is about ±4%. Readings:
- Midpoint is clearly worst inside (~25% slower). The three SAH builders are within noise of each other; SBVH does the fewest primitive tests but its 27% extra references don't buy measurable time here.
- CWBVH is ~5% slower than binary for the incoherent bounces inside the structure and ~4% faster for the mostly-sky outside view. Camera rays alone (720p) put all four SAH configurations within the ~9% run-to-run noise.
- Builder and layout together move the frame by well under the midpoint gap. What remains is a fixed per-step cost in the traversal itself (§8), not tree quality.

For scale: 16 samples of basicmesh at 320×180 took 714 ms of GPU time with the linear scan and 58 ms with the BVH, and structure.glb, which the linear scan could not render at all, runs at 74 ms/frame from inside the structure (670k rays/frame, ~9 Mrays/s end to end including shading).

The first measurements (2026-09-18) were taken while the laptop was thermally throttling and inflated inside-view frames about 4× (~300 ms); they are superseded by the table above. MoltenVK's timestamps on this GPU have encoder granularity (per-dispatch pairs read zero), so frame-level GPU time and the work counters are the tools.

**Defaults: SBVH + binary** - level with sweep SAH + binary, ahead of every CWBVH configuration where rays are incoherent, which is where a path tracer spends its time.
