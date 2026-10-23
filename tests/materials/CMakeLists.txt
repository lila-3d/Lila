add_executable(lila_material_tests
    main.cpp
    test_materials.cpp)
target_link_libraries(lila_material_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_material_tests COMMAND lila_material_tests)
