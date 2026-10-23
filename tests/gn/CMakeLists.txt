add_executable(lila_gn_tests
    main.cpp
    test_clipboard.cpp
    test_gn.cpp
    test_nodes.cpp)
target_link_libraries(lila_gn_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_gn_tests COMMAND lila_gn_tests)

# Throwaway visual check of the node editor: builds a group, opens the editor offscreen and grabs it to a PNG.
add_executable(lila_gn_demo EXCLUDE_FROM_ALL demo.cpp)
target_link_libraries(lila_gn_demo PRIVATE LilaUI)
