lila_module(lila_io Scene
    Examples.cpp
    References.cpp
    Recovery.cpp
    SceneFile.cpp
    SceneHistory.cpp
    SceneFormats.cpp
    FileTranslators.cpp)

find_package(zstd CONFIG REQUIRED)
target_link_libraries(lila_io PUBLIC lila_scene PRIVATE zstd::libzstd)
