# Overlap, built as <bin>/<config>/extensions/Overlap.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_overlap_extension ID Overlap MANIFEST extension.json UI
    SOURCES Overlap.cpp OverlapUi.cpp Overlap.h OverlapSolver.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_overlap_extension)
    endif()
endforeach()
