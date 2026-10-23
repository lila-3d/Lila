# Terrain Generator, built as <bin>/<config>/extensions/TerrainGenerator.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_terrain_generator_extension ID TerrainGenerator MANIFEST extension.json UI
    SOURCES Extension.cpp TerrainCore.cpp TerrainTables.cpp TerrainScene.cpp TerrainUi.cpp TerrainCore.h TerrainGenerator.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_terrain_generator_extension)
    endif()
endforeach()
