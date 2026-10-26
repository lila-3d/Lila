# The Masonry extension: its layout and stone meshes compiled in directly, and the package driven through its commands.
add_executable(lila_masonry_tests main.cpp test_masonry.cpp ${CMAKE_SOURCE_DIR}/extensions/Masonry/MasonryCore.cpp)
target_include_directories(lila_masonry_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Masonry)
target_link_libraries(lila_masonry_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_masonry_tests lila_masonry_extension)
target_compile_definitions(lila_masonry_tests PRIVATE
    LILA_TEST_MASONRY_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/Masonry.lilaext")
add_test(NAME lila_masonry_tests COMMAND lila_masonry_tests)
