# PySide6 for lila.ui and user scripts, pinned to the Qt version found above so the two can't drift. Only the modules
# whose Qt DLLs Lila deploys are kept; they bind to Lila's loaded Qt.
option(LILA_PYSIDE "Bundle PySide6 Essentials at Lila's Qt version (pip downloads it on the first build)" ON)
set(LILA_PYSIDE_MODULES Core Gui Widgets Network Svg Concurrent Xml Sql PrintSupport OpenGL OpenGLWidgets UiTools)
# Qt libraries only the bindings use; Lila itself links none of them.
set(LILA_PYSIDE_QT_LIBS Xml Sql PrintSupport OpenGL OpenGLWidgets UiTools)
if(LILA_PYSIDE AND WIN32)
    find_package(Qt6 ${Qt6_VERSION} EXACT REQUIRED COMPONENTS ${LILA_PYSIDE_QT_LIBS})
endif()

# Installs into python-site beside target, with the Qt libraries and plugins only the bindings need. Debug builds load
# Qt6Cored.dll, which release PySide can't bind to, so they get no python-site.
function(lila_bundle_pyside target)
    if(NOT LILA_PYSIDE OR NOT WIN32)
        return()
    endif()
    set(_dir ${CMAKE_BINARY_DIR}/pyside-${Qt6_VERSION})
    set(_script ${PROJECT_SOURCE_DIR}/scripts/provision_pyside.py)
    add_custom_command(OUTPUT ${_dir}/PySide6/__init__.py
        COMMAND ${Python_EXECUTABLE} -I ${_script} ${Python_EXECUTABLE} ${Qt6_VERSION} ${_dir} ${LILA_PYSIDE_MODULES}
        DEPENDS ${_script}
        COMMENT "Installing PySide6 ${Qt6_VERSION}")
    add_custom_target(lila_pyside DEPENDS ${_dir}/PySide6/__init__.py)
    add_dependencies(${target} lila_pyside)

    set(_out $<TARGET_FILE_DIR:${target}>)
    set(_cmds)
    foreach(_lib IN LISTS LILA_PYSIDE_QT_LIBS)
        list(APPEND _cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::${_lib}> ${_out})
    endforeach()
    foreach(_plugin QSQLiteDriverPlugin)
        get_target_property(_type Qt6::${_plugin} QT_PLUGIN_TYPE)
        list(APPEND _cmds
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_out}/${_type}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::${_plugin}> ${_out}/${_type})
    endforeach()
    # The SQLite driver links vcpkg's sqlite3, which nothing Lila links pulls in.
    set(_bin ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>bin)
    list(APPEND _cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_bin}/sqlite3.dll ${_out})
    add_custom_command(TARGET ${target} POST_BUILD ${_cmds}
        COMMAND "$<$<NOT:$<CONFIG:Debug>>:${CMAKE_COMMAND};-E;copy_directory_if_different;${_dir};${_out}/python-site>"
        COMMAND_EXPAND_LISTS
        COMMENT "Deploying PySide6")
endfunction()
