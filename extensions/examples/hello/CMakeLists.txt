# Sample C++ extension. Builds hello.dll / hello.so into <build>/bin/<config>/extensions, which Lila searches.
add_library(lila_hello_extension MODULE Hello.cpp)
target_include_directories(lila_hello_extension PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
# Headers only: the registries live in the Lila executable.
target_link_libraries(lila_hello_extension PRIVATE nlohmann_json::nlohmann_json glm::glm)
target_compile_definitions(lila_hello_extension PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL)
set_target_properties(lila_hello_extension PROPERTIES
    OUTPUT_NAME hello
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/extensions)
