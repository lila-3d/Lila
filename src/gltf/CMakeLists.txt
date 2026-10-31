find_package(meshoptimizer CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(draco CONFIG REQUIRED)

lila_module(lila_gltf Scene
    GltfCommands.cpp
    GltfDraco.cpp
    GltfExport.cpp
    GltfImport.cpp
    GltfMaterials.cpp
    GltfUtil.cpp
    GltfWebViewer.cpp)
target_include_directories(lila_gltf SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/cgltf ${CMAKE_SOURCE_DIR}/third_party/stb)
target_link_libraries(lila_gltf PUBLIC lila_scene PRIVATE lila_fbx meshoptimizer::meshoptimizer miniz::miniz draco::draco)
lila_quiet_module(lila_gltf_libs Scene GltfLibs.cpp)
target_include_directories(lila_gltf_libs SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/cgltf ${CMAKE_SOURCE_DIR}/third_party/stb)
