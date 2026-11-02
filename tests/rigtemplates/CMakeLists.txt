# The Rig Templates extension loaded from its staged package next to lila.exe, driven through its commands.
add_executable(lila_rig_templates_tests main.cpp test_rig_templates.cpp test_rig_template_limits.cpp)
target_link_libraries(lila_rig_templates_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_rig_templates_tests lila_rig_templates_extension)
target_compile_definitions(lila_rig_templates_tests PRIVATE
    LILA_TEST_RIG_TEMPLATES_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/RigTemplates.lilaext"
    LILA_TEST_MANNY_FBX="${CMAKE_SOURCE_DIR}/tests/data/rig/SKM_Manny_skeleton.fbx")
add_test(NAME lila_rig_templates_tests COMMAND lila_rig_templates_tests)
