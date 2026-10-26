lila_module(lila_seq Scene
    Alembic.cpp
    AlembicImport.cpp
    MeshSequence.cpp
    PieceJoints.cpp
    RigidPieces.cpp
    SeqCommands.cpp
    Vat.cpp)

find_package(Alembic CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_seq PUBLIC lila_scene PRIVATE Alembic::Alembic miniz::miniz)
