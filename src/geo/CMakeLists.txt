find_package(meshoptimizer CONFIG REQUIRED)

lila_module(lila_geo Scene
    ArrayMesh.cpp
    Attribute.cpp
    BoundingShapes.cpp
    Bvh.cpp
    ColorSets.cpp
    ComponentPaths.cpp
    Conform.cpp
    CrossSection.cpp
    FoldEdges.cpp
    Heightfield.cpp
    Mesh.cpp
    Normals.cpp
    ObjIO.cpp
    OpsBasic.cpp
    OpsBevel.cpp
    OpsExtrude.cpp
    OpsKnife.cpp
    OpsLoop.cpp
    PlyIO.cpp
    Primitives.cpp
    Reduce.cpp
    SmartJoin.cpp
    SplitNgons.cpp
    StlIO.cpp
    RenderMesh.cpp
    Subdivide.cpp
    Symmetry.cpp
    SymmetryRadial.cpp
    Topology.cpp
    Triangulate.cpp
    third_party/mikktspace/mikktspace.c)
target_include_directories(lila_geo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lila_geo PUBLIC lila_core lila_math TBB::tbb PRIVATE meshoptimizer::meshoptimizer)
if(MSVC)
    # Vendored as-is; its shadowed locals and anonymous unions are harmless.
    set_source_files_properties(third_party/mikktspace/mikktspace.c PROPERTIES COMPILE_OPTIONS "/wd4201;/wd4456")
endif()
