find_package(meshoptimizer CONFIG REQUIRED)
find_package(draco CONFIG REQUIRED)

add_executable(lila_gltf_tests
    main.cpp
    test_gltf.cpp
    test_gltf_anim.cpp
    test_gltf_draco.cpp
    test_gltf_fixtures.cpp)
target_include_directories(lila_gltf_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/cgltf)
target_link_libraries(lila_gltf_tests PRIVATE LilaScene meshoptimizer::meshoptimizer draco::draco doctest::doctest)
add_test(NAME lila_gltf_tests COMMAND lila_gltf_tests)
