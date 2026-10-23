add_executable(lila_deform_tests
    main.cpp
    test_deform_math.cpp
    test_deformers.cpp
    test_morphs.cpp)
target_link_libraries(lila_deform_tests PRIVATE LilaScene lila_ufbx doctest::doctest)
add_test(NAME lila_deform_tests COMMAND lila_deform_tests)
