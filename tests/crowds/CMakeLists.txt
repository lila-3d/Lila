# The Crowds extension: its runtime compiled in directly, and the package driven through its commands.
set(_crowds ${CMAKE_SOURCE_DIR}/extensions/Crowds)
add_executable(lila_crowd_tests main.cpp test_core.cpp
    ${_crowds}/CrowdCore.cpp ${_crowds}/Orca.cpp)
target_include_directories(lila_crowd_tests PRIVATE ${_crowds})
target_link_libraries(lila_crowd_tests PRIVATE LilaUI doctest::doctest)
add_test(NAME lila_crowd_tests COMMAND lila_crowd_tests)
