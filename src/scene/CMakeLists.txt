add_library(lila_scene STATIC
    AnimCommands.cpp
    AnimCurve.cpp
    AutoBevel.cpp
    Booleans.cpp
    Camera.cpp
    ComponentPick.cpp
    ComponentQuery.cpp
    Components.cpp
    Constraint.cpp
    Deformers.cpp
    DisplayLayers.cpp
    Graph.cpp
    HudStats.cpp HudStats.h
    ImagePlane.cpp
    LastOp.cpp
    Materials.cpp
    Measure.cpp
    MeasureMath.cpp
    MeasureSelection.cpp
    MeshData.cpp
    MeshEdit.cpp
    MeshNodes.cpp
    ModifierCommands.cpp
    Modifiers.cpp
    Naming.cpp
    ObjectOps.cpp
    Node.cpp
    Plug.cpp
    RenderSettings.cpp
    Rig.cpp
    SceneCommands.cpp
    SceneScripts.cpp
    Skin.cpp
    SoftSelect.cpp
    TransferAttributes.cpp
    Transform.cpp
    TransformCommands.cpp
    TransformOps.cpp)

find_package(meshoptimizer CONFIG REQUIRED)
find_package(manifold CONFIG REQUIRED)
target_link_libraries(lila_scene PUBLIC lila_cmds lila_geo PRIVATE lila_deform meshoptimizer::meshoptimizer manifold::manifold)
