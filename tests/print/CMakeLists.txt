# The Print extension: print checks, analysis, hollow, split and 3MF, on known meshes and through its commands.
find_package(miniz CONFIG REQUIRED)
add_executable(lila_print_tests main.cpp test_print_geo.cpp test_print_commands.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Print/PrintGeo.cpp ${CMAKE_SOURCE_DIR}/extensions/Print/ThreeMf.cpp)
target_include_directories(lila_print_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Print ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(lila_print_tests PRIVATE LilaUI miniz::miniz doctest::doctest)
add_dependencies(lila_print_tests lila_print_extension)
target_compile_definitions(lila_print_tests PRIVATE LILA_TEST_PRINT_PACKAGE="$<TARGET_FILE_DIR:lila_print_extension>")
add_test(NAME lila_print_tests COMMAND lila_print_tests)
