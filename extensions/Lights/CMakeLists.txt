# Placeholder Lights, built as <bin>/<config>/extensions/Lights.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_lights_extension ID Lights MANIFEST extension.json UI
    SOURCES Lights.cpp Glyphs.cpp Glyphs.h Handles.cpp Handles.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_lights_extension)
    endif()
endforeach()
