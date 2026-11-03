# The Settlement extension: its layout compiled in directly, and the package driven through its commands.
set(_settlement ${CMAKE_SOURCE_DIR}/extensions/Settlement)
add_executable(lila_settlement_tests main.cpp test_settlement.cpp test_structure.cpp test_streets.cpp
    ${_settlement}/Geometry.cpp ${_settlement}/StreetGraph.cpp ${_settlement}/Profiles.cpp ${_settlement}/Generate.cpp
    ${_settlement}/Output.cpp ${_settlement}/Region.cpp
    ${_settlement}/Footprint.cpp ${_settlement}/Walk.cpp ${_settlement}/Edits.cpp)
target_include_directories(lila_settlement_tests PRIVATE ${_settlement})
target_link_libraries(lila_settlement_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_settlement_tests lila_settlement_extension lila_terrain_extension)
target_compile_definitions(lila_settlement_tests PRIVATE
    LILA_TEST_SETTLEMENT_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Settlement.lilaext"
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext")
add_test(NAME lila_settlement_tests COMMAND lila_settlement_tests)
