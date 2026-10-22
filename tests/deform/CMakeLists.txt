add_executable(lila_deform_tests
    main.cpp
    test_deform_math.cpp
    test_deformers.cpp)
target_link_libraries(lila_deform_tests PRIVATE lila_deform lila_io lila_fbx lila_ufbx doctest::doctest)
add_test(NAME lila_deform_tests COMMAND lila_deform_tests)
