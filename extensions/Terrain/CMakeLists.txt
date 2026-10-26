# Terrain, built as <bin>/<config>/extensions/Terrain.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_terrain_extension ID Terrain MANIFEST extension.json
    SOURCES Terrain.cpp Core.cpp Nodes.cpp Commands.cpp Terrain.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_terrain_extension)
    endif()
endforeach()
