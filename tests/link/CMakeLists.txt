add_executable(lila_link_tests
    main.cpp
    test_link.cpp)
target_link_libraries(lila_link_tests PRIVATE lila_link doctest::doctest)
add_test(NAME lila_link_tests COMMAND lila_link_tests)
