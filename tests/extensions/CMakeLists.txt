# The sample extension, built for the tests on its own so these run without the root adding extensions/examples.
add_library(lila_test_hello_extension MODULE ${CMAKE_SOURCE_DIR}/extensions/examples/hello/Hello.cpp)
target_include_directories(lila_test_hello_extension PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lila_test_hello_extension PRIVATE nlohmann_json::nlohmann_json glm::glm)
target_compile_definitions(lila_test_hello_extension PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL)
set_target_properties(lila_test_hello_extension PROPERTIES
    OUTPUT_NAME hello
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/extensions/$<CONFIG>/native)

add_executable(lila_extensions_tests
    main.cpp
    test_extensions.cpp)
target_link_libraries(lila_extensions_tests PRIVATE lila_extensions doctest::doctest nanobind-static Python::Python)
nanobind_compile_options(lila_extensions_tests)
nanobind_link_options(lila_extensions_tests)
add_dependencies(lila_extensions_tests lila_test_hello_extension)
target_compile_definitions(lila_extensions_tests PRIVATE
    LILA_TEST_HELLO_EXTENSION="$<TARGET_FILE:lila_test_hello_extension>"
    LILA_TEST_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/extensions/examples")

if(WIN32)
    cmake_path(GET Python_EXECUTABLE PARENT_PATH _pyhome)
    add_custom_command(TARGET lila_extensions_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_pyhome}/python3.dll ${_pyhome}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll $<TARGET_FILE_DIR:lila_extensions_tests>)
endif()

add_test(NAME lila_extensions_tests COMMAND lila_extensions_tests)
