find_package(meshoptimizer CONFIG REQUIRED)

add_library(lila_geo STATIC
    Attribute.cpp
    Bvh.cpp
    ColorSets.cpp
    FoldEdges.cpp
    Mesh.cpp
    Normals.cpp
    ObjIO.cpp
    OpsBasic.cpp
    OpsBevel.cpp
    OpsExtrude.cpp
    OpsLoop.cpp
    Primitives.cpp
    Reduce.cpp
    RenderMesh.cpp
    Subdivide.cpp
    Symmetry.cpp
    Topology.cpp
    Triangulate.cpp
    third_party/mikktspace/mikktspace.c)
target_include_directories(lila_geo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lila_geo PUBLIC lila_core lila_math TBB::tbb PRIVATE meshoptimizer::meshoptimizer)
if(MSVC)
    # Vendored as-is; its shadowed locals and anonymous unions are harmless.
    set_source_files_properties(third_party/mikktspace/mikktspace.c PROPERTIES COMPILE_OPTIONS "/wd4201;/wd4456")
endif()
