# Trim Sheets: the sheet model, generator, bake and the core hotspot matcher compiled in directly, and the package driven through its commands.
add_executable(lila_trim_tests main.cpp test_trim.cpp ${CMAKE_SOURCE_DIR}/extensions/TrimSheets/TrimSheet.cpp
    ${CMAKE_SOURCE_DIR}/extensions/TrimSheets/Generator.cpp
    ${CMAKE_SOURCE_DIR}/extensions/TrimSheets/TrimBake.cpp
    ${CMAKE_SOURCE_DIR}/extensions/TrimSheets/Strips.cpp)
target_include_directories(lila_trim_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/TrimSheets ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(lila_trim_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_trim_tests lila_trimsheets_extension)
target_compile_definitions(lila_trim_tests PRIVATE
    LILA_TEST_TRIM_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/TrimSheets.lilaext")
add_test(NAME lila_trim_tests COMMAND lila_trim_tests)
