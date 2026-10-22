find_package(Eigen3 CONFIG REQUIRED)

add_library(lila_sculpt STATIC
    Reproject.cpp
    Reproject.h
    Sculpt.cpp
    Sculpt.h)
target_link_libraries(lila_sculpt PUBLIC lila_geo TBB::tbb PRIVATE lila_retopo Eigen3::Eigen)
if(MSVC)
    # Eigen's own headers trip a few /W4 checks.
    target_compile_options(lila_sculpt PRIVATE /wd4127 /wd4244)
endif()

add_library(lila_sculpt_cmds STATIC SculptCommands.cpp SculptSession.h)
target_link_libraries(lila_sculpt_cmds PUBLIC lila_sculpt lila_scene PRIVATE lila_retopo)
