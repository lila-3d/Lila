# The Foliage extension loaded from its staged package next to lila.exe, driven through its commands, plus its blade extrude
# maths and tree geometry and schema compiled in directly.
add_executable(lila_foliage_tests main.cpp test_blades.cpp test_treegen.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Foliage/BladeExtrude.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Foliage/TreeBuild.cpp ${CMAKE_SOURCE_DIR}/extensions/Foliage/TreeSchema.cpp)
target_include_directories(lila_foliage_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Foliage)
target_link_libraries(lila_foliage_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_foliage_tests lila_foliage_extension)
target_compile_definitions(lila_foliage_tests PRIVATE
    LILA_TEST_FOLIAGE_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Foliage.lilaext")
add_test(NAME lila_foliage_tests COMMAND lila_foliage_tests)
