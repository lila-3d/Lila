find_program(SLANGC slangc
    HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shader-slang)

# lila_add_shaders(target FILES a.slang ... [INCLUDES b.slang ...] [OUTPUT_DIR dir]): each .slang in FILES compiles every
# [shader] entry point to <name>.spv in bin/<config>/shaders, or in OUTPUT_DIR (an SDK extension's own folder, read
# back with lila::gpu::Recorder::kernel by absolute path). INCLUDES are #included files: editing one recompiles the FILES.
function(lila_add_shaders target)
    cmake_parse_arguments(ARG "" "OUTPUT_DIR" "FILES;INCLUDES" ${ARGN})
    if(NOT SLANGC)
        message(FATAL_ERROR "lila_add_shaders: slangc not found; install the Slang compiler or set SLANGC")
    endif()
    list(TRANSFORM ARG_INCLUDES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
    set(out_dir ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/shaders)
    if(ARG_OUTPUT_DIR)
        set(out_dir ${ARG_OUTPUT_DIR})
    endif()
    set(outputs)
    foreach(src ${ARG_FILES})
        get_filename_component(name ${src} NAME_WE)
        set(out ${out_dir}/${name}.spv)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
            # 41012: entry points add the capabilities they use (ray query, barycentrics...) to the base profile on purpose.
            COMMAND ${SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/${src} -target spirv -profile spirv_1_6 -warnings-disable 41012
                    -fvk-use-entrypoint-name -matrix-layout-column-major -o ${out}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${ARG_INCLUDES}
            COMMENT "slangc ${src}")
        list(APPEND outputs ${out})
    endforeach()
    add_custom_target(${target}_shaders DEPENDS ${outputs})
    add_dependencies(${target} ${target}_shaders)
endfunction()
