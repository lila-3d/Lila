lila_module(lila_mcp Scene
    AgentIntegration.cpp AgentIntegration.h
    AgentResources.cpp
    BuiltinTools.cpp
    McpServer.cpp)

target_link_libraries(lila_mcp PUBLIC lila_python httplib::httplib)
