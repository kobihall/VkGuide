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

**A real multi-binding compute pass already exists and should be migrated too: `TonemapPass`** (`src/vk_tonemap.h/.cpp`, `shaders/tonemap.comp`, added 2026-09-13 for the CPU raytracer's output). It binds two storage images (`rgba32f` in, `rgba8` out) plus a `float` push constant, and builds its own descriptor set layout, pipeline layout and pipeline by hand in exactly the inline shape this feature replaces. Moving it onto `ComputePassBuilder`/`dispatchComputePass()` proves the >1-binding path against production code, and may make the separate demo effect unnecessary — decide at implementation time. Keep its public contract unchanged (caller owns the layout transitions and supplies the descriptor allocator): `RaytraceJob::publishOutput()` uses it today and `docs/plans/compute-pipeline-raytracing.md` §2.7 builds on it.

## 3. Exact files to create/modify

| File | Role |
|---|---|
| `src/vk_compute.h` (new) | Declares `ComputePass`, `ComputePassBuilder`, `dispatchComputePass()`. |
| `src/vk_compute.cpp` (new) | Implements the above — `ComputePassBuilder::build()` constructs the descriptor set layout, pipeline layout (with the specified push-constant range if any), loads the shader via the existing `vkutil::load_shader_module` (`src/vk_pipelines.h:6`), and builds the `VkComputePipelineCreateInfo`/`vkCreateComputePipelines` call currently inlined in `initComputePipelines()`. |
| `src/vk_engine.h` | Replace `ComputePushConstants`/`ComputeEffect` (`vk_engine.h:41-64`) and `m_drawImageDescriptorLayout`/`m_computePipelineLayout` (`vk_engine.h:175-179`) with the new `ComputePass`-based equivalents; add whatever new members the demo multi-binding effect needs (its own descriptor set layout/set, and its input resource). |
| `src/vk_engine.cpp` | Rewrite `initComputePipelines()` (`vk_engine.cpp:991-1071`) to build each background effect (including the new demo effect) via `ComputePassBuilder` instead of the current inline `VkComputePipelineCreateInfo` code; rewrite `drawBackground()` (`vk_engine.cpp:214-228`) to call `dispatchComputePass()`. |
| `src/vk_tonemap.h/.cpp` | Rebuild `TonemapPass::init()`/`dispatch()` on `ComputePassBuilder`/`dispatchComputePass()`, keeping its public signatures (§2.5). |
| `shaders/` (new file) | One new `.comp` shader for the multi-binding demo effect (§2.5) — picked up automatically by the existing CMake shader glob (`docs/codebase-map.md` §5), no build-file change needed for the shader itself. |
| `src/CMakeLists.txt` | Add `vk_compute.h`/`vk_compute.cpp` to the explicit source list (`src/CMakeLists.txt:2-16` — confirmed not a glob, see `docs/codebase-map.md`/`docs/plans/completed/imgui-display.md` §3). |

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

## 9. As-built notes (implementation, 2026-09-14)

Everything below is what actually got built and verified, written after the fact. Where it disagrees with §§1–8, this section is right.

### 9.1 Deviations from this spec, and why

The three larger ones were proposed and approved by the user before implementation; the rest were judgment calls filling gaps the spec left.

1. **The multi-binding demo effect is a real feature: an `environment` background** (§2.5/§8 q2). `shaders/env_background.comp` binds the draw image as a storage image at binding 0 and `VulkanEngine::m_environmentMap` through `m_defaultSamplerLinear` as a combined image sampler at binding 1, and takes a push-constant block shaped nothing like the other two effects' (`mat4 inverseViewProj; vec2 drawExtent; float exposure; float pad;` — 84 bytes, versus their 64). Each pixel's centre is unprojected through the raster camera's inverse view-projection (the y-flipped one from `m_sceneData`, so no flip in the shader), so the panorama sits exactly behind the models the raster pass draws over it. The equirectangular lookup lives in **`shaders/equirect.glsl`**, a shared include, so the compute raytracer's miss branch (`docs/plans/compute-pipeline-raytracing.md` §2.8) samples the map the same way round: +y up, the map's centre column facing −z (the camera's yaw-0 forward), u increasing turning right. With no map loaded it binds `m_greyImage` and shows flat grey. An `Exposure` slider (0–4) sits in the "background" window; the draw image goes to the swapchain unmapped, so values above 1 clip.
2. **Background effects allocate their descriptor set per frame from `getCurrentFrame().frameDescriptors`**, rather than a persistent set written at init (§3 "its own descriptor set layout/set"). One set per frame is nothing, it copes with the environment map being replaced or cleared at runtime with no invalidation logic (a replaced map is destroyed only after `vkDeviceWaitIdle()`, so a set written this frame can never outlive the view it names), and it is the pattern both future consumers use anyway — the wave simulation rotates buffer roles every step, the raytracer rebinds per bounce. `m_drawImageDescriptors` and `m_drawImageDescriptorLayout` are gone; nothing persistent is left in `initDescriptors()` for compute.
3. **`ComputePass` carries a `workgroupSize`** (builder-set, default 16×16×1 — every `.comp` here) **and there is a `dispatchComputePassOver(cmd, pass, set, push, VkExtent3D domain)`** that does the ceil-divide, next to the spec's group-count `dispatchComputePass()`. `computeGroupCount(domain, workgroupSize)` is exposed too. The size is not validated against the shader; keep them in sync by hand.
4. **`ComputePass::destroy(VkDevice)`** — the spec never said how a pass dies. A member function, matching `TonemapPass::destroy()`/`DescriptorAllocatorGrowable::destroyPools()`.
5. **`ComputePassBuilder::build()` aborts** if the shader file is missing or any Vulkan object fails, the `checkVkResult` convention. The old `initComputePipelines()` only printed and carried on with a null module. The shader path goes in the constructor (a pass without a shader is meaningless); bindings, push-constant size (`setPushConstantSize(bytes)` or `setPushConstants<T>()`) and workgroup size are fluent setters. A pass with no bindings gets no set layout and no set bind at dispatch, so push-constant-only passes work.
6. **`ComputeEffect` became `BackgroundEffect{name, pass, record, drawSettings}`**, where the two closures are the only code that knows an effect's binding and push-constant shape: `record(cmd, pass)` allocates the set, fills the push constants and dispatches; `drawSettings()` draws the effect's imgui controls. The "background" window calls the selected effect's `drawSettings()` in place of the old four raw `InputFloat4`s — which is exactly what the two ported effects' closures draw, so their editing is unchanged. Their parameters are `m_gradientParams`/`m_skyParams` (`ComputePushConstants`, kept as the shape those two shaders share) and `m_environmentBackgroundExposure`, engine members so the closures capture `this` rather than a pointer into the vector. `recordDrawImageEffect()` is the shared one-storage-image record path.
7. **`VulkanEngine::m_gpuProperties`** is filled in `initVulkan()` and the relevant limits are printed once at startup (§8 q1).
8. **Root `CMakeLists.txt`: every shader now depends on `shaders/*.glsl`** as well as its own source. Before this, editing an include (`input_structures.glsl` had the same exposure) did not rebuild the shaders using it; with `equirect.glsl` meant to be shared and edited, that would have been a trap.
9. **Background effects still dispatch over the whole draw image**, not `m_drawExtent`, so the two ported effects look the same at every render scale as they always did. The environment effect takes `drawExtent` in its push constants so its unprojection covers the visible frame; the unused border gets directions beyond the frustum, which is harmless (only `m_drawExtent` is blitted).

### 9.2 The API as built

```cpp
// vk_compute.h
struct ComputePass {
	VkDescriptorSetLayout setLayout;   // VK_NULL_HANDLE for a pass with no bindings
	VkPipelineLayout pipelineLayout;
	VkPipeline pipeline;
	uint32_t pushConstantSize;         // 0 if none
	VkExtent3D workgroupSize;          // the shader's local_size; default 16x16x1
	void destroy(VkDevice device);
};

class ComputePassBuilder {
	explicit ComputePassBuilder(std::string shaderPath);                  // a compiled .comp.spv
	ComputePassBuilder& addBinding(uint32_t binding, VkDescriptorType type);   // all at set 0, count 1, compute stage
	ComputePassBuilder& setPushConstantSize(uint32_t bytes);
	template <typename T> ComputePassBuilder& setPushConstants();         // sizeof(T)
	ComputePassBuilder& setWorkgroupSize(uint32_t x, uint32_t y = 1, uint32_t z = 1);
	ComputePass build(VkDevice device);                                   // aborts on failure
};

VkExtent3D computeGroupCount(VkExtent3D domain, VkExtent3D workgroupSize);
void dispatchComputePass(VkCommandBuffer cmd, const ComputePass& pass, VkDescriptorSet set, const void* pushData, VkExtent3D groupCount);
void dispatchComputePassOver(VkCommandBuffer cmd, const ComputePass& pass, VkDescriptorSet set, const void* pushData, VkExtent3D domain);

// vk_engine.h
struct BackgroundEffect {
	const char* name;
	ComputePass pass;
	std::function<void(VkCommandBuffer cmd, const ComputePass& pass)> record;   // set + push + dispatch, draw image in GENERAL
	std::function<void()> drawSettings;                                        // imgui, inside "background"
};
// members: std::vector<BackgroundEffect> m_backgroundEffects (gradient, sky, environment); ComputePushConstants m_gradientParams, m_skyParams;
//          float m_environmentBackgroundExposure; VkPhysicalDeviceProperties m_gpuProperties
// private: void recordDrawImageEffect(VkCommandBuffer cmd, const ComputePass& pass, const ComputePushConstants& params);

// vk_tonemap.h - public contract unchanged; privately one ComputePass
// shaders/equirect.glsl - vec2 equirectUv(vec3 direction)   (direction normalised; +y up, centre column faces -z)
```

The idiom every future pass follows (`TonemapPass::dispatch()` is the reference, 12 lines):

```cpp
const VkDescriptorSet set = getCurrentFrame().frameDescriptors.allocate(m_device, pass.setLayout);
DescriptorWriter writer;
writer.writeImage(0, ...);  writer.writeBuffer(1, ...);
writer.updateSet(m_device, set);
dispatchComputePassOver(cmd, pass, set, &pushConstants, image.imageExtent);
```

The caller still owns every layout transition and barrier around it. Nothing here inserts a compute→compute memory barrier; the raytracer's bounce loop and the wave simulation's step→visualize will need one (a `VkMemoryBarrier2` on `SHADER_WRITE → SHADER_READ` at the compute stage). `vkutil::transition_image()` covers image-layout cases; a buffer/global barrier helper next to it is the natural next addition to `vk_images.h` when the first consumer arrives.

### 9.3 Verification actually performed

All under **validation layers** (`b_UseValidationLayers = true` temporarily; `VK_LOADER_DEBUG=layer` confirmed `VK_LAYER_KHRONOS_validation` inserted at instance and device level; the default vk-bootstrap messenger prints to stdout, and a deliberately wrong layout in a throwaway readback hook proved it does), with a temporary self-driving hook (since removed, source restored from a backup and re-diffed) that cycled the effect index every 60 frames, loaded the 8192×4096 `kiara_9_dusk_8k.hdr` at frame 30, pitched the camera ±0.5 rad, and closed the window at frame 500 so `cleanup()` ran under the layers too:

| Test | Result |
|---|---|
| Startup: `maxPushConstantsSize` / `maxComputeWorkGroupInvocations` / `maxComputeWorkGroupSize` on the Radeon Pro 560X via MoltenVK | **4096 bytes** / 1024 / 1024×1024×1024 |
| 500 frames cycling gradient → sky → environment (no map, then 8k map), then shutdown | **zero validation messages** from the feature code |
| Draw-image readback (rgba16f → PPM) of gradient and sky at the defaults | same image as before the port (blue→navy gradient; navy starfield) |
| Readback of environment, pitch +0.5 vs −0.5 | spheres move down / up while the panorama shows dusk sky darkening to the zenith / the ground sphere — background and geometry agree on up and down and on the horizon |
| Release build (`b_UseValidationLayers = false`, hook removed) | builds clean; `env_background.comp.spv` produced by the existing glob |

### 9.4 Not verified — needs a human at the keyboard

Interactive checks the self-driving hook could not cover: the "Effect Index" slider and the raw `data1..4` editors for gradient/sky; the exposure slider; panning the camera with the environment effect selected; File > Set/Clear Environment Map while the environment effect is selected; a CPU raytrace render completing and displaying through the migrated `TonemapPass` (the hook never started a render); the "Scene Mirror" window.

### 9.5 Answers to §8's open questions

1. **`maxPushConstantsSize` = 4096 bytes** on the Radeon Pro 560X via MoltenVK (Metal's limit), 32× the spec minimum. The raytracing doc's ray-gen push constant has plenty of room; the environment effect's 84-byte block is a real 64-byte-plus example already.
2. **The demo effect** — the environment background, §9.1 item 1.

### 9.6 Known limitations left in place

- **The environment map has no mipmaps**, and `m_defaultSamplerLinear` uses repeat addressing in both axes. An 8k map minified into a 1700-pixel frame will shimmer under camera motion, and the pole rows wrap onto each other. A mipmapped upload and a clamp-v sampler are the fix, on the asset side, when it matters (the raytracer's importance sampling will want mips anyway).
- **`sky.comp` still declares its image as `rgba8`** while the draw image is `rgba16f` — pre-existing, MoltenVK accepts it, untouched here.
- `DescriptorLayoutBuilder::addBinding()` is still fixed at `descriptorCount = 1`, so a pass wanting an array of textures needs that builder extended first.
- One descriptor set per pass, at set 0. A shared "scene" set bound once across several passes (something the raytracer's three stages might want) would need the builder to take extra pre-built layouts; add it when a consumer wants it, not before.
- `ComputePass::workgroupSize` is not reflected from the SPIR-V; a mismatch under-dispatches silently.
