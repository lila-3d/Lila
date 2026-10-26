# The Ground Tiles extension loaded from its staged package next to lila.exe and driven through its commands, plus
# its scatter maths compiled in directly.
add_executable(lila_groundtiles_tests main.cpp test_ground_tiles.cpp ${CMAKE_SOURCE_DIR}/extensions/GroundTiles/TileCore.cpp
    ${CMAKE_SOURCE_DIR}/extensions/GroundTiles/TileTables.cpp)
target_include_directories(lila_groundtiles_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/GroundTiles)
target_link_libraries(lila_groundtiles_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_groundtiles_tests lila_ground_tiles_extension)
target_compile_definitions(lila_groundtiles_tests PRIVATE
    LILA_TEST_GROUND_TILES_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/GroundTiles.lilaext")
add_test(NAME lila_groundtiles_tests COMMAND lila_groundtiles_tests)
