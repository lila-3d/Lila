# Seamwork: the join core compiled in directly, and the package driven through its commands with Terrain loaded.
add_executable(lila_seamwork_tests main.cpp test_seams.cpp test_seam_scene.cpp)
target_sources(lila_seamwork_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Seamwork/Seams.cpp ${CMAKE_SOURCE_DIR}/extensions/Seamwork/Scatter.cpp ${CMAKE_SOURCE_DIR}/extensions/Seamwork/Strip.cpp ${CMAKE_SOURCE_DIR}/extensions/Seamwork/Skirt.cpp)
target_include_directories(lila_seamwork_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Seamwork ${CMAKE_SOURCE_DIR}/sdk/include ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(lila_seamwork_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_seamwork_tests lila_seamwork_extension lila_terrain_extension lila_stagehand_extension)
target_compile_definitions(lila_seamwork_tests PRIVATE
    LILA_TEST_SEAMWORK_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Seamwork.lilaext"
    LILA_TEST_TERRAIN_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Terrain.lilaext"
    LILA_TEST_STAGEHAND_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Stagehand.lilaext")
add_test(NAME lila_seamwork_tests COMMAND lila_seamwork_tests)
