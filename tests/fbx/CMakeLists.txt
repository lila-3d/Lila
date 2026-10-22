add_executable(lila_fbx_tests
    main.cpp
    test_fbx.cpp)
target_link_libraries(lila_fbx_tests PRIVATE lila_fbx lila_ufbx doctest::doctest)
add_test(NAME lila_fbx_tests COMMAND lila_fbx_tests)
