lila_module(lila_ops Foundation
    Command.cpp
    CommandOptions.cpp
    Undo.cpp)

target_link_libraries(lila_ops PUBLIC lila_core)
