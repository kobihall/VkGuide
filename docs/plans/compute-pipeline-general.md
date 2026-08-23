# Feature: General-Purpose Compute Pipeline Framework

## 0. How to use this doc

Standalone implementation spec — treat it as the only context you have. For exhaustive facts about the current codebase state, read `docs/codebase-map.md` first (§2 covers the existing compute-shader pattern this feature generalizes, in full). No dependency on any other planned feature — this is pure infrastructure and can be built any time.

This is one of two docs covering "the compute shader pipeline overhaul": this doc is the general-purpose half (arbitrary compute passes over arbitrary bound resources). The other half — a swappable raytracing-style staged pipeline (ray-gen/hit-detect/hit-shade) with progressive accumulation — is `docs/plans/compute-pipeline-raytracing.md`, which depends on and reuses the `ComputePass` abstraction this doc builds.

## 1. Feature goal

Replace the current hardcoded "exactly one storage image, exactly 4×vec4 push constants" compute pattern (`docs/codebase-map.md` §2) with a reusable abstraction that can bind any mix of images and buffers with any push-constant shape, so future compute passes — a GPU raytracer, a physics simulation, an arbitrary image-processing effect — don't each have to hand-roll pipeline/descriptor plumbing from scratch the way `initComputePipelines()` does today.

## 2. Architecture decisions made and WHY

### 2.1 A `ComputePass` type, not an extension of `PipelineBuilder`

`PipelineBuilder` (`src/vk_pipelines.h/.cpp`) is a fluent builder for graphics pipelines — rasterizer state, blend state, depth-stencil state, dynamic-rendering color/depth formats, all fields a compute pipeline has none of. A `VkComputePipelineCreateInfo` is genuinely simple: one shader stage plus a layout. Rather than bolting an awkward compute path onto a builder whose entire fluent API is graphics-specific (leaving most of its methods meaningless for a compute caller), this feature adds a small, separate `ComputePass`/`ComputePassBuilder` pair in a new `src/vk_compute.h/.cpp`, matching the existing per-concern-file convention (`vk_descriptors.h/.cpp`, `vk_images.h/.cpp`, `vk_pipelines.h/.cpp`).

```cpp
struct ComputePass {
    VkDescriptorSetLayout setLayout;
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
    uint32_t pushConstantSize;   // 0 if the shader takes none
};
```

`ComputePassBuilder` wraps a `DescriptorLayoutBuilder` (reused as-is — it already supports arbitrary `addBinding(binding, type)` calls, `src/vk_descriptors.h:9-11` — the existing limitation is entirely in how it's *used* today, hardcoded to one call site with one binding, not in the builder itself) plus a push-constant byte size and a shader path, and produces a `ComputePass` via `.build(device)`.

### 2.2 The pass owns the layout/pipeline; the caller owns the descriptor set

`ComputePass` does not include an allocated `VkDescriptorSet` — only the `VkDescriptorSetLayout` that describes its shape. Which specific images/buffers actually get bound varies per use (different effects, different frames, different call sites), so the caller allocates a set against `ComputePass::setLayout` (via the existing `DescriptorAllocatorGrowable`) and writes it (via the existing `DescriptorWriter`) with whatever resources are relevant that call. This mirrors a distinction the codebase already makes implicitly — `m_drawImageDescriptorLayout` (shape) vs. `m_drawImageDescriptors` (one specific allocated-and-written instance of that shape), `docs/codebase-map.md` §2 — this feature just makes that split an explicit, reusable pattern instead of a one-off.

### 2.3 Push constants are a raw byte size, not a typed/reflected system

`ComputePass::pushConstantSize` is just a `uint32_t`; the caller supplies a matching `void*` at dispatch time. This was considered against building a more elaborate typed or SPIR-V-reflection-driven system (auto-deriving descriptor layouts and push-constant shapes directly from compiled shader bytecode, e.g. via SPIRV-Reflect) and rejected for this pass — reflection-driven binding is a legitimate technique but a materially larger engineering investment than this codebase's existing style calls for (every other Vulkan abstraction here — `DescriptorLayoutBuilder`, `PipelineBuilder` — is explicit, hand-specified C-style setup, not reflection-driven). A raw byte-size-plus-pointer keeps `ComputePass` consistent with that style and with how push constants are already used elsewhere (`ComputePushConstants`, `GPUDrawPushConstants` — plain structs, `memcpy`'d in).

### 2.4 Dispatch is a small free function, not a method on `ComputePass`

```cpp
void dispatchComputePass(VkCommandBuffer cmd, const ComputePass& pass, VkDescriptorSet set,
                          const void* pushData, VkExtent3D groupCount);
```
Binds the pipeline, binds `set` at index 0, pushes `pushData` (if `pass.pushConstantSize > 0`), and calls `vkCmdDispatch`. Kept as a free function (in `vk_compute.h/.cpp`, taking a `VkCommandBuffer` directly) rather than a method, matching `vkutil::transition_image`/`copy_image_to_image`'s existing shape (`src/vk_images.h`) — these are thin, stateless wrappers around a handful of Vulkan calls, not behavior that belongs to an owning object.

### 2.5 Prove the generalization with a real multi-binding example, not just a refactor

Refactoring the two existing background effects (`gradient_color.comp`, `sky.comp`) onto `ComputePass` would compile and work, but wouldn't actually demonstrate anything the old system couldn't already do — both effects use exactly one storage-image binding, same as today. To prove the generalization is real, this feature adds one new example compute effect that binds **more than one resource** (e.g. an input texture sampled alongside the output storage image, or an auxiliary input buffer) — something the old hardcoded single-`STORAGE_IMAGE`-binding layout (`docs/codebase-map.md` §2) could not express at all. Exact content of this demo effect is left to implementation time (§8) — its purpose is purely to exercise the >1-binding path, not to be visually interesting.

## 3. Exact files to create/modify

| File | Role |
|---|---|
| `src/vk_compute.h` (new) | Declares `ComputePass`, `ComputePassBuilder`, `dispatchComputePass()`. |
| `src/vk_compute.cpp` (new) | Implements the above — `ComputePassBuilder::build()` constructs the descriptor set layout, pipeline layout (with the specified push-constant range if any), loads the shader via the existing `vkutil::load_shader_module` (`src/vk_pipelines.h:6`), and builds the `VkComputePipelineCreateInfo`/`vkCreateComputePipelines` call currently inlined in `initComputePipelines()`. |
| `src/vk_engine.h` | Replace `ComputePushConstants`/`ComputeEffect` (`vk_engine.h:41-64`) and `m_drawImageDescriptorLayout`/`m_computePipelineLayout` (`vk_engine.h:175-179`) with the new `ComputePass`-based equivalents; add whatever new members the demo multi-binding effect needs (its own descriptor set layout/set, and its input resource). |
| `src/vk_engine.cpp` | Rewrite `initComputePipelines()` (`vk_engine.cpp:991-1071`) to build each background effect (including the new demo effect) via `ComputePassBuilder` instead of the current inline `VkComputePipelineCreateInfo` code; rewrite `drawBackground()` (`vk_engine.cpp:214-228`) to call `dispatchComputePass()`. |
| `shaders/` (new file) | One new `.comp` shader for the multi-binding demo effect (§2.5) — picked up automatically by the existing CMake shader glob (`docs/codebase-map.md` §5), no build-file change needed for the shader itself. |
| `src/CMakeLists.txt` | Add `vk_compute.h`/`vk_compute.cpp` to the explicit source list (`src/CMakeLists.txt:2-16` — confirmed not a glob, see `docs/codebase-map.md`/`docs/plans/imgui-display.md` §3). |

## 4. Implementation order and dependencies

1. **`vk_compute.h/.cpp`** — write `ComputePass`/`ComputePassBuilder`/`dispatchComputePass()` against the existing `DescriptorLayoutBuilder`/`DescriptorAllocatorGrowable`/`DescriptorWriter`/`vkutil::load_shader_module`. No `VulkanEngine` changes yet — compiles in isolation.
2. **Refactor the two existing background effects** (`gradient_color`, `sky`) onto the new types. Verify no visual regression — both effects should look pixel-identical to before (same shaders, same single-binding layout, just built/dispatched through the new abstraction).
3. **Add the new multi-binding demo effect** (§2.5) — new shader + new descriptor layout/set with >1 binding, added to the effect list. Verify it renders correctly and is selectable via the existing effect-picker slider (`vk_engine.cpp:454-467`).
4. **Smoke test**: cycle through all three effects via the UI, confirm all three work, confirm push-constant editing (for the two ported effects) still works as before.

## 5. Edge cases / traps identified during planning

- **Push-constant size limits**: `VkPhysicalDeviceLimits::maxPushConstantsSize` is only guaranteed to be at least 128 bytes by the Vulkan spec — the existing `ComputePushConstants` already uses 64 of those. A future compute pass wanting significantly more per-dispatch data than fits in that budget will need a small UBO/SSBO instead of push constants; `ComputePass` doesn't prevent this, but callers should be aware push constants aren't unlimited. Worth confirming the actual limit on the Radeon Pro 560X via MoltenVK (see §8) since the companion raytracing doc's camera-parameter push constant may be close to this boundary.
- **Distinct descriptor set layouts per effect**: once an effect can have more than one binding, it can no longer share the single global `m_drawImageDescriptorLayout`/`m_drawImageDescriptors` the way all background effects do today (`docs/codebase-map.md` §2, written once at init, reused every frame). Each `ComputePass` with a different binding shape needs its own layout and its own allocated-and-written set.

## 6. Code patterns from the existing codebase to follow

- **The exact thing being generalized**: `docs/codebase-map.md` §2 (existing `drawBackground()`/`m_backgroundEffects`/`initComputePipelines()`) is both the porting source and the acceptance bar — after refactoring, the two existing effects must behave identically.
- **Descriptor tooling reused as-is**: `DescriptorLayoutBuilder`, `DescriptorAllocatorGrowable`, `DescriptorWriter` (`src/vk_descriptors.h/.cpp`) — no changes needed to any of these; `ComputePassBuilder` is a thin composition layer on top.
- **Shader loading reused as-is**: `vkutil::load_shader_module` (`src/vk_pipelines.h:6`, `.cpp:6-50`).
- **File-per-concern convention**: `vk_descriptors.h/.cpp`, `vk_images.h/.cpp`, `vk_pipelines.h/.cpp` are the direct precedent for `vk_compute.h/.cpp`'s existence as its own file pair rather than more code piled into `vk_engine.cpp`.

## 7. What NOT to do (alternatives rejected and why)

- **Do not** build a SPIR-V-reflection-driven descriptor/push-constant system (§2.3) — a legitimate technique, but a materially bigger investment than this codebase's existing explicit, hand-specified Vulkan style calls for. Revisit only if manually specifying bindings for every new compute pass becomes a real, repeated pain point.
- **Do not** merge this into `PipelineBuilder` (§2.1) — compute and graphics pipeline construction share almost nothing (no rasterizer/blend/vertex-input/depth-stencil/dynamic-rendering-format state for compute), so a shared builder would mostly be dead fields for one side or the other.
- **Do not** stop at "refactor only" without the new multi-binding demo effect (§2.5) — a refactor with identical capability doesn't prove the generalization actually removed the old limitation; the demo effect is the concrete evidence that it did.

## 8. Open questions / things to verify before starting

1. **`maxPushConstantsSize` on the target hardware** (Radeon Pro 560X via MoltenVK) — not verified during planning; relevant both here (§5) and for the companion raytracing doc.
2. **Exact content of the multi-binding demo effect** (§2.5) — deliberately left open; anything genuinely exercising >1 binding satisfies the requirement, no specific visual target was specified.
