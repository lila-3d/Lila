lila_module(lila_link Scene
    Exporters.cpp
    LinkCommands.cpp
    RpcClient.cpp
    Socket.cpp
    TextureSend.cpp
    UnrealLink.cpp)

target_link_libraries(lila_link PUBLIC lila_scene)
if(WIN32)
    target_link_libraries(lila_link PRIVATE ws2_32)
endif()
