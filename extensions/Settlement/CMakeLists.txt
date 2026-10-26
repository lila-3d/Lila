# Settlement, built as <bin>/<config>/extensions/Settlement.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_settlement_extension ID Settlement MANIFEST extension.json UI
    SOURCES Settlement.cpp Geometry.cpp StreetGraph.cpp Profiles.cpp Generate.cpp Output.cpp Region.cpp Commands.cpp Ui.cpp
            Settlement.h SettlementCore.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_settlement_extension)
    endif()
endforeach()
