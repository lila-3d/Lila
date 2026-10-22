add_library(lila_link STATIC
    Exporters.cpp
    LinkCommands.cpp
    RpcClient.cpp
    Socket.cpp
    UnrealLink.cpp)

target_link_libraries(lila_link PUBLIC lila_scene)
if(WIN32)
    target_link_libraries(lila_link PRIVATE ws2_32)
endif()
