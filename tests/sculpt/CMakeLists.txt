add_executable(lila_sculpt_tests
    main.cpp
    test_sculpt.cpp)
target_link_libraries(lila_sculpt_tests PRIVATE lila_sculpt_cmds lila_retopo_cmds lila_io doctest::doctest)
target_include_directories(lila_sculpt_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME lila_sculpt_tests COMMAND lila_sculpt_tests)
