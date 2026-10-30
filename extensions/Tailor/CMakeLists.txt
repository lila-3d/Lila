# Tailor, built as <bin>/<config>/extensions/Tailor.lilaext next to lila.exe, which loads it by default.
lila_add_extension(lila_tailor_extension ID Tailor MANIFEST extension.json UI
    SOURCES Tailor.cpp Commands.cpp Ui.cpp ClothSolver.cpp Fabrics.cpp Meshing.cpp Blocks.cpp GarmentCodeImport.cpp
            Tailor.h TailorCore.h Blocks.h GarmentCodeImport.h)
target_link_libraries(lila_tailor_extension PRIVATE nlohmann_json::nlohmann_json)
foreach(_host lila lilabatch)
    if(TARGET ${_host})
        add_dependencies(${_host} lila_tailor_extension)
    endif()
endforeach()
