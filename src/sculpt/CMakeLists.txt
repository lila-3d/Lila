find_package(Eigen3 CONFIG REQUIRED)

lila_module(lila_sculpt Scene
    Reproject.cpp
    Reproject.h
    Sculpt.cpp
    Sculpt.h
    SubdLevels.cpp
    SubdLevels.h)
target_link_libraries(lila_sculpt PUBLIC lila_geo lila_brush TBB::tbb PRIVATE lila_retopo Eigen3::Eigen)
if(MSVC)
    # Eigen's own headers trip a few /W4 checks.
    target_compile_options(lila_sculpt PRIVATE /wd4127 /wd4244)
endif()

lila_module(lila_sculpt_cmds Scene SculptCommands.cpp SculptSession.h)
target_link_libraries(lila_sculpt_cmds PUBLIC lila_sculpt lila_scene PRIVATE lila_retopo)
