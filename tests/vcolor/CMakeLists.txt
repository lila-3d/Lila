add_executable(lila_vcolor_tests
    main.cpp
    test_vcolor.cpp)
target_link_libraries(lila_vcolor_tests PRIVATE lila_vcolor lila_io lila_fbx doctest::doctest)
target_include_directories(lila_vcolor_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME lila_vcolor_tests COMMAND lila_vcolor_tests)
