# Add source to this project's executable.
add_executable(vulkan_guide
    main.cpp
    vk_engine.cpp
    vk_engine.h
    vk_images.cpp
    vk_images.h
    vk_types.h
    vk_initializers.cpp
    vk_initializers.h)


set_property(TARGET vulkan_guide PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:vulkan_guide>")

target_include_directories(vulkan_guide PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# target_link_libraries(vulkan_guide vkbootstrap vma glm tinyobjloader imgui stb_image)
target_link_libraries(vulkan_guide PUBLIC vk-bootstrap glm imgui stb_image glfw fmt::fmt)

target_link_libraries(vulkan_guide PRIVATE GPUOpen::VulkanMemoryAllocator)

if (APPLE)
    target_compile_options(vulkan_guide PRIVATE -Wno-nullability-completeness)
endif()

target_link_libraries(vulkan_guide PUBLIC Vulkan::Vulkan)

target_precompile_headers(vulkan_guide PUBLIC <optional> <vector> <memory> <string> <vector> <unordered_map> <glm/mat4x4.hpp>  <glm/vec4.hpp> <vulkan/vulkan.h>)

add_dependencies(vulkan_guide Shaders)