# Destruction, built as <bin>/<config>/extensions/Destruction.lilaext next to lila.exe, which loads it by default. Jolt
# comes in statically with lila_physics.
lila_add_extension(lila_destruction_extension ID Destruction MANIFEST extension.json UI
    SOURCES Destruction.cpp Commands.cpp ChaosExport.cpp Damage.cpp Freeze.cpp Ui.cpp RigidWorld.cpp Sounds.cpp Destruction.h Pieces.h PieceTransform.h RigidWorld.h
            RigidState.h)
target_link_libraries(lila_destruction_extension PRIVATE lila_physics)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_destruction_extension)
    endif()
endforeach()
