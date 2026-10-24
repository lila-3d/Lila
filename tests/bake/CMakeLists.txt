add_executable(lila_bake_tests
    main.cpp
    test_bake.cpp
    test_cage.cpp
    test_bake_groups.cpp
    test_bake_maps.cpp
    test_bake_pipeline.cpp)
target_link_libraries(lila_bake_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_bake_tests COMMAND lila_bake_tests)

# GPU backend against the CPU reference; the GPU cases pass without checking when no Vulkan device can be created.
add_executable(lila_bake_gpu_tests
    gpu_main.cpp
    test_bake_gpu.cpp
    test_gpu_image.cpp
    test_live_maps.cpp)
target_link_libraries(lila_bake_gpu_tests PRIVATE LilaUI doctest::doctest)
add_test(NAME lila_bake_gpu_tests COMMAND lila_bake_gpu_tests)
