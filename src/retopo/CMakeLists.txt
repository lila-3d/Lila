find_package(Eigen3 CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)

add_library(lila_retopo STATIC
    Extract.cpp
    Field.cpp
    Internal.h
    Metrics.cpp
    QuadRemesh.cpp
    Relax.cpp
    Repair.cpp
    Retopo.h
    Surface.cpp
    Voxel.cpp)
target_link_libraries(lila_retopo PUBLIC lila_geo TBB::tbb PRIVATE Eigen3::Eigen meshoptimizer::meshoptimizer)
if(MSVC)
    # Eigen's own headers trip a few /W4 checks.
    target_compile_options(lila_retopo PRIVATE /wd4127 /wd4244)
endif()

add_library(lila_retopo_cmds STATIC RetopoCommands.cpp RetopoCommands.h)
target_link_libraries(lila_retopo_cmds PUBLIC lila_retopo lila_scene)
