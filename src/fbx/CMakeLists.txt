include(LilaUfbx)

add_library(lila_fbx STATIC
    FbxBinary.cpp
    FbxCommands.cpp
    FbxExport.cpp
    FbxImport.cpp)
target_link_libraries(lila_fbx PUBLIC lila_scene PRIVATE lila_ufbx)
