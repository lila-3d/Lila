# The base simulation framework, and the Overlap and Physics Animation extensions built on it.
add_executable(lila_sim_tests main.cpp test_simulation.cpp test_overlap.cpp test_physics.cpp)
target_link_libraries(lila_sim_tests PRIVATE LilaUI doctest::doctest)
add_dependencies(lila_sim_tests lila_overlap_extension lila_physics_animation_extension)
target_compile_definitions(lila_sim_tests PRIVATE LILA_TEST_OVERLAP_PACKAGE="$<TARGET_FILE_DIR:lila_overlap_extension>"
    LILA_TEST_PHYSICS_PACKAGE="$<TARGET_FILE_DIR:lila_physics_animation_extension>")
if(WIN32)
    cmake_path(GET Python_EXECUTABLE PARENT_PATH _pyhome)
    add_custom_command(TARGET lila_sim_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pyhome}/python3.dll ${_pyhome}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll $<TARGET_FILE_DIR:lila_sim_tests>)
endif()
add_test(NAME lila_sim_tests COMMAND lila_sim_tests)
