# Viewport renderer overrides composited offscreen, and the example renderer loaded as an extension; the cases pass
# without checking when no Vulkan device can be created.
lila_add_extension(lila_test_viewport_renderer_extension ID viewportRenderer UI
    MANIFEST ${CMAKE_SOURCE_DIR}/extensions/examples/viewport_renderer/extension.json
    SOURCES ${CMAKE_SOURCE_DIR}/extensions/examples/viewport_renderer/ViewportRenderer.cpp
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests/viewport_renderers/$<CONFIG>/native)
add_executable(lila_viewport_renderer_tests
    main.cpp
    test_viewport_renderers.cpp)
target_link_libraries(lila_viewport_renderer_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_viewport_renderer_tests lila_test_viewport_renderer_extension)
target_compile_definitions(lila_viewport_renderer_tests PRIVATE
    LILA_TEST_VIEWPORT_RENDERER_PACKAGE="${CMAKE_BINARY_DIR}/tests/viewport_renderers/$<CONFIG>/native/viewportRenderer.lilaext")
add_test(NAME lila_viewport_renderer_tests COMMAND lila_viewport_renderer_tests)
