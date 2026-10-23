# Tree Generator, built as <bin>/<config>/extensions/TreeGenerator.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_tree_generator_extension ID TreeGenerator MANIFEST extension.json UI
    SOURCES TreeGenerator.cpp TreeUi.cpp TreeBuild.cpp TreeSchema.cpp TreeGenerator.h TreeBuild.h TreeSchema.h)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_tree_generator_extension)
    endif()
endforeach()
