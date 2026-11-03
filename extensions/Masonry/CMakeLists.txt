# Masonry, built as <bin>/<config>/extensions/Masonry.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_masonry_extension ID Masonry MANIFEST extension.json UI
    SOURCES Masonry.cpp MasonryCore.cpp MasonryArch.cpp MasonryBuilding.cpp MasonryRuin.cpp MasonryShip.cpp MasonryBake.cpp MasonryAgent.cpp Commands.cpp Ui.cpp
    Masonry.h MasonryCore.h MasonryArch.h MasonryBuilding.h MasonryRuin.h MasonryShip.h MasonryBake.h)
target_link_libraries(lila_masonry_extension PRIVATE lila_physics)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_masonry_extension)
    endif()
endforeach()
