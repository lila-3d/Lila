add_executable(lila_rig_tests
    main.cpp
    test_joints.cpp
    test_skin.cpp
    test_constraints.cpp)
target_link_libraries(lila_rig_tests PRIVATE lila_io doctest::doctest)
add_test(NAME lila_rig_tests COMMAND lila_rig_tests)

# Viewport side: joint drawing/picking and the weight paint tool, driven headless (offscreen Qt platform).
add_executable(lila_rig_ui_tests ui_main.cpp test_rig_ui.cpp)
target_link_libraries(lila_rig_ui_tests PRIVATE lila_ui doctest::doctest)
