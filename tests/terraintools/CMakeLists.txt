# The Terrain Tools extension loaded from its staged package next to lila.exe, driven through its commands, plus its blade
# extrude maths compiled in directly.
add_executable(lila_terraintools_tests main.cpp test_terrain_tools.cpp ${CMAKE_SOURCE_DIR}/extensions/TerrainTools/BladeExtrude.cpp)
target_include_directories(lila_terraintools_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/TerrainTools)
target_link_libraries(lila_terraintools_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_terraintools_tests lila_terrain_tools_extension)
target_compile_definitions(lila_terraintools_tests PRIVATE
    LILA_TEST_TERRAIN_TOOLS_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/TerrainTools.lilaext")
add_test(NAME lila_terraintools_tests COMMAND lila_terraintools_tests)
