# Optional, with no dependency on Qt, OpenGL or the SDK's broken volk target.
find_program(MELONDS_GLSLANG_VALIDATOR NAMES glslangValidator glslang)
find_program(MELONDS_SPIRV_VAL NAMES spirv-val)
find_path(MELONDS_VULKAN_HEADERS vulkan/vulkan.h)
find_path(MELONDS_VOLK_HEADERS volk.h)
find_path(MELONDS_VOLK_SOURCE volk.c HINTS "${MELONDS_VOLK_HEADERS}")
if (NOT MELONDS_GLSLANG_VALIDATOR OR NOT MELONDS_VULKAN_HEADERS OR
    NOT MELONDS_VOLK_HEADERS OR NOT MELONDS_VOLK_SOURCE OR
    (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    message(STATUS "Vulkan 3D disabled: native shader build tool, glslang and Vulkan/volk headers/source are required")
    return()
endif()

add_executable(ComputeShaderExport ShaderExport.cpp ../GPU3D_ComputeShader.cpp)
target_include_directories(ComputeShaderExport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../GPU3D_ComputeShader.h" scale_limit
    REGEX "^inline constexpr int VulkanMaxScale = [0-9]+;")
string(REGEX MATCH "[0-9]+" vulkan_max_scale "${scale_limit}")
if (NOT vulkan_max_scale)
    message(FATAL_ERROR "Vulkan shader scale range missing")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ../GPU3D_ComputeShader.h)
set(vulkan_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(vulkan_glsl_files)
set(vulkan_spirv_files)
foreach(scale RANGE 1 ${vulkan_max_scale})
    foreach(variant RANGE 0 31)
        list(APPEND vulkan_glsl_files "${vulkan_shader_dir}/${scale}/${variant}.comp")
    endforeach()
endforeach()
add_custom_command(OUTPUT "${vulkan_shader_dir}/export.stamp"
    BYPRODUCTS ${vulkan_glsl_files}
    COMMAND ComputeShaderExport "${vulkan_shader_dir}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${vulkan_shader_dir}/export.stamp"
    DEPENDS ComputeShaderExport VERBATIM)
foreach(scale RANGE 1 ${vulkan_max_scale})
    foreach(variant RANGE 0 31)
        set(spirv "${vulkan_shader_dir}/${scale}/${variant}.spv")
        set(validate_command)
        if (MELONDS_SPIRV_VAL)
            set(validate_command COMMAND "${MELONDS_SPIRV_VAL}" --target-env vulkan1.1 "${spirv}")
        endif()
        add_custom_command(OUTPUT "${spirv}"
            COMMAND "${MELONDS_GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
                "${vulkan_shader_dir}/${scale}/${variant}.comp" -o "${spirv}"
            ${validate_command}
            DEPENDS "${vulkan_shader_dir}/export.stamp" "${vulkan_shader_dir}/${scale}/${variant}.comp" VERBATIM)
        list(APPEND vulkan_spirv_files "${spirv}")
    endforeach()
endforeach()
set(embedded_shaders "${CMAKE_CURRENT_BINARY_DIR}/EmbeddedShaders.cpp")
add_custom_command(OUTPUT "${embedded_shaders}"
    COMMAND "${CMAKE_COMMAND}" "-DSHADER_DIR=${vulkan_shader_dir}" "-DOUTPUT_FILE=${embedded_shaders}" "-DMAX_SCALE=${vulkan_max_scale}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedShaders.cmake"
    DEPENDS ${vulkan_spirv_files} EmbedShaders.cmake VERBATIM)
add_library(vulkan-compute STATIC Device.cpp Loader.cpp ComputePipeline.cpp "${embedded_shaders}")
target_include_directories(vulkan-compute PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/.."
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(vulkan-compute SYSTEM PUBLIC "${MELONDS_VULKAN_HEADERS}" "${MELONDS_VOLK_HEADERS}"
    PRIVATE "${MELONDS_VOLK_SOURCE}")
target_compile_features(vulkan-compute PUBLIC cxx_std_26)
target_link_libraries(vulkan-compute PRIVATE ${CMAKE_DL_LIBS})
target_sources(core PRIVATE ../GPU_Vulkan.cpp ../GPU_VulkanCapture.cpp ../GPU3D_Vulkan.cpp)
target_link_libraries(core PUBLIC vulkan-compute)
target_compile_definitions(core PUBLIC VULKANRENDERER_ENABLED)
install(FILES LICENSE.volk DESTINATION share/licenses/melonDS/volk RENAME LICENSE)
