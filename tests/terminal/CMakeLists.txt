add_executable(lila_terminal_tests
    main.cpp
    test_vt.cpp
    test_scroll.cpp)
target_link_libraries(lila_terminal_tests PRIVATE LilaUI doctest::doctest)
add_test(NAME lila_terminal_tests COMMAND lila_terminal_tests)

# Throwaway visual check: the API reference and terminal panels, grabbed to PNGs. Usage: lila_panels_demo <outdir>
add_executable(lila_panels_demo EXCLUDE_FROM_ALL demo.cpp)
target_link_libraries(lila_panels_demo PRIVATE LilaUI)

# Real children on a pseudo console through PtyProcess, headless.
add_executable(lila_terminal_pty_tests test_pty.cpp)
target_link_libraries(lila_terminal_pty_tests PRIVATE LilaUI doctest::doctest)
add_test(NAME lila_terminal_pty_tests COMMAND lila_terminal_pty_tests)
