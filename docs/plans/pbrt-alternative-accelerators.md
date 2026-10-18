# Alternative acceleration structures

The tracer has one family of acceleration structure, the two-level BVH, with four CPU builders (midpoint, binned SAH, sweep SAH, SBVH) and two GPU layouts (binary Aila–Laine and 8-wide CWBVH). pbrt covers more: the 3rd edition's kd-tree accelerator (§4.4) and linear BVHs built from Morton codes (§4.3.3, HLBVH), pbrt-v4's equal-counts split, and in its further reading uniform grids and treelet restructuring. This plan adds the kd-tree and the uniform grid as traversals, and the missing BVH builders, so each can be compared against the others with the same tools.

*Not implemented. No plan dependencies; follows the BVH library's existing conventions. [pbrt-gpu-bvh-construction.md](pbrt-gpu-bvh-construction.md) builds on the LBVH builder here. Part of the [roadmap](pbrt-roadmap.md).*

## What the renderer does today

- `rt_bvh` (`src/bvh*.{h,cpp}`) is Vulkan-free and always built at `-O2`. `BvhBuilder` selects the builder; `BvhLayout` packs the tree for the GPU; `bvh_scene` holds the CPU traversals that mirror `crt_bvh.glsl` line for line.
- A traversal is one strategy file supplying `traceScene()` and `occluded()` behind the contract in `crt_traverse.glsl`, one branch in `crt_trace.glsl`, and one `TraversalVariant` row ([integrator-layer.md](completed/integrator-layer.md)). The build compiles every kernel that traces once per traversal, so a new traversal reaches kernels 02 and 08 and every tracing stage of every integrator with no `.comp` file of its own. The node layout follows the selected traversal.
- `bin/bvh_bench` checks every builder and layout against brute force and reports the work counters: nodes visited and primitives tested. The laptop throttles, so these counters, not timings, are the comparison (memory note, BVH state).

## Design

### Builders

Three more `BvhBuilder` values, all producing the same binary `Bvh`, so every layout accepts them:

- **Equal counts**, pbrt-v4's `EqualCounts`: split at the median centroid along the longest axis. A second baseline beside midpoint.
- **LBVH** sorts primitives by the 30-bit Morton code of their centroid and builds the hierarchy from the code's bits (Lauterbach et al. 2009). HLBVH (Pantaleoni and Luebke 2010; PBR 3ed §4.3.3) then builds the top levels over clusters with the SAH. It builds fastest and traces slowest, and it is the CPU reference for the GPU builder of [pbrt-gpu-bvh-construction.md](pbrt-gpu-bvh-construction.md).
- **Treelet restructuring** (Karras and Aila 2013), a post-pass for any builder: for each treelet of up to 7 leaves, find the topology with the lowest SAH cost by dynamic programming and rebuild it. LBVH plus restructuring approaches SAH quality at a fraction of its build time.

### Kd-trees

pbrt 3's `KdTreeAccel` (PBR 3ed §4.4; `KdTreeAggregate` in pbrt-v4's CPU code): axis-aligned splits chosen by the SAH over the primitives' bound edges, with a bonus for cutting off empty space. A primitive straddling a split goes to both sides.

- Build parameters as in pbrt-v4: intersection cost 5, traversal cost 1, empty bonus 0.5, at most 1 primitive per leaf before the SAH decides, maximum depth `8 + 1.3 log₂ N`.
- Nodes are pbrt's 8-byte `KdAccelNode`: a split position and a flags word holding the axis, the child offset or the leaf's primitive count and index.
- Traversal walks front to back with a small stack of far children (pbrt's `KdToDo`, 64 entries) and stops as soon as the hit lies within the current node's range.
- The kd-tree is a BLAS structure. The TLAS stays the binary BVH, and the kd-tree strategy reuses its TLAS walk: kd-trees handle overlapping instance boxes poorly, since every straddling instance is duplicated.

### Uniform grids

A grid per BLAS with `3 ∛N` cells along the longest axis, after pbrt 2's heuristic, and proportionally fewer along the others. Cells list their triangles in compressed arrays (a cell-offset array and one index array), and rays walk the cells with Amanatides and Woo's 3D DDA (1987), accepting a hit only inside the current cell. Grids are the simplest structure to build and do badly on scenes with uneven detail, such as a teapot in a stadium, which is the point of having one to compare.

### Strategies

Each structure is a traversal:

- `crt_kdtree.glsl` and `crt_grid.glsl`, reached through `CRT_TRAVERSAL_KDTREE` and `CRT_TRAVERSAL_GRID` branches in `crt_trace.glsl`, with `TraversalVariant` rows `kdtree` and `grid`. Kernel 02, kernel 08, the transmittance variant of kernel 08 from [pbrt-participating-media.md](pbrt-participating-media.md), and every other tracing stage are then built for them by CMake.
- `BvhLayout` gains `KdTree` and `Grid`, packed by `bvh_layout.cpp`; the BLAS cache builds whichever the selected traversal needs.
- `bvh_scene` gains the CPU mirrors, line for line, as for the BVH layouts.

Every strategy maintains the traversal counters (`g_nodesVisited`, `g_primitivesTested`), which the contract requires. For a grid, a visited cell counts as a node.

## Registry, bindings and data

- `BvhBuilder`: `EqualCounts`, `Lbvh`, and a **Restructure treelets** option on every builder.
- `BvhLayout`: `KdTree`, `Grid`. Both pack into the existing node binding: the kd-tree's nodes and leaf index lists, and the grid's header, cell offsets and index array.
- Traversals `kdtree` and `grid`: two strategy files, two `crt_trace.glsl` branches, two `TraversalVariant` rows.
- `bvh_bench` flags: `--builders` gains `e` and `l`, `--layouts` gains `k` and `g`, `--restructure`.

## Verification

- The existing strategies render bit-identical, and scenes saved with them load unchanged.
- `bin/bvh_bench assets/structure.glb --rays 2000 --perturb --builders mbwsel --layouts bckg`: 0 mismatches for every builder and layout pair.
- The kd-tree and grid strategies render the same images as the binary BVH on the Cornell box, Sponza and the sphere scene, as the existing three traversals do with each other.
- A table in this file, filled in when built: build time and nodes visited plus primitives tested per ray for every builder and structure on `structure.glb` and Sponza.

## Order of work

1. Equal counts, then LBVH and HLBVH, with `bvh_bench`.
2. Treelet restructuring.
3. The kd-tree: build, CPU traversal, `bvh_bench`, then the GLSL strategy and its traversal row.
4. The uniform grid, the same way.

## Sources

PBR 3ed §4.3.3 (linear BVHs and HLBVH), §4.4 (the kd-tree accelerator: representation, SAH construction, traversal); PBR 4ed §7.3 (BVH construction and the SAH) and chapter 7 "Further Reading" (grids, kd-trees, LBVH, treelets); pbrt-v4 `src/pbrt/cpu/aggregates.h` and `aggregates.cpp` (`BVHAggregate` split methods, `KdTreeAggregate`); Lauterbach et al. 2009, *Fast BVH construction on GPUs*; Pantaleoni and Luebke 2010, *HLBVH: hierarchical LBVH construction for real-time ray tracing of dynamic geometry*; Karras and Aila 2013, *Fast parallel construction of high-quality bounding volume hierarchies*; Amanatides and Woo 1987, *A fast voxel traversal algorithm for ray tracing*; Wald and Havran 2006, *On building fast kd-trees for ray tracing, and on doing that in O(N log N)*; `docs/plans/completed/bvh-acceleration.md`.
