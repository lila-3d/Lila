# Print, built as <bin>/<config>/extensions/Print.lilaext next to lila.exe: 3D print checks, analysis, preparation and 3MF.
find_package(miniz CONFIG REQUIRED)
lila_add_extension(lila_print_extension ID Print MANIFEST extension.json UI SOURCES Print.cpp PrintGeo.cpp PrintGeo.h ThreeMf.cpp ThreeMf.h)
target_link_libraries(lila_print_extension PRIVATE miniz::miniz)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_print_extension)
    endif()
endforeach()
