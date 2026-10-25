# Destruction, built as <bin>/<config>/extensions/Destruction.lilaext next to lila.exe, which loads it by default. Jolt
# (static, cross-platform deterministic) links into this extension only.
find_package(Jolt CONFIG REQUIRED)
lila_add_extension(lila_destruction_extension ID Destruction MANIFEST extension.json UI
    SOURCES Destruction.cpp Commands.cpp Ui.cpp RigidWorld.cpp Destruction.h PieceTransform.h RigidWorld.h RigidState.h)
target_link_libraries(lila_destruction_extension PRIVATE Jolt::Jolt)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_destruction_extension)
    endif()
endforeach()
