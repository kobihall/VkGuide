# GPU BVH construction

Every BVH is built on the CPU: each BLAS when its model is imported, and the TLAS whenever an object moves. On `structure.glb`, the 108 BLASes take 610 to 690 ms with the default SBVH builder, even in parallel. Dragging an object with the gizmo rebuilds and uploads the TLAS every frame. PBR 4ed §16.2.4 names fast BVH building and refitting as what interactive and animated rendering need. This plan builds BVHs on the GPU with LBVH and PLOC, refits the TLAS when only transforms change, and uses a fast GPU tree while the CPU's better tree is still being built.

*Not implemented. Depends on [alternative-accelerators.md](alternative-accelerators.md), whose CPU LBVH builder is the reference. Part of the [roadmap](roadmap.md).*

## What the renderer does today

- `RaytraceRenderer::prepareGeometry()` builds each BLAS on CPU threads at import (`rt_accel.h`), and `buildSceneAccel()` rebuilds the TLAS, instances, materials and light list on the CPU for every edit that changes them.
- The layouts (binary, CWBVH 8-wide) are packed on the CPU and uploaded.
- The recorded build times, in `docs/plans/completed/bvh-acceleration.md` §9: midpoint 60 to 100 ms, binned SAH 105 to 150 ms, sweep SAH 420 to 490 ms, SBVH 610 to 690 ms, for all 108 BLASes of `structure.glb`.

## Design

### Building on the GPU

Compute passes under `shaders/bvh/`, recorded with `immediateSubmit()` or into the frame:

1. Compute 30-bit Morton codes of the primitives' centroids within the mesh's centroid bounds.
2. Radix-sort the (code, primitive index) pairs: an LSD sort with 8-bit digits, four passes, each a histogram with subgroup operations (the GPU's subgroup size is 64) and a scatter. `shaders/bvh/radix_sort.comp` becomes a general utility; [wavefront-performance.md](wavefront-performance.md)'s ray sorting uses it too.
3. Build the hierarchy with one of two builders:
   - LBVH (Karras 2012) finds every internal node's range and split independently from the sorted codes, in one dispatch. Bounds then propagate bottom-up, each thread climbing until an atomic flag shows it is the first of two children to arrive.
   - PLOC (Meister and Bittner 2018) starts from one cluster per primitive in Morton order; repeatedly, each cluster finds its nearest neighbour by SAH within a radius of 8 or 16 positions in the list, mutual nearest neighbours merge, and the list is compacted. It builds trees close to sweep-SAH quality, several times slower than LBVH.
4. Pack the tree into the binary Aila–Laine layout. CWBVH conversion (Ylitie et al. 2017) optimises each 8-wide node by dynamic programming and stays on the CPU at first. A GPU-built BLAS uses the binary layout until a CPU CWBVH conversion replaces it.

Large meshes are built in chunks of dispatches, so that no single submission runs long enough to trip the watchdog that once panicked this Mac (memory note, GPU watchdog crash).

### The TLAS

- **Rebuild** on the GPU with LBVH over the instances' world boxes each time any instance changes. There are hundreds of instances, not millions of triangles, so this costs well under a millisecond, and the CPU no longer rebuilds or uploads the TLAS.
- **Refit** when only transforms change, as while dragging with the gizmo: recompute each node's box bottom-up, keeping the topology. A refitted tree's quality falls as objects move far from where it was built. The renderer rebuilds when the root's surface area has grown by more than half since the last rebuild.

Instances, materials and the light list are still assembled on the CPU. Only the tree over them moves to the GPU.

### Progressive quality

A model import builds every BLAS on the GPU with LBVH first, so the model renders within a frame or two, and queues the CPU build with the selected builder on a worker thread. When the CPU tree is ready it replaces the GPU one between frames. The swap does not restart the render: both trees find the same hits, so the image converges to the same result. `RenderKey` today includes the BVH settings; it gains a distinction between the selected builder, which restarts, and a progressive swap, which does not. The panel shows which BLASes are still on their fast tree.

### Settings

The **Acceleration structure** section gains:

- **BLAS builder** offers the CPU builders as today, GPU LBVH and GPU PLOC. **Progressive** (default on) builds on the GPU first whatever the choice.
- **TLAS** offers CPU (today's), GPU rebuild, and GPU refit with a rebuild when the tree degrades.

## Registry, bindings and data

- No new kernel slot: these passes are `ComputePass`es outside the integrator's schedule, like the tonemap pass.
- Transient build buffers (codes, indices, cluster lists, flags), freed after each build.
- `validateBvh()` on a read-back GPU tree, in `rt_bvh`.

## Verification

- With the CPU builders and TLAS selected, every scene renders bit-identical.
- A GPU-built tree, read back, passes `validateBvh()`: every primitive is referenced once, and every node's box contains its children's.
- `bin/bvh_bench` gains a `--gpu` mode that reads back GPU-built BLASes and checks them against brute force: 0 mismatches.
- GPU LBVH and CPU LBVH (from [alternative-accelerators.md](alternative-accelerators.md)) produce the same tree for the same input, since the codes and the sort are deterministic.
- Renders with GPU-built trees match renders with CPU-built trees, as the different traversals do.
- A table in this file, filled in when built: build time (CPU wall clock around the submission, since MoltenVK's per-dispatch timestamps read zero on this GPU) and nodes plus primitives per ray for GPU LBVH, GPU PLOC and the CPU builders on `structure.glb` and Sponza.
- Dragging an object in Sponza: frame time with CPU TLAS rebuilds against GPU refit.

## Order of work

1. Morton codes and the radix sort, checked against `std::sort` on read-back.
2. GPU LBVH for BLASes, with validation and `bvh_bench --gpu`.
3. The GPU TLAS rebuild, then refit.
4. Progressive quality.
5. PLOC.

## Sources

PBR 4ed §16.2.4 (interactive and animation rendering: fast building and refitting), chapter 7 "Further Reading" (GPU construction); Karras 2012, *Maximizing parallelism in the construction of BVHs, octrees, and k-d trees*; Lauterbach et al. 2009, *Fast BVH construction on GPUs*; Meister and Bittner 2018, *Parallel locally-ordered clustering for bounding volume hierarchy construction*; Ylitie, Karras and Laine 2017, *Efficient incoherent ray traversal on GPUs through compressed wide BVHs*; Meister et al. 2021, *A survey on bounding volume hierarchies for ray tracing*; `docs/plans/completed/bvh-acceleration.md` §9.
