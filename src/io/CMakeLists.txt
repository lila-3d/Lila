add_library(lila_io STATIC
    References.cpp
    Recovery.cpp
    SceneFile.cpp
    SceneHistory.cpp
    SceneFormats.cpp)

find_package(zstd CONFIG REQUIRED)
target_link_libraries(lila_io PUBLIC lila_scene PRIVATE zstd::libzstd)
