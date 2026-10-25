lila_module(lila_scene Scene
    AnimCommands.cpp
    AnimCurve.cpp
    AnimLayer.cpp
    AnimInterpolator.cpp
    RotationCurve.cpp
    RotationCommands.cpp
    AutoBevel.cpp
    BooleanModifier.cpp
    Booleans.cpp
    BoundingMesh.cpp
    Camera.cpp
    CenterInMesh.cpp
    ComponentPick.cpp
    ComponentQuery.cpp
    ConformCommands.cpp
    Components.cpp
    SelectPaths.cpp
    Constraint.cpp
    Deformers.cpp
    Drivers.cpp
    DisplayLayers.cpp
    EditSkeleton.cpp
    DominantInfluence.cpp
    Ghosts.cpp
    Graph.cpp
    HudStats.cpp HudStats.h
    Ik.cpp
    IkSolverRegistry.cpp
    ImagePlane.cpp
    Isolation.cpp
    LastOp.cpp
    Materials.cpp
    Measure.cpp
    MeasureMath.cpp
    MeasureSelection.cpp
    MeshData.cpp
    MeshEdit.cpp
    MeshBoolean.cpp
    MeshNodes.cpp
    ModifierCommands.cpp
    Morphs.cpp
    Modifiers.cpp
    Naming.cpp
    NodeBounds.cpp NodeBounds.h
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
    SceneData.cpp
    SceneScripts.cpp
    SelectAs.cpp
    Simulation.cpp
    Skin.cpp
    SmartJoin.cpp
    BatchRename.cpp
    SoftSelect.cpp
    Spaces.cpp
    Take.cpp
    TextureFolders.cpp
    TransferAttributes.cpp
    Transform.cpp
    TransformCommands.cpp
    TransformOps.cpp)

find_package(meshoptimizer CONFIG REQUIRED)
find_package(manifold CONFIG REQUIRED)
target_link_libraries(lila_scene PUBLIC lila_ops lila_geo PRIVATE lila_deform lila_retopo meshoptimizer::meshoptimizer manifold::manifold)
