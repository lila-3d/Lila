add_library(lila_math STATIC
    Color.cpp
    Math.cpp
    Matrix.cpp
    Rotation.cpp
    Shapes.cpp
    Vector.cpp)
target_include_directories(lila_math PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lila_math PUBLIC lila_core)
