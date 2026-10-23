lila_module(lila_gn Scene
    Clipboard.cpp
    Commands.cpp
    Exec.cpp
    Field.cpp
    Geometry.cpp
    Group.cpp
    Modifier.cpp
    NodeLib.cpp
    NodeLibrary.cpp
    NodesBoolean.cpp
    NodesCurve.cpp
    NodesGroup.cpp
    NodesGeometry.cpp
    NodesInput.cpp
    NodesMesh.cpp
    NodesSample.cpp
    NodesShading.cpp
    NodesTexture.cpp
    NodesUtility.cpp
    GroupCommands.cpp
    Viewer.cpp)

find_package(manifold CONFIG REQUIRED)
target_link_libraries(lila_gn PUBLIC lila_scene PRIVATE lila_fbx manifold::manifold)
