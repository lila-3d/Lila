lila_module(lila_retarget Scene
    Bvh.cpp
    Bvh.h
    RetargetCommands.cpp
    Retarget.cpp
    Retarget.h
    Rig.cpp
    Rig.h
    SceneRetarget.cpp
    SceneRetarget.h
    Semantic.cpp
    Semantic.h
    Skeleton.cpp
    Skeleton.h)
target_link_libraries(lila_retarget PUBLIC lila_scene lila_fbx lila_io TBB::tbb)
