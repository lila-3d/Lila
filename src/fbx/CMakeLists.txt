include(LilaUfbx)
find_package(ZLIB REQUIRED)

lila_module(lila_fbx Scene
    FbxBinary.cpp
    FbxCommands.cpp
    FbxExport.cpp
    FbxImport.cpp
    ImportCommon.cpp
    GameExport.cpp)
target_link_libraries(lila_fbx PUBLIC lila_scene PRIVATE lila_ufbx ZLIB::ZLIB)
