add_executable(lila_collada_tests
    main.cpp
    test_collada.cpp)
target_link_libraries(lila_collada_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_collada_tests COMMAND lila_collada_tests)
