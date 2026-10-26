# Foliage, built as <bin>/<config>/extensions/Foliage.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_foliage_extension ID Foliage MANIFEST extension.json UI
    SOURCES Foliage.cpp Blades.cpp BladesUi.cpp BladeExtrude.cpp TreeGenerator.cpp TreeUi.cpp TreeBuild.cpp TreeSchema.cpp
            Blades.h BladeExtrude.h TreeGenerator.h TreeBuild.h TreeSchema.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_foliage_extension)
    endif()
endforeach()
