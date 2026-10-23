# The Terrain Generator extension loaded from its staged package next to lila.exe and driven through its commands, plus
# its scatter maths compiled in directly.
add_executable(lila_terraingen_tests main.cpp test_terrain_generator.cpp ${CMAKE_SOURCE_DIR}/extensions/TerrainGenerator/TerrainCore.cpp
    ${CMAKE_SOURCE_DIR}/extensions/TerrainGenerator/TerrainTables.cpp)
target_include_directories(lila_terraingen_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/TerrainGenerator)
target_link_libraries(lila_terraingen_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_terraingen_tests lila_terrain_generator_extension)
target_compile_definitions(lila_terraingen_tests PRIVATE
    LILA_TEST_TERRAIN_GENERATOR_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/TerrainGenerator.lilaext")
add_test(NAME lila_terraingen_tests COMMAND lila_terraingen_tests)
