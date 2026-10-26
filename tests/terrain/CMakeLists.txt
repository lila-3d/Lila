# The Terrain extension: its core compiled in directly, and the package driven through its commands.
add_executable(lila_terrain_tests main.cpp test_terrain.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Core.cpp)
target_include_directories(lila_terrain_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Terrain)
target_link_libraries(lila_terrain_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_terrain_tests lila_terrain_extension)
target_compile_definitions(lila_terrain_tests PRIVATE
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext")
add_test(NAME lila_terrain_tests COMMAND lila_terrain_tests)
