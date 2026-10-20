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