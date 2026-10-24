lila_module(lila_bake Scene
    Arrange.cpp
    AutoDistance.cpp
    Backend.cpp
    Bake.cpp
    BakeCache.cpp
    BakeCommands.cpp
    BakeGroupCommands.cpp
    BakeInputs.cpp
    BakeNodes.cpp
    BakePipeline.cpp
    BakeTypes.cpp
    Cage.cpp
    CageEstimate.cpp
    CagePreview.cpp
    BakeWrite.cpp
    CpuBackend.cpp
    HighFiles.cpp
    Lint.cpp
    Maps.cpp
    MapsCommon.cpp
    MapsDerived.cpp
    MapsMaterial.cpp
    MapsPack.cpp
    MapsSurface.cpp
    MapsUv.cpp
    Naming.cpp
    Targets.cpp
    TexelRaster.cpp)

target_link_libraries(lila_bake PUBLIC lila_scene lila_brush)
