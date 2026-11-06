# Terrain, built as <bin>/<config>/extensions/Terrain.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_terrain_extension ID Terrain MANIFEST extension.json UI RESOURCES resources
    SOURCES Terrain.cpp Core.cpp Nodes.cpp Commands.cpp Export.cpp Erosion.cpp ErosionGpu.cpp Inputs.cpp Stamps.cpp Analysis.cpp SculptTool.cpp Ui.cpp ExportEngines.cpp ExportEnginesCommands.cpp
            Water.cpp WaterCommands.cpp Roads.cpp RoadCommands.cpp Volume.cpp VolumeCommands.cpp
            Terrain.h Export.h ExportEngines.h Erosion.h ErosionGpu.h Inputs.h Stamps.h Analysis.h Water.h Roads.h Volume.h)
lila_add_shaders(lila_terrain_extension FILES shaders/terrain_erosion.slang)
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_terrain_extension PRIVATE miniz::miniz)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_terrain_extension)
    endif()
endforeach()
