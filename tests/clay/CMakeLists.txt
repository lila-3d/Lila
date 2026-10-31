add_executable(lila_clay_tests main.cpp test_field.cpp test_mesher.cpp test_clay_nodes.cpp)
target_link_libraries(lila_clay_tests PRIVATE LilaScene doctest::doctest)
target_include_directories(lila_clay_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME lila_clay_tests COMMAND lila_clay_tests)
