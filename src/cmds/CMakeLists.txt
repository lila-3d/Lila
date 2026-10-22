add_library(lila_cmds STATIC
    Command.cpp
    CommandOptions.cpp
    Undo.cpp)

target_link_libraries(lila_cmds PUBLIC lila_core)
