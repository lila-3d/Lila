# Ground Tiles, built as <bin>/<config>/extensions/GroundTiles.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_ground_tiles_extension ID GroundTiles MANIFEST extension.json UI
    SOURCES Extension.cpp TileCore.cpp TileTables.cpp TileScene.cpp TileUi.cpp TileCore.h GroundTiles.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_ground_tiles_extension)
    endif()
endforeach()
