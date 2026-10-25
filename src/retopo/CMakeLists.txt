find_package(Eigen3 CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)

lila_module(lila_retopo Scene
    AngleSnap.cpp
    Extract.cpp
    Border.cpp
    Budget.cpp
    Construct.cpp
    Construct.h
    Field.cpp
    Generators.cpp
    Handles.cpp
    Handles.h
    Internal.h
    LiveRelax.cpp
    LiveRelax.h
    Metrics.cpp
    Patch.cpp
    Patch.h
    Poles.cpp
    QuadRemesh.cpp
    Relax.cpp
    Repair.cpp
    Retopo.h
    Seamless.cpp
    Surface.cpp
    SurfaceStroke.cpp
    SurfaceStroke.h
    Voxel.cpp)
target_link_libraries(lila_retopo PUBLIC lila_geo TBB::tbb PRIVATE Eigen3::Eigen meshoptimizer::meshoptimizer)
if(MSVC)
    # Eigen's own headers trip a few /W4 checks.
    target_compile_options(lila_retopo PRIVATE /wd4127 /wd4244)
endif()

lila_module(lila_retopo_cmds Scene RetopoCommands.cpp RetopoCommands.h TopologyCommands.cpp)
target_link_libraries(lila_retopo_cmds PUBLIC lila_retopo lila_scene)
