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
add_executable(bvh_bench tests/bvh_bench.cpp)
target_link_libraries(bvh_bench PRIVATE rt_bvh fastgltf::fastgltf)
if (NOT MSVC)
    target_compile_options(bvh_bench PRIVATE -O2)
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

## iterate each shader
foreach(GLSL ${GLSL_SOURCE_FILES})
  message(STATUS "BUILDING SHADER")
  ## keep each shader's path relative to shaders/ so the raytracing kernels in shaders/rt/
  ## compile beside their sources instead of colliding in one flat output directory
  file(RELATIVE_PATH REL_NAME "${PROJECT_SOURCE_DIR}/shaders" ${GLSL})
  set(SPIRV "${PROJECT_SOURCE_DIR}/shaders/${REL_NAME}.spv")
  get_filename_component(SPIRV_DIR ${SPIRV} DIRECTORY)
  file(MAKE_DIRECTORY ${SPIRV_DIR})
  message(STATUS ${GLSL})
  ##execute glslang command to compile that specific shader
  add_custom_command(
    OUTPUT ${SPIRV}
    COMMAND ${GLSL_VALIDATOR} -V -I${PROJECT_SOURCE_DIR}/shaders ${GLSL} -o ${SPIRV}
    DEPENDS ${GLSL} ${GLSL_INCLUDE_FILES})
  list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach(GLSL)

add_custom_target(
    Shaders 
    DEPENDS ${SPIRV_BINARY_FILES}
    )