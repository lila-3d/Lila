# The NanoVDB extension: geometry-node volumes to .nvdb files and back.
add_executable(lila_nanovdb_tests main.cpp test_nanovdb.cpp)
target_link_libraries(lila_nanovdb_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_nanovdb_tests lila_nanovdb_extension)
target_compile_definitions(lila_nanovdb_tests PRIVATE LILA_TEST_NANOVDB_PACKAGE="$<TARGET_FILE_DIR:lila_nanovdb_extension>")
add_test(NAME lila_nanovdb_tests COMMAND lila_nanovdb_tests)
