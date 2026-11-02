add_executable(lila_retarget_tests main.cpp test_rig.cpp test_retarget.cpp test_semantic.cpp test_scene.cpp)
target_link_libraries(lila_retarget_tests PRIVATE LilaScene doctest::doctest)
target_compile_definitions(lila_retarget_tests PRIVATE LILA_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
add_test(NAME lila_retarget_tests COMMAND lila_retarget_tests)

# The dialog and panel, driven headless (offscreen Qt platform).
add_executable(lila_retarget_ui_tests ui_main.cpp test_retarget_ui.cpp)
target_link_libraries(lila_retarget_ui_tests PRIVATE LilaUI doctest::doctest)
add_test(NAME lila_retarget_ui_tests COMMAND lila_retarget_ui_tests)
