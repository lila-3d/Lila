# Qt comes from vcpkg. vcpkg's applocal step copies the DLLs a target links; Qt's plugins are loaded at run time, so
# lila_deploy_qt copies them into the plugin folders beside the target.

set(LILA_QT_PLUGINS
    QWindowsIntegrationPlugin QOffscreenIntegrationPlugin QModernWindowsStylePlugin
    QSvgIconPlugin QSvgPlugin QJpegPlugin QICOPlugin QGifPlugin
    QSchannelBackendPlugin QTlsBackendCertOnlyPlugin QNLMNIPlugin)

function(lila_deploy_qt target)
    if(NOT WIN32)
        return()
    endif()
    set(_dir $<TARGET_FILE_DIR:${target}>)
    set(_cmds)
    foreach(_module Core Gui Widgets Svg Network)
        list(APPEND _cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::${_module}> ${_dir})
    endforeach()
    foreach(_plugin IN LISTS LILA_QT_PLUGINS)
        if(TARGET Qt6::${_plugin})
            get_target_property(_type Qt6::${_plugin} QT_PLUGIN_TYPE)
            list(APPEND _cmds
                COMMAND ${CMAKE_COMMAND} -E make_directory ${_dir}/${_type}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::${_plugin}> ${_dir}/${_type})
        endif()
    endforeach()
    # The JPEG plugin's libjpeg: nothing Lila links pulls it in, so applocal never copies it.
    if(VCPKG_INSTALLED_DIR)
        list(APPEND _cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>bin/jpeg62.dll ${_dir})
    endif()
    add_custom_command(TARGET ${target} POST_BUILD ${_cmds} COMMENT "Deploying Qt for ${target}")
endfunction()
