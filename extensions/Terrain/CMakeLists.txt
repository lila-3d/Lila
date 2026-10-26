# Terrain, built as <bin>/<config>/extensions/Terrain.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_terrain_extension ID Terrain MANIFEST extension.json
    SOURCES Terrain.cpp Core.cpp Nodes.cpp Commands.cpp Export.cpp Erosion.cpp Inputs.cpp Terrain.h Export.h Erosion.h Inputs.h)
find_package(miniz CONFIG REQUIRED)
target_link_libraries(lila_terrain_extension PRIVATE miniz::miniz)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_terrain_extension)
    endif()
endforeach()
