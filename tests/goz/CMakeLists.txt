# The GoZ extension: .GoZ files and the ZBrush round trip through the GoZ folders.
add_executable(lila_goz_tests main.cpp test_goz.cpp ${CMAKE_SOURCE_DIR}/extensions/GoZ/GoZFile.cpp)
target_include_directories(lila_goz_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/GoZ ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(lila_goz_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_goz_tests lila_goz_extension)
target_compile_definitions(lila_goz_tests PRIVATE LILA_TEST_GOZ_PACKAGE="$<TARGET_FILE_DIR:lila_goz_extension>")
add_test(NAME lila_goz_tests COMMAND lila_goz_tests)
