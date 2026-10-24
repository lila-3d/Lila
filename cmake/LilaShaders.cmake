find_program(SLANGC slangc
    HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shader-slang
    REQUIRED)

# lila_add_shaders(target FILES a.slang ... [INCLUDES b.slang ...]): each .slang in FILES compiles every [shader] entry
# point to <name>.spv in bin/<config>/shaders. INCLUDES are #included files: editing one recompiles the FILES.
function(lila_add_shaders target)
    cmake_parse_arguments(ARG "" "" "FILES;INCLUDES" ${ARGN})
    list(TRANSFORM ARG_INCLUDES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
    set(out_dir ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/shaders)
    set(outputs)
    foreach(src ${ARG_FILES})
        get_filename_component(name ${src} NAME_WE)
        set(out ${out_dir}/${name}.spv)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
            COMMAND ${SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/${src} -target spirv -profile spirv_1_6
                    -fvk-use-entrypoint-name -matrix-layout-column-major -o ${out}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${ARG_INCLUDES}
            COMMENT "slangc ${src}")
        list(APPEND outputs ${out})
    endforeach()
    add_custom_target(${target}_shaders DEPENDS ${outputs})
    add_dependencies(${target} ${target}_shaders)
endfunction()
