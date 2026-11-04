add_executable(lila_rigmath_tests main.cpp test_rigmath.cpp test_rigmath_ramp.cpp test_expressions.cpp)
target_link_libraries(lila_rigmath_tests PRIVATE LilaScene doctest::doctest)
target_compile_definitions(lila_rigmath_tests PRIVATE LILA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME lila_rigmath_tests COMMAND lila_rigmath_tests)
