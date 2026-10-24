# Dock behaviour on an offscreen QMainWindow, no GPU or display needed.
add_executable(lila_dock_tests main.cpp test_docks.cpp test_action_docs.cpp test_channel_text.cpp test_outliner_widget.cpp test_progress_overlay.cpp test_hotkey_text.cpp test_option_box.cpp test_node_editor.cpp test_vim_binding.cpp test_tearoff_widget.cpp test_prefs_sections.cpp test_overlay_contrast.cpp test_extension_manager.cpp test_modes.cpp test_theme_widgets.cpp test_baker_panel.cpp)
target_link_libraries(lila_dock_tests PRIVATE LilaUI doctest::doctest)
if(WIN32)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    cmake_path(GET _qmake PARENT_PATH _qtbin)
    add_custom_command(TARGET lila_dock_tests POST_BUILD
        COMMAND ${_qtbin}/windeployqt.exe --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime $<TARGET_FILE:lila_dock_tests> $<TARGET_FILE:LilaUI> > NUL
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:lila_dock_tests>/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_qtbin}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll $<TARGET_FILE_DIR:lila_dock_tests>/platforms
        COMMENT "Deploying Qt and the offscreen platform for lila_dock_tests")
endif()
target_compile_definitions(lila_dock_tests PRIVATE LILA_TEST_PACKAGES_DIR="${CMAKE_SOURCE_DIR}/tests/extensions/packages")
add_test(NAME lila_dock_tests COMMAND lila_dock_tests)
