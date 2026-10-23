add_executable(lila_fbxskin_tests
    main.cpp
    test_fbxskin.cpp)
target_link_libraries(lila_fbxskin_tests PRIVATE LilaScene lila_ufbx doctest::doctest)
add_test(NAME lila_fbxskin_tests COMMAND lila_fbxskin_tests)
