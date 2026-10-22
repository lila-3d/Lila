add_executable(lila_material_tests
    main.cpp
    test_materials.cpp)
target_link_libraries(lila_material_tests PRIVATE lila_io lila_fbx doctest::doctest)
add_test(NAME lila_material_tests COMMAND lila_material_tests)
