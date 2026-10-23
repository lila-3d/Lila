# The Tree Generator extension: its geometry and schema compiled in directly, and the staged package driven through its commands.
add_executable(lila_treegen_tests main.cpp test_treegen.cpp
    ${CMAKE_SOURCE_DIR}/extensions/TreeGenerator/TreeBuild.cpp ${CMAKE_SOURCE_DIR}/extensions/TreeGenerator/TreeSchema.cpp)
target_include_directories(lila_treegen_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/TreeGenerator)
target_link_libraries(lila_treegen_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_treegen_tests lila_tree_generator_extension)
target_compile_definitions(lila_treegen_tests PRIVATE
    LILA_TEST_TREE_GENERATOR_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/TreeGenerator.lilaext")
add_test(NAME lila_treegen_tests COMMAND lila_treegen_tests)
