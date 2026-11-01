add_executable(lila_sculpt_tests
    main.cpp
    test_sculpt.cpp
    test_polypaint.cpp
    test_alphas.cpp)
target_link_libraries(lila_sculpt_tests PRIVATE LilaScene doctest::doctest)
target_include_directories(lila_sculpt_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME lila_sculpt_tests COMMAND lila_sculpt_tests)
