lila_module(lila_scene Scene
    AnimCommands.cpp
    AnimCurve.cpp
    AnimLayer.cpp
    AnimInterpolator.cpp
    RotationCurve.cpp
    RotationCommands.cpp
    AutoBevel.cpp
    Booleans.cpp
    Camera.cpp
    CenterInMesh.cpp
    ComponentPick.cpp
    ComponentQuery.cpp
    Components.cpp
    Constraint.cpp
    Deformers.cpp
    DisplayLayers.cpp
    EditSkeleton.cpp
    DominantInfluence.cpp
    Ghosts.cpp
    Graph.cpp
    HudStats.cpp HudStats.h
    Ik.cpp
    IkSolverRegistry.cpp
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
    ObjectSets.cpp
    Offsets.cpp
    Node.cpp
    Plug.cpp
    Picker.cpp
    Poses.cpp
    RenderSettings.cpp
    Rig.cpp
    RigElements.cpp
    RigElementCommands.cpp
    SceneCommands.cpp
    SceneScripts.cpp
    SelectAs.cpp
    Simulation.cpp
    Skin.cpp
    SoftSelect.cpp
    Spaces.cpp
    Take.cpp
    TransferAttributes.cpp
    Transform.cpp
    TransformCommands.cpp
    TransformOps.cpp)

find_package(meshoptimizer CONFIG REQUIRED)
find_package(manifold CONFIG REQUIRED)
target_link_libraries(lila_scene PUBLIC lila_cmds lila_geo PRIVATE lila_deform meshoptimizer::meshoptimizer manifold::manifold)
