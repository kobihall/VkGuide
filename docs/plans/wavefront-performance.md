# Wavefront performance

The BVH work left one question open: about 9 million rays per second inside `structure.glb` (74 ms per frame at 640 × 360), with the builder barely mattering, which points at per-step costs rather than tree quality (memory note, BVH state). Suspects are traversal stacks in private arrays that spill to scratch memory on AMD, TLAS overlap, queues that thin out over the bounces while the loop keeps launching full dispatches, and the three barriers of every queue append. PBR 4ed chapter 15 and its exercises name the known remedies: path regeneration, a separate light-sampling kernel, and queue sizing. This plan measures first, then applies each remedy behind a switch so that it is kept only if it helps.

*Not implemented. Uses the radix sort of [gpu-bvh-construction.md](gpu-bvh-construction.md) for ray sorting. Part of the [roadmap](roadmap.md).*

## What the renderer does today

- Each frame records `00 → [01 → 02 → {03, 04, 06} → 08] × depth → 09` with indirect dispatches whose group counts the queue headers hold, for a fixed number of bounce iterations.
- `queueAppend()` (`crt_common.glsl`) aggregates a workgroup's appends in shared memory with three barriers and one global atomic. The workgroup is 64 invocations, which is also this GPU's subgroup size.
- Traversal stacks are private arrays: `uint stack[CRT_BINARY_STACK_SIZE]` in `crt_bvh.glsl`, and a `uvec2` stack for CWBVH.
- `PathState` is an array of structs, 64 bytes today and 96 after the roadmap's fields.
- The pool holds up to 4M paths (`CRT_MAX_POOL`), and every plan adds per-slot memory.
- Vulkan timestamps on this GPU under MoltenVK have encoder granularity: per-dispatch pairs read zero, so only whole-frame GPU time and the work counters are measured today.

## Design

### Measuring

Before any change:

- Per-kernel GPU time comes from Metal. MoltenVK runs on Metal, so Xcode's Metal System Trace and GPU frame capture see each compute encoder. `MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE` captures a frame from a running render without code changes. A short guide in `docs/` records the steps.
- Queue occupancy per bounce comes from the queue-header readback that already exists: how many paths each dispatch actually processes.
- A benchmark script renders the four NEE scenes and `structure.glb` at fixed settings, interleaving runs so that thermal throttling affects every variant alike. The memory note records how much throttling distorted the first BVH measurements.

Every remedy below is compared against this baseline, with its switch on and off, and the results go into this file.

### Queue appends with subgroup ballots

When the workgroup is exactly one subgroup, as on this GPU, `subgroupBallot()` and `subgroupBallotExclusiveBitCount()` give each surviving invocation its offset, and `subgroupElect()` chooses the one invocation that does the global atomic. That removes the shared counter and its barriers. Devices whose subgroup is smaller than the workgroup keep today's path, chosen at pipeline creation with a specialisation constant. The device reports subgroup ballot support and `subgroupSizeControl` with `computeFullSubgroups`, so the size can be required.

### Traversal stacks

The strategies gain a stack-storage option, a compile-time define, so each is a separate build of the same strategy file:

- Private arrays, as today.
- Shared memory: each invocation's stack lives in a slice of workgroup shared memory. 64 invocations × 32 entries × 4 bytes is 8 KB of the 64 KB per workgroup.
- A short stack with restart (Laine 2010): a 4- to 8-entry private stack, and on overflow a restart from the root guided by a restart trail, which needs no parent pointers.

Private and shared-memory stacks visit the same nodes in the same order, so their work counters match and any difference in frame time is the stack's cost alone. The short stack revisits nodes after each restart, and the counters show how often.

### Path regeneration

With a fixed number of bounce iterations, the last bounces run on nearly empty queues. Novák et al. 2010 (PBR 4ed exercise 15.5) refill the freed pool slots with new camera paths at the start of each iteration:

- Stage 24, regenerate paths, fills free slots, taking the next (pixel, sample) pair from a global counter, so every pixel still receives its samples in order and the samplers see the sample indices they expect.
- A path writes its radiance to the film when it terminates, not at the end of the frame, so stage 25, film per finished path, runs per iteration over a queue of finished paths.
- The frame ends when the sample budget is spent and every queue is empty. The CPU reads the counters back with the usual `FRAME_OVERLAP` latency and stops recording iterations.

This is a second integrator, `path_regen`, with its own schedule under the `IntegratorSchedule` interface of [integrator-layer.md](completed/integrator-layer.md), since it changes how frames are recorded rather than what the stages compute. It runs the path tracer's 01 to 08 and stages 24 and 25 in place of 00 and 09. Those two are stages rather than variants of 00 and 09, because the path tracer's schedule cannot run them. Russian roulette's splitting ([russian-roulette.md](russian-roulette.md)) needs its free slots.

### Pool sizing and passes

Every plan adds per-slot memory, and at 4M slots the pool would exceed the GPU's memory. pbrt-v4 renders in passes over scanlines when the queues cannot hold every pixel sample (PBR 4ed §15.3). The schedule does the same: when `pixels × samples per frame` exceeds what the budget allows, a frame renders in passes over image bands. The budget is a setting with the current per-slot size shown beside it. Queue size also affects cache behaviour (PBR 4ed exercise 15.6), so the benchmark sweeps it.

### Ray sorting

Sorting rays by a Morton code of origin and direction before kernel 02 makes neighbouring invocations traverse similar nodes (Meister et al. 2020 survey the options). The radix sort from [gpu-bvh-construction.md](gpu-bvh-construction.md) sorts the ray queue's indices by that key. Sorting surface hits by material before kernel 06 reduces divergence inside each material class (Hoberock et al. 2009). Each is a switch, kept only if the sort costs less than it saves.

### Structure of arrays

pbrt-v4 stores its work items as structures of arrays (PBR 4ed §15.2.3). A kernel that reads two fields of `PathState` still loads the whole 96-byte struct. Splitting the hottest fields (`throughput`, `bounce`, `flags`, the MIS record) from the rest is measured on kernels 03 and 04, which read little.

### Fewer idle dispatches

pbrt-v4 skips kernels for features the scene lacks (`haveMedia`, `haveSubsurface`, per-material-type flags). The schedule already dispatches everything indirectly, but an empty dispatch still costs a barrier and an encoder. It skips kernels 05 and 07 without media, slot 10 without subsurface materials, the complex material queue without complex materials, and kernel 03 when no ray can escape to anything.

### A separate light-sampling kernel

PBR 4ed exercise 15.3: kernel 06 samples lights inline, which diverges when the scene mixes light types. Kernel 06 can instead push light-sampling requests to a queue, sorted by light kind, for a light-sampling kernel that emits the shadow rays. It is a switch, measured on the 980-light scene, which has every kind.

### Not planned

A hardware ray-tracing backend (PBR 4ed exercise 15.8): MoltenVK exposes no ray query or ray tracing pipeline extension on this GPU.

## Registry, bindings and data

- Kernel 00 variant `0003_regenerate`, the regenerating schedule, and its setting.
- Build-time defines for the stack storage of each traversal strategy.
- Settings for each switch; none changes a saved scene's image.
- No new binding.

## Verification

- With every switch off, every scene renders bit-identical, and the regenerating schedule renders the same image as the fixed one, within noise, since it reorders which pixel samples run together.
- The ballot append, the stack variants, sorting and structure of arrays each render bit-identical with a fixed seed: they change where data sits and in what order threads run, not what a path computes.
- The results table in this file: frame time and queue occupancy per bounce for each switch on each benchmark scene, with the measurement conditions.

## Order of work

1. Measurement: Metal capture, the occupancy readout, the benchmark script.
2. Subgroup-ballot appends.
3. Stack storage variants.
4. Skipping idle dispatches.
5. Path regeneration, and pool passes.
6. Ray and hit sorting, structure of arrays, and the light-sampling kernel.

## Sources

PBR 4ed chapter 15 (§15.1 mapping path tracing to the GPU, §15.2.3 structure-of-arrays layout, §15.2.4 work queues, §15.3 the path tracer's kernels and passes) and exercises 15.3, 15.5, 15.6 and 15.8; Laine, Karras and Aila 2013, *Megakernels considered harmful: wavefront path tracing on GPUs*; Novák, Havran and Dachsbacher 2010, *Path regeneration for interactive path tracing*; Laine 2010, *Restart trail for stackless BVH traversal*; Hoberock et al. 2009, *Stream compaction for deferred shading*; Meister et al. 2020, *On ray reordering techniques for faster GPU ray tracing*; `docs/plans/completed/bvh-acceleration.md` §9.
