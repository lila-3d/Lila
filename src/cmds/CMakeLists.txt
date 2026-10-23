lila_module(lila_cmds Foundation
    Command.cpp
    CommandOptions.cpp
    Undo.cpp)

target_link_libraries(lila_cmds PUBLIC lila_core)
