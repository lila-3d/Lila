add_library(lila_extensions STATIC
    Library.cpp
    ExtensionManager.cpp)
target_link_libraries(lila_extensions PUBLIC lila_scene lila_python)
