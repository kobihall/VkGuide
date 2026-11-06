# VkGuide + RayTracingInAWeekend — Codebase Map

Reference document for making future changes. Facts only, no recommendations.

**Sections 1-7 describe the state as of 2026-08-22** (full reads of `src/`, `shaders/`, and the sibling project `/Users/kobihall/Documents/Code/RayTracingInAWeekend`) and are kept for the raster path, which they still describe accurately. The CPU backend they also describe was deleted on 2026-09-15 (commit `a7b70fd`). They **predate the raytracer entirely** and several of their claims are now false - each such claim carries an inline correction. In particular §2's "there is exactly one compute pattern in the codebase" has not been true since 2026-09-14.

**Section 8 (2026-09-25) is the current reference for the GPU path tracer** and supersedes anything earlier that touches it. For a readable account of the same material, see `docs/raytracing-overview.md`.

---

## 1. Current rendering architecture and `draw()` call structure

`VulkanEngine` (`src/vk_engine.h`) owns all Vulkan state and drives the render loop from `run()` (`src/vk_engine.cpp:419-493`).

### `run()` — `vk_engine.cpp:419-493`
Per-iteration: `glfwPollEvents()`, handle minimize/pause, `ImGui_ImplVulkan_NewFrame()` / `ImGui_ImplGlfw_NewFrame()` / `ImGui::NewFrame()` (450-452), builds a `"background"` window (454-468, render-scale slider + compute-effect picker/push-constant editors), a `"Stats"` window (470-476), `ImGui::ShowDemoWindow()` (479), `ImGui::Render()` (482), then calls `draw()` (484).

### `draw()` — `vk_engine.cpp:60-170`
1. `updateScene()` (62) — populates `m_mainDrawContext` and `m_sceneData`.
2. `vkWaitForFences` on `getCurrentFrame().renderFence`, 1s timeout (66).
3. `getCurrentFrame().deletionQueue.flush()` + `frameDescriptors.clearPools()` (68-69) — cleans up the frame slot from two frames ago.
4. `vkResetFences`, `vkAcquireNextImageKHR` (71-81); `VK_ERROR_OUT_OF_DATE_KHR` sets `m_resizeRequested=true` and returns early.
5. `vkResetCommandBuffer` + `vkBeginCommandBuffer` (88-97).
6. `m_drawExtent = min(m_drawImage.imageExtent, m_swapchainExtent) * m_renderScale` (93-94).
7. Transition `m_drawImage` `UNDEFINED → GENERAL`, `m_depthImage` `UNDEFINED → DEPTH_ATTACHMENT_OPTIMAL` (101-103).
8. `drawBackground(cmd)` (106) — compute pass writing into `m_drawImage` while in `GENERAL` layout.
9. Transition `m_drawImage` `GENERAL → COLOR_ATTACHMENT_OPTIMAL` (108).
10. `drawGeometry(cmd)` (110) — dynamic-rendering mesh pass.
11. Transition `m_drawImage` `COLOR_ATTACHMENT_OPTIMAL → TRANSFER_SRC_OPTIMAL`, swapchain image `UNDEFINED → TRANSFER_DST_OPTIMAL` (113-114).
12. `vkutil::copy_image_to_image(cmd, m_drawImage.image, swapchainImage, m_drawExtent, m_swapchainExtent)` (117) — a `vkCmdBlitImage2KHR` (supports scale between `m_drawExtent` and `m_swapchainExtent`).
13. Transition swapchain image `TRANSFER_DST_OPTIMAL → COLOR_ATTACHMENT_OPTIMAL` (120).
14. `drawImgui(cmd, m_swapchainImageViews[swapchainImageIndex])` (123) — renders ImGui **directly onto the swapchain image**, on top of the already-blitted 3D scene.
15. Transition swapchain image `COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC_KHR` (126).
16. `vkEndCommandBuffer`; submit via `VkSubmitInfo2` (wait `swapchainSemaphore`@`COLOR_ATTACHMENT_OUTPUT`, signal `renderSemaphore`@`ALL_GRAPHICS`) through `vkinit::VkFunctionLoader::get_instance().vkQueueSubmit2KHR` with `renderFence` (135-144).
17. `vkQueuePresentKHR` waiting on `renderSemaphore`; `OUT_OF_DATE`/`SUBOPTIMAL` sets `m_resizeRequested=true` (150-166).
18. `m_frameNumber++` (169).

*Update 2026-10-29 (§8.27): steps 11 to 13 are gone. The draw image ends every drawn frame in `SHADER_READ_ONLY_OPTIMAL` and reaches the screen only through the Viewport window, an ImGui image inside the dock space; `drawImgui()` clears the swapchain image itself. `m_drawExtent` is the Viewport window's size times the render scale, not the swapchain's.*

### The raster pause
`draw()` compares `rasterKey()` (the view-projection, the draw extent, `m_drawImagesCreated`, the editor's `revision()`, `m_sceneRevision`, the background effect and its parameters, `m_environmentMapsLoaded` and the environment's exposure, the occlusion toggle) with the last frame's. After `FRAME_OVERLAP + 1` frames of the same key, and when `m_rasterPauseMode` allows it (`Always`, or `WhileRendering` with `RaytraceRenderer::renderRunning()`), it skips `drawBackground()` and `drawGeometry()` and blits the draw image as the last frame left it (`SHADER_READ_ONLY_OPTIMAL`). The wait of `FRAME_OVERLAP + 1` frames lets the occlusion queries catch up with the view, so the picture kept is not missing a surface. Anything new that changes the raster picture must be added to `RasterKey`, or the preview keeps showing the old picture.

### `drawGeometry()` — `vk_engine.cpp`
1. Reads back the occlusion queries this frame slot issued two frames earlier (`FrameData::occlusionQueries`, valid after `draw()`'s fence wait) into `MeshDrawList::occluded`, unless the draw list was rebuilt since (`MeshDrawList::generation`) or **Stats** → **Occlusion culling** is off.
2. `MeshDrawList::cull()` appends the scene's opaque mesh surfaces in the view frustum, in draw order (material, mesh, first index), less those marked occluded, and lists the surfaces to query this frame: those `MeshDrawList::queried` marks (4,096 triangles or more, at most 4,096 of them) whose box does not hold the eye.
3. Frustum-culls `m_mainDrawContext.opaqueSurfaces` (the raytracer's shapes) via `is_visible()`, sorts them the same way and appends them, then the transparent surfaces.
4. Resets the frame's query pool (grown when needed) and begins dynamic rendering: color attachment = `m_drawImage.imageView` (no clear), depth attachment = `m_depthImage.imageView`, via `vkCmdBeginRenderingKHR`. Sets viewport/scissor to `m_drawExtent`.
5. Allocates a per-frame `GPUSceneData` uniform buffer (`createBuffer`, `CPU_TO_GPU`), pushed to `getCurrentFrame().deletionQueue`; written via mapped pointer + explicit `vmaFlushAllocation`. Allocates `globalDescriptor` (set 0) from `getCurrentFrame().frameDescriptors`.
6. Writes every draw's world matrix, in order, to `FrameData::instanceBuffer` (grown when needed) and binds it as vertex binding 1.
7. Each run of consecutive draws of one surface (index buffer, vertex buffer, first index, count) with one material is one `vkCmdDrawIndexed` with an instance count and `firstInstance` at the run's first matrix. Rebinds pipeline + set 0 only when `material->pipeline` changes, set 1 when the material changes, the index buffer and vertex binding 0 when they change; updates `m_stats`.
8. Draws each queried surface's world box (`m_occlusionBoxPipeline`, `shaders/occlusion_box.vert`: no color or depth writes) inside its own occlusion query.
9. `vkCmdEndRenderingKHR`; clears both surface lists for next frame.

### Draw image / depth image lifecycle
- Created once in `initSwapchain()` (`vk_engine.cpp:807-867`): `m_drawImage` format `VK_FORMAT_R16G16B16A16_SFLOAT`, usage `TRANSFER_SRC | TRANSFER_DST | STORAGE | COLOR_ATTACHMENT`; `m_depthImage` format `VK_FORMAT_D32_SFLOAT`, usage `DEPTH_STENCIL_ATTACHMENT`. Both sized to `m_windowExtent` **at init time**, GPU-only VMA memory, destructors pushed to `m_mainDeletionQueue`.
- `resizeSwapchain()` (`vk_engine.cpp:638-652`) only calls `destroySwapchain()`/`createSwapchain()` — it does **not** recreate `m_drawImage`/`m_depthImage`. Effective render resolution is clamped every frame via `m_drawExtent = min(drawImage, swapchain) * m_renderScale`. Growing the window past its initial size does not increase draw-image resolution.

### `drawBackground()` — `vk_engine.cpp:214-228`
Picks `m_backgroundEffects[m_currentBackgroundEffect]`, binds its compute pipeline, binds `m_drawImageDescriptors` (set 0), pushes `ComputePushConstants` (the effect's stored data), dispatches `ceil(width/16.0), ceil(height/16.0), 1`.

---

## 2. How compute shaders are currently dispatched

*Update 2026-09-14 (`docs/plans/completed/compute-pipeline-general.md`): the pattern below is gone. Every compute pipeline is now a `ComputePass` built by `ComputePassBuilder` (`src/vk_compute.h/.cpp`: arbitrary set-0 bindings, any push-constant size, a `workgroupSize`) and recorded with `dispatchComputePass()`/`dispatchComputePassOver()`. Background effects are `BackgroundEffect{name, pass, record, drawSettings}` closures, allocate their descriptor set per frame from `frameDescriptors`, and there are three: `gradient`, `sky`, and a new `environment` (`shaders/env_background.comp` + `shaders/equirect.glsl`: the draw image as a storage image plus `m_environmentMap` through a sampler). `TonemapPass` is a wrapper over one `ComputePass`. `m_drawImageDescriptors`, `m_drawImageDescriptorLayout`, `m_computePipelineLayout` and `ComputeEffect` no longer exist; `ComputePushConstants` remains as the shape shared by `gradient_color.comp`/`sky.comp`. Root `CMakeLists.txt` now rebuilds every shader when a `shaders/*.glsl` include changes. The rest of this section describes the state before that change.*

~~There is exactly one compute pattern in the codebase: the "background effects" system. No other compute usage exists.~~ **False since 2026-09-15**: the GPU path tracer adds ten more compute pipelines built through the same `ComputePass` abstraction. See §8.

- **Descriptor set layout** (`initDescriptors()`, `vk_engine.cpp:930-934`): single binding 0, `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE`, stage `VK_SHADER_STAGE_COMPUTE_BIT`, built via `DescriptorLayoutBuilder`. Stored as `m_drawImageDescriptorLayout` (`vk_engine.h:176`).
- **Descriptor set**: allocated once from `m_globalDescriptorAllocator` (937), written once at init pointing at `m_drawImage.imageView` in `VK_IMAGE_LAYOUT_GENERAL` (939-942). Stored as `m_drawImageDescriptors` (`vk_engine.h:175`). Never rewritten per-frame (valid because `m_drawImage` is a single fixed image, never recreated after init).
- **Pipeline layout** (`initComputePipelines()`, `vk_engine.cpp:993-1007`): one set layout (`m_drawImageDescriptorLayout`) + one push-constant range covering `sizeof(ComputePushConstants)` (64 bytes = 4×`vec4`), stage `COMPUTE`. Stored as `m_computePipelineLayout` (`vk_engine.h:179`).
- **Push constants struct** (`vk_engine.h:41-46`):
  ```cpp
  struct ComputePushConstants { glm::vec4 data1, data2, data3, data4; };
  ```
- **Pipeline creation** (`vk_engine.cpp:1009-1063`): loads `shaders/gradient_color.comp.spv` and `shaders/sky.comp.spv` via `vkutil::load_shader_module`, builds a `VkComputePipelineCreateInfo` per shader (same layout, different module), wraps each as `ComputeEffect{name, pipeline, layout, data}` (`vk_engine.h:57-64`), stored in `std::vector<ComputeEffect> m_backgroundEffects` (`vk_engine.h:216`). Shader modules destroyed immediately after pipeline creation; pipelines + layout destroyed via `m_mainDeletionQueue`.
- **Dispatch**: see `drawBackground()` above. Workgroup convention is `local_size_x=16, local_size_y=16` in every `.comp` file, matching the `ceil(w/16), ceil(h/16)` dispatch math.
- **Effect selection UI** lives in `run()` (`vk_engine.cpp:454-467`): slider over `m_currentBackgroundEffect` index, raw float4 editors for the push-constant data.
- A dead field remains at `vk_engine.h:178`: `//VkPipeline m_computePipeline; //unused for now, instead shaders are in m_backgroundEffects`.
- `PipelineBuilder` (`vk_pipelines.h/.cpp`) is graphics-only; there is no compute pipeline builder abstraction — compute pipelines are hand-built inline in `initComputePipelines()`.
- `MaterialPass` enum (`vk_types.h:61-65`) has values `MainColor`, `Transparent`, `Other` — `Other` is declared but not routed anywhere in `drawGeometry()`/`writeMaterial()`.

### Compute shader files (`shaders/`)
*As of 2026-09-25 this list covers only the background effects; the raytracing kernels are in `shaders/rt/` and are listed in §8.2.* All three declare `layout (local_size_x = 16, local_size_y = 16) in;` and bind a single storage image at set 0, binding 0.
- `shaders/gradient_color.comp` (32 lines, loaded/used) — vertical two-color gradient, `layout(rgba16f, ...)`, blends `PushConstants.data1`/`data2` by `texelCoord.y / size.y`. `data3`/`data4` unused.
- `shaders/sky.comp` (91 lines, loaded/used) — procedural starfield (Shadertoy-derived, CC BY-NC-SA 3.0 license comment), `layout(rgba8, ...)` — **note this differs from `gradient_color.comp`'s `rgba16f` and from the actual draw image format (`VK_FORMAT_R16G16B16A16_SFLOAT`)**. Only `data1` used (`.xyz` = tint, `.w` = star threshold).
- `shaders/gradient.comp` (28 lines) — UV-gradient with black gridlines at workgroup boundaries. **Not referenced anywhere in `src/`** — compiled by the CMake glob but never loaded by any C++ code.

---

## 3. How Vulkan images/descriptors are currently managed

### Image/buffer creation primitives (on `VulkanEngine` itself, not a separate utility class)
```cpp
AllocatedBuffer createBuffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);       // vk_engine.h:226, .cpp:654
void            destroyBuffer(const AllocatedBuffer& buffer);                                                // vk_engine.h:227, .cpp:674
AllocatedImage  createImage(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped=false);            // vk_engine.h:228, .cpp:679 — empty GPU-only image
AllocatedImage  createImage(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped=false); // vk_engine.h:229, .cpp:715 — staging-buffer upload + immediateSubmit copy; always adds TRANSFER_SRC|TRANSFER_DST; ends in SHADER_READ_ONLY_OPTIMAL
void            destroyImage(const AllocatedImage& img);                                                     // vk_engine.h:230, .cpp:751
GPUMeshBuffers  uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);                         // vk_engine.h:225, .cpp:758
```
The empty-image `createImage()` overload always allocates `VMA_MEMORY_USAGE_GPU_ONLY` with `DEVICE_LOCAL_BIT` — there is no CPU-visible/mappable image creation path in the codebase.

The data-upload `createImage(void* data, ...)` overload originally **hardcoded its staging-buffer size as `width * height * depth * 4`** — correct only for RGBA8. *Update 2026-09-13: fixed.* It now sizes the upload with a per-format `bytes_per_pixel()` lookup (RGBA8/BGRA8 = 4, RGBA16F = 8, RGBA32F = 16, aborts otherwise), pulled forward from `docs/plans/completed/scene-and-asset-management.md` §2.4 for the CPU raytracer's `R32G32B32A32_SFLOAT` output (`docs/plans/completed/raytracing-in-a-weekend.md` §9.6).

### Image transition/copy helpers (`src/vk_images.h/.cpp`)
```cpp
namespace vkutil {
    void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout);
    void copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D dstSize);
}
```
- `transition_image` (`vk_images.cpp:4-29`): builds a `VkImageMemoryBarrier2` with blanket `srcStageMask/dstStageMask = ALL_COMMANDS_BIT`, `dstAccessMask = MEMORY_WRITE|MEMORY_READ` (coarse, not fine-grained). Aspect mask is `DEPTH_BIT` only when `newLayout == DEPTH_ATTACHMENT_OPTIMAL`, else `COLOR_BIT` — cannot handle combined depth-stencil or other aspect combos without modification. Dispatched through `vkinit::VkFunctionLoader::get_instance().vkCmdPipelineBarrier2KHR`.
- `copy_image_to_image` (`vk_images.cpp:31-63`): a `vkCmdBlitImage2KHR` (linear filter, full `[0,size]` region) — implicit scaling when `srcSize != dstSize`.

### Descriptor tooling (`src/vk_descriptors.h`, 47 lines / `.cpp`, 202 lines)
```cpp
struct DescriptorLayoutBuilder {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    void addBinding(uint32_t binding, VkDescriptorType type);   // descriptorCount fixed at 1
    void clear();
    VkDescriptorSetLayout build(VkDevice device, VkShaderStageFlags shaderStages, void* pNext = nullptr, VkDescriptorSetLayoutCreateFlags flags = 0);
};

struct DescriptorAllocatorGrowable {
    struct PoolSizeRatio { VkDescriptorType type; float ratio; };
    void init(VkDevice device, uint32_t initialSets, std::span<PoolSizeRatio> poolRatios);
    void clearPools(VkDevice device);
    void destroyPools(VkDevice device);
    VkDescriptorSet allocate(VkDevice device, VkDescriptorSetLayout layout, void* pNext = nullptr);
    // auto-grows pool size ×1.5 per new pool, capped at 4092
};

struct DescriptorWriter {
    std::deque<VkDescriptorImageInfo> imageInfos;    // deque so pointers stay valid as more entries are pushed
    std::deque<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkWriteDescriptorSet> writes;
    void writeImage(int binding, VkImageView image, VkSampler sampler, VkImageLayout layout, VkDescriptorType type);
    void writeBuffer(int binding, VkBuffer buffer, size_t size, size_t offset, VkDescriptorType type);
    void clear();
    void updateSet(VkDevice device, VkDescriptorSet set);
};
```
There is **no plain (non-growable) `DescriptorAllocator`** anywhere in the codebase, despite the module table in `CLAUDE.md` listing one — confirmed by grep. `DescriptorAllocatorGrowable` is the only allocator type, used both as `m_globalDescriptorAllocator` (persistent, `vk_engine.h:173`) and per-frame (`FrameData::frameDescriptors`, `vk_engine.h:33`).

### Descriptor set layouts currently in use
- `m_gpuSceneDataDescriptorLayout` — set 0: 1 binding, `UNIFORM_BUFFER`, stage `VERTEX|FRAGMENT`.
- Material layout (`GLTFMetallic_Roughness::materialLayout`) — set 1: binding 0 = `MaterialConstants` UBO, binding 1/2 = combined-image-sampler (color/metal-rough), stage `FRAGMENT`.
- `m_drawImageDescriptorLayout` — set 0: 1 binding, `STORAGE_IMAGE`, stage `COMPUTE`.
- `m_singleImageDescriptorLayout` — 1 binding, `COMBINED_IMAGE_SAMPLER`, stage `FRAGMENT` (used by the `tex_image.frag`/`colored_triangle_mesh.vert` pipeline).

### `DeletionQueue` (`vk_engine.h:11-27`)
```cpp
struct DeletionQueue {
    std::deque<std::function<void()>> deletors;
    void push_function(std::function<void()>&& function) { deletors.push_back(function); }
    void flush() { for (auto it = deletors.rbegin(); it != deletors.rend(); it++) (*it)(); deletors.clear(); }
};
```
LIFO lambda queue. `m_mainDeletionQueue` flushed once in `cleanup()` (`vk_engine.cpp:1318`); per-frame `FrameData::deletionQueue` flushed at the top of `draw()` each time that frame slot recurs (`vk_engine.cpp:68`).

### `immediateSubmit()` (`vk_engine.h:236`, `.cpp:395-417`)
```cpp
void immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function);
```
Uses dedicated `m_immFence`/`m_immCommandBuffer`/`m_immCommandPool` (separate from per-frame data). Pattern: reset fence + cmd buffer → begin one-time-submit cmd buffer → invoke `function(cmd)` → end → submit via `vkQueueSubmit2KHR` with no wait/signal semaphores, only `m_immFence` → blocking `vkWaitForFences` (~10s timeout). Synchronous by design.

### Samplers
`m_defaultSamplerLinear` / `m_defaultSamplerNearest` exist, created in `initDefaultData()` (`vk_engine.h:201-202`).

---

## 4. How ImGui currently receives images to display

**It doesn't.** Confirmed via grep across all of `src/`: zero occurrences of `ImGui_ImplVulkan_AddTexture` (or `RemoveTexture`). There is no code path anywhere in VkGuide that samples a non-font Vulkan image from within ImGui draw commands, and no descriptor management for it.

### ImGui init (`initIMGUI()` — `vk_engine.cpp:1143-1204`)
- Creates its own `VkDescriptorPool` (`imguiPool`, all descriptor types ×1000, `FREE_DESCRIPTOR_SET_BIT`) — separate from `m_globalDescriptorAllocator`/`DescriptorAllocatorGrowable` (1148-1168). This pool is a **local variable**, only captured by the shutdown-cleanup lambda; it is not stored as a member of `VulkanEngine`.
- `ImGui_ImplGlfw_InitForVulkan(m_window, true)` (1176).
- `ImGui_ImplVulkan_InitInfo`: `UseDynamicRendering = true`, `MSAASamples = VK_SAMPLE_COUNT_1_BIT`, `PipelineRenderingCreateInfo.pColorAttachmentFormats = &m_swapchainImageFormat` (1179-1194) — **targets the swapchain color format**, not `m_drawImage`'s format (`R16G16B16A16_SFLOAT`).
- `ImGui_ImplVulkan_CreateFontsTexture()` called once at init (1197), no manual command-buffer plumbing.
- Cleanup: `ImGui_ImplVulkan_Shutdown()` + pool destroy pushed to `m_mainDeletionQueue` (1200-1203).
- No `ConfigFlags` are set on `ImGui::GetIO()` anywhere in `vk_engine.cpp` (confirmed via grep) — docking/viewports flags are not currently enabled either way. **Update**: subsequently confirmed during `docs/plans/completed/imgui-display.md`'s planning that the vendored ImGui at `../CPPLibraries/imgui` (`1.90.9 WIP`) is from upstream **master**, not the **docking** branch — grepping for `Docking`/`DockSpace`/`DockNode` in `imgui.h` finds nothing but incidental comments. `docs/plans/completed/imgui-display.md` §2.1 covers what vendoring a docking-branch build requires.

### `drawImgui()` (`vk_engine.cpp:383-393`)
Opens a dynamic-rendering pass on the *target view passed in* (always a swapchain image view in current call sites) with no clear value (blends over whatever's already there — the already-blitted 3D scene), then calls `ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd)`.

### ImGui UI content today (`run()`, `vk_engine.cpp:454-479`)
`"background"` window (render-scale slider, compute-effect index slider, push-constant float4 editors), `"Stats"` window, `ImGui::ShowDemoWindow()`. No image-display windows exist.

---

## 5. Key abstractions and where they live

| Abstraction | File | Notes |
|---|---|---|
| `VulkanEngine` | `src/vk_engine.h/.cpp` | Owns all Vulkan state, drives `run()`/`draw()` |
| `FrameData`, `FRAME_OVERLAP=2` | `vk_engine.h:30-46` | Double-buffered per-frame command pool/buffer, semaphores, fence, `frameDescriptors`, `deletionQueue`. `getCurrentFrame()` = `vk_engine.h:164`, `m_frames[m_frameNumber % FRAME_OVERLAP]` |
| `DeletionQueue` | `vk_engine.h:11-27` | LIFO lambda cleanup queue, global + per-frame instances |
| `ComputePushConstants`, `ComputeEffect` | `vk_engine.h:41-46`, `57-64` | Fixed 4×vec4 push constants; `{name, pipeline, layout, data}` |
| `GPUSceneData` | `vk_engine.h:48-55` | view/proj/viewproj + ambientColor/sunlightDirection/sunlightColor |
| `AllocatedImage`, `AllocatedBuffer`, `GPUMeshBuffers`, `GPUDrawPushConstants` | `src/vk_types.h` | Shared GPU resource structs. `GPUDrawPushConstants` is only `m_meshPipeline`'s, which nothing draws with |
| `MaterialPass`, `MaterialPipeline`, `MaterialInstance` | `vk_types.h:61-76` | `Other` pass value unrouted |
| `IRenderable`, `Node` | `vk_types.h:78-113` | `Node::Draw()` recurses into children by default |
| `GLTFMetallic_Roughness` | `vk_engine.h:87-115`, impl `vk_engine.cpp:1363-1466` | Owns opaque/transparent pipelines, `writeMaterial()` |
| `RenderObject` | `vk_engine.h:66-75` | Sort key: `(material ptr, indexBuffer)` |
| `DescriptorLayoutBuilder`, `DescriptorAllocatorGrowable`, `DescriptorWriter` | `src/vk_descriptors.h/.cpp` | Only descriptor tooling in the codebase |
| `PipelineBuilder` | `src/vk_pipelines.h/.cpp` | Graphics-only fluent builder; no compute equivalent |
| `vkutil::load_shader_module` | `vk_pipelines.h:6`, `.cpp:6-50` | Generic `.spv` file → `VkShaderModule` |
| `vkutil::transition_image`, `copy_image_to_image` | `src/vk_images.h/.cpp` | See section 3 |
| `vkinit::VkFunctionLoader` | `src/vk_initializers.h:30-46` | Singleton manually loading KHR extension function pointers (`vkCmdPipelineBarrier2KHR`, `vkQueueSubmit2KHR`, `vkCmdBlitImage2KHR`, `vkCmdBeginRenderingKHR`, `vkCmdEndRenderingKHR`) since the project targets Vulkan 1.2 core + KHR rather than core 1.3 |
| `Camera`, `ViewportNavigation` | `src/camera.h/.cpp`, `src/viewport_navigation.h/.cpp` | The raster viewport's free camera (yaw, pitch, a pivot), owned by `VulkanEngine::m_viewportNav` with its controls, settings and preferences; see §8.29 |
| glTF loading | `src/vk_loader.h/.cpp` | `loadGltfMeshes()`, `loadGltf()`, `load_image()` (fastgltf + stb_image) |
| `input_structures.glsl` | `shaders/input_structures.glsl` | Set 0 = `SceneData` UBO, Set 1 = material UBO + 2 samplers; included by `mesh.vert`/`mesh.frag`; stage mask is `VERTEX|FRAGMENT` only |
| Buffer-device-address vertex pulling | `shaders/mesh.vert` (44 lines) | No `layout(location=N) in` anywhere in the codebase; `PipelineBuilder::buildPipeline` zero-fills `VkPipelineVertexInputStateCreateInfo` |
| CMake shader glob | root `CMakeLists.txt` | `file(GLOB_RECURSE ... shaders/*.frag *.vert *.comp)`; output `<name>.<stage>.spv` next to source; `.glsl` files not compiled directly (include-only) |

---

## 6. CPU raytracing structure (`/Users/kobihall/Documents/Code/RayTracingInAWeekend`)

Standalone project (not part of VkGuide's build). Flat layout, no `src/` subdirectory. Derived from a Walnut/imgui_boilerplate-style docking ImGui+Vulkan app shell (`git remote -v` shows `upstream = https://github.com/kobihall/imgui_boilerplate.git`); `Application.h/.cpp` and `Image.h/.cpp` are structurally near-identical to `imgui`'s own `examples/example_glfw_vulkan/main.cpp`. The raytracing logic itself is original.

### Application shell
- `Layer` interface (`Layer.h:1-11`): `OnAttach()/OnDetach()/OnUIRender()`.
- `Application` (`Application.h:24-62`) owns a layer stack, calls each layer's `OnUIRender()` once per GLFW frame.
- `Application::Run()` (`Application.cpp:529-650`): `glfwPollEvents()` → swapchain rebuild check → `ImGui::NewFrame()` → full-viewport `DockSpace` window → menubar → all layers' `OnUIRender()` → `ImGui::Render()` → `FrameRender()` → `FramePresent()`.
- `Application::GetCommandBuffer(bool begin)` / `FlushCommandBuffer()` (`Application.cpp:672-723`): one-off command buffer + `vkQueueSubmit` + blocking fence-wait, used for the image upload below.
- No separate render thread; ray tracing runs synchronously on the UI/present thread inside `OnUIRender()`.

### Render loop / progressive state
`ExampleLayer::Render()` (`main.cpp:70-86`): if viewport size changed, reallocates `Image` + `uint32_t* m_ImageData`; calls `m_renderer.render(m_ImageData, width, height)` (fully blocking); calls `m_Image->SetData(m_ImageData)` (full re-upload).

`Renderer::render()` (`Renderer.cpp:40-59`): rebuilds `camera` from scratch every call; plain nested `for` loop over every pixel, single-threaded, no tiling, no work queue. `renderPerPixel()` (`Renderer.cpp:61-74`) does MSAA sampling (`settings.msaaSamples`, slider 1–100) within a single call, recursive `ray_color()` (`Renderer.cpp:10-25`: `if(depth<=0) return black; if(hit) recurse(scattered, depth-1)`).

**There is no cross-frame accumulation.** Every `Render()` call recomputes the entire image from zero with fresh random samples; the previous frame's data is fully discarded. The "Render every frame" checkbox (`main.cpp:39-42`) just re-triggers `Render()` unconditionally each ImGui frame — it reruns the same full render repeatedly, it does not accumulate samples over time.

`RenderSettings` (`Renderer.h:8-13`): `bool MSAA`, `int msaaSamples` (1–100), `int rayDepth` (1–16). `Random` (`Random.h/.cpp`) is a single global static `std::mt19937` engine (`Random::s_RandomEngine`), not thread-local or seeded per-invocation.

### Image upload / ImGui display (`Image.h/.cpp`)
- `VK_IMAGE_TYPE_2D`, format `VK_FORMAT_R8G8B8A8_UNORM` for the `RGBA` path used by the raytracer (an `RGBA32F`/`VK_FORMAT_R32G32B32A32_SFLOAT` path also exists but is only used by an unrelated HDR-file-loading constructor).
- `Image::AllocateMemory` (`Image.cpp:95-166`): creates `VkImage` (`usage = SAMPLED_BIT | TRANSFER_DST_BIT`, `TILING_OPTIMAL`, `DEVICE_LOCAL_BIT` memory found via manual `GetVulkanMemoryType`), a matching `VkImageView`, and a `VkSampler` (linear filter, repeat addressing).
- `m_DescriptorSet = (VkDescriptorSet)ImGui_ImplVulkan_AddTexture(m_Sampler, m_ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);` (`Image.cpp:165`) — the returned `VkDescriptorSet` is reinterpreted as an `ImTextureID`.
- `Image::SetData` (`Image.cpp:168-257`): lazily creates a host-visible staging `VkBuffer` (`TRANSFER_SRC_BIT`, `HOST_VISIBLE_BIT`); `vkMapMemory` → `memcpy` → `vkFlushMappedMemoryRanges` → `vkUnmapMemory`; gets a one-off command buffer via `Application::GetCommandBuffer(true)`; barrier `UNDEFINED → TRANSFER_DST_OPTIMAL`; `vkCmdCopyBufferToImage`; barrier `TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL`; `Application::FlushCommandBuffer(cmd)` (blocking).
- Display (`main.cpp:59-67`): `ImGui::Begin("Viewport")`; viewport width/height read from `ImGui::GetContentRegionAvail()` each frame (render target resized to match panel size when it changes); `ImGui::Image(m_Image->GetDescriptorSet(), {width, height}, ImVec2(0,1), ImVec2(1,0))` — the UV flip corrects CPU row order vs. Vulkan texture-space convention.

### Object / material dispatch
- `hittable` (`hittable.h:23-27`): pure-virtual base — `virtual bool hit(const ray&, double t_min, double t_max, hit_record&) const = 0;` and `virtual void params() = 0;` (each subtype implements its own ImGui control code). Only one concrete subtype: `sphere` (`sphere.h/.cpp`) — no triangles/planes/boxes.
- `hittable_list` (`hittable_list.h/.cpp`): itself a `hittable` (composite), holds `std::vector<std::shared_ptr<hittable>> objects` + parallel `std::vector<char const*> IDs`. `hit()` (`hittable_list.cpp:15-29`) is a flat O(n) linear scan tracking `closest_so_far`. **No BVH or any acceleration structure, no `aabb`/`bounding_box()` method anywhere.**
- `material` (`material.h:9-12`): pure-virtual base — `virtual bool scatter(const ray&, const hit_record&, glm::dvec3& attenuation, ray& scattered) const = 0;`. Four concrete materials, all `std::shared_ptr<material>`-dispatched via `hit_record::mat_ptr`:
  - `lambertian` — diffuse, `normal + random_in_ball()`.
  - `metal` — reflect + fuzz.
  - `phong` — not in the book; cosine-power specular lobe via ONB (`getTangentSpace`, `Random.cpp:91-98`), `alpha = pow(1000, smoothness²)`.
  - `dielectric` — Schlick reflectance, refract/reflect.
- Scene construction is hardcoded in `Renderer::Renderer()` (`Renderer.cpp:27-38`): 4 spheres (ground/center/left/right) with materials assigned by hand — not data-driven.
- `sphere::params()` (`sphere.cpp:5-14`) example of the self-describing UI pattern: `ImGui::SliderScalar("radius", ImGuiDataType_Double, &radius, &rMin, &rMax, "%.3f")`, `ImGui::SliderScalarN("position", ...)`. Invoked from `main.cpp:56`: `m_renderer.world.objects[item_current]->params();` — driven by virtual dispatch on the currently-selected object.

### ImGui control inventory
"Settings" window: render-time readout text, `ImGui::Button("Render")`, `ImGui::Checkbox("Render every frame", ...)`, read-only viewport-dimension text, `ImGui::SliderInt("Depth of ray bounces", 1, 16)`, `ImGui::Checkbox("MSAA", ...)`, `ImGui::SliderInt("# of samples", 1, 100)` (shown when MSAA on). "Scene" window: `ImGui::ListBox("Objects", ...)` backed by `hittable_list::IDs`, followed by the selected object's `params()` call. "Viewport" window: just the `ImGui::Image(...)` call, auto-sized to `ImGui::GetContentRegionAvail()`. Also present: `ImGui::ShowDemoWindow()`, a `File > Exit` menu via `Application::SetMenubarCallback`.

There is no progress bar, no cancel/stop control, and no explicit render-state machine (idle/rendering/done) — the render is synchronous, so by the time control returns to the UI it has already completed.

### Non-book additions (differences from a standard RTIAW implementation)
Full docking ImGui+Vulkan application shell (vs. writing a `.ppm`); interactive re-triggerable rendering via UI controls; custom `phong` material; `params()` self-describing virtual UI method on `hittable`; `glm::dvec3` double precision throughout (vs. the book's custom `vec3`); no multithreading (confirmed via grep — no `std::thread`/`std::async`/OpenMP usage outside vendored `stb_image.h`/`imgui`/`glm`).

---

## 7. Facts relevant to constraining future feature design

- No `ImGui_ImplVulkan_AddTexture` infrastructure exists in VkGuide today (section 4) — any feature that displays a non-swapchain image in ImGui needs this built.
- `m_drawImage`/`m_depthImage` in VkGuide are fixed at init-time size and never recreated on resize (section 1) — any new output image with the same requirement (track window/panel size) needs new resize-handling code; none exists to copy today.
- VkGuide's compute descriptor layout is hardcoded to exactly one `STORAGE_IMAGE` binding (section 2) and push constants are hardcoded to exactly `4×vec4` (`ComputePushConstants`) — reusing this pattern as-is for a pass needing more bindings or richer parameters is not possible without extending it.
- No `VK_KHR_ray_tracing_pipeline` / `VK_KHR_acceleration_structure` usage anywhere in VkGuide (still true - the BVH is built on the CPU and traversed in plain compute, §8); project targets Vulkan 1.2 core + KHR extension forms loaded through `vkinit::VkFunctionLoader` (confirmed no core-1.3 unsuffixed entry points are called for dynamic rendering/sync2/blit).
- VkGuide has a single GLFW window and a single Vulkan surface/swapchain; no multi-window code exists.
- VkGuide's ImGui init pins `PipelineRenderingCreateInfo.pColorAttachmentFormats` to the swapchain format (section 4); the `imguiPool` descriptor pool is a local variable in `initIMGUI()`, not currently exposed as an engine member.
- The vendored ImGui at `../CPPLibraries/imgui` is confirmed **not** the docking branch (section 4) — any feature wanting real docking/tabbing needs to vendor a docking-branch build first (`docs/plans/completed/imgui-display.md` §2.1 covers this).
- ~~`createImage(void* data, ...)`'s staging-buffer size is hardcoded to 4 bytes/pixel~~ — fixed 2026-09-13 (section 3).
- *Update 2026-09-14* (`docs/plans/completed/scene-and-asset-management.md`): the scene is now composed at runtime: `VulkanEngine::m_models` (replacing `m_loadedScenes`) holds any number of imported glTF files, `importGltf()/removeModel()` change it and bump `m_sceneRevision`, and `openScene()/saveScene()` move models + environment map + spheres together as one `.gltf` scene file (`src/scene_io.h`) via native dialogs (`src/file_dialog.mm`); the tutorial's hardcoded Suzanne/cubes (`m_testMeshes`, `m_loadedNodes`) are gone; `m_raytraceMeshData` holds object-space mesh data rebuilt only on model changes; `m_environmentMap` (rgba16f) is loaded by `loadEnvironmentMap()`; `TransformGizmo` (`src/vk_gizmo.h`, ImGuizmo 1.10 at `../CPPLibraries/ImGuizmo`) is the shared viewport gizmo; `IoResult` (`vk_types.h`) is the recoverable-error type for user-facing file I/O, the one deliberate exception to the hard-abort convention below.
- `GPUSceneData`/`m_mainCamera` in VkGuide are singular — there is exactly one camera and one scene-data uniform wired into `drawGeometry()`; nothing else reads or writes a second view.
- The RTIAW project's `hittable`/`material` virtual-dispatch model, `hittable_list`'s O(n) linear scan (no BVH), the global mutable `std::mt19937`, `double`-precision math, and full-image-recompute-per-trigger render loop are all CPU-only constructs with no direct GPU-compute equivalent present anywhere in either codebase — none of this is portable to a compute shader as-is.
- VkGuide's `checkVkResult`/`vkbErr` error macros (`vk_types.h:115-129`) hard-abort on any failure; there is no exception-based or recoverable error path in the codebase.

---

## 8. The GPU path tracer: wavefront kernels and swappable variants

*2026-09-25. Supersedes §2's claims about compute usage. Readable companion: `docs/raytracing-overview.md`. Design sources: PBR 4ed ch. 15 (figure 15.2's kernel numbering, §15.1.2 megakernel vs wavefront, §15.2.4 work queues, §15.3.6 the `WavefrontAggregate` interface that the traversal contract is modelled on); Laine, Karras & Aila 2013.*

### 8.1 The one-paragraph model

A wavefront path tracer, as the first integrator above a stage library that later integrators share (§8.16). Each kernel of the path-tracing algorithm is its own compute pipeline running over a compacted queue of live paths. Which shader runs in each stage, and which traversal every tracing kernel is built for, are chosen **at runtime** from a registry, saved with the scene, and drive the descriptor set layout, the dispatch, the settings UI and the render guard. One frame is `00 → [01 → 02 → {03, 04, 06} → 08] × rayDepth → 09 → tonemap`.

### 8.2 File inventory

```
shaders/rt/
  0001_generate_camera_rays.comp        00 Generate camera rays: a sample uniform in its pixel (old files)
  0002_generate_camera_rays_filtered.comp  00 a sample placed by the pixel filter (the new-scene default)
  0101_generate_samples.comp            01 PCG per bounce: no sample record (old files' choice)
  0102_generate_samples_independent.comp   01 the PCG stream written into the record
  0103_generate_samples_stratified.comp    01 jittered strata; needs a known sample count
  0104_generate_samples_halton.comp        01 Halton over the image
  0105_generate_samples_sobol.comp         01 one Sobol sequence over the image
  0106_generate_samples_padded_sobol.comp  01 padded Sobol, in blocks of 2^L samples
  0107_generate_samples_zsobol.comp        01 ZSobol (the new-scene default)
  0108_generate_samples_pmj02bn.comp       01 PMJ02BN; needs a known sample count
  0201_intersect_closest.comp           02 Intersect closest; traces: built once per traversal
  0301_handle_escaped.comp              03 Handle escaped
  0401_handle_emissive.comp             04 Handle emissive geometry
  0501_sample_medium_interaction.comp   05 STUB, implemented = false
  0601_surface_scatter_bsdf.comp        06 BSDF sampling; shadow rays to delta lights only
  0602_surface_scatter_light.comp       06 Light sampling
  0603_surface_scatter_mis_balance.comp 06 MIS, balance heuristic
  0604_surface_scatter_mis_power.comp   06 MIS, power heuristic (the default)
  0701_sample_medium_scattering.comp    07 STUB, implemented = false
  0801_trace_shadow_rays.comp           08 Trace shadow rays; traces: built once per traversal
  0901_update_film.comp                 09 Update film (not a PBR figure box): the float running mean (old files)
  0902_update_film_compensated.comp     09 compensated weighted sums, variance, clamp (the new-scene default)
  0903_update_film_outliers.comp        09 0902 plus outlier reweighting, in two passes
  include/
    crt_common.glsl      buffer structs, GLOBAL BINDING NUMBERS, push constants, queue set,
                         queueAppend(), hitTriangle(), the robust section (offsetRayOrigin(),
                         hitTriangleWatertight(), toWorldPoint()), the instance transforms, srgbToLinear()
    crt_random.glsl      PCG hash + sampling
    crt_samples.glsl     what kernels 00 and 06 read: sampleDimension() from the RaySamples record or
                         the PCG stream, the CRT_SAMPLE_RECORD specialization constant, the warps
    crt_sampler.glsl     kernel 01's samplers and body (camera pass and bounce pass); one
                         CRT_SAMPLER_<KIND> per .comp; mirrors src/sampler.cpp
    crt_halton_tables.glsl  GENERATED: Halton's primes, reciprocals and digit counts
    crt_camera.glsl      the camera models: generateCameraRay() for the perspective, orthographic and
                         spherical kinds, the apertures, the tilt; mirrors src/camera_model.cpp
    crt_camera_rays.glsl kernel 00's body, shared by 0001 and 0002 (CRT_PIXEL_FILTER)
    crt_warp.glsl        the warps the lights and the cameras share: the concentric disk, the uniform
                         triangle, the equal-area square; mirrors src/light_sampling.cpp
    crt_image_table.glsl binding 65's piecewise-constant 2D tables over images: sampleImageTable(),
                         imageTablePdf()
    crt_filter.glsl      sampleFilter(): the pixel filter's table inverted; mirrors src/film.cpp
    crt_film.glsl        the clamp, Neumaier sums, Welford, resolve, and the outlier cascade;
                         mirrors src/film.cpp
    crt_film_update.glsl kernel 09's compensated variants' body (CRT_FILM_CASCADE for 0903)
    crt_aov.glsl         the auxiliary outputs: aovClear(), aovWantsSurface(), aovRecord() (PixelState) and
                         aovFoldPixel() (kernel 09's fold into AovImages and the light groups' weight sum);
                         mirrors src/aov.cpp
    crt_light_groups.glsl lightGroupAdd(): the atomic add into ChannelSums (kernels 03, 04, 08)
    crt_aov_color.glsl   aovIdColor(), for the output windows
    crt_trace.glsl       the traversal a tracing kernel is built for: one CRT_TRAVERSAL_<ID> branch
                         per traversal, which CMake reads to compile the permutations
    crt_traverse.glsl    THE TRAVERSAL CONTRACT + TraceHit, work counters, testTriangles(),
                         instanceRay(), testShapeInstance(), resetTrace(); includes crt_shape.glsl
    crt_bvh.glsl         traceScene()/occluded() via two-level BVH; CRT_BVH_CWBVH picks the wide traversal
    crt_linear.glsl      traceScene()/occluded() via brute force
    crt_shape.glsl       hitShape(), shapePart(), shapeNormal(), shapeUv(), shapeDerivatives();
                         mirrors src/shape.cpp
    crt_optic.glsl       the optical bodies of binding 55: surface records, sags, apertures, the
                         intersector, normals, opticReproject(); mirrors src/optic_surface.cpp (§8.28)
    crt_coating.glsl     coatingLookup() (binding 56's tables) and coatingExact() (the transfer
                         matrix); mirrors src/optic_coating.cpp
    crt_optic_material.glsl  the optical interface: media, absorption, dispersion, the ABg lobe
    crt_intersect.glsl   kernel 02's body: write the thin HitRecord, classify by material flags,
                         push the four classification queues
    crt_material.glsl    each material type's parameter record, read from binding 26: ClassicMaterial
                         for the six original types; mirrors the pack functions in src/rt_material.cpp
    crt_material_class.glsl  the material class a kernel 06 build is for: one CRT_MATERIAL_CLASS_<ID>
                         branch per class, which CMake reads to compile the permutations
    crt_interaction.glsl surfaceInteraction(): the surface rebuilt from a HitRecord and its ray;
                         facingShadingNormal(), shadingFrame()
    crt_scatter.glsl     the BSDFs + resolveMaterial() + materialShadingNormal() (the normal map)
                         + evalSurface()/pdfSurface()
    crt_surface.glsl     kernel 06's body: next-event estimation, continuation, the MIS record;
                         CRT_DIRECT_LIGHTING (and CRT_MIS_EXPONENT) pick the strategy
    crt_shadow.glsl      kernel 08's body: occluded(), then add the ShadowRay's contribution
    crt_light.glsl       light selection by power, per-kind sampling and pdfs, the environment
                         tables, misWeight(); mirrors src/light_sampling.cpp
    crt_background.glsl  backgroundRadiance() + skyGradient()
    crt_debug.glsl       bounceHeat(), traversalHeat(), debugTerminalValue()
shaders/image_error.comp  the Compare to reference error, 16×16 pixels per workgroup (not a kernel)
shaders/aov_view.comp     one auxiliary output as an rgba8 picture for its window (not a kernel)
shaders/relight.comp      the picture rebuilt from the light groups' sums (not a kernel)
```

`shaders/equirect.glsl` stays at `shaders/` (shared with `env_background.comp`) and is reached through the `-I${PROJECT_SOURCE_DIR}/shaders` that the root `CMakeLists.txt` now passes to `glslangValidator`. That same CMake block resolves each shader's path **relative to `shaders/`** so `.spv` output lands beside its source rather than colliding in one flat directory, and compiles a `.comp` that includes `include/crt_trace.glsl` once per traversal, to `<name>.comp.<id>.spv` with `-DCRT_TRAVERSAL_<ID>` (§8.16).

C++: `src/rt_kernels.h/.cpp` (registry: stages, traversals, integrators), `src/rt_gpu.h/.cpp` (`GpuRenderer`: uploads, film, reference error, resource table, pipeline cache), `src/rt_gpu_types.h` (C++ mirrors, `CrtParams`, `CrtResourceTable`), `src/rt_wavefront.h/.cpp` (`WavefrontPool`), `src/rt_schedule.h/.cpp` and `src/rt_path_schedule.h/.cpp` (the schedule interface and the path tracer's), `src/rt_renderer.h/.cpp` (panels, guard, BVH settings, direct-lighting settings), `src/rt_accel.*` (the TLAS, and the light list built with it) + `src/bvh*` (`occludedScene()` mirrors `occluded()`), `src/rt_lights.h/.cpp` (the light list), `src/light_sampling.h/.cpp` (static lib `rt_light`, no Vulkan: the CPU mirror of `crt_light.glsl` and the environment tables), `tests/light_test.cpp` (`bin/light_test`), `src/sampler.h/.cpp` and the generated `src/sampler_halton_tables.h` (static lib `rt_sampler`, no Vulkan: the CPU mirror of `crt_sampler.glsl` and the loader of `assets/sampler/`), `tests/sampler_test.cpp` (`bin/sampler_test`), `src/bsdf.h/.cpp` (static lib `rt_bsdf`: the CPU mirror of `crt_scatter.glsl`) with `tests/bsdf_test.cpp` and `tests/test_stats.h`, `src/image_io.h/.cpp` (static lib `rt_image`), `src/film.h/.cpp` (static lib `rt_film`: the CPU mirror of `crt_filter.glsl` and `crt_film.glsl`) with `tests/film_test.cpp` (`bin/film_test`), `src/aov.h/.cpp` and `src/render_export.h/.cpp` (static lib `rt_aov`: the CPU mirror of `crt_aov.glsl`, and an export's channels) with `tests/aov_test.cpp` (`bin/aov_test`), `src/vk_readback.h/.cpp`, `src/command_line.h/.cpp`, `src/gpu_selftest*.cpp` with `shaders/test/`, and `tools/verify/` (sections 8.21, 8.22 and 8.23).

### 8.3 The registry (`src/rt_kernels.h/.cpp`) — the file to edit

The enums and tables:

- `KernelSlot` — the stage library every integrator shares: 00..09 the path tracer's, later stages appended (the roadmap reserves 10..25). **The numeric values are the `NN` in filenames and are part of the scene file format; never renumber.**
- `CrtBinding` — **the value IS the descriptor binding number** and must match `crt_common.glsl` exactly. Append only.
- `KernelDomain` — `Pool` / `Pixels` (direct dispatch) or `CurrentRayQueue` / `FixedQueue` (indirect).
- `KernelSettings`, `TraversalCost` — capability flags the UI and the render guard read.
- `KernelVariant` — one table row: slot, `id` (scene-file constant), `name`, `description`, `shader`, `domain`, `queue`, `bindings`, `implemented`, `isDefault`, `traces` (built once per traversal, §8.7), `settings`, `nextEvent` (which lights a kernel 06 variant sends shadow rays to: `None`, `DeltaLights` or `AllLights`), for the samplers (§8.20) `writesRecord`, `cameraPass`, `needsSampleCount`, `sampler` and, on kernels 00 and 06, `readsRecord`, and for the films (§8.22) `keepsFilmRecord`, `keepsFilmCascade` and `passes`.
- `TraversalVariant` — one traversal: `id` (scene-file constant, and the `.spv` suffix), `name`, `description`, `bindings` (what the search reads, added to every tracing kernel's own), `settings`, `cost`, `requiresLayout`/`layout`, `isDefault`. Rows `bvh_binary` (default), `bvh_cwbvh`, `linear`.
- `IntegratorId` / `IntegratorInfo` — `key` (scene-file constant), `name`, `description`, `stages` in dispatch order. Row `path`, running 00..09.

`KernelSelection` is `uint32_t variant[Count]` plus `traversal` and `integrator`, `==`-comparable, serialised by id via `kernelSelectionToIds()` / `kernelSelectionFromIds()` (`KernelSelectionIds`). The variant flagged `isDefault`, or else entry 0, is a slot's default; `kernelVariants(slot)` returns a contiguous span, which relies on the registry being **grouped by slot** — keep it that way.

**Adding a variant = one `v.push_back(KernelVariant{...})`.** Nothing in `rt_gpu.cpp`, `rt_renderer.cpp` or `scene_io.cpp` names a strategy.

**A variant of a stage must work in every integrator that runs the stage.** A kernel that only makes sense inside one integrator is a new stage. Kernels 02 and 08 walk the same traversal because both are built for the one selected, which replaced `followsSlot`, the link that used to pair a kernel 08 variant with each kernel 02 variant.

### 8.4 Descriptor strategy: global numbers, per-variant subsets

The problem this solves: different variants want different resources bound.

- Binding numbers are **global and stable**, declared once in `crt_common.glsl`, mirrored by `CrtBinding`. One include file serves every kernel.
- Each variant declares the **subset** it uses; a tracing variant also gets its traversal's. `GpuRenderer::pass()` builds a set layout containing only those numbers, each once. **A Vulkan set layout with gaps is legal** — that is what makes this work.
- Each owner publishes its resources into `CrtResourceTable` by `CrtBinding` when it allocates or uploads them: `GpuRenderer` the scene, geometry, environment, film and counters (and the engine's environment map and texture array every frame), `WavefrontPool` the per-path buffers. `GpuRenderer::dispatch()` writes exactly the declared bindings from the table, one set per launch per frame from `frameDescriptors`. A declared binding nobody published aborts with its number.

Consequence: the linear traversal never binds `BlasNodes`/`TlasNodes` at all; kernel 09 binds three descriptors where a BVH kernel 02 binds fourteen.

**To add a resource:** new number at the *end* of `crt_common.glsl`, matching enumerator at the end of `CrtBinding` (same position), one publish call by its owner, and list it in the variants that want it. Existing numbers never move. Rejected alternatives and why: one fat 18-binding set for everything (what this replaced — junk-drawer growth); buffer device addresses for everything (most flexible, but rewrites every shader).

### 8.5 The queue set

`shaders/rt/include/crt_common.glsl`:

| Index | Name | Holds | Written by | Drained by |
|---|---|---|---|---|
| 0, 1 | `CRT_QUEUE_RAY_A` / `_B` | **path indices**, ping-pong per bounce | 00, 06 | 01, 02 |
| 2 | `CRT_QUEUE_ESCAPED` | **ray-queue positions** | 02 | 03 |
| 3 | `CRT_QUEUE_EMISSIVE` | ray-queue positions | 02 | 04 |
| 4 | `CRT_QUEUE_SURFACE` | ray-queue positions of hits on basic materials | 02 | 06, basic build |
| 5 | `CRT_QUEUE_SHADOW` | ray-queue positions, which also index `shadowRays[]` | 06 | 08 |
| 6 | `CRT_QUEUE_SURFACE_COMPLEX` | ray-queue positions of hits on complex materials | 02 | 06, complex build |

`CRT_QUEUE_COUNT = 7`; `m_queues` is `4 × CRT_QUEUE_COUNT × poolSize` bytes and `m_headers` is `CRT_QUEUE_COUNT` headers. The C++ mirrors are `constexpr` in `rt_wavefront.h` — **change both together**.

**Two indexing conventions coexist.** `paths[]`/`radiance[]` are by pool slot (stable for a path's life); `hits[]` and `shadowRays[]` are by ray-queue position (compacted, different every bounce). The classification queues store the ray-queue position *because it is also the hit index*, so a consumer reaches both hit and path from one number. `PathState.radianceSlot` maps back to the pixel.

`QueueHeader` is a `VkDispatchIndirectCommand` plus `rayCount`; `queueAppend()` keeps `groupCountX == ceil(rayCount/64)` by having each workgroup add the number of workgroup boundaries its contiguous range crosses. Headers are reset by `vkCmdUpdateBuffer` from the command buffer, never by a shader.

### 8.6 The schedule (`PathSchedule::record()`)

`GpuRenderer::record()` clears the film on a render's first frame, zeroes the traversal counters and hands the frame to the active schedule, then copies the counters back, measures the reference and tonemaps. The path tracer's schedule, between those:

```
fillBuffer(sampleBudget) on a render's first frame; resetHeader(RAY_A); barrier
01 camera pass                 direct over poolSize, bounce = CRT_CAMERA_PASS; a sampler that writes
                               the record only (§8.20)    [+ barrier]
00 Generate camera rays        direct over poolSize
barrier; copyHeader(RAY_A, 0)
for bounce in [0, rayDepth):
    resetHeader(next, ESCAPED, EMISSIVE, SURFACE, SURFACE_COMPLEX, SHADOW); barrier
    01 Generate samples        indirect over rayQueueFor(bounce)   [+ barrier]
    02 Intersect closest       indirect over rayQueueFor(bounce)
    barrier                                     <-- hits[], 4 queues, 4 headers
    03 Handle escaped          indirect over ESCAPED     no barrier: a miss is on no other queue
    04 Handle emissive         indirect over EMISSIVE
    barrier                                     <-- a glowing pbr hit is on EMISSIVE and SURFACE;
                                                    04 reads the throughput 06 overwrites
    06 Surface scattering      indirect over SURFACE, basic build; then over SURFACE_COMPLEX,
                               complex build. Only 06 writes paths[] and the next ray queue; the
                               two builds touch disjoint paths and need no barrier between them
    barrier
    08 Trace shadow rays       indirect over SHADOW      each shadow ray carries its own origin
    barrier; copyHeader(next, bounce + 1); copyHeader(SHADOW, CRT_MAX_DEPTH + 1 + bounce);
    copyHeader(SURFACE, 2 * CRT_MAX_DEPTH + 1 + bounce); copyHeader(SURFACE_COMPLEX, 3 * CRT_MAX_DEPTH + 1 + bounce)
09 Update film                 direct over width × height, `passes` times (2 for 0903: accumulate,
                               then resolve from the neighbors), a barrier between
barrier; copy the frame's counters to readback, 09's dropped-sample count last
image_error.comp against the reference, when one is captured; tonemap
```

Every `if (const KernelVariant* k = passFor(SLOT))` is skipped when the slot has no implemented variant, so 05 and 07 cost nothing. Kernel 08 always runs; a bounce with no shadow rays leaves its header at zero groups. `launch()` takes the dispatch size from `WavefrontPool::dispatchSize()`, by the variant's domain. **Nothing in this function names a strategy.**

### 8.7 The traversal contract

`crt_traverse.glsl` defines what a traversal strategy must supply:

```glsl
bool traceScene(vec3 origin, vec3 direction, float tMin, float tMax, out TraceHit hit);
bool occluded(vec3 origin, vec3 direction, float tMin, float tMax);
```

World-space ray, normalised direction. `traceScene()` returns the closest hit in `[tMin, tMax]`; `occluded()` returns true as soon as anything, alpha test included, blocks the segment. No kernel includes a strategy header directly. A tracing kernel includes `crt_trace.glsl`, whose `CRT_TRAVERSAL_<ID>` branches include the strategy, and sets `KernelVariant::traces`; CMake compiles it once per branch, and `GpuRenderer` runs the build of the selected traversal. Kernel 02 is `crt_common.glsl` + `crt_trace.glsl` + `crt_intersect.glsl`, and kernel 08 the same with `crt_shadow.glsl` last. **A new traversal** is a strategy file, a `crt_trace.glsl` branch, and a `TraversalVariant` row; **a new tracing kernel** is one `.comp`, built for every traversal.

Also in `crt_traverse.glsl`, shared by every strategy: the alpha test (`cutAway()`, called from `testTriangles()` for a triangle flagged `CRT_TRIANGLE_CUTOUT`, so no strategy can forget it); `TraceHit`; `g_nodesVisited`/`g_primitivesTested` (**every strategy must maintain these** — they are the hardware-independent measure, and this laptop throttles ~70% within a minute so timings are not comparable); `safeReciprocal()`; `testTriangles()`; `instanceRay()` (the object-space transform, direction deliberately **not** renormalised so `t` carries across unchanged); `testShapeInstance()`; `resetTrace()`.

Both strategy files implement `occluded()` with the same walk as `traceScene()` (`walkScene()` in `crt_bvh.glsl`, `scanScene()` in `crt_linear.glsl`) and an `anyHit` flag: `testTriangles()` takes it too, and `traceDone()` ends the walk at the first hit. `occludedScene()` in `bvh_scene.cpp` mirrors it, and `bvh_bench` checks it against brute force.

### 8.8 Changes to existing structures

- **`GpuInstance` is 80 bytes, not 64** (`rt_accel.h`, `static_assert` updated). Added `uint32_t triangleCount` + 3 pad, the first of which is now `lightBase` (§8.14). Filled from the new `BlasPlacement::triangleCount` in `packGeometry()`. Only `crt_linear.glsl` reads it — a BVH reaches triangles through leaves. It is 128 bytes since §8.17, which adds the object-to-world rows.
- **`CrtParams` gained `uint32_t instanceCount`** in place of one pad float; still 176 bytes. Deliberately distinct from `tlasInstanceCount`, which goes to 0 when the TLAS fails to pack — that must not make geometry vanish from a strategy that never walks a TLAS.
- **`GpuRenderSnapshot` gained `KernelSelection kernels`**, taken at Render like everything else, so changing a strategy mid-render restarts rather than mixes.
- **`RenderKey` gained `KernelSelection kernels`**, so a swap restarts a running render under restart-on-change.
- **`RaytraceTextureArray::init()` now transitions the array to `SHADER_READ_ONLY_OPTIMAL`.** Pre-existing bug, not introduced here: `rebuild()` only runs on a model change, so a scene with no models (a Cornell box of analytic shapes) bound an `UNDEFINED` image. It produced validation errors on every stage before the split and on kernel 06 after it.

### 8.9 UI wiring

- **"Raytracer Shaders" window** (`RaytraceRenderer::drawKernelPanel()`, Windows menu). An **Integrator** combo and a **Traversal** combo, then one row per stage the integrator runs, combo over `kernelVariants(slot)`, greyed when `!implemented`; a tracing stage names the traversal it is built for. Entirely registry-driven — a new variant, traversal or integrator appears with no edit to this function. The **Integrator** combo also heads the Render Settings window (`drawIntegratorCombo()`).
- **"Acceleration structure" section** returns early unless the selected traversal declares `KernelSettings::AccelerationStructure`. The **node-layout combo was removed**: `requiredLayout()` derives it from the selected traversal, because a CWBVH kernel cannot read binary nodes. Changing the selection calls `setAccelSettings()`, which rebuilds the BLASes only when the layout actually differs.
- **Render guard**: `sceneCostPerRay()` switches on the selected traversal's `TraversalCost`. `Acceleration` → `tlas.sceneSahCost`; `AllPrimitives` → `placedTriangles + instances.size()`. Four orders of magnitude apart on a real model, and the linear scan is precisely the case `WORK_PER_FRAME_BUDGET` exists to catch (a 1M-triangle linear render once took the window server down). `RaytraceRenderer::estimatedWorkPerFrame()` hands that cost to the active schedule's `estimatedWorkPerFrame()`; `PathSchedule`'s doubles it when kernel 06's `nextEvent` finds a light of its kind in the scene, since each bounce then walks the scene twice.
- **"Direct lighting"** (`drawLightingSettings()`, in the Render Settings window, shown while the integrator runs kernel 06): the "Strategy" combo over kernel 06's variants, and a line counting the light list by kind.
- **Readouts**: the per-bounce lines (paths alive, shadow rays) are the active schedule's `counters()`, printed by label.
- **"Compare to reference"** (`drawReference()`): "Use this render as reference" copies the accumulation into `m_reference`; from then on each frame's RMSE and relative MSE against it are read back and shown.

### 8.10 Scene file, payload versions 8 to 12

Version 9 adds one material type, `"pbr"` (`"albedo"`, `"metallic"`, `"roughness"`, `"emission"`, `"strength"`). Nothing else changed, so a version 8 file reads unchanged. Version 10 adds `"lights"` (§8.14). Version 11 adds `"integrator"` and `"traversal"` (§8.16). Version 12 replaces inline materials with the material library (§8.18). `readQuat()` normalizes an orientation only when its squared length is more than 1e-5 from 1, and reads a zero one as no rotation: re-normalizing a unit quaternion can move it by an ulp, which made orientations alternate between two floats on every save before 2026-10-19.

```json
"integrator": "path",
"traversal": "linear",
"kernels": {"00":"thin_lens", ..., "02":"closest", ..., "08":"occlusion", ...},
"accel":   {"builder":"spatial splits (SBVH)","maxLeafSize":4,"binCount":32,
            "traversalCost":1.0,"intersectionCost":1.0,"spatialAlpha":1e-05,
            "spatialBudget":0.3,"maxDepth":48}
```

By **id, not index**. Before version 11 the traversal was kernel 02's variant, so `readKernelSelection()` reads an older file's `"02"` as the traversal and skips its `"08"`, which was never read; a file from before shadow rays, which says `"08":"none"`, still opens without a warning. A file without `"kernels"` opens with kernel 06's default, `mis_power`. `kernelSelectionFromIds()` collects an unknown integrator, traversal, slot or variant id into `SceneDescription::kernelWarnings`, which `openScene()` folds into the `IoResult` problem list; the slot keeps its default. Both blocks are optional, so every file back to version 1 still opens. **The node layout is deliberately not saved** — it follows the traversal, so storing it would let a file contradict itself. `newScene()` resets both to their defaults.

### 8.11 Verified behaviour (2026-09-25 smoke run, hooks since stripped)

Cornell box, 200×150, 16 spp, fixed seed 12345, validation layers on:

- `bvh_binary`, `bvh_cwbvh` and `linear` produce **bit-identical** rgba32f accumulation buffers (max abs difference 0.0 across all channels). The traversal swap is transparent, and the 03/04/06 split did not change the estimator.
- **Zero validation errors** after the texture-array fix in §8.8.
- Scene round trip: saved with `linear` selected → reopened as `linear`. A version-4 file (`assets/scenes/sphere_scene.gltf`) opens with the default `bvh_binary`.
- `bin/bvh_bench assets/structure.glb --rays 500`: PASS, 0 mismatches across all 4 builders × 2 layouts.

The shadow-ray work repeated it: `bsdf` renders bit-identical to the commit before it (Cornell box 200×150×16, the sphere scene 200×150×16, Sponza 160×90×4), and the three traversal pairs agree bit for bit with shadow rays. `docs/plans/completed/shadow-rays-nee.md` §9 has the convergence numbers.

The integrator layer repeated it on 2026-09-29: eight renders (the Cornell box under all three traversals and under `bsdf`, the sphere and Veach scenes, Sponza under both BVH layouts) matched the commit before byte for byte, with zero validation messages. `docs/plans/completed/integrator-layer.md` has the details.

The bit-identical comparison is the regression test to repeat after any change in this area; it is far more sensitive than looking at an image.

### 8.12 glTF materials (2026-09-25)

Every glTF material now becomes the `pbr` material type (since §8.18 a library entry made by `gltfMaterialDef()` in `rt_accel.cpp`) with its textures as layers of `RaytraceTextureArray`, which holds base colour, normal, metal/rough and emissive textures alike (up to 256 layers of 512 x 512).

- **`CrtMaterial` / `GpuMaterial` was 64 bytes**: adds the alpha cutoff, metallic, roughness, emission and the normal, metal/rough and emissive layers. Since §8.18 that record is `CrtClassicMaterial` in binding 26, behind a 16-byte header.
- **`GpuTriangleAttributes` is 80 bytes**: adds `uvec4 tangents`, three octahedral-packed vertex tangents plus handedness and missing-tangent bits. `MeshAsset::cpuTangents` holds the source, from the file's `TANGENT` or generated from the uvs. It never reaches the raster vertex buffer.
- **`BvhTriangle::v0.w`'s top bit** (`BVH_TRIANGLE_CUTOUT`) marks a triangle whose glTF material is `alphaMode` `MASK`. Mask the index with `BVH_TRIANGLE_INDEX_MASK` / `CRT_TRIANGLE_INDEX_MASK` before using it. The CPU traversals ignore the bit.
- **Binding 12 is `materialTextures`** (`CrtBinding::MaterialTextures`). The traversal (alpha test), kernel 04 (emissive texture) and kernel 06 (base colour, metal/rough, normal maps) declare it.
- **Normal maps are applied in kernel 06** since 2026-10-18 (§8.17), in `materialShadingNormal()`. The front face is still decided by the unmapped normal, and a mapped normal that faces away from the ray is dropped.
- **Radiance slots accumulate.** Kernels 03, 04 and 08 add to the slot rather than write it, because a hit on a `pbr` surface with emission goes onto both the emissive and the surface queue, and a shadow ray adds to the path that sent it.
- **Punctual lights** (`KHR_lights_punctual`) load into `LoadedModel::lights`, and each import turns them into `SceneLight` objects (§8.14).

The Cornell box and sphere scenes render bit-identical to the previous commit with a fixed seed.

### 8.13 Deliberate behaviour change

Kernel 01 hashes each live path's PCG state with the bounce index, where the pre-refactor tracer drew sequentially from one stream. **The noise pattern differs from before; the estimator and the converged image do not.** Slot 01 exists so a low-discrepancy sampler can replace one file, the reason pbrt gives sample generation its own kernel (PBR 4ed §15.3.5); since 2026-10-19 the samplers are its other variants (§8.20).

New scenes, and files without `"kernels"`, select `mis_power` for kernel 06, so their noise differs from before; the converged image does not. Selecting `bsdf` renders bit-identical to the tracer before shadow rays, in a scene without punctual lights. Punctual lights were not rendered at all before.

### 8.14 Light sampling and MIS (2026-09-25)

Design and verification: `docs/plans/completed/shadow-rays-nee.md` (§9 is the as-built record). Readable account: `docs/raytracing-overview.md` §5.

- `PathState` is 64 bytes and carries the MIS record: `misPdf`, `misExponent` and two pads follow `bounce`. Kernel 00 sets `misPdf = CRT_MIS_FULL` (-1), and only kernel 06 changes it, for the ray it continues: -1 for full weight (BSDF sampling, or a specular surface), 0 when light sampling already covered every light in the list, otherwise the BSDF sample's solid-angle density. Kernels 03 and 04 compute the light sampling density of what the ray found, from `path.origin`, and weigh by `misWeight()`. Only a weight other than 1 is multiplied in, so `bsdf` adds exactly what it did before.
- The light record a hit landed on is `SurfaceInteraction.lightIndex` (`CRT_NO_LIGHT` otherwise), which `surfaceInteraction()` finds; until 2026-10-18 kernel 02 stored it in the hit record (§8.17). A shape's is `GpuInstance.lightBase`, plus the box face (`HitRecord.primitive`) for a box; a mesh triangle's is `triangleLights[lightBase + triangle index]`.
- Binding 18, `Lights`, is a 32-byte `LightHeader` (count, `deltaCount`, the environment's index, inverse total and delta powers) and 96-byte `GpuLight` records. Binding 19, `ShadowRays`, is 48 bytes per ray-queue position since §8.17. Binding 20, `TriangleLights`, maps each emissive mesh triangle to its record, and binding 21, `EnvironmentSampling`, holds the two environment tables.
- The light list (`src/rt_lights.cpp`, `LightListBuilder`) is built by `buildSceneAccel()` on every scene edit, in world space. Punctual lights come first, because `lights[0, deltaCount)` is what the delta-only alias table covers; `addPunctual()` refuses a light once any other kind was added. Then shape faces (a box's six in -x, +x, -y, +y, -z, +z order), emissive triangles and the environment. Each record carries its entries in two Vose alias tables, one over every light and one over the delta lights.
- `GpuLight.a/b/c` hold, by kind, a sphere's centre and radius; a rectangle's corner and the two edges from it; a cylinder's centre and radius, unit x axis and height, and unit length axis (its caps are part of the one record); a triangle's vertices, with `a.w` the attribute record, `b.w` the emissive layer and `c.w` the material, bit-cast. Point and spot keep position and range in `a`, the spot its direction and cone cosines in `b` and `c`; directional keeps its direction in `a`.
- A light's power is πAL for a sphere, box face and cylinder; 2πAL for a quad and a triangle (two-sided), times the emissive texture's mean over the triangle (`RaytraceTextureArray::meanLuminance()`, 16 stratified points on a CPU copy of the layer); 4πI for a point; 2πI((1 − cos θi) + (cos θi − cos θo)/3) for a spot; πr²E for a directional light and πr²∫L dω for the environment, with r the scene's bounding-sphere radius.
- The environment tables are pbrt-v4's `PiecewiseConstant2D`, built by `buildEnvironmentDistribution()` when the map loads (`VulkanEngine::m_environmentDistribution`) and uploaded device-local by `GpuRenderer::uploadEnvironment()`. `envData[]` holds table 0 (luminance × sin θ, for light sampling alone) then table 1 (the same with the mean subtracted, for MIS), each as marginal CDF (h + 1), row CDFs (h × (w + 1)) and function values (h × w). Kernel 03 picks the table by the sign of `misPdf`.
- The header readback holds `READBACK_HEADERS_PER_SLOT = 2 × CRT_MAX_DEPTH + 1` headers per frame slot, the shadow queue's at `CRT_MAX_DEPTH + 1 + bounce`. The traversal counters double in size, the shadow rays' at `2 × CRT_MAX_DEPTH + 2 × bounce`, and feed `TraversalWork`'s shadow fields. `CRT_MAX_DEPTH` is 16.
- One more counter follows them, `traversalStats[CRT_STAT_DROPPED_SAMPLES]` at `4 × CRT_MAX_DEPTH`: the samples kernel 09 left out of the mean as non-finite. Kernel 09 declares `TraversalStats` for it, `record()` copies the counters back after kernel 09, and `GpuRenderer::droppedSamples()` totals them over the render for the panel's warning.
- The environment map's GPU copy is rgba16f, and a map whose peak exceeds half float's 65504 is stored divided by `VulkanEngine::m_environmentMapScale`, the smallest power of two that fits (`environmentStorageScale()`, `packEnvironmentTexels()`). `CrtParams.environmentIntensity` and the raster background's exposure carry intensity × scale. The light list prices the environment at the unscaled intensity, against tables built from the file's floats.
- `SceneLight` (`rt_scene_types.h`) is a scene object like a camera: kind, pose, colour, intensity in the tracer's radiometric units, range and cones. `sceneLightOf()` imports `KHR_lights_punctual` with candela and lux divided by 683, and `removeModel()` removes a model's lights with it. Scene payload version 10 saves them as `"lights"` records with the index of the `"model"` that brought them.
- `crt_light.glsl` and `light_sampling.cpp` are line-for-line mirrors; `bin/light_test` checks every sampler's pdf against its density function and its solid angle against a counted reference, and the environment tables' normalisation.

### 8.15 Traps

- `CrtBinding` values **are** binding numbers, and `KernelSlot` values **are** filename prefixes and scene-file keys. Neither may be renumbered.
- `kernelVariants()` assumes the registry is grouped by slot.
- A new resource must be appended in the **same position** to `CrtBinding` and `crt_common.glsl`, and be published into `CrtResourceTable` by its owner. A kernel that declares a binding nobody published aborts at its first dispatch, naming the binding number.
- `queueAppend()` must be reached by **every invocation of the workgroup in uniform control flow**; a dead lane passes `survives = false`. Kernels 00, 02 and 06 therefore have no early return. Kernels 01, 03, 04 and 09 append to nothing and may return early.
- Kernel 02 calls `queueAppend()` three times (once per classification queue) for exactly this reason.
- `crt_bvh.glsl`/`crt_shape.glsl`/`crt_light.glsl` and the `crt_common.glsl` structs are mirrored line for line by `bvh_layout.h`, `shape.cpp`, `light_sampling.cpp` and `rt_gpu_types.h`/`rt_accel.h`/`rt_lights.h`. Change both sides together, then run `bvh_bench` and `light_test`.
- A new kernel 06 variant must write `misPdf` for every path it continues. Leaving the value kernel 00 or the last bounce wrote counts a light found by the BSDF sample at the wrong weight, which shows as a wrong mean, not as noise.
- A tracing kernel must include `include/crt_trace.glsl` **directly** in its `.comp` (CMake looks for that line) and set `KernelVariant::traces`. Adding the include to an existing `.comp` needs a manual `cmake ..`; editing `crt_trace.glsl` re-runs the configure step by itself.
- A variant of a stage must work in every integrator that runs the stage. A kernel that only makes sense in one integrator is a new stage (numbers reserved in `docs/plans/pbrt-roadmap.md`), not a variant.
- A kernel that evaluates surface materials includes `include/crt_material_class.glsl` **directly** in its `.comp`, as a tracing kernel does `crt_trace.glsl`, and sets `KernelVariant::perMaterialClass`. CMake compiles it once per class (and per traversal, if it also traces) to `<name>.comp[.<traversal>].<class>.spv`; adding the include to an existing `.comp` needs a manual `cmake ..`.
- A material type's pack function in `rt_material.cpp` and its reader in `crt_material.glsl` agree on the record's layout by hand; change both together. Kernel 02 and the traversal read only the header's flags, so a flag the pack function forgets to set (`CRT_MATERIAL_FLAG_EMITS`, `SCATTERS`, `COMPLEX`, `CUTOUT`) misroutes hits silently.
- Every kernel that forms a hit point from `t` writes it as `fma(vec3(t), direction, origin)`. The shader compiler fuses `origin + t * direction` in some kernels and not others, and a point one ulp off changes kernel 04's MIS weight for a cylinder light (§8.17).
- A kernel that calls `surfaceInteraction()` declares the bindings it reads (`withInteraction()` in `rt_kernels.cpp`), even if it never uses the field that needs one: the SPIR-V still references the buffer, and the validation layer reports `VUID-VkComputePipelineCreateInfo-layout-07988`.
- A degenerate box face keeps its record at zero power, so the six faces stay in the order kernel 02 names them. A zero-power record prices at pmf 0, which gives a BSDF sample full weight.
- `environmentMap` holds the texels divided by `m_environmentMapScale`. A new shader that reads it must multiply by `pc.environmentIntensity`, which includes the scale, or it renders a bright map up to that factor too dark.
- `crt_scatter.glsl` (the six original materials, without textures or the normal map) is mirrored line for line by `src/bsdf.cpp`; change both together and run `bin/bsdf_test` and **Windows** → **GPU self-tests**.
- `crt_sampler.glsl` is mirrored line for line by `src/sampler.cpp`; change both together and run `bin/sampler_test`. Neither side may divide floats: a GPU's division is not IEEE, so the CPU mirror would drift by an ulp. Divide on the CPU and pass the reciprocal (`CrtParams::samplerInv*`), or tabulate it (`crt_halton_tables.glsl`).
- Code that only runs with a sample record goes behind the `CRT_SAMPLE_RECORD` specialization constant, never a run-time flag: a branch Metal cannot fold moved the fuzzy metal's arithmetic by one ulp and broke `pcg_hash`'s bit-identity (§8.20). A new kernel that includes `crt_samples.glsl` sets `KernelVariant::readsRecord`, or it is built for the default (no record) and reads PCG numbers under a sampler.
- `sampler` is a reserved word in GLSL; the push-constant field is `samplerOptions`.
- Both `.spv` outputs and `#include` resolution depend on the `-I` and relative-path handling in the root `CMakeLists.txt`; a new shader subdirectory works, a new include root does not without adding another `-I`.

### 8.16 The integrator layer (2026-09-29)

Design and as-built record: `docs/plans/completed/integrator-layer.md`.

- **Stage library.** `KernelSlot` is shared by every integrator. An integrator (`IntegratorInfo`) is a key, a name and the list of stages it runs; `path` runs 00..09. `KernelSelection` holds one variant per stage for every integrator, plus the integrator and the traversal.
- **Traversals** are scene-wide (`TraversalVariant`). Every tracing kernel is compiled once per traversal, as `<name>.comp.<id>.spv`, and gets the traversal's bindings added to its own, so the BVH code is written once and every integrator's tracing kernels use it.
- **`GpuRenderer`** (`rt_gpu.h`) holds what every integrator shares: the scene, geometry, and environment uploads, the film, the reference, the tonemap, the timestamps, the traversal counters, the dropped-sample count, the resource table, the pipeline cache keyed by (variant, traversal), and `WavefrontPool` (`rt_wavefront.h`), the per-path buffers. `init()` builds the default selection's pipelines; `start()` builds whatever else the selection needs.
- **`IntegratorSchedule`** (`rt_schedule.h`) records one integrator's frame between the renderer's shared head and tail. `createSchedule()` in `rt_schedule.cpp` ties each `IntegratorId` to its class. `PathSchedule` (`rt_path_schedule.h`) is the path tracer: the former `GpuPathTracer::record()` body, its header readback and its counters, and its guard estimate (the former `tracesPerBounce()` logic).
- **Push constants**: one `CrtParams` block for every kernel of every integrator, now in `rt_gpu_types.h`. `GpuRenderer::baseParams()` fills the integrator-independent fields; the schedule adds `samplesPerFrame` and `poolSize`.

### 8.17 The thin hit record (2026-10-18)

Design and as-built record: `docs/plans/completed/pbrt-thin-hit-record.md`.

- **`HitRecord` is 24 bytes**: `t`, `instance`, `barycentrics`, `primitive`, `traversalCost`, in that order so the `vec2` sits at an 8-byte offset. For a mesh `primitive` is the global `blasTriangles[]` index; for a shape it is the part hit (`shapePart()`: a box face in `boxFace()` order, a cylinder's side, bottom or top), and `barycentrics` holds the shape's uv.
- **`crt_interaction.glsl`** rebuilds a `SurfaceInteraction` from a hit and its ray: the point, `pError` (zero unless `CRT_FLAG_ERROR_BOUNDS`, §8.19), the geometric normal `n` and the shading normal `ns` (both outward as authored; a triangle's `n` is turned to its vertex normals' side), `dpdu`/`dpdv` and `dndu`/`dndv` in world space, the uv, the material and the light record, and `frontFace` against `n`. Kernels 04 and 06 call it. `facingShadingNormal()` turns `ns` to face the ray the way kernel 02 used to, for the existing materials.
- **Kernel 02** writes the record and classifies from the material alone: a triangle's surface index is in `BvhTriangle.v1.w` (set in `buildMeshBlas()`; it was `e1.w` before §8.19), so it no longer fetches the attribute record or the triangle-light table.
- **Normal maps** are applied by `materialShadingNormal()` in `crt_scatter.glsl`, with kernel 02's arithmetic unchanged. Kernel 04 uses the unmapped normal for its debug views, because only `pbr` materials carry maps and 06 writes their debug views.
- **`ShadowRay` is 48 bytes** with `origin` and a reserved `mediumAndGroup` word first. Kernel 08 no longer declares `Hits`.
- **`GpuInstance` is 128 bytes**: `toObject[3]` (the former `row0`..`row2`) and `toWorld[3]`, the object-to-world rows, which carry derivatives out of the instance.
- **Shapes have uvs and derivatives** in `crt_shape.glsl` and `shape.cpp`, in glTF's orientation: seen from outside, u runs right and v runs down, so `cross(dpdu, dpdv)` points into the surface. `bvh_bench` checks them against finite differences, and their continuity across the sphere seam and the box edges, before its BVH tests.
- **Debug view 8**, "shading vs geometric normal", is red where the shading normal the materials use and the geometric normal, both facing the ray, are more than 90 degrees apart. Cornell box: 0 pixels. Sponza at 320 × 180: 264 of 57,446 hit pixels.
- **Pool memory** is 176 bytes per path, down from 184 (180 since §8.18 adds a queue).
- **Verified**: 22 fixed-seed renders matched byte for byte. 17 compared against the commit before: the Cornell box under all four kernel 06 variants and all three traversals, the sphere scene, the Veach scene, Sponza under both BVH layouts, and the normal, MIS-weight and traversal-cost views. 5 were of a scratch scene with every shape kind, emissive boxes and cylinders, and glass boxes and cylinders, against a build of the old sources. The validation layer reported nothing across five configurations. The traversal-cost view of Sponza has two possible outcomes even on unchanged code (49 of 57,600 floats differ between runs), and the new build matched one of them.

### 8.18 The material system (2026-10-19)

Design and as-built record: `docs/plans/completed/pbrt-material-system.md`.

- **The library.** `RaytraceSceneEditor::materials()` holds every `MaterialDef` (`rt_material.h`): a type, its parameters by name in a `ParamSet`, an optional `ModelMaterialOrigin` (model key and glTF material index), and for a glTF entry the set of `edited` parameter names. Ids share the editor's id space. `SceneShape::materialId` and an override `SceneMeshObject::materialId` reference entries; a glTF-mode mesh object's surfaces use the entries its model's import made. Unreferenced user materials are kept; the **Materials** window has **Delete** (refused while referenced) and **Remove unused**.
- **`MATERIAL_TRAITS`** (`materialTraits()`) is the only per-type knowledge outside the shaders: name, label, `MaterialParam`s (name, label, `ParamKind`, default, range, flags, tooltip), type flags, and the pack, emission and preview-color functions. `SpectrumRef` is any spectrum kind (§8.25) and `TexRef` a legacy texture-array layer; the texture library's references are `MaterialDef::textures` (§8.32). Texture parameters and the alpha cutoff are `PARAM_READ_ONLY`: derived from the glTF file, re-resolved by `refreshModelMaterials()` after every texture-array rebuild, and never saved.
- **Import and removal.** `importGltf()` adds one entry per glTF material (`LoadedModel::materialsByIndex` and `materialNames` keep the file's order). `removeModel()` drops them, except one a shape or override still references, which becomes a user material without its textures, named in the status line.
- **Shapes and duplicates.** A new shape gets a new user material named after it; **Duplicate** copies the material too. A material on a shape is packed without texture layers (`PackContext::textures`), so a textured glTF material on a shape shows its factors.
- **GPU records.** Binding 7 is a 16-byte `GpuMaterial` header (`type`, `flags`, `paramOffset`, `paramCount`); binding 26, `MaterialParams`, holds each record as vec4s. The six original types pack the former 64-byte record unchanged as `CrtClassicMaterial`, read by `classicMaterial()` in `crt_material.glsl`. `buildSceneAccel()` packs one record per (library id, textures) pair.
- **Flags.** `CRT_MATERIAL_FLAG_EMITS`, `SCATTERS`, `COMPLEX`, `CUTOUT` and `NORMAL_MAP` are set by `packClassic()`. Kernel 02 classifies from them alone, and the traversal's alpha test reads parameters only for a `CUTOUT` material.
- **Two surface queues.** Kernel 02 sends a hit on a `COMPLEX` material to queue 6. Every kernel 06 variant is `perMaterialClass` with domain `KernelDomain::SurfaceQueues`, built as `.basic.spv` and `.complex.spv`, and dispatched basic first, then complex. The basic build evaluates basic types only; the complex build is pbrt's universal evaluator, every type. `glass` and `conductor` (§8.25) are the complex types; queue 6's counter shows while it has hits. `GpuRenderer`'s pipeline cache key is (variant, traversal, class).
- **Scene file.** Payload version 12 writes `"materials"` (user materials in full), `"materialOverrides"` (`model`, `material`, and the edited parameters) and a reference per shape and override object: an index into `"materials"`, or `{"model":m,"material":i}`. An older file's inline material becomes a user material named after its object; a glTF-mode mesh object's stored override material is dropped.
- **Verified** (seed 12345, rgba32f accumulation): the 22 renders of the thin hit record's checks matched the commit before byte for byte after the GPU records, after the library, and after the two queues. With `pbr` temporarily marked complex, all of Sponza's hits and a third of Veach's went through queue 6 and every render still matched, and the two queues' counts added up to the single queue's at every bounce. Every scene in `assets/scenes/` and the shape scene saved as version 12 and reopened with the same library. Sponza's edits saved as overrides, re-applied on reopen, and left the file after a revert. Zero validation messages across five configurations.

### 8.19 Robust intersections (2026-10-19)

Design and as-built record: `docs/plans/completed/pbrt-robust-intersections.md`. Camera-relative space was split off to `docs/plans/pbrt-camera-relative-space.md` and is not built.

- **Two flags**, `RenderSettings::errorBoundedOffsets` and `watertightTriangles`, become `CRT_FLAG_ERROR_BOUNDS` (16) and `CRT_FLAG_WATERTIGHT` (32) in `CrtParams.flags`. Both default on, including for an older file (payload version 13 saves them in `"render"`). Both off renders bit-identical to the commit before. The panel section is **Intersection precision**, apart from **Acceleration structure** since the linear traversal takes both.
- **`BvhTriangle` is three vertices** (`v0`, `v1`, `v2`; `v0.w` index and cutout bit, `v1.w` surface). Triangles that share a vertex hold identical copies, which the watertight test needs. Möller–Trumbore takes `v1 - v0` and `v2 - v0`, the floats the old layout stored.
- **`traceTMin()`** is 1e-30 under error-bounded offsets and `CRT_T_MIN` otherwise; kernels 02 and 08 trace with it. Strictly above zero, so a ray spawned exactly on a plane does not hit it at `t = ±0`.
- **Hit point and error.** `surfaceInteraction()` rebuilds `p` and `pError` under the flag: a triangle's from its barycentrics, a shape's from the object-space ray reprojected by `shapeReproject()` (`shapeObjectHit()` is shared with kernel 02). `toWorldPoint()` adds the instance transform's rounding and twice the round trip `toWorld (toObject p) - p`. `src/bvh_scene.cpp` `sceneHitPoint()` mirrors it for the tests.
- **Spawn.** Kernel 06 offsets the continuation and the shadow ray start with `offsetRayOrigin()`, and the shadow ray's end by the light normal that `sampleLight()` now returns (`CRT_LIGHT_END_GAMMA` 16). `sampleLight()` returns no point.
- **Watertight test.** `hitTriangleWatertight()` and `intersectBvhTriangleWatertight()`, with the axis permutation and shear computed in `instanceRay()` (`prepareWatertightRay()`). No double-precision fallback; a ray exactly through an edge is accepted by both triangles.
- **Box tests.** Slabs widen their far distance by `1 + 2 gamma(3)`. The CWBVH node test pads each axis's near and far distance by `gamma(4) (|offset| + 255 |scaled|)` (`boxSlack`).
- **Trap: exposing an intermediate changes the bits.** Returning `s.point` from `sampleLight()` changed how MoltenVK fused the sampling arithmetic, shifting renders by about 1e-6 with identical hits. Hand kernel 06 only what it needs, and re-run the flags-off comparison after any change to a sampler.
- **Verified**: see the plan. Flags off, eight renders and their counters match the commit before. `bin/bvh_bench` runs the checks before the file-based ones (**--robust N**).

### 8.20 Sample generation (2026-10-19)

Design and as-built record: `docs/plans/completed/pbrt-sample-generation.md`.

- **The record.** `RaySamples` (binding 23) is 48 bytes per pool slot, indexed by path index: `direct`, `indirect` and `medium`, a field per dimension of the bounce (`direct.x` the light choice, `direct.yz` the light point, `indirect.x` the lobe, `indirect.yz` the direction, `indirect.w` roulette; `direct.w` and `medium` are not generated yet). Dimensions are 0 to 5 for the camera and 6 + 12 b + field for bounce b. `WavefrontPool::ensure()` allocates it only while the selected kernel 01 variant `writesRecord`, and a 16-byte stub otherwise.
- **The camera pass.** A record-writing variant has `cameraPass`: `PathSchedule::record()` dispatches it over the pool before kernel 00 with `bounce = CRT_CAMERA_PASS`, and it writes the pixel offset and lens position into `direct`, time and wavelength into `indirect.xy`. Kernel 00 reads them; the bounce passes then overwrite the record.
- **Consumers.** Kernels 00 and 06 draw through `sampleDimension()`/`sampleDimension2D()` in `crt_samples.glsl`. They are built with the specialization constant `CRT_SAMPLE_RECORD` (`ComputePassBuilder::addSpecialization()`) set to whether the sampler writes a record, and `GpuRenderer`'s pipeline cache key is (variant, traversal, class, record). Without one they draw from the PCG stream in the old order and render bit-identical. The fuzzy metal's radius comes from the stream in either mode. With a record, kernel 00 salts the path's PCG seed (`CRT_RECORD_STREAM_SALT`).
- **The samplers** are `crt_sampler.glsl`, one `CRT_SAMPLER_<KIND>` per `.comp`, mirrored by `SampleGenerator` in `src/sampler.cpp`. Their parameters reach the shader packed in `CrtParams` (now 208 bytes): `samplerOptions` (randomization, block size, resolution, Halton exponents), `samplerHalton` (Halton's inverses), `samplerStrata` and three reciprocals. `makeSamplerParams()` computes them and `packSamplerParams()` packs them.
- **Tables.** `assets/sampler/*.bin` (pbrt-v4's Sobol matrices, PMJ02BN sets and blue noise; `tools/sampler_tables/convert_pbrt_tables.py`) go to binding 46, `SamplerTables`, once at start-up. PMJ02BN's pixel samples are rebuilt into a reserved region of it when a render with another sample count starts (`GpuRenderer::uploadPmjPixelSamples()`).
- **Fallback.** `GpuRenderer::start()` runs the independent sampler in place of one that cannot run the render (a sampler with `needsSampleCount` under unlimited samples, missing tables, an image side above 65535, PMJ02BN above 65536 samples) and logs why. The decision is `samplerFallbackReason()`, which the **Sampler** section also calls on the settings as they stand (`RaytraceRenderer::renderSettings()`), so it never shows a reason left over from the last render.
- **UI.** **Render Settings** → **Sampler** (`drawSamplerSettings()`): the variant, **Randomization** for the variants with `KernelSettings::Sampler`, the fallback note, and the record's size.
- **Scene file.** Payload version 14 adds `"samplerRandomization"` to `"render"`. `readKernelSelection()` adds `"01":"pcg_hash"` to a file that names no sampler; a new scene gets `zsobol`, the variant marked `isDefault`.
- **Verified**: `pcg_hash` against the build before, bit for bit, on five scenes (one with every material type), four strategies and two samples-per-frame settings; every sampler's record against `rt_sampler`, bit for bit, through bounce 15; every sampler converging to a 4096-sample reference; no validation messages. The plan has the error and GPU-time tables.

### 8.21 The verification harness (2026-10-20)

Design and as-built record: `docs/plans/completed/pbrt-verification-harness.md`.

- **`rt_bsdf`** (`src/bsdf.h/.cpp`) mirrors `crt_scatter.glsl`: `scatterSurface()`, `evalSurface()`, `pdfSurface()`, their helpers, and the parts of `crt_random.glsl` and `crt_samples.glsl` they draw from. `BsdfDraws` is the PCG state or a sample record, the shader's `CRT_SAMPLE_RECORD`. It has no textures or normal map.
- **`bin/bsdf_test`** (`tests/bsdf_test.cpp`) runs per case (material, parameters, incident angle): samples against density (chi-square over a 64 × 128 grid, the absorbed samples one more cell), mean weights against the integral of `evalSurface()` per coarse cell, per-sample `f cos / pdf` for the materials where it holds, directional albedo by sampler and by quadrature, reciprocity, delta checks, and corner samples. A lobe narrower than a cell lays its grid around the mirror direction, with rings that thicken away from it. `tests/test_stats.h` holds the chi-square, Sidak and quadrature code for any later test.
- **GPU self-tests** (`src/gpu_selftest*.cpp`, `shaders/test/`) run a shader on a fixed batch beside the same batch through the mirror. A test shader includes the real `crt_*.glsl` files, so it declares every binding they do and gets descriptors for the ones it uses. Only bindings 23 (the sample record) and 90 and 91 are used by `bsdf_parity.comp`; 90 and above are the tests' own and are not in the roadmap's tables. A struct holding an array does not copy in SPIRV-Cross's Metal output, so test structs use named members.
- **Traps.** `randomFloat()` is clamped below 1, and `scatterPbr()` draws its lobe choice before it tests for a material with no diffuse lobe: skipping the draw shifts the stream of every `pcg_hash` render of a fully metallic `pbr`. A fix near a branch that draws random numbers keeps the draw.
- **`--render`** (`VulkanEngine::beginBatch()`, `stepBatch()`; `VKGUIDE_VERIFY` until 2026-10-21, §8.23) renders a scene to N samples in the normal frame loop and writes the accumulation. Samples per frame sets its speed: one sample per frame is one engine frame, about 17 ms, so 4096 samples take over a minute at any resolution. The verification scenes use 32.
- **Reference images.** `GpuRenderer::readAccumulation()`, `readReference()` and `loadReference()` move images between the GPU and `Image` (`src/image_io.h`). `image_error.comp` writes four sums per workgroup (squared error, relative squared error, absolute percentage error, signed difference). `GpuRenderer::convergence()` is the running render's curve, one point per sample count, and `RaytraceRenderer` keeps the previous renders' curves.
- **`tools/verify/`** is Python with numpy only. `vkscene.py` builds version 7 scene files and runs the renderer; `cornell.py` is the one description of the Cornell box that the scene files, `radiosity.py` and `pbrt_cornell.py` are made from.
- **Analysis views** (`CrtAnalysisView`, `shaders/analysis_view.comp`) are display-time: a pass after the tonemap reads the film, the reference, `m_noiseBase` (a copy of the film from when its count last doubled, taken at the start of the next frame so the two differ) and the sample-count image, and overwrites the display image. They need no restart and no kernel knows them. The debug views (`CrtDebugView`) still replace shading inside kernels 03, 04 and 06. The noise view's estimate, `|x - b| sqrt(Ns / (N - Ns))` in the error view's relative RMS measure, agrees with the measured error against a reference to about 25 percent (0.0026 against 0.0032 on the Cornell box at 128 samples).
- **Trap: the film count is the sample index.** Kernel 00 seeds a path from `samplesTaken + k`, and the running mean (`0901`) counts accepted samples only, so with it a dropped sample is redrawn every frame at 1 sample per frame. See finding 7 of the harness plan. The compensated films count every spawned slot (§8.22).

### 8.22 Film and reconstruction (2026-10-21)

Design and as-built record: `docs/plans/completed/pbrt-film-and-reconstruction.md`.

- **Three stages' worth of variants.** Kernel 00 has `thin_lens` (`0001`, a sample uniform in its pixel) and `filter_importance` (`0002`, drawn from the pixel filter). Kernel 09 has `running_mean` (`0901`), `compensated_mean` (`0902`) and `outlier_reweighted` (`0903`). The new-scene defaults are `filter_importance` and `compensated_mean`; `readKernelSelection()` adds `"00":"thin_lens"` and `"09":"running_mean"` to a file that names neither.
- **A sample's weight is the `w` of its radiance slot.** Kernel 00 writes the sign of the filter where the sample landed (+1, or -1 for Mitchell and Lanczos); every other writer of `radiance[]` leaves `w` alone except the debug views, which write 1. Kernel 09's compensated variants read it as `sum(w L) / sum(w)`. `PixelState` (binding 29) is not allocated.
- **`samplesTaken`.** `CrtBinding::SamplesTaken` is binding 9, the `r32ui` image that was `sampleCount`. Kernel 00 and the samplers seed a path from it. `0901` advances it by the samples it accepts, `0902` and `0903` by every spawned slot, and keep the accepted count in `CrtFilmPixel::accepted`.
- **The film record** (binding 31, 48 bytes a pixel, `CrtFilmPixel`, `FilmPixel` in `crt_common.glsl`): two `vec4` of Neumaier sums and compensation terms, Welford's mean and M2, the accepted count. `GpuRenderer::ensureFilmRecords()` allocates it while the selected variant has `keepsFilmRecord`, a one-pixel stub otherwise, and `record()` zeroes it when a render starts. `0903` adds the cascade, binding 62, six `vec4` a pixel.
- **The filter table** (binding 30) is built and uploaded by `GpuRenderer::uploadFilterTable()` when the filter changes, through a staging buffer, like the environment's tables.
- **Fallback.** `filmFallback()` runs the compensated mean in place of the running mean when the filter has negative lobes or the clamp is on, and the running mean in place of a film record past 1 GiB. `GpuRenderer::start()` and the **Film** section call it, so the panel shows the reason for the settings as they stand.
- **Precision.** `crt_film.glsl` marks the compensation `precise`; Metal kept it, which the GPU self-test measures. The compensation is itself a float sum and holds only while the terms times float's epsilon stay well below 1 (exact to 2^17 terms).
- **Shared bodies.** `crt_camera_rays.glsl` (`crt_camera.glsl` until 2026-11-01) is kernel 00's body, and `0001`'s instruction stream after the refactor is identical to the one before once the ids are stripped (`spirv-dis`, then a diff). `crt_film_update.glsl` is kernel 09's compensated body.
- **Analysis pass.** `analysis_view.comp` gains binding 5 (the film record) and `hasFilmRecord`: **Dropped samples** compares samples taken with samples accepted under the compensated film, and **Standard error** reads the Welford sums.
- **Scene file.** Payload version 15 adds `"pixelFilter"`, `"clampSamples"`, `"clampValue"` and `"outlierKappa"` to `"render"`. `readRenderSettings()` starts from a box filter, so a file without the block renders as it did.
- **Tests.** `bin/film_test` (CPU), three GPU self-tests (`gpu_selftest_film.cpp`: parity with the mirror, kernel 09's accounting through the real shaders, `0903` against the mirror), and in `tools/verify/` `filter_check.py`, `film_check.py` and the filter section of `furnace_check.py`. `caustic_scene.py` writes `assets/scenes/caustic_glass.gltf`, a caustic of fireflies.
- **Traps.** A tracked verification scene regenerated by `make_scenes.py` differs from the committed one in `samplesPerFrame`; restore it. Kappa has to stay small against the samples per pixel (4 at a few hundred); at 64 samples kappa 32 darkens the Cornell box by a third. A render at one sample per pixel shows no filter, since a pixel is then one point sample.

### 8.23 Auxiliary outputs and image export (2026-10-21)

Design and as-built record: `docs/plans/completed/pbrt-aovs-and-image-output.md`.

- **Specialization constants.** A kernel with `KernelVariant::usesOutputs` (00, 03, 04, 06, 08, 09) is built with `CRT_AOV` (constant_id 1) and `CRT_LIGHT_GROUPS` (constant_id 2), beside `CRT_SAMPLE_RECORD` (0). `GpuRenderer::pipelineFeatures()` gives the bits (1 record, 2 AOV, 4 groups), and the pipeline cache key carries them. Off, the kernels are the code they were: the four older scenes, a scene with a fuzzy metal, glass and `pbr` under four samplers, and Sponza render byte for byte as before.
- **PixelState** (binding 29, `CrtPixelState`, 72 bytes of scalars) holds a camera path's first visible surface. Kernel 00 clears `state` each frame. Kernel 04 records a pure emitter (albedo 0) and kernel 06 a scattering surface, through `aovWantsSurface()`: bounce 0, or the first non-specular surface under **Capture through specular**. Kernel 03 records nothing: a miss has no surface.
- **AovImages** (binding 32, a `rgba32f` 2D array) keeps up to eight float4 layers (`AovLayer`): the weights and ids, position and depth, normal, shading normal, uv, albedo, and the variance's mean and M2. `makeAovLayout()` numbers the layers the render keeps, `AovLayout::pack()` puts the indices in `CrtParams::aovLayers`, and `aovFoldPixel()` in kernel 09 loads a pixel's layers, adds each accepted sample (the arithmetic of `AovPixel::add()`) and stores them once. All three kernel 09 variants call it, the outlier film in its first pass.
- **Albedo.** `aovCapture()` in `crt_surface.glsl` averages `scatterSurface()`'s weights over pbrt's 16 numbers, held in `crt_samples.glsl` and answered by `sampleDimension()` while `g_albedoSample >= 0`. The fuzzy metal case in `crt_scatter.glsl` reads the same table. `bsdfAlbedoEstimate()` mirrors it.
- **Light groups.** `GpuLight::group` (from the light, the emissive material or the environment), `ShadowRay::mediumAndGroup >> 24`, and `CrtParams::environmentGroup` give each contribution's group. `ChannelSums` (binding 45) holds `3 g + 1` floats a pixel, added to by `atomicAdd()` in kernels 03, 04 and 08 and, for `sum(w)`, plainly by kernel 09. `shaders/relight.comp` divides the weighted sums by `sum(w)` into a linear image the tonemap reads in place of the film.
- **Output windows.** `GpuRenderer::syncAovViews()` creates and registers one rgba8 image per view when a render starts, and `recordAovViews()` runs `shaders/aov_view.comp` over the layers after the tonemap each frame.
- **Ambient occlusion** is `CrtDebugView::AmbientOcclusion`. Kernel 06 sends the ray through `spawnShadowRay()` and kernel 08 adds 1; `debugTerminalValue()` answers the miss (0) and the emitter (1) in kernels 03 and 04.
- **Export.** `RaytraceRenderer::exportRender()` reads the accumulation, the layers, the sums and the sample counts through `src/vk_readback.h`, builds the channels in `src/render_export.cpp` and writes them with `saveChannelImage()`. A PNG reads the display image back and writes it with `saveImage8()`. **Save when finished** runs it from `RaytraceRenderer::update()` when a render reaches its target.
- **Batch mode.** `src/command_line.h` parses `--render`, `--out`, `--spp`, `--seed`, `--size`, `--timeout`, `--aov`, `--capture-through-specular`, `--light-groups`, `--half`, `--debug-view`, `--ao-distance` and `--relight`. `beginBatch()` applies them over the scene's settings. The log lines start with `render:`.
- **Scene file.** Payload version 16 (see the changelog).
- **Tests.** `bin/aov_test`, three GPU self-tests (`gpu_selftest_film.cpp` runs kernel 09's fold through the real shader with `runFilmKernel()`, `gpu_selftest_aov.cpp` the albedo and the views), `bin/exr_dump` and `bin/image_convert`, and `tools/verify/aov_check.py`.
- **Traps.** A kernel that declares bindings 29, 32 and 45 must be given them in every harness that builds it from raw bindings: MoltenVK numbers Metal's buffers from the layout, and a missing binding collides with another ("cannot reserve 'buffer' resource location"). The SPIR-V of kernels 03, 04 and 08 declares `AtomicFloat32AddEXT` whether or not light groups are on, so the device must enable the feature. A render's group sums leave out a non-finite term and ignore the clamp and the outlier film.

### 8.24 pbrt scene import (2026-10-21)

Design and as-built record: `docs/plans/completed/pbrt-scene-import.md`.

- **The pipeline.** `importPbrtFile()` (`src/pbrt_import.h`) parses with `parsePbrtFile()` (`src/pbrt_parser.h`) into a `PbrtScene` and maps it to an `ImportedModel` (`src/imported_model.h`), all without Vulkan (`rt_pbrt`). `VulkanEngine::importPbrt()` hands that to `loadImportedModel()` in `src/vk_loader.cpp`, which decodes the images, writes a raster material per material, uploads the meshes and makes one top-level `MeshNode` per `ImportedNode`. `addModel()` then does what every import does: key, `rebuildSceneDerivedData()`, library entries through `modelMaterialDef()`, default mesh objects. `importModel()` picks glTF or pbrt by the file name, for the File menu and for `openScene()`.
- **The parser.** One `Scanner` per open file (pbrt's `Tokenizer::Next()`); `Parser::directive()` is pbrt's `parse()` switch and keeps `GraphicsState` (two transforms, the active-transform bits, reverse orientation, the material by index or name, the area light, media, `Attribute` defaults) on a stack of `StackEntry`. `Import` saves the state and restores it at the start of the directive after its file ends (`m_pendingRestores`). Parameters remember lookups (`PbrtParam::used`), which `Importer::reportUnused()` turns into report lines.
- **The world transform.** `Importer::W` is a mirror, `diag(1, 1, -1)` or the identity when the camera transform's determinant is negative (a `Scale -1 1 1` before `LookAt`), followed by the quarter-turn rotation that takes the scene's up axis to +y (`chooseWorldMirror()`). The up axis is the one nearest the camera's up when that is pbrt's +y or +z; otherwise a camera within 25 degrees of looking along an axis is taken to look down it (watercolor's and kroken's plan views), and failing that the camera's up is used. The import report names any up axis other than +y. `W` multiplies every shape, light, instance and the environment lookup. The camera is `W * inverse(cameraFromWorld) * diag(1, 1, -1)`.
- **What belongs to the model.** `LoadedModel::imported` keeps the material definitions, lights, shapes, cameras, environment, settings and report (its meshes and images are cleared after upload). The model owns its lights (`SceneLight::modelKey`), spheres (`SceneShape::modelKey`) and environment (`VulkanEngine::m_environmentModelKey`, saved as `"environmentModel"`), and `removeModel()` removes them. Its camera is the scene's; `m_modelCameras` remembers which camera **Use pbrt settings** renders from.
- **Meshes without normals.** `MeshAsset::cpuNormalsMissing` makes `buildMeshBlas()` write each triangle's own normal into its three `GpuTriangleAttributes` normals, so the path tracer shades it flat; the raster preview gets smoothed normals.
- **Normal maps.** The importer supplies tangents in pbrt's frame (`normalMapTangents()`, w = +1), so they do not go through the glTF loader's `generateTangents()`.
- **Traps.** pbrt resolves every relative path against the first file's directory, not the including file's. pbrt's `maxdepth` counts bounces and the renderer's ray depth segments, so the depth is `maxdepth + 1`. pbrt's image carries the imaging ratio and the EXR export does not; `pbrt_compare.py` reads it from `render: pbrt imaging ratio`. A scene without an infinite light needs a black background, which the import sets, or the renderer's sky lights it.
- **Scenes of many objects.** The raster pass draws `VulkanEngine::m_meshDrawList` (`MeshDrawList`, `src/raster_draw_list.cpp`, compiled `-O2` in every configuration), rebuilt only when `RaytraceSceneEditor::contentRevision()` or `m_sceneRevision` moves and culled per frame by world-space boxes against the frustum's planes. `contentRevision()` skips camera edits (`markCameraChanged()`), which is also what lets `RaytraceRenderer::ensureSceneAccel()` skip its by-value comparison of the scene on a frame that changed nothing. The **Scene** panel lists a model's objects through `ImGuiListClipper`. `drawGeometry()` draws each run of one surface with one material as one instanced draw, and skips the surfaces its occlusion queries found hidden. `RaytraceMeshData::findNode()` is a binary search over nodes sorted by model key and node index. In a Debug build with no render running, `sanmiguel-courtyard` (69,408 objects) looking into the courtyard took 1,864 ms a frame, 1,806 ms of it in `vkQueueSubmit` (see CLAUDE.md, "No buffer device addresses in raster shaders"); it takes 23 ms. `pavilion-day` (97 million triangles) is bound by the GPU's triangle rate, about 1.25 billion a second, and occlusion culling takes its worst view from 63 ms of GPU time to 24. pbrt's `landscape` (4.3 billion triangles) opens, and its raster preview takes 1 to 2.5 s a frame.

### 8.25 Spectral rendering (2026-10-23)

Design and as-built record: `docs/plans/completed/pbrt-spectral-rendering.md`.

- **Builds.** A `.comp` that includes `include/crt_spectrum.glsl` is compiled without `CRT_LANES` (the exact RGB build, `Spectrum` = `vec3`, today's `.spv` name) and once per `VKGUIDE_SPECTRAL_FAMILIES` entry (`1;4;8;16;32`) as `<name>.comp[.<traversal>][.<class>].lanes<N>.spv`. `KernelVariant::laneDependent` marks kernels 00, 03, 04, 06 and 08. `GpuRenderer::pass()` adds `laneFamilySuffix()` and the pipeline cache key carries the family in `pipelineFeatures()`: bits 0 to 3 the sample record, outputs, light groups and spectral images, bits 8 to 15 the family, 16 to 19 the representation, 20 to 23 the sampling.
- **Specialization constants.** 3 `CRT_REPRESENTATION_SPEC`, 4 `CRT_SAMPLING_SPEC` and 5 `CRT_SPECTRAL_IMAGES`, beside 0 to 2. A lane loop in the 8-, 16- and 32-lane builds runs to `CRT_LANE_END` (`spectralWords[1]`).
- **Data.** `SpectralDataBuilder` (`src/spectral.h`) packs binding 24: the header (`SH_*`), CIE tables, the illuminant, the bands (16 words each), the wavelength mixture, and the pool of packed references (`packSpectrumRef()`, tags `TAG_RGB` to `TAG_BANDS`, the role in bits 8 to 15). `SceneSpectra` interns the scene's references, `buildSceneAccel()` fills it from each `PackedMaterial::spectra` slot and each light's `LightSpectrum`, and `SH_REFS` indexes it on the GPU (`sceneSpectrumRef()`). `GpuRenderer::uploadSpectral()` uploads binding 24 and the uplift table (binding 25) and falls back to exact RGB, with a note in the panel, when the table is missing.
- **Path state.** `PathState` keeps 64 bytes: `lambdaU`, `laneMask` and a pad take the exact RGB throughput's twelve bytes, and the throughput is `pathSpectra[]` (binding 63, `WavefrontPool::m_pathSpectra`). `ShadowRay` is `shadowRayBytes(family)`. `spectralBegin(lambdaU)` sets `g_lambda[]` and `g_lambdaPdf[]` at the top of each kernel that needs them.
- **Film.** `filmAdd()` in `crt_spectrum.glsl` converts at every add. `spectrumToFilm()` is the hero estimate or the band matrix, and `reflectanceToFilm()` the albedo output's conversion under the working space's white.
- **Materials and lights.** `ClassicMaterial::color` is a `Spectrum` in a spectral build (`materialAlbedo()`, `spectrumFromRefOrRgb()`). `glass` and `conductor` pack a `CrtBxdfMaterial` (`src/rt_material_record.h`, 64 bytes, laid out like `CrtClassicMaterial`) and evaluate through `ComplexMaterial` in `crt_scatter.glsl`, with `frontFace` set by the kernel. `GpuLight::spectrum` and `spectrumScale` replace two pad words. `sampleLight()` in `crt_light.glsl` keeps its RGB text under `#ifndef CRT_SPECTRAL`.
- **Display.** `TonemapDisplay` (`src/vk_tonemap.h`, 32 bytes of push constants) chooses `tonemap.comp`'s RGB, tint or colormap path for a monochromatic render. `RaytraceRenderer::m_spectralDisplay` holds the display and the scene file saves it as `"spectralDisplay"`.
- **Spectral images.** `spectralImageBuckets()` buckets after the light groups and the weight sum in `ChannelSums`, `channelStride()` in `crt_common.glsl`. `spectralImageAdd()` in `crt_light_groups.glsl` adds each lane; `buildExportImage()` writes the `S0.<nm>` channels.
- **UI.** `RaytraceRenderer::drawSpectralSettings()`, and `spectrumEdit()` (`src/spectrum_edit.h`) for every spectrum parameter in the **Scene** panel and the **Materials** window.
- **Tests.** `bin/spectrum_test`, `bin/bsdf_test --material glass|conductor` (`tests/bsdf_test_bxdf.cpp`), two GPU self-tests in `src/gpu_selftest_spectral.cpp` (`shaders/test/bxdf_parity.comp`, `spectrum_parity.comp`, built with `#define CRT_LANES 4`), `tools/verify/spectral_check.py`, and `pbrt_compare.py --spectral`.
- **Traps.** `1u << 32` is undefined in GLSL, and Metal gives `1u`: a full 32-lane mask is `spectrumFullLaneMask()`. Before the fix, dispersive glass at 32 lanes was 2.7 percent dark in hero and 25 percent bright in bands, and only those builds. Metal unrolls a loop with a constant bound, and the complex kernel 06 at 16 lanes then took 177 s to compile; at 32 lanes the compiler failed with "Compiler encountered an internal error". Keep one uplift lookup per evaluation and one emission evaluation in `sampleLight()`. A descriptor range is `VK_WHOLE_SIZE`, not the VMA allocation's size, which can exceed the buffer. `active` and `half` are reserved words in GLSL. `SpectralSceneContext::operator==` compares the uplift table pointer, so switching from exact RGB to a spectral mode rebuilds the records that hold uplifted RGB.

### 8.26 The BSDF library (2026-10-26)

Design and as-built record: `docs/plans/completed/pbrt-bsdf-library.md`.

- **Types.** `diffuse`, `thin_glass`, `diffuse_transmission` and `mix` join `glass` and `conductor` as complex-class `MATERIAL_TRAITS` rows (`MaterialType` values 8 to 11, `CRT_MATERIAL_*` in `crt_common.glsl`). The first three pack a `CrtBxdfMaterial` through `packBxdf()`: a diffuse's reflectance is `rgbA` and its Oren-Nayar A and B ride in `alphaX` and `alphaY`; a diffuse transmission's transmittance is `rgbB` and its scale `alphaX`; a thin glass's index is `etaDesign` and spectrum slot 2, as a glass's is.
- **BxDFs.** `src/bxdf.h` holds `DiffuseBxdf`, `DiffuseTransmissionBxdf` and `ThinDielectricBxdf` beside the dielectric and conductor; `crt_bxdf.glsl` mirrors them and dispatches on `BxdfMaterial::type`. Sampling uses pbrt's concentric-disk cosine warp, written out in both files.
- **Mix.** `ParamKind::Material` holds a `MaterialLink`. `packMix()` writes a `CrtMixMaterial` (children as table indices in `x` and `y` of the first vec4, the amount in `w`); `buildSceneAccel()` gets the indices from `PackContext::materialIndex`, which reserves a slot before packing so a mix's children follow it. Kernel 06's complex build calls `resolveMixMaterial()` before anything reads `si.material`, so the child's record, normal map and spectra are the ones used. `mixHash()` is PCG over the float bits of the hit point, the outgoing direction and the mix's index, in `bxdf.cpp` and `crt_bxdf.glsl`.
- **Scene file.** Payload version 19. A link is written like a shape's material reference (`MaterialLink::fileUser`, `fileModel`, `fileModelMaterial`); `VulkanEngine::saveScene()` converts ids to references and `openScene()` converts back after the library is built. An imported mix names its children by `MaterialLink::modelIndex`, which `addModel()` resolves.
- **pbr F0.** `ClassicMaterial::param` is the dielectric F0 for `pbr`, 0 for 0.04 (`pbrF0()`, `src/bsdf.cpp`). The `f0` parameter sets it; the glTF loader fills `GLTFMaterial::dielectricF0` from `KHR_materials_ior` and `_specular`, and `gltfMaterialDef()` turns a transmission of 1 into `glass`.
- **Tests.** `bin/bsdf_test --material diffuse|thin|mix` (`tests/bsdf_test_bxdf.cpp`; the thin pane and mix checks run with every filter), the BxDF parity GPU self-test, `bin/pbrt_test` (`testBsdfMaterials()`), and `tools/verify/bsdf_check.py`.
- **Traps.** `CRT_MAX_DEPTH` is 16, so a closed cavity of Lambertian and refracting glass loses light that total internal reflection holds past the limit (2.08 against 2.25 at index 1.5). `bsdf_check.py` avoids such cavities. A `mix` hides its children's emission and alpha cutoff, because kernels 02 and 04 read the mix's header. A GPU parity case that changes how many random numbers an earlier case draws moves the Fresnel check's worst case; the mix's amounts draw from their own stream.

### 8.27 Scene capabilities, the dock space and workspaces (2026-10-29)

Design and as-built record: `docs/plans/completed/physoptics-scene-capabilities.md`; the readable account, with the checks every later physoptics plan runs, is `docs/physical-optics-overview.md` §3 and §11.

- **The registry.** `CAPABILITIES` in `src/scene_capabilities.cpp`, one `CapabilityInfo` row per `CapabilityId` (`static_assert`ed): `key` (the scene file's, never renamed), name, description, `recommendedSettings` and `dependentObjects`, and one optional hook per UI site: `sceneFolders`, `addMenu`, `sceneStatus` (Scene panel), `menus`, `fileMenu` (menu bar), `renderSettings` (Render Settings window), `materialFolders` (Materials window), `viewportOverlays` (Viewport) and `workspace`. Every site walks the enabled rows with `forEachEnabledCapability()`. The only row is **Optical bench**, which fills `sceneStatus` (the mount library's placeholder warning, moved from the Scene panel, where it showed in every scene) and `workspace` (Optics), and recommends a fixed seed.
- **Engine state.** `VulkanEngine::m_capabilities` (`SceneCapabilities`: a bitset and `unknownKeys`), `m_sceneTemplate` (a label) and `m_pendingRecommendations`. `enableCapability(id, offer)` queues the note `drawRecommendationNotes()` shows at the top of the Scene panel; `disableCapability(id)` returns false while `dependentObjects` lists anything, and the Scene Capabilities window then shows the refusal. `openScene()` sets the capabilities from the file, keeps unknown keys with a warning, and turns on any capability whose objects the file holds.
- **Templates.** `BUILTIN_TEMPLATES` (`render_scene`, which is `newScene()`, and `optical_bench`, whose `populate` turns the capability on, applies its recommendations, places `bench_camera` at (0, 0.55, 0.45) m aimed at (0, 0.1, 0) m and sets a black background), then the files in `assets/templates/` (`listTemplates()`, read when the menu opens). `requestNewScene()` runs `newSceneFromTemplate()` between frames as `FileAction::NewFromTemplate`; a file template opens untitled. `saveTemplate()` writes `describeScene()` (the body `saveScene()` used to hold) with `templateKey` and `templateInfo`, leaving `m_scenePath` alone. The window title is `<file or Untitled> - <template name> - VkGuide` (`updateWindowTitle()`, recomputed only when the path or template changes).
- **Scene file.** Payload version 20: `"capabilities"` (keys, unknown ones after the known), `"template"` and, in a template file, `"templateInfo"` (`"name"`, `"description"`), each written only when non-empty. `SceneDescription::capabilities` holds raw keys; the engine resolves them, so `scene_io` does not depend on the registry.
- **Window registry.** `WorkspaceManager` (`src/workspace.h`) holds every top-level window as an `AppWindow` (name, `bool* open`, draw function, owning capability, `WindowGroup` Main or Debug, `drawnLast`), registered once by `VulkanEngine::registerWindows()`. `drawWindows()` draws the ones the scene allows, the Viewport last, and docks an open window the layout has no place for in the center (`SetNextWindowDockID`, `ImGuiCond_FirstUseEver`). The **Windows** menu lists Main windows, then **Images** (the `DisplayRegistry`'s), **Debug** (GPU self-tests, Dear ImGui Demo, ImPlot Demo) and **Workspace**. Since 2026-10-31 Stats is a Main window, open by default, and background is gone (§8.31).
- **Dock space and workspaces.** `beginFrame()`, first in the frame, calls `DockSpaceOverViewport()` with the id `ImHashStr("MainDockSpace")` and builds the active workspace's default with `DockBuilder` when it has none (`splitDockColumns()`: left column of two, center, right column of two). `applyPending()`, between frames, switches: it captures the layout being left with `SaveIniSettingsToMemory()` and loads the target's with `LoadIniSettingsFromMemory()`, which re-applies window settings (dock ids included) to live windows. The `[Workspace]` ini handler writes `[Workspace][State]` (`Version=1`, `Active=`) and per workspace `Open=` (names joined by `|`) and, for inactive ones, `Layout=` (ini text, `\n` and `\\` escaped). `initIMGUI()` reads `imgui.ini` itself, after registering the handler, and `afterIniLoaded()` discards a file without `[Workspace][State]`. `run()` saves the file on quit. `io.ConfigWindowsMoveFromTitleBarOnly` is on.
- **The Viewport window.** `drawViewportWindow()` shows the draw image's corner `m_drawExtent` = the window's content size × `m_renderScale` (clamped to the image), with `ImGui::Image` and uv `m_drawExtent / imageExtent`. The draw image stays the window's size in points, recreated only on a window resize, so dragging a splitter allocates nothing. `rasterProjection()` takes its aspect from `m_drawExtent`, not the window. `m_viewportHovered` (the image hovered, no window in front) is what a right press needs to look around, replacing `!WantCaptureMouse`. The overlays and `TransformGizmo::draw()` take a `ViewportView` (`src/viewport_view.h`: rect, matrices, the window's draw list, `project()`, `line()`); the gizmo uses `ImGuizmo::SetDrawlist()` and is enabled only while the preview is hovered or mid-drag.
- **First-person edit.** The Viewport keeps its own aspect, so `rasterVerticalFov()` widens the flown camera's fov until the camera's frame (its vfov at `renderAspect()`, the render resolution's aspect or under Match viewport the viewport's) fits, with `CAMERA_FRAME_MARGIN` (1.06) to spare on the tight axis: half height $H = 1.06 \max(t, a_c t / a_v)$ for $t = \tan(\theta/2)$. `flownCameraFrame()` gives the frame as fractions of the viewport's half extents, $(a_c t / a_v H,\ t / H)$; `drawViewportWindow()` greys the viewport outside it and outlines it in yellow, before the overlays. Checked against a path-traced render at 600 × 1200 and 1920 × 600.
- **`draw()`.** No blit: the swapchain goes `UNDEFINED → COLOR_ATTACHMENT_OPTIMAL`, `drawImgui()` clears it to black and draws the whole UI. The raster pass is skipped while the Viewport is closed, collapsed or a hidden tab (`m_viewportVisible`), which also resets the pause's stable-frame count; a drawn frame leaves the draw image in `SHADER_READ_ONLY_OPTIMAL`, which a paused or skipped frame keeps. The "Scene Mirror" display window is gone: the draw image is a `DisplayRegistry::registerTexture()` entry, unlisted, named "Viewport". `vkutil::copy_image_to_image()` has no caller left.
- **Render Settings.** `RaytraceRenderer::drawSettingsPanel()` holds the integrator combo, the settings, Direct lighting and every collapsing section, then the capabilities' `renderSettings`. Since 2026-10-31 it also holds the camera combo, after the integrator, and Compare to reference, after the analysis view; Render and Stop are in the main menu bar and the readouts in Stats (§8.31).
- **Startup.** The window opens maximized (`GLFW_MAXIMIZED`), and `m_windowExtent` is read back from it before the swapchain is made.
- **Verified.** The Cornell box, sphere and Veach scenes rendered byte for byte as the commit before (batch mode, 200 × 150, 16 spp). The GPU self-tests pass. Turning Optical bench on and off, the note, switching Render ↔ Optics with a window opened in one only, Reset Layout, two restarts (layout and active workspace kept; Optics falls back to Render for a scene without the capability), an unknown capability key round trip, the Optical bench template, and Save as Template followed by New from Template (an untitled scene saving the same as the template, less `templateInfo`) were driven by a temporary hook with validation layers on: no message other than those of the hook's own swapchain readback.

### 8.28 Optical bodies, materials and elements (2026-10-29)

Design and as-built records: `docs/plans/completed/physoptics-optical-surfaces.md`, `physoptics-optical-materials.md` and `physoptics-optical-elements.md`; the readable account is `docs/physical-optics-overview.md` §6.

- **`rt_optics`** (static, no Vulkan, always `-O2`, linked by `rt_bvh`): `optic_surface.*` (`SurfaceShape` Conic, Cylinder, EvenAsphere; `ApertureKind` Circle, Annulus, Rectangle, Polygon, Ellipse; `OpticPrescriptionT<T>` instantiated for float, double and long double; `intersectOpticBody()`, `surfaceNormalLocal()`, `principalCurvatures()`, `reprojectOntoSurface()`, `packOpticBody()`), `optic_glass.*` (`OpticGlass`, `parseAgf()`, `ciddorAirIndex()`), `optic_coating.*` (`OpticCoating`, `evaluateCoating()`, `transferMatrix()`, `presetCoatings()`, `coatingTable()`, `coatingTableRgb()`, `lookupCoatingTable()`), `optic_library.*` (`OpticalLibrary`: catalogs, user glasses and coatings, `indexLookup()`, `revision()`), `optic_catalog.*` (`OpticKind`, `OPTIC_TRAITS`, `SceneOptic`, the builders, `opticBodyKey()`, `defaultSlotMaterial()`, `traceEvents()`) and `optic_mesh.*` (`tessellateOptic()`). Test: `bin/optics_test` (`tests/optics_test.cpp`, `--rays`, `--root`, `--selfcheck`).
- **Instances.** An optic is a `SceneInstanceKind::Optic` on the CPU (`SceneInstance::optic`, the body in float; its TLAS box is the body's bounds, its SAH cost its surface count) and a shape instance of kind 255 on the GPU (`GPU_SHAPE_OPTIC`, `CRT_SHAPE_OPTIC`). `GpuInstance::attributeBase` is its first record in binding 55, `triangleCount` its surface count, `materialBase` its slot materials in `instanceMaterials[]` followed by one coating table index per surface, `opticMedia` (a former pad word) its media table and `opticSlots` its slot count.
- **Binding 55 `OpticSurfaces`.** Per body: six vec4 per surface (`CRT_OPTIC_SURFACE_VEC4S`, `OPTIC_SURFACE_VEC4S`), then the payloads (a cylinder's bounds, an asphere's terms, a polygon's corners), then two vec4 per medium: (n at the design wavelength, alpha at 630, 532, 465 nm) and (the index's `SceneSpectra` entry, the absorption's, flags). `buildSceneAccel()` packs each distinct body once by `opticBodyKey()` (`OpticPacker`).
- **Binding 56 `OpticCoatings`.** Per table: a header (columns, flags, layer count, kind), (lambda min, lambda step, design lambda, 0), then [2 directions][columns][91 cosines] of (Rs, Rp, Ts, Tp), the cosine at u = 1 - cos(pi j / 180) in the lower-index medium; then, for **Exact coatings**, three vec4 per layer and one for a metal. Flags: transmitted from plus 1, from minus 2, RGB 4, layers 8, opaque 16. Tables are cached across rebuilds (`CoatingTableCache`) by coating, media and `SpectralSceneContext`.
- **`CRT_OPTICS`**, specialization constant 6, `pipelineFeatures()` bit 4 and record-key bit 16, set from `RaytraceSceneAccel::opticCount`. `GpuRenderer::bindingsOf()` adds bindings 55 and 56 to every kernel that declares `Instances`, and `uploadScene()` publishes 16-byte stubs when the scene has no optics. `CRT_FLAG_EXACT_COATINGS` (1u << 6) in `CrtParams::flags`.
- **Shaders.** `crt_traverse.glsl` `testOpticBody()` inside `testShapeInstance()`; `crt_intersect.glsl` and `crt_interaction.glsl` rebuild an optic hit (`SurfaceInteraction::opticSurface`, `opticMedia`, `opticCoating`, `t`; `CRT_NO_OPTIC`); `crt_scatter.glsl` `ComplexMaterial::isOptic` and `optic`; `crt_surface.glsl` applies absorption and ends the secondary lanes before anything else at the vertex, and `continuationDelta` reads `g_opticSampledDelta` for the MIS record.
- **Material.** `MaterialType::OpticalInterface` (12, complex class), `OPTICAL_INTERFACE_PARAMS`, `packOpticalInterface()` writing a `CrtOpticMaterial` and, when TIS > 0, a `CrtAbgLobe` (208 bytes: A, r max, 32 CDF radii, 16 visible fractions). `ParamKind::Coating` holds a `CoatingRef` by key; `MaterialParam::labels` makes an Enum a combo. `MaterialDef::opticOrigin` (`OpticSlotOrigin`: optic id, slot) marks an element's slot material; `isUserMaterial()` is false for it.
- **Editor and UI.** `RaytraceSceneEditor` holds `m_optics` (`optics()`, `findOptic()`, `replaceOptics()`, `addOptic()`, `removeOptic()`, `setOpticSlotMaterial()`) and `SelectionKind::Optic`; `src/rt_scene_editor_optics.cpp` draws the folder, the Add menu and the panel (`drawSelectedOptic()`, `beginOpticGizmo()`). `src/optics_ui.*` draws the glass and coating combos, the **Glasses** and **Coatings** folders and details, the **Optics** menu and the Render Settings section, reached through the Optical bench hooks, among them the new `materialDetails`.
- **Length unit.** `src/length_unit.*`: `LengthUnit` (meters, millimeters), `currentLengthUnit()`, `formatLength()`, `dragLength()`, `dragLength3()`, `sliderLength()`, `inputLength()`; every length widget of the Scene panel goes through them. `VulkanEngine::m_lengthUnit`, `setLengthUnit()`, **Edit** → **Length Unit**.
- **Engine.** `VulkanEngine::m_opticalLibrary` (loaded with the bundled catalog at init, its scene part cleared by `newScene()`), `opticBuildContext()`, the file actions `LoadGlassCatalog`, `ImportCoatingReflectance` and `ImportCoatingTransmittance`. `drawOpticPreviews()` draws each optic into the raster pass: the mount library's PF blank for a preset planar mirror, otherwise a tessellation cached in `m_opticPreviewMeshes` by body key.
- **Scene file.** Payload version 21: `"optics"` (each a type, name, size, the parameters it sets, position, orientation and the slot materials that differ from the default), `"opticalLibrary"` (catalog paths, user glasses, user coatings), `"lengthUnit"` and `render.optics` (`surrounding`, `exactCoatings`), each written only when non-default. `SceneOpticRecord`; `describeScene()` skips slot materials, which travel in their optic's record.
- **Checks.** `bin/optics_test`; `bin/bvh_bench` (four optics in its scene); the **Optic parity** GPU self-test (`src/gpu_selftest_optics.cpp`, `shaders/test/optic_parity.comp`, bindings 90 and 91); `tools/verify/optics_scenes.py`, which writes `assets/scenes/optics_*.gltf`, and `tools/verify/optics_check.py`, both run by `run_all.py`.
- **Traps.** A float quadratic from a far origin cancels: re-center first. Newton refinement near a tangent must be limited (|dt| at most 1e-4 t). A hit's error bound must not vanish along a flat face's normal, or a flat window re-hits itself (185 in a white furnace). Coating tables sampled in angle miss the critical angle's rise. A size preset overrides the values it sets, so a test scene with its own thickness uses `"size": "custom"`. The transparent draw list must be cleared on a paused raster frame too (`updateScene()`), or the descriptor pool is reset under a live set.

### 8.29 Viewport navigation (2026-10-29)

- **The rig.** `Camera` (`src/camera.h`) is a position, yaw, pitch (clamped to ±89.5°) and `pivot`, with no input state. Its moves keep a point fixed on screen: `orbit(center, ...)` turns the camera's offset from the center by the same rotation as its frame, `dolly(center, factor)` scales that offset, and `look()` and `translate()` move it in place. Each takes `carryPivot`, which applies the motion to the pivot too, so a pivot on the view axis stays on it. A standalone test of these invariants (orbit and dolly keep the center's view coordinates, a clamped orbit still keeps it) passed; it is not in the build.
- **The controls.** `ViewportNavigation` (`src/viewport_navigation.h`, which lists the bindings) owns the camera, `ViewportCameraSettings` (`rt_scene_types.h`: speed in m/s, vertical fov, near and far clip, and their limits in `clamped()`) and `ViewportCameraPrefs` (look sensitivity, trackpad gestures, `ViewportOrbitPivot`). GLFW's mouse button callback starts a `ViewportDrag` from `dragFor(button, mods)` (RMB look, Option + LMB orbit, Option + RMB dolly, MMB pan) and captures the cursor through `setCameraCapture()`, which now also sets `ImGuiConfigFlags_NoKeyboard`. The cursor callback feeds `mouseMoved()`, the scroll callback `scrolled()`, and `updateViewportCamera()` builds a `ViewportFrameInput` (real dt clamped to 0.1 s, W A S D Q E from `glfwGetKey()` while looking, the trackpad's gestures, the preview's fov and height, a lazy selection center) and calls `update()`. The key callback is gone.
- **The pivot.** An orbit, dolly or pan turns around the selection's box center (`RaytraceSceneEditor::selectionBounds()`) while something is selected, else around `Camera::pivot`. Under `ViewCenter` looking, flying and panning carry the pivot, and the wheel moves the camera alone and then `pivotToViewAxis()` pushes the pivot ahead if the camera passed it. Under `LookAt` only `frame()` and `lookAt()` move it.
- **Framing.** `VulkanEngine::frameInViewport(all)` (F over the Viewport or with it focused, **Camera** → **Frame Selected** and **Frame All**) takes `selectionBounds()` or `sceneBounds()` (bounded shapes, visible mesh objects by their surfaces' boxes, optics by their prescription's bounds; else cameras and lights as 0.3 m boxes). `ViewportNavigation::frame()` fits the box's bounding sphere in the narrower half angle and eases the position over 0.25 s (cubic out); other input cancels the ease. A first-person edit frames with the flown camera's fov and `renderAspect()`.
- **Trackpad.** `src/trackpad.mm` installs an `NSEvent` local monitor for scroll and magnify events, which only watches. A scroll with `hasPreciseScrollingDeltas` and no momentum phase is a swipe, put in `swipe`, `shiftSwipe` or `controlSwipe` by its modifiers; a magnify is the pinch. `scrollIsFromTrackpad()` lets the GLFW scroll callback leave such a scroll, momentum included, to the gestures while they are on. A Magic Mouse reports precise deltas too, which is why the gestures are a preference. `trackpad_stub.cpp` elsewhere.
- **Persistence.** The settings and the pose are the scene file's `"viewport"` (payload version 22, `SceneViewportRecord`), applied by `openScene()`; a file without it, and `newScene()`, reset the settings and leave the pose. The Optical bench template sets 0.5 m/s and a 1 mm near clip (5 mm before 2026-10-30), and Optical bench recommends 1 mm. The preferences are imgui.ini's `[ViewportCamera][Preferences]`, registered before the file is read.
- **Verified.** A temporary hook in the running app framed the sphere scene (all 8 box corners on screen), orbited 20 frames (the pivot stayed at the screen center to 1e-7, yaw and pitch exactly the drag's), dollied and saved and reopened the scene (pose, pivot, speed and near clip exact). The trackpad's directions and the Option-click path were not driven by the hook.

### 8.30 The bench and the mounts (2026-10-30)

Design and as-built record: `docs/plans/completed/physoptics-bench-and-mounts.md`; the readable account is `docs/physical-optics-overview.md` §7.

- **The pose.** `src/optic_mount.h`: `MountKind` (Kinematic, Fixed, PlateHolder, Rotation, PrismMount, IrisAssembly, None; saved by `mountKindKey()`), `FineAdjust` (adjuster X and Y turns, rotor radians) and `MountPose` (the optic's origin's x and z, yaw, height, fork angle and reach, the fine adjustment, `freePlacement`, and `freePosition` and `freeOrientation` for a free or unmounted optic, `isFree()`, `freeTransform()`). `SceneOptic` holds `mount` in place of its old position and orientation, and `world`, the body-to-world transform `bodyToWorld()` returns, derived and never saved. `OpticTraits::mounts` lists a kind's mount kinds; `allowedMounts()` adds None.
- **`rt_optics`** gains `optic_bench.*` and links `rt_optic_models`. `BenchSettings` (`table`, `size`, `beamHeight`, `snap`), `benchGrid()`, `nearestHole()`, `isHole()`, `snapBenchSize()`, `buildBreadboardMesh()` (`BenchMesh`: the skins' indices, the side walls', then the holes' floors, and uvs of x and z in centimeters, which orient the skins' brushing). `BenchContext` (settings, a `MountModelLookup`, the `OpticBuildContext`). `opticMountModel()` picks the model by kind and size. `assembleStack()` returns an `OpticStack`: `opticToWorld`, the `StackPart`s (model, part, `StackRole` Mount, PostHolder or Fork, `toWorld` with the part's motion, `traced`), the post's ends, `holderTop`, `minimumHeight`, the fork's outline and bolt, `forkBlocked`, a `KinematicReadout` and notes. `opticWorldTransform()` is its `opticToWorld`. `opticOrigin()` is the point an optic is placed by, in its body frame; `OpticStack::origin` is it in world space and `postAxis` where the post stands. `assembleBench()` and `markForkClashes()` do every stack; `chooseFork()`, `placeOnBench()`, `refitFork()` and `dropOntoBench()` move one. `kinematicGeometry()`, `solveKinematic()` and `adjusterTurnLimits()` are the adjusters.
- **Mates.** The adjuster tips of KM05T/M, KM100, KM200 and KM200PM/M carry `angle_per_rev` (Thorlabs' angle per revolution) and `angle_source` beside `pitch`, in `tools/optical_models/manifest.json` and the mates files; the effective pitch is the angle times the lever `kinematicGeometry()` measures. A stack's parts come from the models' mates: `optic_seat`, `prism_seat`, `plate_seat` or `aperture_center`, `post_tap`, the holder's `base_contact` and `post_entry` (its `depth`), and the fork's `holder_seat`, `slot` (`length`) and `bolt`.
- **Editor.** `RaytraceSceneEditor::m_bench` (`bench()`, `setBench()`), `m_stacks` (`benchStacks()`, in the order of `optics()`), `resolveBench()`, which stores each optic's `world` and marks the scene changed when one moved, `SelectionKind::Bench` (the breadboard's row), `drawBenchSettings()`, `drawOpticMount()` (the mount combo, free placement, the pose, the post and fork readouts, the fine adjustment), `beginOpticGizmo()` (`TRANSLATE_X | TRANSLATE_Y | TRANSLATE_Z | ROTATE_Y` in world space at the optic's origin on the bench; translate and rotate in local space at the origin for a free optic), `otherStacks()` and `standOnBench()`. `resolveBench()` first runs `refitFork()` on every optic in order.
- **Engine.** `VulkanEngine::benchContext()`, `m_benchScene` and `updateBench()`, called after the panels each frame (before `draw()`), at the end of `newScene()` and in `openScene()`. When `BenchScene::update()` reports new meshes, `updateBench()` rebuilds `m_raytraceMeshData` and calls `prepareGeometry()`, which builds only the new BLASes. `RasterKey::benchAssets`. The **Bench Settings** window belongs to Optical bench, which also gains `rasterGeometry` (a new `CapabilityInfo` hook, called in `draw()` after `drawRaytraceShapes()`) and lists the breadboard in `dependentObjects`. `drawOpticPreviews()` skips a mounted iris, whose housing's blades show instead, and draws every optic with `GLTFMetallic_Roughness::opaqueBiasedPipeline` or `transparentBiasedPipeline` (`PipelineBuilder::setDepthBias()`, +1 and +0.25 under reverse-Z), the blanks through this frame's copies of their materials in `m_opticBiasedMaterials`, so an optic wins its tie with the mount face it rests on.
- **`BenchScene`** (`src/bench_scene.*`): uploads the breadboard (`ImportedModel` of `buildBreadboardMesh()`, again when the size changes: brushed stainless skins, `brushedStainless()`, a conductor with alpha 0.05 along x and 0.30 across, and black anodized side walls), the unit post cylinder, a red marker material, and each model a stack names (`MountLibrary::meshes()`), indexing their parts and each surface's `MountFinish` (`mountFinishOfMaterial()`). `draw()` puts the table, every stack part and every post into `m_mainDrawContext`, a blocked fork with the red material. `traceInput()` builds the `BenchTrace`. Released models wait in the frame's deletion queue.
- **Tracer.** `RaytraceMeshData::benchMeshes`: the bench's meshes after the models', by index. `BenchTrace` (`rt_accel.h`): `BenchMeshInstance` (mesh index, transform, a material id per surface, tag), `BenchPostInstance` (a `ShapeKind::Cylinder` instance), the finish materials (ids from `BENCH_MATERIAL_ID` = 2^62, from `mountFinishMaterial()`, plus the holes' floor), and `BenchTag`s. `buildSceneAccel(..., bench)` adds the posts after the optics and the meshes after the mesh objects, and fills `RaytraceSceneAccel::benchTags` and `instanceBenchTag` (GPU instance order, -1 for anything else). `ensureSceneAccel()` compares the trace with the last one it built from. No binding or kernel change.
- **Scene file.** Payload version 23: `"bench"` and each optic's `"mount"` (`mountJson()`, `readMount()`, `readFreePlacement()`). An optic without `"mount"` (version 22 or older) is unmounted at its position and orientation.
- **Reverse-Z.** The raster pass draws with `VulkanEngine::rasterDepthProjection()`: reverse-Z in Vulkan's 0 to 1 depth, built by hand (`glm::perspective()` maps to -1 to 1 here, and swapped it clips everything), depth cleared to 0, `GREATER_OR_EQUAL` on every raster pipeline. `rasterProjection()` stays OpenGL's for ImGuizmo and `ViewportView`. Both frustum culls (`is_visible()`, `MeshDrawList::cull()`) accept either depth direction; `env_background.comp` unprojects depth 1 as the near plane. The optics' depth bias is +1 and +0.25.
- **Preview color.** An 8-bit color texture is a UNORM image created with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and given an sRGB view, `AllocatedImage::colorView` (`createImage(..., colorView)`, destroyed by `destroyImage()`), which `GLTFMetallic_Roughness::writeMaterial()` binds as the color texture. The draw image is linear; `m_viewportEncodePass` (`shaders/viewport_encode.comp`) encodes it in place after `drawGeometry()`, never on a paused frame. `shaders/srgb.glsl` has `srgbEncode()` and `srgbDecode()`; `gradient_color.comp` and `sky.comp` decode their display colors. The texture array still blits the UNORM images, so the path tracer is unchanged.
- **Checks.** `bin/optics_test`'s `testBench()` (stacks, the example assembly, snapping, forks, adjusters, free placement, the breadboard) loads every mount model from `--root`. `tools/verify/optics_scenes.py` writes the check scenes' optics unmounted and `optics_bench.gltf` on a 700 × 300 mm breadboard, every kind in its default mount.
- **Traps.** A matrix saved as a quaternion comes back one bit off on every save: keep the free pose as a quaternion. `14 * 0.025` is 0.35000000000000003: snap sizes in millimeters. A stack's lowest height can be above the beam height (a 50.8 mm cube on KM200PM/M, 103.1 mm), so a check for "at the beam height" compares with the larger of the two.

### 8.31 The Stats window, the menu bar's Render and the background modes (2026-10-31)

The Raytrace Render and background windows are gone. Their controls moved to the menu bar, **Render Settings**, the **View** menu and **Scene** → **Background**, and their readouts to **Stats**.

- **Background.** `BackgroundMode` (`src/rt_scene_types.h`: Gradient, Solid, Sky, Environment; `backgroundModeName()` is the file key) is `VulkanEngine::m_backgroundMode`, beside `m_backgroundColor`, which Solid shows and which is kept while another mode is chosen. `environmentShown()` is Environment with a map loaded. `RaytraceSceneEditor::drawBackground()` is the radio group: editing the swatch chooses Solid, choosing Environment with no map queues `FileAction::SetEnvironmentMap`, and **Preview** calls `showEnvironmentPreview()`.
- **Mode changes.** `installEnvironmentMap()` chooses Environment and registers the **Environment Map** image hidden, or as visible as the replaced map's was. `clearEnvironmentMap()` falls back from Environment to Gradient. `newScene()`, the bench template and a pbrt import without an infinite light choose Gradient. `openScene()` sets the file's mode after the map loads, so an Environment whose map failed to load stays chosen, and the radio says no map is loaded.
- **Raster.** `drawBackground()`: Solid is `vkCmdClearColorImage` with the linear color; Gradient is `m_gradientPass` with the constants `GRADIENT_TOP` and `GRADIENT_BOTTOM`; Sky and Environment are `m_environmentPass` with `EnvironmentBackgroundPushConstants::sky`. Environment without a map draws the gradient. `RasterKey` holds `background` and `backgroundColor` in place of the effect index and its parameters.
- **Tracer.** `startRender()` turns the mode into the snapshot's three sources: `solidBackground` for Gradient, Solid and an Environment with no map, with black for all but Solid; `useEnvironmentMap` for `environmentShown()`; neither for Sky. `environmentLight()` puts the map in the light list only when `environmentShown()`. `RenderKey::background` restarts a render when the mode changes. `skyGradient()` moved to `shaders/sky_gradient.glsl`, which kernel 03 and `env_background.comp` include.
- **Scene file.** Payload version 24 writes `"backgroundMode"` and always `"background"`. A file of version 23 or older implies the mode: `"background"` is Solid, else a map or an `"environmentModel"` is Environment, else Sky. A version-23 copy of `sphere_scene.gltf` without a background renders bit-identical to a version-24 copy on Sky.
- **Rotation.** `VulkanEngine::m_environmentRotation`, degrees from -180 to 180, the slider under **Intensity**, saved as `"environmentRotation"` (written when not 0, wrapped into the range on read) and reset by `newScene()` and a pbrt import, whose map is already in the renderer's frame. `environmentRotation()` (`src/light_sampling.h`) gives its cosine and sine; `CrtParams::environmentCos` and `environmentSin`, and `EnvironmentBackgroundPushConstants::rotation`, carry them. Every map lookup reads `equirectUv(environmentToMap(direction, rotation))`, and `sampleEnvironment()` returns `environmentFromMap()` of the sampled direction. The tables stay in the map's frame, and the turn about +y changes no latitude, so no density changes. `RenderKey` and `RasterKey` hold it. Verified: light-only and BSDF-only direct lighting of a map turned 73° agree to 3.6e-5 of the image's mean, and -180 renders as 180.
- **Menu bar.** `RaytraceRenderer::drawMenuBarControls()`, after the capabilities' menus and before the workspace tabs: one button, **Render** or **Stop**, as wide as the wider word, at the bar's center unless the menus reach past it; the progress bar after it while a render runs; "Refused" with `m_blockedReason` as its tooltip; "(!)" while samples were dropped. `estimate()` (`RenderEstimate`) builds the TLAS if stale and prices the next render; over `WORK_PER_FRAME_BUDGET` and not accepted, the button is orange and opens `##renderGuard`, whose **Render anyway** sets `m_acceptedHeavyRender` and `m_acceptedWork` as the old checkbox did.
- **View menu.** `VulkanEngine::drawViewMenu()`: **Render Scale** (`m_renderScale`, from the background window), **Pause Raster Preview** and **Occlusion Culling** (from Stats).
- **Stats.** `drawStatsWindow()` (`src/stats_window.cpp`): Frame (a line per `FRAME_SERIES` entry over `FRAME_HISTORY_SECONDS`, a 60 Hz guide line, the y axis to 1.25 times the 98th percentile of what is shown, and in place of ImPlot's legend a table of each series' visibility, mean over the last second and peak), Raster preview (state, draws and triangles with sparklines, occlusion), the renderer's sections, and GPU memory (`vmaGetHeapBudgets()` per heap, and horizontal bars of `GpuMemoryUse`, the environment map, the draw images and the rest of VMA's allocations). `FrameHistory` is a ring of 4,096 `FrameSample`s, all doubles so ImPlot reads each field with one stride and the ring's offset; a time a frame did not have is NaN, a gap in the plot.
- **Render stats.** `RaytraceRenderer::drawStats()` (`src/rt_renderer_stats.cpp`): the status line, a table (GPU time, samples a frame, triangles placed, cost per ray, the work a frame as a progress bar of the budget, BVH and shadow ray work, dropped samples), the throughput plot, the per-bounce subplots (one per non-empty `ScheduleCounter`, x limits fixed to its bins, a log y axis set from 0.5 to ten times the largest count, since a log axis fits a bar's base of 0 a thousand decades down) and the BLAS and TLAS table. `recordThroughput()` runs in `update()` while a render runs: a `ThroughputPoint` (seconds since the start, samples, cumulative rays from `TraversalWork`), halved at `MAX_THROUGHPUT_POINTS`; the plot takes rates over `THROUGHPUT_WINDOW_SECONDS`.
- **Raster GPU time.** `FrameData::rasterTimestamps`, two timestamp queries per slot, written around the raster pass when it runs (`rasterTimed`) and read after the slot's fence wait into `EngineStats::rasterGpuTime`, NaN otherwise. `m_timestampPeriodNs` is 0, and the pools null, on a device without `timestampComputeAndGraphics`.
- **Memory.** `allocationBytes()` (`src/vk_types.h`), `WavefrontPool::deviceBytes()`, `GpuRenderer::memoryUse()` and `RaytraceRenderer::memoryUse()`, which adds the display image to the film.
- **ImPlot.** v1.0 at `../CPPLibraries/implot`, the `implot` static target in the root `CMakeLists.txt`, linked by `vulkan_guide`. `initIMGUI()` creates its context and the main deletion queue destroys it. v1.0 styles an item through an `ImPlotSpec` argument; `SetNextLineStyle()` and its kin are gone.

### 8.32 The texture library and the light types (2026-10-31)

Design and as-built records: `docs/plans/completed/pbrt-textures.md` (with the descriptor array spike's results) and `docs/plans/completed/pbrt-light-types.md`.

- **Set 1.** `TextureLibrary` (`src/texture_library.h`, `VulkanEngine::m_textureLibrary`) owns every library texture and descriptor set 1: binding 0 the samplers (64 states, each twice: its filters, then nearest), binding 27 the images (variable count, at most 1024, update-after-bind). `ComputePassBuilder::setSecondSetLayout()` gives a pass the layout; `dispatchComputePass*()` take the set as their last argument. `GpuRenderer::bindingsOf()` adds `CrtBinding::Textures` to every kernel that declares `Materials`, and `TriangleUv1` (binding 64) to every kernel that declares `TriangleAttributes`. `VulkanEngine::run()` calls `syncSceneImages()` (`syncLightImages()` until 2026-11-01, when it took the cameras' aperture images too) and `TextureLibrary::update()` before the tracer's update each frame; a change waits for the device and rewrites set 1.
- **Sources.** A model's image (`TextureSource::modelImage`, an index into `LoadedModel::imagesByIndex`) or a file (8-bit, float, or IES). `VulkanEngine::resolveModelTextures()` gives a model's library entries their references: a glTF material's four textures from `GLTFMaterial`'s `GltfTextureRef`s, a pbrt material's from `TextureBinding::importImage`. `ImportedModel::images` keeps its entries after upload, without their pixels.
- **Records.** `packMaterial()` appends, after the type's record, an emission block (`CrtEmissionBlock`, a type without emission of its own) and the texture table (`CrtTextureEntry`, six vec4s each), and an extension header as the last vec4 (`CrtMaterialExtension`, flag `CRT_MATERIAL_FLAG_EXTENDED`). Under native storage a mesh's materials pack without legacy texture layers (`PackContext::textures` false). Flags `CRT_MATERIAL_FLAG_ONE_SIDED`, `_REVERSED` and `_ALPHA_TEXTURE` are new. `buildSceneAccel()` packs a material per (id, layers, reversed).
- **Shaders.** `crt_texture.glsl` (set 1, `TextureSite`, `sampleTextureEntry()`, `materialTextureColor()`, `materialEmissionTexture()`), included by `crt_material.glsl` and `crt_interaction.glsl` (`surfaceTextureSite()`, `continuedRayCone()`). `resolveMaterial()`, `resolveBxdfMaterial()`, `resolveMixMaterial()` and `resolveComplexMaterial()` take a `TextureSite`; `materialShadingNormal()` reads a library normal map on a mesh or a shape. The traversal's `cutAway()` takes the ray, for pbrt's stochastic alpha. Specialization constants 8 `CRT_TEXTURES` and 9 `CRT_RAY_CONES`; `pipelineFeatures()` bits 5 and 6. `CrtParams::pixelSpread` replaces the last pad; the filter is `CrtParams::flags` bits 7 and 8, and `CRT_FLAG_UV1` is bit 9.
- **Lights.** `CRT_LIGHT_PROJECTION` (8) and `CRT_LIGHT_GONIOMETRIC` (9) are delta lights evaluated by `imagedLightFactor()`. `GpuLight::kind`'s flags (bits 8 to 12) are read with `lightKindOf()`; a textured shape light's instance slot is remapped to the GPU's order at the end of `buildSceneAccel()`. Kernel 04's `emittedRadiance()` reads the emission block for every type but `emissive` and `pbr`, and `emitsTowardsRay()` the one-sided flags.
- **Traps.** `TextureLibrary::MAX_TEXTURES` is what `vkGetDescriptorSetLayoutSupport` allows on this GPU; asking for more aborts at startup. A one-sided light's front is the hit's normal (an object-space normal carried by the normal matrix), not the world winding, which a mirroring transform reverses. The ray cone took `PathState`'s two pads, which the roadmap had kept for participating media and path regularization. A light record has no padding left.
- **Tests.** `bin/light_test` (falloffs, equal-area mapping, IES, projection solid angle), `tools/verify/light_texture_check.py` over the scenes `light_texture_scenes.py` writes, and `pbrt_compare.py` over `assets/models/pbrt_lights/` (`pbrt_light_scenes.py`).

### 8.33 The camera models (2026-11-01)

Design and as-built record: `docs/plans/completed/pbrt-cameras.md`.

- **The model.** `src/camera_model.h` (static lib `rt_camera`, no Vulkan): `CameraKind`, `SphericalMapping`, `ApertureShape`, `LensMapping`, `CAMERA_TRAITS` (which fields a kind uses, and whether light tracing can connect to it), `RTCameraSnapshot` (moved here from `rt_scene_types.h`), `CameraFrame` (what the shaders read) and `generateCameraRay()`, `sampleAperture()`, `apertureDensity()`. `shaders/rt/include/crt_camera.glsl` mirrors it; `bin/camera_test` and the **Camera parity** self-test check both.
- **Push constants.** `CrtParams` is 320 bytes (352 since §8.34). `packCameraParams()` (`rt_gpu_types.h`) writes the camera's fields from a `CameraFrame`, for `GpuRenderer::baseParams()` and the self-test. The RTIOW fields (`origin`, `lowerLeft`, `horizontal`, `vertical`, `lensU` with the lens radius in w, `lensV`) mean what they always did, the film on the untilted plane of focus; the new ones follow `pixelSpread`.
- **Specialization constant 7, `CRT_CAMERA_MODEL`.** Off, `generateCameraRay()` is the old camera's code. `KernelVariant::readsCamera` marks a kernel built with it, `pipelineFeatures()` bit 7 says whether `CameraFrame::needsModel()`. A camera ray never touches the path's throughput: its weight is 1.
- **Binding 65, `ImageDistributions`.** `packPiecewiseConstant2D()` tables (`crt_image_table.glsl`). `GpuRenderer::uploadImageDistributions()` writes the aperture image's at the start of a render, a stub without one; `RaytraceRenderer::apertureTable()` builds it from the texture library entry's luminance copy and puts its pointer in the `RenderKey`, so a render started before the image loaded restarts once it has.
- **Focus on.** `VulkanEngine::beginFocusPick()`, `updateFocusPick()` (the click, before the camera overlays), `drawFocusPlane()`; `RaytraceRenderer::pickDistance()` traces on the CPU through `RaytraceSceneAccel::sceneInstances` (the `SceneInstance` list the TLAS was built over, kept for this); `RaytraceSceneEditor::setCameraFocusDistance()`. Navigation ignores the viewport's mouse while a pick waits.
- **Scene file** version 26, `readCamera()`: a camera without `"lensMapping"` is polar. **Importer**: `camera()` in `src/pbrt_import.cpp` maps orthographic, spherical and `screenwindow`.
- **Traps.** Moving the camera into a function with branches moved a lens ray's arithmetic by an ulp under Metal's fast math; the specialization constant is what keeps old renders bit-identical, so keep the `!CRT_CAMERA_MODEL` branch literal. pbrt-v4's orthographic lens is a disk at the camera's origin; the importer reports it rather than copy it.
- **Tests.** `bin/camera_test`, `tools/verify/camera_check.py` over the scenes `camera_scenes.py` writes, and `pbrt_compare.py` over `assets/models/pbrt_cameras/` (`pbrt_camera_scenes.py`).

### 8.34 The bidirectional integrators (2026-11-01)

Design and as-built record: `docs/plans/completed/pbrt-bidirectional-path-tracing.md`.

- **Registry.** `IntegratorId::Light`, `Lvc` and `Bdpt`, each with `IntegratorInfo::bidirectional`, and their schedules `LightSchedule`, `LvcSchedule` and `BdptSchedule` (`src/rt_light_schedule.cpp`, `rt_lvc_schedule.cpp`, `rt_bdpt_schedule.cpp`). Stages 11 to 18 are `KernelSlot::EmitLightRays` to `ConnectSubpaths`; 10 stays reserved. `CrtBinding::SplatAccumulation` (42), `LightVertexCache` (52) and `BdptVertices` (53); `GpuRenderer::publishBidirectionalStubs()` binds a stub to 52 and 53 while their schedule is not running.
- **Specialization constants.** 10 `CRT_SPLATS` (kernel 09, `KernelVariant::foldsSplats`), 11 `CRT_LIGHT_VERTEX_CACHE` (kernels 11 to 13 under `lvc`, `servesLightVertexCache`), 12 `CRT_BDPT` (kernel 11 under `bdpt`, `storesBdptVertices`); `pipelineFeatures()` bits 24 to 26. The bidirectional stages that call `crt_camera.glsl` set `readsCamera`.
- **Push constants.** `CrtParams` is 352 bytes: `sampleDimensionBase`, `lightPathScale`, `splatNormalization`, `sceneSphere`, `lightPathCount` (under `bdpt`, the tile's pixel samples), `cacheConnections`, `frameLambdaU` and `bidirFlags` (`CRT_BIDIR_FLAG_*`: splats, frame wavelengths, filtered splats, the emissive, light and escaped passes, the strategy images and their weighting).
- **Transport.** `crt_transport.glsl` holds `g_transportMode`, `transportEval()` (the adjoint by swapped directions for the six original materials), `transportPdf()`, `transportSample()` and `shadingNormalCorrection()`. It is included twice in a kernel that evaluates both classes, the second time after `#define SurfaceMaterial ComplexMaterial`. `crt_scatter.glsl` and `crt_optic_material.glsl` read the mode only through `CRT_SURFACE_TRANSPORT` and `CRT_OPTIC_RADIANCE_SCALE`, which default to radiance, so the path tracer's kernels compile as before.
- **Shared code.** `crt_spawn.glsl` (`spawnShadowRay()`), `crt_surface_views.glsl` (`aovCapture()`, `ambientOcclusionRay()`) and `crt_emitter.glsl` (`emittedRadiance()`, `emittedSpectrum()`, `emitsTowardsRay()`) moved out of `crt_surface.glsl` and kernel 04 unchanged. `crt_light_path.glsl` rebuilds a light path's surface (`lightVertexAt()`), which stages 12, 13, 16 and 17 share.
- **`lvc`.** `crt_light_vertex_cache.glsl`: binding 52's header (count, capacity, stride, where the vertices start), a state per pool slot, then `CachedVertex` records; `lvcUpdateAtHit()`, `lvcLightPartials()`, `lvcCameraPartials()` and `lvcScatter()` keep SmallVCM's `dVCM` and `dVC`.
- **`bdpt`.** `crt_bdpt.glsl`: binding 53's header (the tile's first pixel sample, vertices a slot, record size, where the records start), a count pair per slot, then `BdptVertex` records of `5 + spectrum words` `uvec4`s (`bdptVertexWords()` in `rt_bdpt_schedule.h` mirrors it). Stage 18's `prepareMisSums()` and `misWeight()` keep pbrt's weights as running sums; `strategyImageAdd()` writes the strategy images into binding 45 at `bdptStrategyIndex()` (`src/aov.h`), and `GpuRenderer::m_strategyDepths` says how many depths those slots hold, while `lightGroupCount()` reports none.
- **Progress.** `IntegratorSchedule::samplesPerFrame()` is read after each frame is recorded, so `BdptSchedule` reports 1 on the frame that finishes a sweep of the tiles and 0 on the others.
- **Traps.** A kernel that sets a vertex's density by area must set its `CRT_BDPT_ON_SURFACE` flag first, or `bdptConvertDensity()` leaves out the cosine and the MIS weights stop summing to 1. In stage 18 a reverse density's direction points towards the vertex it came from: t = 1 passes `cs.wi`, towards the camera. Stage 18's spectral build was one storage buffer over this GPU's 31 a stage with the scatter kernels' bindings, so its list names only what it reads. Including `crt_material_class.glsl` makes CMake build a kernel once per class; stage 18 defines `CRT_MATERIAL_CLASS` itself instead.
- **Tests.** `bin/camera_test` and `bin/light_test` (importance and emitted rays), `tools/verify/bidirectional_check.py` over the scenes `bidirectional_scenes.py` writes, and `pbrt_compare.py --cornell --integrator bdpt [--visualize-strategies]`.

### 8.35 Laser sources (2026-11-02)

Design and as-built record: `docs/plans/completed/physoptics-laser-sources.md`; the readable account is `docs/physical-optics-overview.md` §8.

- **The element.** `OpticKind::Laser`, the last row of `OPTIC_TRAITS` (`src/optic_catalog.cpp`): parameters by name in SI units (`OpticParamKind::Wavelength` is meters shown in nanometers), the HNL050LB preset as its one size (`OpticTraits::sizeLabel` "preset"), the slot `aperture` (`SlotDefault::LaserAperture`), the port `Output`, no mounts (`OpticTraits::snapsFree`). `buildLaser()` makes one disk square to the beam. `src/optic_laser.h`: `LaserBeam` and `LaserAxisBeam` from `laserBeam()`, `fitLaserAxis()`, `laserHousing()`, `singleLaser()` and `designWavelength()`.
- **The editor.** `rt_scene_editor_optics.cpp`: `drawLaserDataSheet()` (diameter and divergence fields), the laser's own gizmo in `beginOpticGizmo()` (slide, raise, turn about the vertical; `m_laserDragYaw` steps the turn in quarter turns while snapping), and `standOnBench()` putting a new laser at the beam height across the view. `snapFreeToBench()` (`src/optic_bench.h`) is the snap. `opticalBenchOverlays()` (`src/scene_capabilities.cpp`) draws the housings and beam arrows through the capability's `viewportOverlays` hook.
- **The light.** `LightListBuilder::addLaser()` keeps lasers apart and `finish()` appends them, two records each (`GpuLightKind::Laser` 10 and `LaserBeam` 11), after every light, with their alias table in their first records; `GpuLightHeader` holds `laserBase`, `laserCount` and `invEmitPower`. `buildSceneAccel()` builds each laser's `LaserLight` from its optic (the plane 10 µm in front of the aperture), then sets its instance's `lightBase` to the laser's first record.
- **Sampling.** `LaserEmitter`, `sampleLaserLe()`, `laserPdfLe()`, `laserRadiance()`, `sampleStandardNormal2()` and `gaussian2()` in `src/light_sampling.cpp`, mirrored by `shaders/rt/include/crt_laser.glsl`, which adds `laserEmitter()`, `laserRadiance()` by record, `sampleEmitterIndex()` and `emitterPmf()`. `crt_light_emission.glsl` includes it and samples a laser ahead of its switch.
- **Specialization.** 13 `CRT_LASERS`, `CRT_FEATURE_LASERS` (bit 27 of `pipelineFeatures()`), set with `CRT_OPTICS` on every kernel that reads the instances. Kernel 11 starts light paths at lasers (not under `lvc` or `bdpt`) and evaluates the beam toward the camera; kernel 04's `surfaceEmission()` lights a `laser_aperture` hit.
- **Materials.** `MaterialType::LaserAperture` (`laser_aperture`, 13): `packLaserAperture()` writes a classic record with the emits flag and no emission block.
- **Glass indices.** `OpticGlass::relativeToAir`: `glassIndex()` is absolute (a catalog's formula times Ciddor's air), `indexRelativeToAir()` is a catalog's (nd, the Abbe number, the panel), and `absoluteIndexRef()` tabulates the absolute index for the GPU's media and coating layers. A user glass saves `"absolute"` when it is not relative to air.
- **Checks.** `bin/light_test` and `bin/optics_test`; `tools/verify/beam_probe.py` (the screen and camera, ABCD, the waist fit), `wave_optics.py` (the wave reference and its self-test), `laser_scenes.py` and `laser_check.py`, and `optics_verification.py` (gaussian_train, the telescopes, oap_focus, tilted_spherical_mirror), all in `run_all.py`.
- **Traps.** A light path that starts on its emitter's own absorbing surface is absorbed at t = 0, whatever the ray offset: the emission plane leads the aperture. The scene file clamps a camera to at least 1 degree, so narrow checks move the camera in rather than narrowing it. A check camera must stand between the last element and the screen, or what it looks through hides the beam. Uncoated lenses' ghosts widen a measured second moment by about 1 %.


### 8.36 Detectors (2026-11-04)

Design and as-built record: `docs/plans/completed/physoptics-detectors.md`; the readable account is `docs/physical-optics-overview.md` §9.

- **The kinds.** `OpticKind::PowerMeter`, `CameraSensor`, `BeamProfiler`, `AlignmentCard`, `Screen`, `Spectrometer`, `BeamDump` and `ProbePlane`, after the laser in `OPTIC_TRAITS`, all `snapsFree`, no presets. `isDetectorKind()`, `detectorGeometry()` (`DetectorGeometry`: `SensorShape`, `DetectorAction` Absorb, Scatter or Transparent, half sizes, the film's pixels (one for a power meter, a spectrometer and a beam dump), a physical sensor's pixels, a spectrometer's range and bins, the housing's placeholder id) and `buildDetector()` are in `src/optic_detectors.h`, part of `rt_optics`. Slots: `sensor` (`SlotDefault::Sensor`, `MaterialType::Detector`) or `face` (`SlotDefault::WhiteDiffuse`, pbrt's diffuse); a probe plane has none.
- **The editor.** Detectors are sensors: `drawToolbar()`'s **Sensor** menu lists the camera projections (`createCamera(engine, kind)`) and walks the new `CapabilityInfo::sensorMenu` hook (`drawDetectorAddItems()`), and `drawSensorRows()` (once `drawCameraRows()`) the cameras by projection and `sensorFolders` (`drawDetectorRows()`). `drawSelectedOptic()` hides the size combo of a kind without presets, fills a physical sensor's film pixels when it turns on, and calls `drawDetectorTargetButton()`. `detectorOverlay()` in `src/scene_capabilities.cpp` outlines a housing or dashes a probe plane through Optical bench's `viewportOverlays`. `drawOpticPreviews()` skips a probe plane. `populateOpticalBench()` adds the HNL050LB and a power meter and sets light tracing (`RaytraceRenderer::setIntegrator()`).
- **The records.** `buildSceneAccel()` makes one `GpuDetector` (96 bytes, `src/rt_accel.h`) per detector in the order of the optics, at most `MAX_DETECTORS` (32): the world-space frame (`origin` with the half width, `uAxis` with the half height, `vAxis` and `normal`, the front's, with a spectrometer's range in their w), the sensor's GPU instance slot (`GPU_DETECTOR_NO_INSTANCE` for a probe plane, which gets no instance), `GPU_DETECTOR_*` flags (disk, probe, spectrometer, log2 of the frame copies at bits 8 to 11), the film's pixels and channels (3 in exact RGB, 1 in a spectral mode) and the bins. `RaytraceSceneAccel::detectors`, `detectorOptics`, `detectorsLeftOut`.
- **Stage 26.** `KernelSlot::DepositOnDetectors` (slots 19 to 25 are named as the roadmap reserves them), variant `deposit`, `KernelDomain::CurrentRayQueue`, bindings core + `Hits`, `Detectors`, `DetectorFilm`, lane dependent. `LightSchedule::record()` launches it after kernel 02, beside kernel 12 and before the barrier ahead of kernel 13, only while `GpuRenderer::detectors().deposits()`. The shader tests each record: a probe plane analytically along the segment, a sensor by the hit's instance; the front only; `crt_detector.glsl` `detectorDeposit()` adds the throughput (times a point light's range window on the first segment) into the copy `pathIndex` picks, and a spectral build's lanes converted by `detectorLaneWatts()`. Shared counters per workgroup add the paths that reached each detector.
- **The render target.** `RaytraceRenderer::m_renderDetectorId` beside `m_renderCameraId`; `resolveTarget()` returns a `RenderTarget` (the camera or the detector, its `RTCameraSnapshot`, the film's size): a camera's from the render settings, a light-traced detector's a square of `RenderSettings::lightPathsPerSample` (`lightPathSide()`), a camera-path detector's its pixels with `sensorSnapshot()`. `RenderKey::detectorTarget`; `GpuRenderSnapshot::detectorTarget` and `cameraConnections`, which `LightSchedule::record()` reads to skip stages 12 and 14. `drawCameraCombo()` is the **Render target** combo; `drawSettings()` shows a detector's film and **Light paths per sample** in place of `drawResolution()`. The scene file's `renderDetector`.
- **The sensor camera.** `CameraKind::Sensor`: `buildCameraFrame()` makes the film the sensor (`RTCameraSnapshot::sensorSize`, lookFrom its center, lookAt in front, vUp its +y), and `generateCameraRay()` starts at the film point 10 µm in front and draws the direction cosine-weighted from the lens sample (`crt_camera.glsl` mirrors it; kernel 00 draws the lens dimensions for it under `CRT_CAMERA_MODEL`). `bin/camera_test` `testSensor()`, the camera parity self-test's "sensor" case.
- **`DetectorFilms`** (`src/rt_detectors.h`): `start()` with each render (the same scene and spectral settings keep the buffers), `recordClear()` on the first frame, `recordFrame()` after the schedule (one `detector_fold.comp` dispatch per detector, then the totals' copy, the running sums' copy twice a second and when the render ends, the totals cleared), `collect()` from `GpuRenderer::collectReadbacks()`, `readNow()` for the batch mode, `recordViews()` (`detector_view.comp` into the picture of the detector the output window shows, registered with `DisplayRegistry::registerTexture()`: a first pass finds the film's peak on the GPU, unless `DetectorViewSettings::autoNormalize` is off, and a second draws by it, so the picture never waits on a readback), and for a camera-path target `recordSensorCopy()` and `readSensorFilm()`, which read the render's accumulation back as the target's film (π × the mean × the pixel's area) and `readSensorNow()` for the batch mode. A restart keeps the readings: `collect()` reads a frame of any render no older than the reading's (`Entry::readingSerial`, `adopt()`) recorded in the current layout (`m_layout`), so a render an edit restarts every frame still has readings, a frame or two behind. Binding 59 is a `DetectorHeader` (count, frame floats, totals offset, counts offset; eight vec4 of band widths) and the records. Binding 60 is the frame sums (an image in `frameCopies()` copies, up to 4096, a spectrum in 8), the running sums and their compensations, then 4 totals and 1 count per detector. The paths a frame emitted are its pixel samples, `samplesPerFrame() x width x height`.
- **The UI.** `RaytraceRenderer::drawOutputWindow()` draws the Raytraced Output window (now an `AppWindow`, `outputVisibilityFlag()`; the display image a registry texture): the **Showing** combo, the picture, and while a detector shows, `drawDetectorPictureSettings()` and `drawDetectorReadout()` (`src/detectors_ui.h`). There is no detector window of its own.
- **Batch mode.** `CommandLine::detectors`; `stepBatch()` calls `writeDetectorReadings()` (`src/rt_detectors.h`) after `exportRender()`, which writes a light-traced detector target's film.
- **Sensor stacks (2026-11-05).** `SensorStack` (`src/optic_detectors.h`): a template detector (`sensor`, which owns the slot materials), `start`, `end`, `count` and an `orientation` that, with `alignToAxis`, keeps only its turn about the axis (`sensorStackOrientation()`); `sensorStackPosition()` spaces the members evenly, `sensorStackMemberName()` names them `<stack>_<i>`. `RaytraceSceneEditor::m_sensorStacks`; `resolveSensorStacks()`, first in `resolveBench()`, rebuilds the members as ordinary detector optics (`SceneOptic::stack` the stack's id, the stack's material ids shared, each member's id kept by its place) after the other optics, so the accel, the films, the readings and the render target see detectors and nothing else knows of stacks. A member is not deleted, duplicated or edited alone: `drawSelectedStackMember()` shows it with its target button and a way back to the stack. `SelectionKind::SensorStack`, `drawSelectedSensorStack()` (the sensor's parameters through `drawOpticParams()`, shared with `drawSelectedOptic()`), `drawSensorStackRows()` under the Sensors folder, **Add** → **Sensor** → **Sensor stack**, and three gizmos (`beginSensorStackGizmo()`: start and end translate in world axes, the rotation turns about the axis alone while aligned). The overlay draws the stack's axis. What a render records: `RenderSettings::renderAllSensors` (off: the target, or every member of its stack; a camera target, none), which `ensureSceneAccel()` turns into `buildSceneAccel()`'s `recordedDetectors` (`m_sceneAccelRecorded` keys the rebuild); the batch mode's `--detectors` turns it on. `drawCameraCombo()` lists a stack once and draws the checkbox. `RaytraceRenderer::drawOutputWindow()` lists a stack once in **Showing** and picks the member with a slider (`m_shownStackMember`, the member each stack showed last); `drawStackPlot()` (`src/detectors_ui.cpp`) plots a `StackQuantity` of every recorded member along the axis, with `fitCaustic()`'s fit of the radii. `bin/optics_test` `testSensorStacks()`.
- **Scene file.** Payload version 29; the detector types are ordinary optic records. Version 30 adds `sensorStacks` (`SceneSensorStackRecord`), whose members are not saved; `renderDetector` counts the stacks' members after the optics. It also lets a mounted optic's record give its body's `position` and `orientation` with no `origin` (`SceneOpticRecord::standOnBench`): the load stands it in its mount there through `RaytraceSceneEditor::standOpticOnBench()` (`dropOntoBench()`) and reports a mount that turns the body. `tools/verify/beam_probe.py` writes every mountable optic so (`MOUNTS`), with a breadboard under the scene (`breadboard()`) and stacks (`detector_stack()`).
- **Checks.** `bin/optics_test` (`testDetectors()`), `tools/verify/detector_scenes.py` and `detector_check.py`, and the laser and geometric checks, which measure on detectors (`beam_probe.detector()`, `measure_detector()`, `vkscene.run_detectors()`).
- **Traps.** A sensor's camera rays must start in front of its own surface, as a laser's emission plane leads its aperture. A float summing a frame's million equal terms on one pixel loses 1e-3 of them, which the copies fix. Pixel-center moments widen a beam by pitch^2 / 12 (Sheppard). A profiler kind sharing the camera's parameter table took the camera's defaults whenever a file named no preset: each kind has its own defaults now. A detector records light travelling along its body's +z: a check that points one at its source reads nothing.
