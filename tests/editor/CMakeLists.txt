add_executable(lila_editor_tests
    main.cpp
    test_highlighter.cpp
    test_lsp.cpp
    test_text.cpp
    test_vim.cpp)
target_link_libraries(lila_editor_tests PRIVATE LilaUI doctest::doctest)
add_test(NAME lila_editor_tests COMMAND lila_editor_tests)

# Throwaway visual check: opens the ScriptEditor with Python running and can grab screenshots.
add_executable(lila_editor_demo EXCLUDE_FROM_ALL demo.cpp)
target_link_libraries(lila_editor_demo PRIVATE LilaUI)
