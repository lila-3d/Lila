# The Terrain extension: its core compiled in directly, and the package driven through its commands.
add_executable(lila_terrain_tests main.cpp test_terrain.cpp test_stamps.cpp test_contract.cpp test_export_engines.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Core.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Export.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/ExportEngines.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Erosion.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/ErosionGpu.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Inputs.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Terrain/Stamps.cpp ${CMAKE_SOURCE_DIR}/extensions/Terrain/Analysis.cpp)
target_include_directories(lila_terrain_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Terrain)
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_terrain_tests PRIVATE LilaUI doctest::doctest miniz::miniz)
add_dependencies(lila_terrain_tests lila_terrain_extension)
target_compile_definitions(lila_terrain_tests PRIVATE
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext"
    LILA_TEST_TERRAIN_SCRIPTS="${CMAKE_SOURCE_DIR}/extensions/Terrain/resources/engines")
add_test(NAME lila_terrain_tests COMMAND lila_terrain_tests)
