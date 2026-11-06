lila_module(lila_collada Scene
    ColladaCommands.cpp
    ColladaExport.cpp
    ColladaImport.cpp
    Xml.cpp)
target_link_libraries(lila_collada PUBLIC lila_scene PRIVATE lila_fbx lila_gltf)
