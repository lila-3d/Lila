# The OpenUSD extension loaded from its staged package next to lila.exe and driven through importUsd and exportUsd:
# round trips of Lila scenes and imports of the hand-written stages in fixtures/.
add_executable(lila_usd_tests main.cpp test_usd_roundtrip.cpp test_usd_fixtures.cpp)
target_link_libraries(lila_usd_tests PRIVATE LilaScene doctest::doctest)
add_dependencies(lila_usd_tests lila_openusd_extension)
target_compile_definitions(lila_usd_tests PRIVATE
    LILA_TEST_OPENUSD_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/OpenUSD.lilaext"
    LILA_TEST_USD_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME lila_usd_tests COMMAND lila_usd_tests)
