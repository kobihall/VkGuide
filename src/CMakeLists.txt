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
    robust.h
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

# The light sampling next-event estimation uses, as plain C++: the line-for-line mirror of
# shaders/rt/include/crt_light.glsl, built into the app (the environment map's tables, the light
# list's geometry) and checked on its own by tests/light_test.cpp
add_library(rt_light STATIC
    light_sampling.cpp
    light_sampling.h)
target_include_directories(rt_light PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rt_light PUBLIC rt_bvh glm)
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    target_compile_options(rt_light PRIVATE -O2)
endif()

# The sample generators kernel 01 runs, as plain C++: the line-for-line mirror of
# shaders/rt/include/crt_sampler.glsl and the loader of pbrt's Sobol, PMJ02BN and blue-noise tables
# (assets/sampler/). Built into the app and checked on its own by tests/sampler_test.cpp
add_library(rt_sampler STATIC
    sampler.cpp
    sampler.h
    sampler_halton_tables.h)
target_include_directories(rt_sampler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rt_sampler PUBLIC glm fmt::fmt)
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    target_compile_options(rt_sampler PRIVATE -O2)
endif()

# The film, as plain C++: pixel filters and their sampling table, the compensated sums, the variance and the
# clamp kernels 00 and 09 run - the line-for-line mirror of shaders/rt/include/crt_filter.glsl and
# crt_film.glsl. Built into the app and checked on its own by tests/film_test.cpp
add_library(rt_film STATIC
    film.cpp
    film.h)
target_include_directories(rt_film PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rt_film PUBLIC rt_light glm)
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    target_compile_options(rt_film PRIVATE -O2)
endif()

# The surface scattering the path tracer runs in kernel 06, as plain C++: the line-for-line mirror of
# shaders/rt/include/crt_scatter.glsl. Checked on its own by tests/bsdf_test.cpp and against the GPU
# by the self-test in the Windows menu
add_library(rt_bsdf STATIC
    bsdf.cpp
    bsdf.h
    bxdf.cpp
    bxdf.h)
target_include_directories(rt_bsdf PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rt_bsdf PUBLIC rt_sampler glm)
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    target_compile_options(rt_bsdf PRIVATE -O2)
endif()

# The auxiliary outputs as plain C++: the layers of AovImages, one pixel's accumulation and its resolve, the
# line-for-line mirror of shaders/rt/include/crt_aov.glsl. Checked on its own by tests/aov_test.cpp and against the
# GPU by the self-test in the Windows menu
add_library(rt_aov STATIC
    aov.cpp
    aov.h
    render_export.cpp
    render_export.h)
target_include_directories(rt_aov PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rt_aov PUBLIC glm rt_image fmt::fmt)
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    target_compile_options(rt_aov PRIVATE -O2)
endif()

# Image files as plain C++: OpenEXR through tinyexr (v1.0.13, ../CPPLibraries/tinyexr/, with the system zlib),
# PFM, Radiance HDR and PNG (../CPPLibraries/stb_image_write.h), plus the image error measures. Used by the
# app's reference images and render export, and by tests/image_diff.cpp
find_package(ZLIB REQUIRED)
add_library(rt_image STATIC
    image_io.cpp
    image_io.h)
target_include_directories(rt_image PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(rt_image PRIVATE "${CMAKE_SOURCE_DIR}/../CPPLibraries/tinyexr" "${CMAKE_SOURCE_DIR}/../CPPLibraries")
target_link_libraries(rt_image PUBLIC ZLIB::ZLIB fmt::fmt)
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    target_compile_options(rt_image PRIVATE -O2)
endif()

# The optomechanical model library as plain C++: the table of Thorlabs mounts, holders, irises and blanks,
# the reader of the converter's .glb and .mates.json files (assets/optical_models/meshes/), and the procedural
# placeholders. Uploaded by the app's MountLibrary and checked on its own by tests/optic_models_test.cpp. It
# decodes the models' decal textures with its own static copy of stb_image, so that a Debug build decodes them
# at -O2 too (unoptimized, the 19 textures took 2.5 s at startup)
add_library(rt_optic_models STATIC
    optic_models.cpp
    optic_models.h)
target_include_directories(rt_optic_models PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(rt_optic_models PRIVATE "${CMAKE_SOURCE_DIR}/../CPPLibraries")
target_link_libraries(rt_optic_models PUBLIC glm fmt::fmt fastgltf::fastgltf)
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    target_compile_options(rt_optic_models PRIVATE -O2)
endif()

# The material model as plain C++: MaterialDef, the traits table and the packing into the GPU's record
# (rt_material_record.h). Shared by the app and the pbrt importer, which builds materials without Vulkan
add_library(rt_material STATIC
    rt_material.cpp
    rt_material.h
    rt_material_record.h)
target_include_directories(rt_material PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rt_material PUBLIC rt_spectrum rt_bsdf glm)

# Spectra on the CPU: pbrt-v4's CIE tables and named spectra (spectrum_tables.cpp, generated by
# tools/spectrum_tables/extract_pbrt_spectra.py), blackbodies and the projection to linear sRGB; the RGB color spaces
# and their uplift tables (color_space.h); SpectrumRef (spectrum_ref.h); and the spectral representations, the
# line-for-line mirror of shaders/rt/include/crt_spectrum.glsl, with the packing of SpectralData (spectral.h). Checked
# by tests/spectrum_test.cpp
add_library(rt_spectrum STATIC
    color_space.cpp
    color_space.h
    spectral.cpp
    spectral.h
    spectrum.cpp
    spectrum.h
    spectrum_ref.cpp
    spectrum_ref.h
    spectrum_tables.cpp)
target_include_directories(rt_spectrum PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rt_spectrum PUBLIC glm)
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    target_compile_options(rt_spectrum PRIVATE -O2)
endif()

# pbrt-v4 scene files as plain C++: the parser (pbrt_parser.h), PLY meshes through rply (../CPPLibraries/rply,
# pbrt's own copy, which reads .ply.gz through zlib) and the mapping onto the scene model (pbrt_import.h). The app
# turns its ImportedModel into a model on the GPU; tests/pbrt_test.cpp checks it without one
add_library(rt_pbrt STATIC
    imported_model.h
    pbrt_import.cpp
    pbrt_import.h
    pbrt_parser.cpp
    pbrt_parser.h
    ply_mesh.cpp
    ply_mesh.h
    "${CMAKE_SOURCE_DIR}/../CPPLibraries/rply/rply.cpp")
target_include_directories(rt_pbrt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rt_pbrt PUBLIC rt_material rt_spectrum rt_film rt_light rt_bvh rt_aov rt_sampler rt_image glm ZLIB::ZLIB fmt::fmt)
set_source_files_properties("${CMAKE_SOURCE_DIR}/../CPPLibraries/rply/rply.cpp" PROPERTIES COMPILE_OPTIONS "-w")
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    target_compile_options(rt_pbrt PRIVATE -O2)
endif()

# Add source to this project's executable.
add_executable(vulkan_guide
    main.cpp
    command_line.cpp
    command_line.h
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
    mount_library.cpp
    mount_library.h
    raster_draw_list.cpp
    vk_tonemap.cpp
    vk_tonemap.h
    vk_readback.cpp
    vk_readback.h
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
    spectrum_edit.cpp
    spectrum_edit.h
    file_dialog.h
    rt_renderer.cpp
    rt_renderer.h
    rt_gpu.cpp
    rt_gpu.h
    rt_gpu_types.h
    rt_wavefront.cpp
    rt_wavefront.h
    rt_schedule.cpp
    rt_schedule.h
    rt_path_schedule.cpp
    rt_path_schedule.h
    rt_accel.cpp
    rt_accel.h
    rt_lights.cpp
    rt_lights.h
    rt_kernels.cpp
    rt_kernels.h
    gpu_selftest.cpp
    gpu_selftest.h
    gpu_selftest_bsdf.cpp
    gpu_selftest_film.cpp
    gpu_selftest_aov.cpp
    gpu_selftest_spectral.cpp
    camera.cpp
    camera.h)


set_property(TARGET vulkan_guide PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:vulkan_guide>")

target_include_directories(vulkan_guide PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# target_link_libraries(vulkan_guide vkbootstrap vma glm tinyobjloader imgui stb_image)
target_link_libraries(vulkan_guide PUBLIC rt_bvh rt_light rt_sampler rt_film rt_bsdf rt_aov rt_image rt_material rt_spectrum rt_pbrt rt_optic_models vk-bootstrap glm imgui imguizmo stb_image glfw fmt::fmt fastgltf::fastgltf)

target_link_libraries(vulkan_guide PRIVATE GPUOpen::VulkanMemoryAllocator)

# the raster preview's draw list runs over every mesh object of the scene (160,000 in a pbrt scene) when the scene
# changes and every frame, and unoptimised glm and STL make that hundreds of milliseconds in a Debug build
if (RT_BVH_ALWAYS_OPTIMIZE AND NOT MSVC)
    set_source_files_properties(raster_draw_list.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "-O2")
endif()

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

add_dependencies(vulkan_guide Shaders UpliftTables)