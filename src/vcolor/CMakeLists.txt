lila_module(lila_vcolor Scene
    Paint.cpp
    Paint.h
    VertexColors.cpp
    VertexColors.h)
target_link_libraries(lila_vcolor PUBLIC lila_scene lila_geo TBB::tbb)
