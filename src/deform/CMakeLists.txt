add_library(lila_deform STATIC
    Deform.cpp
    Deform.h)
target_link_libraries(lila_deform PUBLIC lila_geo TBB::tbb)
