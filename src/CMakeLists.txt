# The BVH builders, layouts and two-level scene: plain C++ with no Vulkan, shared by the app and
# the CPU verification tool (tests/bvh_bench.cpp)
add_library(rt_bvh STATIC
    bvh.h
    bvh_build.cpp
    bvh_split.h
    bvh_split_object.cpp
    bvh_split_spatial.cpp
    bvh_layout.cpp
    bvh_layout.h
    bvh_scene.cpp
    bvh_scene.h
    shape.cpp
    shape.h)
target_include_directories(rt_bvh PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rt_bvh PUBLIC glm fmt::fmt)
# optimised even in a Debug build: BLAS construction runs at model import, and unoptimised glm is
# one to two orders of magnitude slower - minutes instead of seconds on a large model
option(RT_BVH_ALWAYS_OPTIMIZE "Compile the BVH library with -O2 in every configuration" ON)
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    target_compile_options(rt_bvh PRIVATE -O2)
endif()

# Add source to this project's executable.
add_executable(vulkan_guide
    main.cpp
    vk_engine.cpp
    vk_engine.h
    vk_images.cpp
    vk_images.h
    vk_types.h
    vk_initializers.cpp
    vk_initializers.h
    vk_descriptors.cpp
    vk_descriptors.h
    vk_pipelines.cpp
    vk_pipelines.h
    vk_compute.cpp
    vk_compute.h
    vk_display.cpp
    vk_display.h
    vk_gizmo.cpp
    vk_gizmo.h
    vk_ui.h
    vk_loader.cpp
    vk_loader.h
    vk_tonemap.cpp
    vk_tonemap.h
    rt_scene_types.h
    shape_mesh.cpp
    shape_mesh.h
    rt_scene.cpp
    rt_scene.h
    rt_scene_editor.cpp
    rt_scene_editor.h
    rt_textures.cpp
    rt_textures.h
    scene_io.cpp
    scene_io.h
    file_dialog.h
    rt_renderer.cpp
    rt_renderer.h
    rt_gpu.cpp
    rt_gpu.h
    rt_accel.cpp
    rt_accel.h
    camera.cpp
    camera.h)


set_property(TARGET vulkan_guide PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:vulkan_guide>")

target_include_directories(vulkan_guide PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# target_link_libraries(vulkan_guide vkbootstrap vma glm tinyobjloader imgui stb_image)
target_link_libraries(vulkan_guide PUBLIC rt_bvh vk-bootstrap glm imgui imguizmo stb_image glfw fmt::fmt fastgltf::fastgltf)

target_link_libraries(vulkan_guide PRIVATE GPUOpen::VulkanMemoryAllocator)

if (APPLE)
    target_compile_options(vulkan_guide PRIVATE -Wno-nullability-completeness)
    # native NSOpenPanel/NSSavePanel file dialogs, Objective-C++
    enable_language(OBJCXX)
    target_sources(vulkan_guide PRIVATE file_dialog.mm)
    set_source_files_properties(file_dialog.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(vulkan_guide PRIVATE "-framework Cocoa" "-framework UniformTypeIdentifiers")
else()
    target_sources(vulkan_guide PRIVATE file_dialog_stub.cpp)
endif()

target_link_libraries(vulkan_guide PUBLIC Vulkan::Vulkan)

target_precompile_headers(vulkan_guide PUBLIC <optional> <vector> <memory> <string> <vector> <unordered_map> <glm/mat4x4.hpp>  <glm/vec4.hpp> <vulkan/vulkan.h>)

add_dependencies(vulkan_guide Shaders)