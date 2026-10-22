add_library(lila_vcs STATIC
    GitProvider.cpp
    PerforceProvider.cpp
    Process.cpp
    Provider.cpp
    SourceControl.cpp
    VcsCommands.cpp)
target_link_libraries(lila_vcs PUBLIC lila_cmds)
