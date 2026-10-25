lila_module(lila_stream Scene
    ChangeStream.cpp ChangeStream.h
    Internal.h
    StreamCommands.cpp
    StreamServer.cpp)

target_link_libraries(lila_stream PUBLIC lila_scene lila_io lila_link)
