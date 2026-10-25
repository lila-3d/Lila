add_executable(lila_curve_tests
    main.cpp
    test_curve_frames.cpp
    test_curve_objects.cpp
    test_curve_math.cpp
    test_curve_scene.cpp
    test_edge_to_curve.cpp
    test_pipe.cpp)
target_link_libraries(lila_curve_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_curve_tests COMMAND lila_curve_tests)
