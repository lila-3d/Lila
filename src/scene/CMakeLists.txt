lila_module(lila_scene Scene
    AnimCommands.cpp
    AnimCurve.cpp
    AnimLayer.cpp
    AnimInterpolator.cpp
    RotationCurve.cpp
    RotationCommands.cpp
    Repeat.cpp
    ArrayGroup.cpp ArrayGroup.h
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
    ShapeDeformers.cpp ShapeDeformers.h
    SurfaceDeformers.cpp SurfaceDeformers.h
    TextureDeformers.cpp TextureDeformers.h
    DeformEval.cpp DeformEval.h
    DeformWeights.cpp DeformWeights.h
    CorrectionDeformer.cpp
    Fields.cpp Fields.h
    Drivers.cpp
    Duplicate.cpp
    DisplayLayers.cpp
    EditSkeleton.cpp
    ExtrudeModifier.cpp ExtrudeModifier.h
    SplitRingsModifier.cpp SplitRingsModifier.h
    AutoPose.cpp AutoPose.h
    FullBodyIk.cpp FullBodyIk.h
    Fracture.cpp Fracture.h
    DominantInfluence.cpp
    Ghosts.cpp
    Graph.cpp
    HudStats.cpp HudStats.h
    Ik.cpp
    Instancing.cpp
    IkSolverRegistry.cpp
    ImagePlane.cpp
    Diagnostics.cpp
    Isolation.cpp
    LastOp.cpp
    Light.cpp Light.h
    PostProcess.cpp PostProcess.h
    LodGroup.cpp LodGroup.h
    Materials.cpp
    MeshCache.cpp MeshCache.h
    Measure.cpp
    MeasureMath.cpp
    MeasureSelection.cpp
    MeshData.cpp
    MeshEdit.cpp
    MeshEditComponentOps.cpp
    MeshEditKnife.cpp
    MeshEditMeshOps.cpp
    MeshEditNormals.cpp
    MeshEditSymmetry.cpp
    MeshEditTransform.cpp
    MeshBoolean.cpp
    MeshFileCommands.cpp
    MeshNodes.cpp
    ModifierCommands.cpp
    Morphs.cpp
    Modifiers.cpp
    Naming.cpp
    NodeBounds.cpp NodeBounds.h
    ObjectOps.cpp
    ObjectSets.cpp
    Offsets.cpp
    OrientPivot.cpp OrientPivot.h
    Node.cpp
    Plug.cpp
    Picker.cpp
    Poses.cpp
    Relocate.cpp Relocate.h
    ProxySet.cpp ProxySet.h
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
    SkinBake.cpp SkinBake.h
    SkinMethods.cpp SkinMethods.h
    Jiggle.cpp Jiggle.h
    SmartJoin.cpp
    SplitMesh.cpp SplitMesh.h
    BatchRename.cpp
    SoftSelect.cpp
    Spaces.cpp
    Take.cpp
    TextureFolders.cpp
    TransferAttributes.cpp
    Transform.cpp
    TransformCommands.cpp
    TransformOps.cpp
    Turntable.cpp Turntable.h
    ViewState.cpp ViewState.h)

find_package(Eigen3 CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(manifold CONFIG REQUIRED)
target_link_libraries(lila_scene PUBLIC lila_ops lila_geo PRIVATE lila_deform lila_retopo lila_uv meshoptimizer::meshoptimizer manifold::manifold Eigen3::Eigen)
