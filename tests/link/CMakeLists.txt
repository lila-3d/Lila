add_executable(lila_link_tests
    main.cpp
    test_link.cpp
    test_spline.cpp
    test_placements.cpp)
target_link_libraries(lila_link_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_link_tests COMMAND lila_link_tests)
