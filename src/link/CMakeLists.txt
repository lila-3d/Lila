lila_module(lila_link Scene
    Exporters.cpp
    ExpressionRig.cpp
    LinkCommands.cpp
    MlDeformerExport.cpp
    Placements.cpp
    RpcClient.cpp
    Socket.cpp
    SplineSend.cpp
    SplineSend.h
    TextureSend.cpp
    TextureSetSend.cpp
    TextureSetSend.h
    UnrealLink.cpp)

target_link_libraries(lila_link PUBLIC lila_scene lila_tex lila_curve_scene PRIVATE lila_gn lila_fbx)
if(WIN32)
    target_link_libraries(lila_link PRIVATE ws2_32)
endif()
