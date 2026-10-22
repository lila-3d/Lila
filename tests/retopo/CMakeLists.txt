add_executable(lila_retopo_tests
    main.cpp
    test_corpus.cpp
    test_repair.cpp
    test_retopo.cpp
    test_tools.cpp)
target_link_libraries(lila_retopo_tests PRIVATE lila_retopo_cmds lila_io doctest::doctest)
target_include_directories(lila_retopo_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME lila_retopo_tests COMMAND lila_retopo_tests)
