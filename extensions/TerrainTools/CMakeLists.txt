# Terrain Tools, built as <bin>/<config>/extensions/TerrainTools.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_terrain_tools_extension ID TerrainTools MANIFEST extension.json UI
    SOURCES TerrainTools.cpp Ui.cpp BladeExtrude.cpp BladeExtrude.h TerrainTools.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_terrain_tools_extension)
    endif()
endforeach()
