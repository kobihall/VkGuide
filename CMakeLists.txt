cmake_minimum_required (VERSION 3.16)

project ("vulkan_guide")


set(CMAKE_CXX_STANDARD 20)

find_package(Vulkan REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_package(fmt REQUIRED)

include_directories(/usr/local/include)
include_directories(${CMAKE_SOURCE_DIR}/../CPPLibraries)
add_subdirectory(${CMAKE_SOURCE_DIR}/../CPPLibraries/imgui imgui)
add_subdirectory(${CMAKE_SOURCE_DIR}/../CPPLibraries/glm glm)
add_subdirectory(${CMAKE_SOURCE_DIR}/../CPPLibraries/vk-bootstrap vk-bootstrap)
add_subdirectory(${CMAKE_SOURCE_DIR}/../CPPLibraries/VulkanMemoryAllocator VulkanMemoryAllocator)
add_subdirectory(${CMAKE_SOURCE_DIR}/../CPPLibraries/fastgltf fastgltf)
add_subdirectory(${CMAKE_SOURCE_DIR}/../CPPLibraries/ImGuizmo ImGuizmo)
# ImGuizmo's own CMake only links imgui when it is built standalone
target_link_libraries(imguizmo PUBLIC imgui)
add_library(stb_image INTERFACE)
target_include_directories(stb_image INTERFACE ${CMAKE_SOURCE_DIR}/../CPPLibraries)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")

add_subdirectory(src)

# CPU-only verification and comparison of the BVH builders and layouts: ./bin/bvh_bench <file.glb>
add_executable(bvh_bench tests/bvh_bench.cpp tests/bvh_robust.cpp)
target_link_libraries(bvh_bench PRIVATE rt_bvh fastgltf::fastgltf)
if (NOT MSVC)
    target_compile_options(bvh_bench PRIVATE -O2)
endif()

# CPU-only verification of the light sampling behind next-event estimation and MIS: ./bin/light_test
add_executable(light_test tests/light_test.cpp)
target_link_libraries(light_test PRIVATE rt_light fmt::fmt)
if (NOT MSVC)
    target_compile_options(light_test PRIVATE -O2)
endif()

# CPU-only verification of the sample generators behind kernel 01: ./bin/sampler_test
add_executable(sampler_test tests/sampler_test.cpp)
target_link_libraries(sampler_test PRIVATE rt_sampler fmt::fmt)
if (NOT MSVC)
    target_compile_options(sampler_test PRIVATE -O2)
endif()

# CPU-only verification of the surface scattering behind kernel 06: ./bin/bsdf_test
add_executable(bsdf_test tests/bsdf_test.cpp tests/bsdf_test_bxdf.cpp)
target_link_libraries(bsdf_test PRIVATE rt_bsdf rt_spectrum fmt::fmt)
if (NOT MSVC)
    target_compile_options(bsdf_test PRIVATE -O2)
endif()

# CPU-only verification of the film behind kernels 00 and 09: ./bin/film_test
add_executable(film_test tests/film_test.cpp)
target_link_libraries(film_test PRIVATE rt_film fmt::fmt)
if (NOT MSVC)
    target_compile_options(film_test PRIVATE -O2)
endif()

# CPU-only verification of the auxiliary outputs behind kernel 09: ./bin/aov_test
add_executable(aov_test tests/aov_test.cpp)
target_link_libraries(aov_test PRIVATE rt_aov fmt::fmt)
if (NOT MSVC)
    target_compile_options(aov_test PRIVATE -O2)
endif()

# The RGB uplift tables of the spectral modes (src/color_space.h): pbrt-v4's rgb2spec_opt, run once per color space
# into assets/spectra/uplift_<space>.bin (about 5 seconds each, 9.4 MB each, not committed)
add_executable(rgb2spec_opt tools/rgb2spec/rgb2spec_opt.cpp)
set_target_properties(rgb2spec_opt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
if (NOT MSVC)
    target_compile_options(rgb2spec_opt PRIVATE -O2)
endif()
set(UPLIFT_TABLES "")
foreach(SPACE "srgb|sRGB" "dci_p3|DCI_P3" "rec2020|REC2020" "aces2065_1|ACES2065_1")
  string(REPLACE "|" ";" PARTS "${SPACE}")
  list(GET PARTS 0 FILE_NAME)
  list(GET PARTS 1 GAMUT)
  set(TABLE "${PROJECT_SOURCE_DIR}/assets/spectra/uplift_${FILE_NAME}.bin")
  add_custom_command(
    OUTPUT ${TABLE}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_SOURCE_DIR}/assets/spectra"
    COMMAND rgb2spec_opt 64 ${TABLE} ${GAMUT}
    DEPENDS rgb2spec_opt
    COMMENT "Generating the ${GAMUT} uplift table")
  list(APPEND UPLIFT_TABLES ${TABLE})
endforeach()
add_custom_target(UpliftTables DEPENDS ${UPLIFT_TABLES})

# CPU-only check of the spectra behind the pbrt import's RGB: ./bin/spectrum_test
add_executable(spectrum_test tests/spectrum_test.cpp)
target_link_libraries(spectrum_test PRIVATE rt_spectrum fmt::fmt)
add_dependencies(spectrum_test UpliftTables)
if (NOT MSVC)
    target_compile_options(spectrum_test PRIVATE -O2)
endif()

# CPU-only check of the pbrt scene import: the scanner, the parser's transforms, and every scene of a folder
# imported with its report: ./bin/pbrt_test [--scan dir] [--report file.pbrt]
add_executable(pbrt_test tests/pbrt_test.cpp)
target_link_libraries(pbrt_test PRIVATE rt_pbrt fmt::fmt)
if (NOT MSVC)
    target_compile_options(pbrt_test PRIVATE -O2)
endif()

# Converts a pbrt-v4 scene to glTF 2.0 through the renderer's importer, to open in Blender, a web viewer or Unity as a
# raster baseline: ./bin/pbrt_to_gltf scene.pbrt out.gltf [--instancing] [--weld]
add_executable(pbrt_to_gltf tests/pbrt_to_gltf.cpp)
target_link_libraries(pbrt_to_gltf PRIVATE rt_pbrt fmt::fmt)
if (NOT MSVC)
    target_compile_options(pbrt_to_gltf PRIVATE -O2)
endif()

# Lists the channels and attributes of an EXR file and dumps them as .npy: ./bin/exr_dump
add_executable(exr_dump tests/exr_dump.cpp)
target_link_libraries(exr_dump PRIVATE rt_image fmt::fmt)
if (NOT MSVC)
    target_compile_options(exr_dump PRIVATE -O2)
endif()

# Converts an image between EXR, PFM and Radiance HDR: ./bin/image_convert
add_executable(image_convert tests/image_convert.cpp)
target_link_libraries(image_convert PRIVATE rt_image stb_image fmt::fmt)
if (NOT MSVC)
    target_compile_options(image_convert PRIVATE -O2)
endif()

# Compares two images (EXR or PFM) with the render panel's error measures: ./bin/image_diff
add_executable(image_diff tests/image_diff.cpp)
target_link_libraries(image_diff PRIVATE rt_image)
if (NOT MSVC)
    target_compile_options(image_diff PRIVATE -O2)
endif()


find_program(GLSL_VALIDATOR glslangValidator HINTS /usr/bin /usr/local/bin $ENV{VULKAN_SDK}/Bin/ $ENV{VULKAN_SDK}/Bin32/)

## find all the shader files under the shaders folder
file(GLOB_RECURSE GLSL_SOURCE_FILES
    "${PROJECT_SOURCE_DIR}/shaders/*.frag"
    "${PROJECT_SOURCE_DIR}/shaders/*.vert"
    "${PROJECT_SOURCE_DIR}/shaders/*.comp"
    )

## shared include files: not compiled themselves, but a change to one must rebuild every shader
file(GLOB_RECURSE GLSL_INCLUDE_FILES "${PROJECT_SOURCE_DIR}/shaders/*.glsl")

## the raytracer's traversals, read from the defined(CRT_TRAVERSAL_<ID>) lines of the one file that
## lists them. A kernel that includes that file is compiled once per traversal, to
## <name>.comp.<id>.spv; src/rt_kernels.cpp TraversalVariant names the same ids. Editing the file
## re-runs this configure step, since it decides which outputs exist
set(CRT_TRACE_FILE "${PROJECT_SOURCE_DIR}/shaders/rt/include/crt_trace.glsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CRT_TRACE_FILE})
file(STRINGS ${CRT_TRACE_FILE} CRT_TRAVERSAL_LINES REGEX "defined\\(CRT_TRAVERSAL_[A-Z0-9_]+\\)")
set(CRT_TRAVERSALS "")
foreach(LINE ${CRT_TRAVERSAL_LINES})
  string(REGEX MATCH "CRT_TRAVERSAL_[A-Z0-9_]+" DEFINE ${LINE})
  list(APPEND CRT_TRAVERSALS ${DEFINE})
endforeach()

## the material classes, read the same way from the defined(CRT_MATERIAL_CLASS_<ID>) lines of the file
## that lists them. A kernel that includes it is compiled once per class, to <name>.comp.<id>.spv, or
## <name>.comp.<traversal>.<id>.spv for one that also traces; src/rt_kernels.cpp names the same ids
set(CRT_MATERIAL_CLASS_FILE "${PROJECT_SOURCE_DIR}/shaders/rt/include/crt_material_class.glsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CRT_MATERIAL_CLASS_FILE})
file(STRINGS ${CRT_MATERIAL_CLASS_FILE} CRT_MATERIAL_CLASS_LINES REGEX "defined\\(CRT_MATERIAL_CLASS_[A-Z0-9_]+\\)")
set(CRT_MATERIAL_CLASSES "")
foreach(LINE ${CRT_MATERIAL_CLASS_LINES})
  string(REGEX MATCH "CRT_MATERIAL_CLASS_[A-Z0-9_]+" DEFINE ${LINE})
  list(APPEND CRT_MATERIAL_CLASSES ${DEFINE})
endforeach()

## the lane families of the spectral modes (docs/plans/completed/pbrt-spectral-rendering.md, src/spectral.h
## SPECTRAL_FAMILIES): a kernel that includes include/crt_spectrum.glsl itself is compiled once without CRT_LANES (the
## exact RGB build, today's .spv) and once per family, to <name>.comp[.<traversal>][.<class>].lanes<N>.spv
set(VKGUIDE_SPECTRAL_FAMILIES "1;4;8;16;32" CACHE STRING "The lane counts the spectral kernels are compiled for")

## iterate each shader
foreach(GLSL ${GLSL_SOURCE_FILES})
  message(STATUS "BUILDING SHADER")
  ## keep each shader's path relative to shaders/ so the raytracing kernels in shaders/rt/
  ## compile beside their sources instead of colliding in one flat output directory
  file(RELATIVE_PATH REL_NAME "${PROJECT_SOURCE_DIR}/shaders" ${GLSL})
  get_filename_component(SPIRV_DIR "${PROJECT_SOURCE_DIR}/shaders/${REL_NAME}" DIRECTORY)
  file(MAKE_DIRECTORY ${SPIRV_DIR})
  message(STATUS ${GLSL})
  file(STRINGS ${GLSL} TRACES REGEX "#include \"include/crt_trace.glsl\"")
  file(STRINGS ${GLSL} PER_CLASS REGEX "#include \"include/crt_material_class.glsl\"")
  file(STRINGS ${GLSL} PER_FAMILY REGEX "#include \"include/crt_spectrum.glsl\"")
  ## the permutations: one per traversal for a kernel that traces, times one per material class for
  ## a kernel that evaluates materials. Each entry is "<suffix>|<defines>"; a plain shader has one
  ## empty entry
  set(VARIANTS "|")
  if (TRACES)
    set(TRAVERSAL_VARIANTS "")
    foreach(DEFINE ${CRT_TRAVERSALS})
      string(REPLACE "CRT_TRAVERSAL_" "" TRAVERSAL ${DEFINE})
      string(TOLOWER ${TRAVERSAL} TRAVERSAL)
      list(APPEND TRAVERSAL_VARIANTS ".${TRAVERSAL}|-D${DEFINE}=1")
    endforeach()
    set(VARIANTS ${TRAVERSAL_VARIANTS})
  endif()
  if (PER_CLASS)
    set(CLASS_VARIANTS "")
    foreach(VARIANT ${VARIANTS})
      string(REPLACE "|" ";" PARTS "${VARIANT}|")
      list(GET PARTS 0 SUFFIX)
      list(GET PARTS 1 DEFINES)
      foreach(DEFINE ${CRT_MATERIAL_CLASSES})
        string(REPLACE "CRT_MATERIAL_CLASS_" "" CLASS ${DEFINE})
        string(TOLOWER ${CLASS} CLASS)
        list(APPEND CLASS_VARIANTS "${SUFFIX}.${CLASS}|${DEFINES} -D${DEFINE}=1")
      endforeach()
    endforeach()
    set(VARIANTS ${CLASS_VARIANTS})
  endif()
  if (PER_FAMILY)
    set(FAMILY_VARIANTS ${VARIANTS})
    foreach(VARIANT ${VARIANTS})
      string(REPLACE "|" ";" PARTS "${VARIANT}|")
      list(GET PARTS 0 SUFFIX)
      list(GET PARTS 1 DEFINES)
      foreach(LANES ${VKGUIDE_SPECTRAL_FAMILIES})
        list(APPEND FAMILY_VARIANTS "${SUFFIX}.lanes${LANES}|${DEFINES} -DCRT_LANES=${LANES}")
      endforeach()
    endforeach()
    set(VARIANTS ${FAMILY_VARIANTS})
  endif()
  foreach(VARIANT ${VARIANTS})
    string(REPLACE "|" ";" PARTS "${VARIANT}|")
    list(GET PARTS 0 SUFFIX)
    list(GET PARTS 1 DEFINES)
    separate_arguments(DEFINE_ARGS UNIX_COMMAND "${DEFINES}")
    set(SPIRV "${PROJECT_SOURCE_DIR}/shaders/${REL_NAME}${SUFFIX}.spv")
    add_custom_command(
      OUTPUT ${SPIRV}
      COMMAND ${GLSL_VALIDATOR} -V -I${PROJECT_SOURCE_DIR}/shaders ${DEFINE_ARGS} ${GLSL} -o ${SPIRV}
      DEPENDS ${GLSL} ${GLSL_INCLUDE_FILES})
    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
  endforeach()
endforeach(GLSL)

add_custom_target(
    Shaders 
    DEPENDS ${SPIRV_BINARY_FILES}
    )