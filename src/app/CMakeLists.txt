# Startup and shutdown shared by both hosts, without the UI.
lila_module(lila_host Scene Host.cpp Host.h)
target_link_libraries(lila_host PUBLIC lila_scene lila_io lila_python lila_mcp lila_extensions lila_vcs lila_link lila_fbx lila_gltf lila_maya lila_gn
    lila_uv_cmds lila_retopo_cmds lila_sculpt_cmds lila_vcolor lila_layouts lila_batch)

# lila is the GUI app (no console window); lilabatch is the console app for --batch from shells and CI, without the UI library.
add_executable(lila WIN32 main.cpp $<$<PLATFORM_ID:Windows>:lila.rc>)
target_link_libraries(lila PRIVATE LilaUI)
target_compile_definitions(lila PRIVATE LILA_GUI_SUBSYSTEM)
add_executable(lilabatch batch.cpp $<$<PLATFORM_ID:Windows>:lila.rc>)
target_link_libraries(lilabatch PRIVATE LilaScene)
# Loaded at run time for the capture commands when it is there; lilabatch runs without it.
add_dependencies(lilabatch LilaUI)
add_dependencies(lila lilabatch)
if(WIN32)
    # The crash watchdog and the WER helper it relies on for fail-fast crashes; see src/core/CrashWatch.h.
    add_executable(lilacrash WIN32 lilacrash.cpp)
    target_link_libraries(lilacrash PRIVATE lila_crashkit)
    add_library(lilawer SHARED lilawer.cpp)
    target_link_libraries(lilawer PRIVATE lila_crashkit)
    add_dependencies(lilabatch lilacrash lilawer)
endif()
set_target_properties(lila PROPERTIES VS_DEBUGGER_ENVIRONMENT "PATH=${Qt6_DIR}/../../../bin;%PATH%")

if(WIN32)
    # Python and Qt next to the exe so it runs without PATH setup.
    cmake_path(GET Python_EXECUTABLE PARENT_PATH _pyhome)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    cmake_path(GET _qmake PARENT_PATH _qtbin)
    add_custom_command(TARGET lila POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pyhome}/python3.dll ${_pyhome}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll $<TARGET_FILE_DIR:lila>
        COMMAND ${_qtbin}/windeployqt.exe --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime $<TARGET_FILE:lila> $<TARGET_FILE:LilaUI> > NUL
        # The offscreen platform, for the headless widget tests that run from this folder.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_qtbin}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll $<TARGET_FILE_DIR:lila>/platforms
        COMMENT "Deploying Python and Qt runtime")
endif()
