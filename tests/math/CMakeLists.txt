add_executable(lila_math_tests
    main.cpp
    test_math.cpp)
target_link_libraries(lila_math_tests PRIVATE lila_math doctest::doctest)
add_test(NAME lila_math_tests COMMAND lila_math_tests)
