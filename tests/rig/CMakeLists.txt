add_executable(lila_rig_tests
    main.cpp
    test_joints.cpp
    test_skin.cpp
    test_constraints.cpp
    test_center.cpp
    test_ik.cpp
    test_fbik.cpp
    test_edit_skeleton.cpp
    test_ghosts.cpp
    test_poses.cpp
    test_picker.cpp
    test_spaces.cpp
    test_rig_elements.cpp
    test_select_as.cpp
    test_offsets.cpp)
target_link_libraries(lila_rig_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_rig_tests COMMAND lila_rig_tests)

# Viewport side: joint drawing/picking and the weight paint tool, driven headless (offscreen Qt platform).
add_executable(lila_rig_ui_tests ui_main.cpp test_rig_ui.cpp test_picker_ui.cpp)
target_link_libraries(lila_rig_ui_tests PRIVATE LilaUI doctest::doctest)
add_test(NAME lila_rig_ui_tests COMMAND lila_rig_ui_tests)
