# Settlement, built as <bin>/<config>/extensions/Settlement.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_settlement_extension ID Settlement MANIFEST extension.json UI RESOURCES resources
    SOURCES Settlement.cpp Geometry.cpp StreetGraph.cpp Profiles.cpp Generate.cpp Output.cpp Region.cpp Footprint.cpp Walk.cpp Edits.cpp Commands.cpp Unreal.cpp Ui.cpp
            Settlement.h SettlementCore.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_settlement_extension)
    endif()
endforeach()
# Export: files, engine folders and the Unreal cell sync.
target_sources(lila_settlement_extension PRIVATE Export.cpp ExportCommands.cpp Export.h)
