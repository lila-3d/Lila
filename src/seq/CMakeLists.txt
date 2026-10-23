lila_module(lila_seq Scene
    Alembic.cpp
    MeshSequence.cpp
    SeqCommands.cpp)

find_package(Alembic CONFIG REQUIRED)
target_link_libraries(lila_seq PUBLIC lila_scene PRIVATE Alembic::Alembic)
