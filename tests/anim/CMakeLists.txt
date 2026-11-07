add_executable(lila_anim_tests
    main.cpp
    test_curves.cpp
    test_keys.cpp
    test_layers.cpp
    test_playback.cpp
    test_rotation.cpp
    test_key_ops.cpp)
target_link_libraries(lila_anim_tests PRIVATE LilaScene doctest::doctest)
add_test(NAME lila_anim_tests COMMAND lila_anim_tests)

# Throwaway visual check: builds the time slider and graph editor and grabs them to PNGs.
add_executable(lila_anim_demo EXCLUDE_FROM_ALL demo.cpp)
target_link_libraries(lila_anim_demo PRIVATE LilaUI)
