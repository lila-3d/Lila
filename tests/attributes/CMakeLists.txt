# The Attributes stage's panels offscreen: Attribute Editor authoring, Channel Box, Drive popover, connection and
# relationship editors, component spreadsheet. Widget state only, no GPU or display.
add_executable(lila_attribute_tests main.cpp test_ae_authoring.cpp test_ae_header.cpp test_channel_box.cpp)
target_link_libraries(lila_attribute_tests PRIVATE LilaScene LilaUI doctest::doctest)
lila_deploy_qt(lila_attribute_tests)
add_test(NAME lila_attribute_tests COMMAND lila_attribute_tests)
