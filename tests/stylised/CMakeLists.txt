# Stylised shading models and the viewport post, rendered offscreen; the cases pass without checking when no Vulkan
# device can be created.
add_executable(lila_stylised_tests
    main.cpp
    test_instanced_draws.cpp
    test_shading_models.cpp)
target_link_libraries(lila_stylised_tests PRIVATE LilaUI doctest::doctest)
add_test(NAME lila_stylised_tests COMMAND lila_stylised_tests)
