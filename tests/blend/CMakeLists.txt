add_executable(lila_blend_tests
    main.cpp
    test_blend.cpp)
target_link_libraries(lila_blend_tests PRIVATE LilaScene doctest::doctest)
target_compile_definitions(lila_blend_tests PRIVATE LILA_BLEND_TEST_DATA="${CMAKE_SOURCE_DIR}/tests/data/blend")
add_test(NAME lila_blend_tests COMMAND lila_blend_tests)
