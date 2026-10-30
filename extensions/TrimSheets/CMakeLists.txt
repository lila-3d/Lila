# Trim Sheets, built as <bin>/<config>/extensions/TrimSheets.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_trimsheets_extension ID TrimSheets MANIFEST extension.json RESOURCES resources UI
    SOURCES TrimSheets.cpp TrimSheet.cpp Generator.cpp TrimBake.cpp Commands.cpp Ui.cpp SheetEditor.cpp Strips.cpp TrimSheets.h TrimSheet.h Generator.h TrimBake.h Strips.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_trimsheets_extension)
    endif()
endforeach()
