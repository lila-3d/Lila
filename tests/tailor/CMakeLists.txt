# The Tailor extension's garment core: solver, fabrics, meshing and pattern import compiled in directly, no scene needed.
add_executable(lila_tailor_tests main.cpp test_cloth_solver.cpp test_meshing.cpp test_garmentcode.cpp test_blocks.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/ClothSolver.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Fabrics.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Meshing.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/GarmentCodeImport.cpp
    ${CMAKE_SOURCE_DIR}/extensions/Tailor/Blocks.cpp)
target_include_directories(lila_tailor_tests PRIVATE ${CMAKE_SOURCE_DIR}/extensions/Tailor)
target_link_libraries(lila_tailor_tests PRIVATE TBB::tbb glm::glm doctest::doctest nlohmann_json::nlohmann_json)
add_test(NAME lila_tailor_tests COMMAND lila_tailor_tests)
