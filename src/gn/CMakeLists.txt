lila_module(lila_gn Scene
    Clipboard.cpp
    Commands.cpp
    Exec.cpp
    Field.cpp
    Geometry.cpp
    Group.cpp
    Modifier.cpp
    Routing.cpp
    NodeLib.cpp
    NodeLibrary.cpp
    NodesBoolean.cpp
    NodesCurve.cpp
    NodesDestruction.cpp
    NodesFracture.cpp
    NodesGroup.cpp
    NodesGeometry.cpp
    NodesInput.cpp
    NodesMesh.cpp
    NodesRouting.cpp
    NodesSample.cpp
    NodesShading.cpp
    NodesTexture.cpp
    NodesUtility.cpp
    Stateful.cpp
    GroupCommands.cpp
    InstanceView.cpp
    Viewer.cpp)

find_package(manifold CONFIG REQUIRED)
target_link_libraries(lila_gn PUBLIC lila_scene lila_curve PRIVATE lila_fbx manifold::manifold)
