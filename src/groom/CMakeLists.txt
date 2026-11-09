add_subdirectory(groomlab)
# Its own static lib so GroomLab's tests link it too.
add_library(lila_groom_evaluator STATIC GroomEvaluator.cpp GroomEvaluator.h GroomEvaluatorAsync.cpp GroomEvaluatorAsync.h GroomEvaluatorStroke.cpp GroomEvaluatorViews.cpp)
target_include_directories(lila_groom_evaluator PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lila_groom_evaluator PUBLIC groomlab_core PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(lila_groom_evaluator PROPERTIES FOLDER groom)
lila_module(lila_groom Scene Attrs.cpp Attrs.h Groom.cpp Groom.h GroomCommands.cpp GroomLockCommands.cpp GroomEdit.h GroomLab.cpp GroomLab.h GroomNodes.cpp GroomNodes.h GroomPresets.cpp GroomPresets.h
    HairCards.cpp HairCards.h Locks.cpp Locks.h Settle.cpp Settle.h Shapes.cpp Shapes.h Shell.cpp Shell.h Style.cpp Style.h)
target_link_libraries(lila_groom PUBLIC lila_curve_scene lila_clay lila_brush TBB::tbb PRIVATE groomlab_core lila_groom_evaluator)
