lila_module(lila_batch Scene
    BatchCommands.cpp BatchCommands.h
    BatchJob.cpp BatchJob.h
    BatchRunner.cpp BatchRunner.h
    BatchWorker.cpp)
target_link_libraries(lila_batch PUBLIC lila_scene PRIVATE lila_io lila_python lila_vcs)
