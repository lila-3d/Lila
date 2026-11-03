# The Stagehand extension: the physics scene compiled in directly, and the package driven through its commands.
add_executable(lila_stagehand_tests main.cpp test_stagehand.cpp test_physics_scale.cpp test_level.cpp)
target_sources(lila_stagehand_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Stagehand/Arrange.cpp)
target_include_directories(lila_stagehand_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Stagehand ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(lila_stagehand_tests PRIVATE LilaUI lila_physics doctest::doctest)
add_dependencies(lila_stagehand_tests lila_stagehand_extension lila_terrain_extension)
target_compile_definitions(lila_stagehand_tests PRIVATE
    LILA_TEST_STAGEHAND_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Stagehand.lilaext"
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext")
add_test(NAME lila_stagehand_tests COMMAND lila_stagehand_tests)
