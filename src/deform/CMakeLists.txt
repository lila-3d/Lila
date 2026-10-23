lila_module(lila_deform Scene
    Deform.cpp
    Deform.h)
target_link_libraries(lila_deform PUBLIC lila_geo TBB::tbb)
