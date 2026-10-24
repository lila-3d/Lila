add_executable(lila_brush_tests
    main.cpp
    test_brush.cpp)
target_link_libraries(lila_brush_tests PRIVATE LilaScene doctest::doctest)
target_include_directories(lila_brush_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME lila_brush_tests COMMAND lila_brush_tests)
