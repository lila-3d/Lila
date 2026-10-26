# Masonry, built as <bin>/<config>/extensions/Masonry.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_masonry_extension ID Masonry MANIFEST extension.json UI
    SOURCES Masonry.cpp MasonryCore.cpp MasonryShip.cpp MasonryBake.cpp Commands.cpp Ui.cpp Masonry.h MasonryCore.h MasonryShip.h MasonryBake.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_masonry_extension)
    endif()
endforeach()
