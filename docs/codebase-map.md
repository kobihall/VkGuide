# VkGuide + RayTracingInAWeekend — Codebase Map

Reference document capturing the current state of the code as of 2026-08-22. Facts only, no recommendations. Sources: full reads of `src/`, `shaders/`, and the sibling project `/Users/kobihall/Documents/Code/RayTracingInAWeekend`.

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

### `drawGeometry()` — `vk_engine.cpp:230-381`
1. Frustum-culls `m_mainDrawContext.opaqueSurfaces` via `is_visible()` into an `opaque_draws` index list (232-239).
2. Sorts `opaque_draws` by `(material pointer, then indexBuffer)` (242-251).
3. Begins dynamic rendering: color attachment = `m_drawImage.imageView` (no clear), depth attachment = `m_depthImage.imageView` (260-264) via `vkCmdBeginRenderingKHR`.
4. Sets viewport/scissor to `m_drawExtent` (267-283).
5. Allocates a per-draw-call `GPUSceneData` uniform buffer (`createBuffer`, `CPU_TO_GPU`), pushed to `getCurrentFrame().deletionQueue` (287-292); written via mapped pointer + explicit `vmaFlushAllocation` (295-297).
6. Allocates `globalDescriptor` (set 0) from `getCurrentFrame().frameDescriptors` against `m_gpuSceneDataDescriptorLayout`, writes buffer via `DescriptorWriter` (300-305).
7. Local `draw` lambda (312-361): rebinds pipeline + set 0 only when `material->pipeline` changes; always rebinds set 1 (material set) when material changes; rebinds index buffer only if changed; pushes `GPUDrawPushConstants{worldMatrix, vertexBufferAddress}` (vertex stage); `vkCmdDrawIndexed`; updates `m_stats`.
   - Note: inside the material-change branch, viewport/scissor are re-set to `m_windowExtent` (323-339) — inconsistent with the `m_drawExtent`-based viewport set at the top of the function (pre-existing, not touched by this document).
8. Draws sorted opaques, then unsorted `transparentSurfaces` (363-368).
9. `vkCmdEndRenderingKHR`; clears both surface lists for next frame (370-374).

### Draw image / depth image lifecycle
- Created once in `initSwapchain()` (`vk_engine.cpp:807-867`): `m_drawImage` format `VK_FORMAT_R16G16B16A16_SFLOAT`, usage `TRANSFER_SRC | TRANSFER_DST | STORAGE | COLOR_ATTACHMENT`; `m_depthImage` format `VK_FORMAT_D32_SFLOAT`, usage `DEPTH_STENCIL_ATTACHMENT`. Both sized to `m_windowExtent` **at init time**, GPU-only VMA memory, destructors pushed to `m_mainDeletionQueue`.
- `resizeSwapchain()` (`vk_engine.cpp:638-652`) only calls `destroySwapchain()`/`createSwapchain()` — it does **not** recreate `m_drawImage`/`m_depthImage`. Effective render resolution is clamped every frame via `m_drawExtent = min(drawImage, swapchain) * m_renderScale`. Growing the window past its initial size does not increase draw-image resolution.

### `drawBackground()` — `vk_engine.cpp:214-228`
Picks `m_backgroundEffects[m_currentBackgroundEffect]`, binds its compute pipeline, binds `m_drawImageDescriptors` (set 0), pushes `ComputePushConstants` (the effect's stored data), dispatches `ceil(width/16.0), ceil(height/16.0), 1`.

---

## 2. How compute shaders are currently dispatched

There is exactly one compute pattern in the codebase: the "background effects" system. No other compute usage exists.

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
All three declare `layout (local_size_x = 16, local_size_y = 16) in;` and bind a single storage image at set 0, binding 0.
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

The data-upload `createImage(void* data, ...)` overload **hardcodes its staging-buffer size as `width * height * depth * 4`** (`vk_engine.cpp:717`) — correct only for its one current caller (`load_image()`, always `VK_FORMAT_R8G8B8A8_UNORM`, 4 bytes/pixel). Passing any other format's data through this overload as-is (e.g. an HDR float format at 16 bytes/pixel) would under-size the staging buffer and corrupt/truncate the upload. Found during planning; fixed as part of `docs/plans/scene-and-asset-management.md` §2.4.

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
- No `ConfigFlags` are set on `ImGui::GetIO()` anywhere in `vk_engine.cpp` (confirmed via grep) — docking/viewports flags are not currently enabled either way. **Update**: subsequently confirmed during `docs/plans/imgui-display.md`'s planning that the vendored ImGui at `../CPPLibraries/imgui` (`1.90.9 WIP`) is from upstream **master**, not the **docking** branch — grepping for `Docking`/`DockSpace`/`DockNode` in `imgui.h` finds nothing but incidental comments. `docs/plans/imgui-display.md` §2.1 covers what vendoring a docking-branch build requires.

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
| `AllocatedImage`, `AllocatedBuffer`, `GPUMeshBuffers`, `GPUDrawPushConstants` | `src/vk_types.h:36-59` | Shared GPU resource structs |
| `MaterialPass`, `MaterialPipeline`, `MaterialInstance` | `vk_types.h:61-76` | `Other` pass value unrouted |
| `IRenderable`, `Node` | `vk_types.h:78-113` | `Node::Draw()` recurses into children by default |
| `GLTFMetallic_Roughness` | `vk_engine.h:87-115`, impl `vk_engine.cpp:1363-1466` | Owns opaque/transparent pipelines, `writeMaterial()` |
| `RenderObject` | `vk_engine.h:66-75` | Sort key: `(material ptr, indexBuffer)` |
| `DescriptorLayoutBuilder`, `DescriptorAllocatorGrowable`, `DescriptorWriter` | `src/vk_descriptors.h/.cpp` | Only descriptor tooling in the codebase |
| `PipelineBuilder` | `src/vk_pipelines.h/.cpp` | Graphics-only fluent builder; no compute equivalent |
| `vkutil::load_shader_module` | `vk_pipelines.h:6`, `.cpp:6-50` | Generic `.spv` file → `VkShaderModule` |
| `vkutil::transition_image`, `copy_image_to_image` | `src/vk_images.h/.cpp` | See section 3 |
| `vkinit::VkFunctionLoader` | `src/vk_initializers.h:30-46` | Singleton manually loading KHR extension function pointers (`vkCmdPipelineBarrier2KHR`, `vkQueueSubmit2KHR`, `vkCmdBlitImage2KHR`, `vkCmdBeginRenderingKHR`, `vkCmdEndRenderingKHR`) since the project targets Vulkan 1.2 core + KHR rather than core 1.3 |
| `Camera` | `src/camera.h/.cpp` | Single instance `m_mainCamera` (`vk_engine.h:185`); WASD + mouse-look; no zoom/FOV control, no pitch clamp, no delta-time scaling |
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
- No `VK_KHR_ray_tracing_pipeline` / `VK_KHR_acceleration_structure` usage anywhere in VkGuide; project targets Vulkan 1.2 core + KHR extension forms loaded through `vkinit::VkFunctionLoader` (confirmed no core-1.3 unsuffixed entry points are called for dynamic rendering/sync2/blit).
- VkGuide has a single GLFW window and a single Vulkan surface/swapchain; no multi-window code exists.
- VkGuide's ImGui init pins `PipelineRenderingCreateInfo.pColorAttachmentFormats` to the swapchain format (section 4); the `imguiPool` descriptor pool is a local variable in `initIMGUI()`, not currently exposed as an engine member.
- The vendored ImGui at `../CPPLibraries/imgui` is confirmed **not** the docking branch (section 4) — any feature wanting real docking/tabbing needs to vendor a docking-branch build first (`docs/plans/imgui-display.md` §2.1 covers this).
- `createImage(void* data, ...)`'s staging-buffer size is hardcoded to 4 bytes/pixel (section 3) — any future caller uploading a non-RGBA8 format needs this fixed first; `docs/plans/scene-and-asset-management.md` §2.4 does this.
- `GPUSceneData`/`m_mainCamera` in VkGuide are singular — there is exactly one camera and one scene-data uniform wired into `drawGeometry()`; nothing else reads or writes a second view.
- The RTIAW project's `hittable`/`material` virtual-dispatch model, `hittable_list`'s O(n) linear scan (no BVH), the global mutable `std::mt19937`, `double`-precision math, and full-image-recompute-per-trigger render loop are all CPU-only constructs with no direct GPU-compute equivalent present anywhere in either codebase — none of this is portable to a compute shader as-is.
- VkGuide's `checkVkResult`/`vkbErr` error macros (`vk_types.h:115-129`) hard-abort on any failure; there is no exception-based or recoverable error path in the codebase.
