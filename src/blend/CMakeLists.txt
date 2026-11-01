lila_module(lila_blend Scene
    BlendCommands.cpp
    BlendFile.cpp
    BlendImport.cpp
    BlendMaterials.cpp
    BlendMesh.cpp
    BlendModifiers.cpp
    BlendRig.cpp)
find_package(zstd CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(lila_blend PUBLIC lila_scene PRIVATE lila_io lila_fbx lila_curve lila_vcs zstd::libzstd ZLIB::ZLIB)
