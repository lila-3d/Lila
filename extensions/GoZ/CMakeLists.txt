# GoZ, built as <bin>/<config>/extensions/GoZ.lilaext next to lila.exe: meshes to ZBrush and back, and .GoZ files.
lila_add_extension(lila_goz_extension ID GoZ MANIFEST extension.json RESOURCES resources UI SOURCES GoZ.cpp GoZFile.cpp GoZFile.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_goz_extension)
    endif()
endforeach()
