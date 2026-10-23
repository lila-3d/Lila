lila_module(lila_extensions Scene
    Library.cpp
    ExtensionManager.cpp)
target_link_libraries(lila_extensions PUBLIC lila_scene lila_python)
