# Terrain, built as <bin>/<config>/extensions/Terrain.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_terrain_extension ID Terrain MANIFEST extension.json UI
    SOURCES Terrain.cpp Core.cpp Nodes.cpp Commands.cpp Export.cpp Erosion.cpp ErosionGpu.cpp Inputs.cpp Stamps.cpp Analysis.cpp SculptTool.cpp Ui.cpp
            Projection.cpp Painted.cpp Osm.cpp Fetch.cpp DemRefine.cpp Http.cpp InputCommands.cpp
            Terrain.h Export.h Erosion.h ErosionGpu.h Inputs.h Stamps.h Analysis.h Projection.h Painted.h Osm.h Fetch.h DemRefine.h Http.h)
lila_add_shaders(lila_terrain_extension FILES shaders/terrain_erosion.slang)
find_package(miniz CONFIG REQUIRED)
find_package(Qt6 COMPONENTS Network REQUIRED)
target_link_libraries(lila_terrain_extension PRIVATE miniz::miniz Qt6::Network)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_terrain_extension)
    endif()
endforeach()
