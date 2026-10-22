add_library(lila_maya STATIC
    MayaAscii.cpp
    MayaBinary.cpp
    MayaCommands.cpp
    MayaImport.cpp
    MayaTypes.cpp)
target_link_libraries(lila_maya PUBLIC lila_scene PRIVATE lila_io lila_fbx)
