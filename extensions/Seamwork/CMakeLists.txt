# Seamwork, built as <bin>/<config>/extensions/Seamwork.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_seamwork_extension ID Seamwork MANIFEST extension.json UI
    SOURCES Seamwork.cpp Seams.cpp SeamNode.cpp Fields.cpp Commands.cpp Ui.cpp Scatter.cpp ScatterCommands.cpp Unreal.cpp Strip.cpp StripCommands.cpp Skirt.cpp SkirtCommands.cpp Agent.cpp Seams.h Seamwork.h Scatter.h Strip.h Skirt.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_seamwork_extension)
    endif()
endforeach()
