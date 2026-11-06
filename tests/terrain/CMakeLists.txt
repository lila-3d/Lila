# The Terrain extension: its core compiled in directly, and the package driven through its commands.
add_executable(lila_terrain_tests main.cpp test_terrain.cpp test_stamps.cpp test_contract.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Core.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Export.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Erosion.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/ErosionGpu.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Inputs.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Stamps.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Analysis.cpp
    test_inputs.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Projection.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Painted.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Osm.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Fetch.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/DemRefine.cpp)
target_include_directories(lila_terrain_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Terrain)
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_terrain_tests PRIVATE LilaUI doctest::doctest miniz::miniz)
add_dependencies(lila_terrain_tests lila_terrain_extension)
target_compile_definitions(lila_terrain_tests PRIVATE
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext")
add_test(NAME lila_terrain_tests COMMAND lila_terrain_tests "--test-case-exclude=*network*")
# Downloads real data; runs only with LILA_NETWORK_TESTS=1 set.
add_test(NAME lila_terrain_network_tests COMMAND lila_terrain_tests "--test-case=*network*")
set_tests_properties(lila_terrain_network_tests PROPERTIES LABELS network)
