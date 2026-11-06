add_subdirectory(groomlab)
lila_module(lila_groom Scene Attrs.cpp Attrs.h Groom.cpp Groom.h GroomCommands.cpp GroomLockCommands.cpp GroomEdit.h GroomLab.cpp GroomLab.h GroomNodes.cpp GroomNodes.h GroomPresets.cpp GroomPresets.h
    HairCards.cpp HairCards.h Locks.cpp Locks.h Settle.cpp Settle.h Shapes.cpp Shapes.h Shell.cpp Shell.h Style.cpp Style.h)
target_link_libraries(lila_groom PUBLIC lila_curve_scene lila_clay lila_brush TBB::tbb PRIVATE groomlab_core)
