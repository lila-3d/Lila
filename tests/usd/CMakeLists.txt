# The OpenUSD extension loaded from its staged package next to lila.exe and driven through importUsd and exportUsd:
# round trips of Lila scenes, imports of the hand-written stages in fixtures/, and live stages.
add_executable(lila_usd_tests main.cpp test_usd_roundtrip.cpp test_usd_fixtures.cpp test_usd_stage.cpp)
# LilaUI: the extension's Attribute Editor sections link it, so its DLLs must sit beside the tests.
target_link_libraries(lila_usd_tests PRIVATE LilaScene LilaUI doctest::doctest)
add_dependencies(lila_usd_tests lila_openusd_extension)
target_compile_definitions(lila_usd_tests PRIVATE
    LILA_TEST_OPENUSD_PACKAGE="${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions/OpenUSD.lilaext"
    LILA_TEST_USD_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
if(WIN32)
    cmake_path(GET Python_EXECUTABLE PARENT_PATH _pyhome)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    cmake_path(GET _qmake PARENT_PATH _qtbin)
    add_custom_command(TARGET lila_usd_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pyhome}/python3.dll ${_pyhome}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll $<TARGET_FILE_DIR:lila_usd_tests>
        COMMAND ${_qtbin}/windeployqt.exe --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime $<TARGET_FILE:lila_usd_tests> $<TARGET_FILE:LilaUI> > NUL
        COMMENT "Deploying Python and Qt for lila_usd_tests")
endif()
add_test(NAME lila_usd_tests COMMAND lila_usd_tests)
